#include "shell/netease_tracks.h"

#include "win32_path.h"

#include <windows.h>
#include <shlobj.h>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>
#include <vector>

namespace atrium {

namespace {

namespace json = winrt::Windows::Data::Json;

std::wstring QueuePath()
{
    PWSTR dir = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &dir)) && dir)
        path = std::wstring(dir) + L"\\NetEase\\CloudMusic\\webdata\\file\\playingList";
    CoTaskMemFree(dir);
    return path;
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

int64_t NeteaseQueueCache::FindLengthUs(const std::wstring& title, const std::wstring& artist) const
{
    const std::wstring wantArtist = FoldCase(artist);
    int64_t byName = 0;
    for (const NeteaseQueuedTrack& t : tracks_) {
        if (t.title != title)
            continue;
        const std::wstring has = FoldCase(t.artists);
        if (!wantArtist.empty() && !has.empty() &&
            (has == wantArtist || wantArtist.find(has) != std::wstring::npos || has.find(wantArtist) != std::wstring::npos))
            return t.lengthUs;
        if (!byName)
            byName = t.lengthUs;
    }
    return byName;
}

int64_t NeteaseTrackLengthUs(const std::wstring& title, const std::wstring& artist)
{
    static NeteaseQueueCache cache;
    const std::wstring path = QueuePath();
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info))
        return 0;
    const uint64_t writeTime = (uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
    const uint64_t size = (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (cache.NeedsRead(writeTime, size)) {
        const std::wstring text = ReadText(path);
        if (!text.empty())
            cache.Update(text, writeTime, size);
    }
    return cache.FindLengthUs(title, artist);
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
