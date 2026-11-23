#include "shell/netease_tracks.h"

#include "log.h"

#include "win32_path.h"

#include <windows.h>
#include <shlobj.h>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winsqlite/winsqlite3.h>

#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>
#include <vector>

namespace atrium {

namespace {

namespace json = winrt::Windows::Data::Json;

std::wstring PlayerPath(const wchar_t* within)
{
    PWSTR dir = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &dir)) && dir)
        path = std::wstring(dir) + L"\\NetEase\\CloudMusic\\" + within;
    CoTaskMemFree(dir);
    return path;
}

std::wstring QueuePath() { return PlayerPath(L"webdata\\file\\playingList"); }

uint64_t WriteTime(const std::wstring& path, uint64_t* size = nullptr)
{
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info))
        return 0;
    if (size)
        *size = (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    return (uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
}

std::wstring Utf8(const unsigned char* s, int bytes)
{
    const int n = s ? MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(s), bytes, nullptr, 0) : 0;
    std::wstring out(size_t(std::max(n, 0)), L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(s), bytes, out.data(), n);
    return out;
}

// The first column of each row `sql` gives (one text parameter bound), until `row` returns false.
template <class Row>
bool Query(sqlite3* db, const char* sql, const std::string& param, Row row)
{
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK)
        return false;
    if (!param.empty())
        sqlite3_bind_text(st, 1, param.c_str(), int(param.size()), SQLITE_TRANSIENT);
    int rc = SQLITE_ROW;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW)
        if (!row(Utf8(sqlite3_column_text(st, 0), sqlite3_column_bytes(st, 0))))
            break;
    sqlite3_finalize(st);
    return rc == SQLITE_ROW || rc == SQLITE_DONE;
}

// The liked playlist's id in the player's cached account playlists: the playlist object holding
// specialType 5 also holds its comment thread, "A_PL_0_<id>" (the cache stores the JSON escaped).
std::wstring LikedPlaylist(sqlite3* db)
{
    std::wstring found;
    Query(db, "SELECT jsonStr FROM requestCache WHERE id LIKE '%/user/playlist%' AND jsonStr LIKE '%specialType%'", {}, [&](const std::wstring& j) {
        for (size_t at = j.find(L"specialType"); at != std::wstring::npos && found.empty(); at = j.find(L"specialType", at + 1)) {
            size_t v = at + 11;
            while (v < j.size() && (j[v] == L'\\' || j[v] == L'"' || j[v] == L':' || j[v] == L' '))
                ++v;
            if (v >= j.size() || j[v] != L'5' || (v + 1 < j.size() && iswdigit(j[v + 1])))
                continue;
            const size_t from = at > 1500 ? at - 1500 : 0, thread = j.rfind(L"A_PL_0_", at);
            if (thread == std::wstring::npos || thread < from)
                continue;
            size_t end = thread + 7;
            while (end < j.size() && iswdigit(j[end]))
                ++end;
            found = j.substr(thread + 7, end - thread - 7);
        }
        return found.empty();
    });
    return found;
}

std::string Narrow(const std::wstring& s)
{
    std::string out;
    for (wchar_t c : s)
        out += char(c); // digits only
    return out;
}

// The file as text (UTF-8), up to 32 MB; empty if unreadable.
std::wstring ReadText(const std::wstring& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};
    LARGE_INTEGER size{};
    std::string bytes;
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < (32LL << 20)) {
        bytes.resize(size_t(size.QuadPart));
        DWORD read = 0;
        if (!ReadFile(file, bytes.data(), DWORD(bytes.size()), &read, nullptr))
            read = 0;
        bytes.resize(read);
    }
    CloseHandle(file);
    const int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), int(bytes.size()), nullptr, 0);
    std::wstring text(size_t(std::max(n, 0)), L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, bytes.data(), int(bytes.size()), text.data(), n);
    return text;
}

NeteaseQueuedTrack ParseTrackObject(const json::JsonObject& track)
{
    NeteaseQueuedTrack t;
    if (track.HasKey(L"name") && track.GetNamedValue(L"name").ValueType() == json::JsonValueType::String)
        t.title = track.GetNamedString(L"name");
    if (track.HasKey(L"duration") && track.GetNamedValue(L"duration").ValueType() == json::JsonValueType::Number) {
        const double ms = track.GetNamedNumber(L"duration");
        if (std::isfinite(ms) && ms > 0.0 && ms < 86400000.0)
            t.lengthUs = static_cast<int64_t>(std::llround(ms * 1000.0));
    }
    if (track.HasKey(L"artists") && track.GetNamedValue(L"artists").ValueType() == json::JsonValueType::Array) {
        for (const auto& a : track.GetNamedArray(L"artists")) {
            if (a.ValueType() != json::JsonValueType::Object)
                continue;
            const auto obj = a.GetObject();
            if (obj.HasKey(L"name") && obj.GetNamedValue(L"name").ValueType() == json::JsonValueType::String)
                t.artists += (t.artists.empty() ? L"" : L"/") + std::wstring(obj.GetNamedString(L"name"));
        }
    }
    return t;
}

