// The shell's taskbar window. With the tray host on (shell/tray_host.h) Atrium has
// a window of the same class too, above Explorer's so that apps' tray calls reach
// it: every lookup of Explorer's taskbar goes through here, never FindWindow.
#pragma once

#include <windows.h>

namespace atrium {

// Explorer's Shell_TrayWnd, never Atrium's own. No allocation (used by the crash handler).
inline HWND ShellTaskbar()
{
    const DWORD self = GetCurrentProcessId();
    for (HWND h = nullptr; (h = FindWindowExW(nullptr, h, L"Shell_TrayWnd", nullptr)) != nullptr;) {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid != self)
            return h;
    }
    return nullptr;
}

} // namespace atrium
