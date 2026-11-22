#include "shell/netease_tracks.h"

#include <windows.h>
#include <shlobj.h>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <string_view>
#include <vector>

namespace atrium {

namespace {

namespace json = winrt::Windows::Data::Json;

struct QueuedTrack {
    std::wstring title, artists; // artists: their names joined by "/", as the player tells Windows
    int64_t lengthUs = 0;
};

std::wstring Lower(std::wstring s)
{
    if (!s.empty())
        CharLowerBuffW(s.data(), DWORD(s.size()));
    return s;
}

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

std::vector<QueuedTrack> ParseQueue(const std::wstring& text)
{
    json::JsonObject root{nullptr};
    std::vector<QueuedTrack> tracks;
    if (text.empty() || !json::JsonObject::TryParse(text, root) || !root.HasKey(L"list") || root.GetNamedValue(L"list").ValueType() != json::JsonValueType::Array)
        return tracks;
    for (const auto& item : root.GetNamedArray(L"list")) {
        if (item.ValueType() != json::JsonValueType::Object)
            continue;
        const json::JsonObject entry = item.GetObject();
        if (!entry.HasKey(L"track") || entry.GetNamedValue(L"track").ValueType() != json::JsonValueType::Object)
            continue;
        const json::JsonObject track = entry.GetNamedObject(L"track");
        QueuedTrack t;
        if (track.HasKey(L"name") && track.GetNamedValue(L"name").ValueType() == json::JsonValueType::String)
            t.title = track.GetNamedString(L"name");
        if (track.HasKey(L"duration") && track.GetNamedValue(L"duration").ValueType() == json::JsonValueType::Number)
            t.lengthUs = int64_t(track.GetNamedNumber(L"duration") * 1000.0);
        if (track.HasKey(L"artists") && track.GetNamedValue(L"artists").ValueType() == json::JsonValueType::Array)
            for (const auto& a : track.GetNamedArray(L"artists"))
                if (a.ValueType() == json::JsonValueType::Object && a.GetObject().HasKey(L"name") &&
                    a.GetObject().GetNamedValue(L"name").ValueType() == json::JsonValueType::String)
                    t.artists += (t.artists.empty() ? L"" : L"/") + std::wstring(a.GetObject().GetNamedString(L"name"));
        if (!t.title.empty() && t.lengthUs > 0)
            tracks.push_back(std::move(t));
    }
    return tracks;
}

} // namespace

bool IsNeteasePlayer(const std::wstring& appId) { return Lower(appId).find(L"cloudmusic") != std::wstring::npos; }

int64_t NeteaseTrackLengthUs(const std::wstring& title, const std::wstring& artist)
{
    // The queue as last read, with the file's write time and size then.
    static FILETIME written{};
    static ULONGLONG bytes = 0;
    static std::vector<QueuedTrack> queue;
    const std::wstring path = QueuePath();
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info))
        return 0;
    const ULONGLONG size = (ULONGLONG(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (CompareFileTime(&info.ftLastWriteTime, &written) != 0 || size != bytes) {
        queue = ParseQueue(ReadText(path));
        written = info.ftLastWriteTime;
        bytes = size;
    }
    const std::wstring wantArtist = Lower(artist);
    int64_t byName = 0;
    for (const QueuedTrack& t : queue) {
        if (t.title != title)
            continue;
        const std::wstring has = Lower(t.artists);
        if (!wantArtist.empty() && !has.empty() && (has == wantArtist || wantArtist.find(has) != std::wstring::npos || has.find(wantArtist) != std::wstring::npos))
            return t.lengthUs;
        if (!byName)
            byName = t.lengthUs;
    }
    return byName;
}

} // namespace atrium