bool ParseQueue(const std::wstring& text, std::vector<NeteaseQueuedTrack>& out)
{
    out.clear();
    json::JsonObject root{nullptr};
    if (text.empty() || !json::JsonObject::TryParse(text, root) || !root.HasKey(L"list") ||
        root.GetNamedValue(L"list").ValueType() != json::JsonValueType::Array)
        return false;
    for (const auto& item : root.GetNamedArray(L"list")) {
        if (item.ValueType() != json::JsonValueType::Object)
            continue;
        const json::JsonObject entry = item.GetObject();
        if (!entry.HasKey(L"track") || entry.GetNamedValue(L"track").ValueType() != json::JsonValueType::Object)
            continue;
        NeteaseQueuedTrack t = ParseTrackObject(entry.GetNamedObject(L"track"));
        if (entry.HasKey(L"id") && entry.GetNamedValue(L"id").ValueType() == json::JsonValueType::Number)
            t.id = std::to_wstring(int64_t(entry.GetNamedNumber(L"id")));
        if (!t.title.empty() && t.lengthUs > 0)
            out.push_back(std::move(t));
    }
    return true;
}

} // namespace

bool NeteaseQueueCache::Update(const std::wstring& text, uint64_t writeTime, uint64_t sizeBytes)
{
    if (text.empty())
        return false;
    std::vector<NeteaseQueuedTrack> parsed;
    if (!ParseQueue(text, parsed))
        return false;
    tracks_ = std::move(parsed);
    writeTime_ = writeTime;
    sizeBytes_ = sizeBytes;
    return true;
}

const NeteaseQueuedTrack* NeteaseQueueCache::Find(const std::wstring& title, const std::wstring& artist) const
{
    const std::wstring wantArtist = FoldCase(artist);
    const NeteaseQueuedTrack* byName = nullptr;
    for (const NeteaseQueuedTrack& t : tracks_) {
        if (t.title != title)
            continue;
        const std::wstring has = FoldCase(t.artists);
        if (!wantArtist.empty() && !has.empty() &&
            (has == wantArtist || wantArtist.find(has) != std::wstring::npos || has.find(wantArtist) != std::wstring::npos))
            return &t;
        if (!byName)
            byName = &t;
    }
    return byName;
}

int64_t NeteaseQueueCache::FindLengthUs(const std::wstring& title, const std::wstring& artist) const
{
    const NeteaseQueuedTrack* t = Find(title, artist);
    return t ? t->lengthUs : 0;
}

// The queue, read again when its file changes.
const NeteaseQueueCache& Queue()
{
    static NeteaseQueueCache cache;
    const std::wstring path = QueuePath();
    uint64_t size = 0;
    const uint64_t writeTime = WriteTime(path, &size);
    if (writeTime && cache.NeedsRead(writeTime, size)) {
        const std::wstring text = ReadText(path);
        if (!text.empty())
            cache.Update(text, writeTime, size);
    }
    return cache;
}

int64_t NeteaseTrackLengthUs(const std::wstring& title, const std::wstring& artist) { return Queue().FindLengthUs(title, artist); }

std::optional<bool> NeteaseLikes::Liked(const std::wstring& title, const std::wstring& artist)
{
    const std::wstring path = PlayerPath(L"Library\\webdb.dat");
    const uint64_t writeTime = WriteTime(path);
    if (!writeTime)
        return std::nullopt;
    // Read again when the player wrote, and a read that failed (the player writing) again a moment later.
    const ULONGLONG now = GetTickCount64();
    if (writeTime != writeTime_ || (!ok_ && now - readAt_ > 3000)) {
        writeTime_ = writeTime;
        readAt_ = now;
        ok_ = Read(path);
    }
    const std::wstring id = CurrentId(title, artist);
    std::optional<bool> liked;
    if (ok_ && !id.empty())
        liked = ids_.contains(id);
    const std::wstring said = title + L'\n' + std::to_wstring(liked ? int(*liked) : -1);
    if (said != logged_) {
        logged_ = said;
        Log(L"netease: heart of '%s': %d (read %d, %zu liked, track %s)", title.c_str(), liked ? int(*liked) : -1, ok_, ids_.size(), id.c_str());
    }
    return liked;
}

