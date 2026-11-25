#include "shell/screenshot_watch.h"

#include "log.h"

#include <windows.h>

#include <atomic>
#include <string>

namespace atrium {
namespace {

std::atomic<bool> g_active{false};

bool IsWindowsScreenshotTool(const std::wstring& path)
{
    if (path.empty())
        return false;
    const size_t slash = path.find_last_of(L"\\/");
    std::wstring file = slash == std::wstring::npos ? path : path.substr(slash + 1);
    CharLowerBuffW(file.data(), DWORD(file.size()));
    // The same tools Stage Manager refuses to park. Other capture apps stay out of this
    // check: a title or a third-party name is too easy to get wrong.
    if (file == L"snippingtool.exe" || file == L"screenclippinghost.exe" || file == L"screensketch.exe")
        return true;
    return file.find(L"screensketch") != std::wstring::npos;
}

bool WindowIsScreenshotTool(HWND h)
{
    if (!IsWindowVisible(h))
        return false;
    RECT r{};
    if (!GetWindowRect(h, &r))
        return false;
    // Tray leftovers and zero-size hosts are not the overlay the user is capturing with.
    if (r.right - r.left < 80 || r.bottom - r.top < 40)
        return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (!pid)
        return false;
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc)
        return false;
    wchar_t path[MAX_PATH] = {};
    DWORD n = MAX_PATH;
    const bool ok = QueryFullProcessImageNameW(proc, 0, path, &n) != FALSE;
    CloseHandle(proc);
    return ok && IsWindowsScreenshotTool(path);
}

bool Scan()
{
    bool found = false;
    EnumWindows(
        [](HWND h, LPARAM l) -> BOOL {
            if (WindowIsScreenshotTool(h)) {
                *reinterpret_cast<bool*>(l) = true;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found;
}

} // namespace

bool ScreenshotInProgress() { return g_active.load(std::memory_order_relaxed); }

bool PollScreenshotWatch()
{
    const bool now = Scan();
    const bool was = g_active.exchange(now, std::memory_order_relaxed);
    if (now != was)
        Log(L"capture: screenshot tool %s", now ? L"active — widget glass uses wallpaper" : L"gone");
    return now != was;
}

} // namespace atrium
