#include "shell/games.h"

#include <shellapi.h>

#include <algorithm>
#include <cwctype>
#include <mutex>
#include <unordered_map>

namespace atrium {

namespace {

// Library folders of the game stores (folded): what runs from under them is a game.
constexpr const wchar_t* kStoreFolders[] = {L"\\steamapps\\common\\", L"\\epic games\\",       L"\\xboxgames\\",    L"\\riot games\\",
                                            L"\\gog games\\",         L"\\gog galaxy\\games\\", L"\\wegameapps\\",   L"\\hoyoplay\\games\\",
                                            L"\\ubisoft game launcher\\games\\", L"\\ea games\\"};
constexpr ULONGLONG kStoreReadMs = 30000; // Game Bar's list, read again at most this often

std::mutex g_lock;
std::vector<std::wstring> g_user, g_known; // folded paths: marked by the user; Game Bar's
ULONGLONG g_knownAt = 0;
std::unordered_map<DWORD, std::pair<std::wstring, bool>> g_byPid; // program path, a game

std::wstring Fold(std::wstring s)
{
    std::ranges::transform(s, s.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
    return s;
}

// Game Bar's games: HKCU\System\GameConfigStore\Children\*\MatchedExeFullPath.
std::vector<std::wstring> ReadGameBarList()
{
    std::vector<std::wstring> paths;
    HKEY children = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"System\\GameConfigStore\\Children", 0, KEY_READ, &children) != ERROR_SUCCESS)
        return paths;
    wchar_t name[256];
    for (DWORD i = 0;; ++i) {
        DWORD len = 256;
        if (RegEnumKeyExW(children, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        wchar_t path[MAX_PATH * 2] = {};
        DWORD size = sizeof(path);
        if (RegGetValueW(children, name, L"MatchedExeFullPath", RRF_RT_REG_SZ, nullptr, path, &size) == ERROR_SUCCESS && path[0])
            paths.push_back(Fold(path));
    }
    RegCloseKey(children);
    return paths;
}

bool IsGamePath(const std::wstring& path)
{
    if (path.empty())
        return false;
    if (std::ranges::any_of(kStoreFolders, [&](const wchar_t* f) { return path.find(f) != std::wstring::npos; }))
        return true;
    auto matches = [&](const std::wstring& entry) { return entry.ends_with(L'\\') ? path.starts_with(entry) : path == entry; };
    return std::ranges::any_of(g_known, matches) || std::ranges::any_of(g_user, matches);
}

std::wstring ProgramOf(DWORD pid)
{
    std::wstring path;
    if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buffer[MAX_PATH * 2] = {};
        DWORD size = MAX_PATH * 2;
        if (QueryFullProcessImageNameW(process, 0, buffer, &size))
            path = Fold(buffer);
        CloseHandle(process);
    }
    return path;
}

bool Exclusive()
{
    QUERY_USER_NOTIFICATION_STATE state{};
    return SUCCEEDED(SHQueryUserNotificationState(&state)) && state == QUNS_RUNNING_D3D_FULL_SCREEN;
}

} // namespace

void SetUserGames(const std::vector<std::wstring>& entries)
{
    std::vector<std::wstring> paths;
    for (const std::wstring& e : entries)
        if (e.find(L'\\') != std::wstring::npos)
            paths.push_back(Fold(e));
    std::lock_guard g(g_lock);
    if (paths != g_user) {
        g_user = std::move(paths);
        g_byPid.clear();
    }
}

bool IsGameWindow(HWND window)
{
    DWORD pid = 0;
    if (!window || !GetWindowThreadProcessId(window, &pid) || pid == GetCurrentProcessId())
        return false;
    std::lock_guard g(g_lock);
    const ULONGLONG now = GetTickCount64();
    if (!g_knownAt || now - g_knownAt > kStoreReadMs) {
        g_known = ReadGameBarList();
        g_knownAt = now;
        g_byPid.clear();
    }
    const std::wstring path = ProgramOf(pid);
    auto it = g_byPid.find(pid);
    if (it == g_byPid.end() || it->second.first != path) { // a new process, or the id taken by another
        if (g_byPid.size() > 512)
            g_byPid.clear();
        it = g_byPid.insert_or_assign(pid, std::pair{path, IsGamePath(path)}).first;
    }
    return it->second.second;
}

bool GameInFront(HMONITOR monitor)
{
    HWND fg = GetForegroundWindow();
    HWND root = fg ? GetAncestor(fg, GA_ROOT) : nullptr;
    if (!root || IsIconic(root) || MonitorFromWindow(root, MONITOR_DEFAULTTONULL) != monitor)
        return false;
    DWORD pid = 0;
    if (!GetWindowThreadProcessId(root, &pid) || pid == GetCurrentProcessId())
        return false;
    if (Exclusive())
        return true;
    if (!IsGameWindow(root))
        return false;
    // Full screen, borderless or maximized: it covers the work area (all of the screen once Windows' taskbar is hidden).
    MONITORINFO mi{sizeof(mi)};
    RECT r{};
    if (!GetMonitorInfoW(monitor, &mi) || !GetWindowRect(root, &r))
        return false;
    return IsZoomed(root) || (r.left <= mi.rcWork.left && r.top <= mi.rcWork.top && r.right >= mi.rcWork.right && r.bottom >= mi.rcWork.bottom);
}

} // namespace atrium