// The track's id: the one the player last began when it is this one (its history, by name), else the
// queue's by title and artist.
std::wstring NeteaseLikes::CurrentId(const std::wstring& title, const std::wstring& artist) const
{
    const std::wstring a = FoldCase(title), b = FoldCase(recentName_);
    if (!recentId_.empty() && !a.empty() && !b.empty() && (a == b || a.find(b) != std::wstring::npos || b.find(a) != std::wstring::npos))
        return recentId_;
    const NeteaseQueuedTrack* track = Queue().Find(title, artist);
    return track ? track->id : std::wstring();
}

bool NeteaseLikes::Read(const std::wstring& path)
{
    sqlite3* db = nullptr;
    std::string utf8(size_t(WideCharToMultiByte(CP_UTF8, 0, path.c_str(), int(path.size()), nullptr, 0, nullptr, nullptr)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), int(path.size()), utf8.data(), int(utf8.size()), nullptr, nullptr);
    if (sqlite3_open_v2(utf8.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    sqlite3_busy_timeout(db, 200); // the player may be writing
    if (playlist_.empty())
        playlist_ = LikedPlaylist(db);
    Query(db, "SELECT jsonStr FROM historyTracks ORDER BY playtime DESC LIMIT 1", {}, [&](const std::wstring& text) {
        json::JsonObject o{nullptr};
        if (json::JsonObject::TryParse(text, o) && o.HasKey(L"id") && o.HasKey(L"name") && o.GetNamedValue(L"name").ValueType() == json::JsonValueType::String) {
            const auto id = o.GetNamedValue(L"id");
            recentId_ = id.ValueType() == json::JsonValueType::String ? std::wstring(id.GetString()) : std::to_wstring(int64_t(id.GetNumber()));
            recentName_ = o.GetNamedString(L"name");
        }
        return false;
    });
    bool found = false;
    std::unordered_set<std::wstring> ids;
    if (!playlist_.empty())
        Query(db, "SELECT jsonStr FROM playlistTrackIds WHERE id = ?", Narrow(playlist_), [&](const std::wstring& text) {
            json::JsonObject o{nullptr};
            if (!json::JsonObject::TryParse(text, o) || !o.HasKey(L"trackIds") || o.GetNamedValue(L"trackIds").ValueType() != json::JsonValueType::Array)
                return false;
            for (const auto& v : o.GetNamedArray(L"trackIds")) {
                if (v.ValueType() == json::JsonValueType::Object && v.GetObject().HasKey(L"id")) {
                    const auto id = v.GetObject().GetNamedValue(L"id");
                    ids.insert(id.ValueType() == json::JsonValueType::String ? std::wstring(id.GetString()) : std::to_wstring(int64_t(id.GetNumber())));
                } else if (v.ValueType() == json::JsonValueType::Number) {
                    ids.insert(std::to_wstring(int64_t(v.GetNumber())));
                }
            }
            found = true;
            return false;
        });
    sqlite3_close(db);
    if (!found) {
        playlist_.clear(); // looked for again next time
        return false;
    }
    ids_ = std::move(ids);
    return true;
}

void NeteaseTimeline::Fill(const std::wstring& title, const std::wstring& artist, bool playing, double nowMs, MediaTimeline& t)
{
    const std::wstring track = title + L'\n' + artist;
    const bool changed = track != track_ || playing != playing_;
    // The queue may be written a moment after the track starts: asked again while it has no length.
    if (track != track_ || (lengthUs_ == 0 && nowMs - lookedUpMs_ > 2000)) {
        lengthUs_ = NeteaseTrackLengthUs(title, artist);
        lookedUpMs_ = nowMs;
    }
    track_ = track;
    playing_ = playing;
    MediaTimeline est = t;
    estimate_.Update(track, playing, lengthUs_, nowMs, est);
    if (changed || nowMs - readMs_ >= 500) {
        readMs_ = nowMs;
        double pos = 0, len = 0;
        const bool wasFromMemory = fromMemory_;
        fromMemory_ = memory_.Read(double(lengthUs_) / 1e6, playing, nowMs, pos, len);
        if (fromMemory_) {
            MediaTimeline m;
            m.endUs = int64_t(std::llround(len * 1e6));
            m.anchorPosUs = int64_t(std::llround(pos * 1e6));
            m.anchorMs = nowMs;
            m.playing = playing;
            if (!wasFromMemory || changed || last_.playing != m.playing || last_.endUs != m.endUs ||
                std::llabs(last_.PositionUs(nowMs) - m.anchorPosUs) > 250'000)
                last_ = m;
        }
    }
    if (fromMemory_)
        t = last_;
    else
        t = est;
}

} // namespace atrium
