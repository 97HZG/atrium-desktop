// Bar stacking guard and auto-hide cover detection: keeps the bar topmost
// unless a full-screen or maximized app covers its monitor, or the user is
// interacting with a revealed system taskbar.
#include "bar.h"

#include "log.h"
#include "shell/games.h"
#include "shell/shell_taskbar.h"

#include <dwmapi.h>

#include <cwchar>
#include <iterator>
#include <map>
#include <utility>
#include <vector>

namespace atrium {

namespace {

constexpr UINT kGuardFastMs = 150; // while below a system taskbar the user is using

enum class TrayAction { None, Done, Raise };

// Windows the bar never climbs over: menus, tooltips, the shell's own
// surfaces, input method windows.
bool IgnoredCover(HWND w)
{
    static const wchar_t* const kClasses[] = {L"#32768",
                                              L"tooltips_class32",
                                              L"Shell_SecondaryTrayWnd",
                                              L"NotifyIconOverflowWindow",
                                              L"TopLevelWindowForOverflowXamlIsland",
                                              L"Xaml_WindowedPopupClass",
                                              L"XamlExplorerHostIslandWindow",
                                              L"Windows.UI.Core.CoreWindow",
                                              L"SysShadow",
                                              L"IME",
                                              L"MSCTFIME UI",
                                              L"ForegroundStaging",
                                              L"MultitaskingViewFrame",
                                              L"Shell_InputSwitchTopLevelWindow",
                                              L"Progman",
                                              L"WorkerW"};
    wchar_t cls[96] = {};
    GetClassNameW(w, cls, 96);
    for (const wchar_t* c : kClasses)
        if (wcscmp(cls, c) == 0)
            return true;
    const LONG ex = GetWindowLongW(w, GWL_EXSTYLE);
    if ((ex & WS_EX_TRANSPARENT) && (ex & WS_EX_LAYERED))
        return true; // click-through overlays
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    return cloaked != FALSE;
}

bool AllowRaise(std::map<HWND, std::pair<int, ULONGLONG>>& raises, HWND target)
{
    const ULONGLONG now = GetTickCount64();
    for (auto it = raises.begin(); it != raises.end();)
        it = now - it->second.second > 10000 || !IsWindow(it->first) ? raises.erase(it) : std::next(it);
    auto& count = raises.try_emplace(target, 0, now).first->second;
    if (++count.first > 4) {
        if (count.first == 5) {
            wchar_t cls[64] = {};
            GetClassNameW(target, cls, 64);
            Log(L"bar: %s keeps covering the bar; leaving it in front", cls);
        }
        return false;
    }
    return true;
}

bool TrayOverlaps(HWND tray, const RECT& bar, RECT& trayRect)
{
    RECT overlap{};
    return tray && IsWindowVisible(tray) && GetWindowRect(tray, &trayRect) && IntersectRect(&overlap, &trayRect, &bar) && overlap.right - overlap.left > 2 &&
           overlap.bottom - overlap.top > 2;
}

TrayAction GuardTrayTaskbar(HWND visual, HWND input, HWND tray, bool trayOver, bool trayAbove, const RECT& trayRect, const RECT& ours, bool& belowTray,
                            bool& trayHeldUp, std::map<HWND, std::pair<int, ULONGLONG>>& raises, UINT_PTR guardTimer, UINT guardMs)
{
    if (trayOver) {
        POINT cur{};
        GetCursorPos(&cur);
        HWND fg = GetForegroundWindow();
        wchar_t fgCls[64] = {};
        GetClassNameW(fg, fgCls, 64);
        // The pointer counts on the system taskbar outside the bar: over the bar it is meant for the bar.
        const bool usingTray = (PtInRect(&trayRect, cur) && !PtInRect(&ours, cur)) || fg == tray ||
                               wcscmp(fgCls, L"TopLevelWindowForOverflowXamlIsland") == 0 || wcscmp(fgCls, L"Shell_TrayWnd") == 0;
        if (usingTray) {
            // The revealed taskbar stays in front; it slides away again by itself.
            if (!trayAbove) {
                SetWindowPos(visual, tray, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
                SetWindowPos(input, visual, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
                if (!belowTray)
                    Log(L"bar: below the revealed system taskbar (pointer %ld,%ld, foreground %s)", cur.x, cur.y, fgCls);
            }
            if (!belowTray) // the system taskbar gets the pointer now: watch for it coming back to the bar
                SetTimer(input, guardTimer, kGuardFastMs, nullptr);
            belowTray = true;
            return TrayAction::Done;
        }
        if (trayAbove) {
            const bool wasBelow = belowTray;
            if (wasBelow)
                SetTimer(input, guardTimer, guardMs, nullptr);
            belowTray = false;
            const bool wasHeld = trayHeldUp;
            trayHeldUp = true;
            if (!AllowRaise(raises, tray))
                return TrayAction::Done;
            if (wasBelow || !wasHeld)
                Log(L"bar: over the system taskbar, which an app holds up");
            return TrayAction::Raise;
        }
    }
    if (belowTray) {
        Log(L"bar: the system taskbar went down again");
        SetTimer(input, guardTimer, guardMs, nullptr);
    }
    belowTray = false;
    trayHeldUp = trayOver;
    return TrayAction::None;
}

} // namespace

// Focus may be on another screen; use this monitor's app Z-order instead.
bool Bar::MonitorCoveredByApp() const
{
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(hmonitor_, &mi);
    std::vector<RECT> tiles; // snapped windows in front, filling the screen together
    for (HWND window = GetTopWindow(nullptr); window; window = GetWindow(window, GW_HWNDNEXT)) {
        if (!IsWindowVisible(window) || IsIconic(window))
            continue;
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        const HMONITOR windowMonitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
        if (pid == GetCurrentProcessId() || windowMonitor != hmonitor_)
            continue;
        const LONG_PTR ex = GetWindowLongPtrW(window, GWL_EXSTYLE);
        if (!(ex & WS_EX_APPWINDOW) && ((ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) || GetWindow(window, GW_OWNER)))
            continue; // transient UI must not reveal a bar over a full-screen app
        if ((ex & WS_EX_TRANSPARENT) && (ex & WS_EX_LAYERED))
            continue;
        BOOL cloaked = FALSE;
        DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (cloaked)
            continue; // windows on another virtual desktop do not cover this one
        wchar_t cls[64] = {};
        GetClassNameW(window, cls, 64);
        if (wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0 || wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0)
            continue;
        RECT frame{};
        if (!GetWindowRect(window, &frame))
            continue;
        if (auto covered = WindowCoverOnMonitor(windowMonitor, frame, IsZoomed(window) != FALSE, hmonitor_, monitor_)) {
            if (*covered)
                return true;
            // Snapped side by side, the windows fill the screen as a maximized one does.
            RECT bounds = frame; // without the invisible resize borders
            DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds));
            const int slack = MulDiv(8, int(GetDpiForWindow(window)), 96);
            if (IsTile(bounds, mi.rcWork, slack)) {
                tiles.push_back(bounds);
                if (TilesFill(tiles, mi.rcWork, slack))
                    return true;
            }
            // A smaller window in front (a mail opened from a chat, a dialog, a float) does not
            // decide: what fills the screen behind it still does.
        }
    }
    return false;
}

void Bar::UpdateCover()
{
    // Apply the hide mode independently on each display.
    const std::wstring& mode = settings_.autoHide;
    // A game in front always has the screen to itself, whatever the hide mode.
    const bool expected = mode != L"never" && GetTickCount64() < expectCoverUntil_;
    bool covered = mode == L"always" || (mode != L"never" && MonitorCoveredByApp()) || GameInFront(hmonitor_) || expected ||
                   (mode == L"never" && settings_.barFullscreenHide && FullScreenInFront(hmonitor_));
    if (covered == covered_)
        return;
    covered_ = covered;
    // Maximizing with the pointer on the bar keeps it up until the pointer leaves.
    POINT p{};
    GetCursorPos(&p);
    RECT zone = EdgeZone();
    revealed_ = covered && !slidOut_ && PtInRect(&zone, p) && !FullScreenInFront(hmonitor_);
    leftAt_ = 0;
    if (mode == L"fullscreen")
        Log(L"bar: monitor %p %s", hmonitor_, covered ? L"covered by its top app: auto-hide" : L"not covered by its top app");
    else
        Log(L"bar: auto-hide %s (%s)", covered ? L"always" : L"never", mode.c_str());
    if (covered)
        SetTimer(input_, kPointerTimer, kPointerMs, nullptr);
    else
        KillTimer(input_, kPointerTimer);
    UpdateAutoHide();
}

void Bar::Raise()
{
    SetWindowPos(visual_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    SetWindowPos(input_, visual_, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

// Keeps the bar on top: hidden behind a full-screen app, below a revealed
// system taskbar that overlaps it while the user is using that taskbar (the
// pointer on it outside the bar, or the keyboard in it), and otherwise raised again when
// another topmost window covers it. A taskbar held up only by an app's
// flashing button does not push the bar down (the bar shows the app's
// attention itself). A window that keeps pushing itself over the bar
// wins after a few rounds, so two always-on-top windows never fight.
void Bar::GuardStacking(const wchar_t* why)
{
    UpdateCover();
    if (!visible_ || tucked_ || dragging_)
        return;
    const RECT bar = BarRect();
    RECT r{}, overlap{}, trayRect{};
    HWND tray = ShellTaskbar();
    const bool trayOver = TrayOverlaps(tray, bar, trayRect);
    bool trayAbove = false;
    HWND cover = nullptr;
    for (HWND w = GetTopWindow(nullptr); w && w != visual_; w = GetWindow(w, GW_HWNDNEXT)) {
        if (w == tray) {
            trayAbove = true;
            continue;
        }
        if (w == input_ || !IsWindowVisible(w))
            continue;
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid == GetCurrentProcessId())
            continue;
        if (!(GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_TOPMOST)) {
            cover = w; // the bar has lost its topmost place
            break;
        }
        if (GetWindowRect(w, &r) && IntersectRect(&overlap, &r, &bar) && !IgnoredCover(w)) {
            cover = w;
            break;
        }
    }
    const TrayAction action =
        GuardTrayTaskbar(visual_, input_, tray, trayOver, trayAbove, trayRect, EdgeZone(), belowTray_, trayHeldUp_, raises_, kGuardTimer, kGuardMs);
    if (action == TrayAction::Raise) {
        // Up without the user (an app's button flashing): the bar stays in front.
        Raise();
        return;
    }
    if (action == TrayAction::Done || !cover || !AllowRaise(raises_, cover))
        return;
    Raise();
    if (cover != lastCover_) {
        wchar_t cls[64] = {};
        GetClassNameW(cover, cls, 64);
        Log(L"bar: raised over %s (%s)", cls, why ? why : L"check");
        lastCover_ = cover;
    }
}

} // namespace atrium
