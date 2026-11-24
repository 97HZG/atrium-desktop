#include "bar.h"
#include "bar_drop.h"
#include "bar_entries.h"

#include "localization.h"
#include "render/continuous_corners.h"

#include "clock.h"
#include "log.h"
#include "shell/shell_taskbar.h"
#include "render/capture.h"
#include "render/glass_renderer.h"
#include "render/shutdown_wait.h"
#include "render/ui_font.h"
#include "ui/surface.h"

#include <shellapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <exception>
#include <vector>

namespace atrium {

namespace {

constexpr UINT WM_BAR_CAPTURE = WM_APP + 0x70; // wParam 1: Atrium may show in screenshots (render thread -> UI thread)

constexpr wchar_t kVisualClass[] = L"AtriumBarVisual";
constexpr wchar_t kInputClass[] = L"AtriumBarInput";

std::map<HWINEVENTHOOK, Bar*> g_guards; // each display has its own stacking guard
struct CaptureWindows {
    HWND visual, input;
    bool visible;
};
std::map<Bar*, CaptureWindows> g_capturePolicies; // UI-thread only

void ApplyCapturePolicies()
{
    // One capture fallback must exclude every Atrium window to prevent feedback.
    const bool visible = std::ranges::all_of(g_capturePolicies, [](const auto& entry) { return entry.second.visible; });
    for (const auto& [bar, windows] : g_capturePolicies) {
        SetCaptureExclusion(windows.visual, visible);
        SetCaptureExclusion(windows.input, visible);
    }
    Surface::SetCaptureVisible(visible);
}

std::vector<int> BuildDragOrder(int n, bool& dragging, std::wstring& dragKey, int& dragInsert, const std::vector<TaskItem>& items, int& dragIndex)
{
    std::vector<int> order(size_t(std::max(n, 0)));
    for (int i = 0; i < n; ++i)
        order[size_t(i)] = i;
    dragIndex = -1;
    if (!dragging)
        return order;
    for (int i = 0; i < n; ++i)
        if (items[size_t(i)].key == dragKey)
            dragIndex = i;
    if (dragIndex < 0) {
        dragging = false; // the app went away mid-drag
        dragKey.clear();
        return order;
    }
    dragInsert = std::clamp(dragInsert, 0, n - 1);
    order.erase(order.begin() + dragIndex);
    order.insert(order.begin() + dragInsert, dragIndex);
    return order;
}

// Widest rendering of a clock string: every digit replaced by the widest digit.
std::wstring WidestDigits(std::wstring s)
{
    for (auto& c : s)
        if (iswdigit(c))
            c = L'8';
    return s;
}

void RegisterClasses(HINSTANCE instance)
{
    static bool done = false;
    if (done)
        return;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpfnWndProc = DefWindowProcW; // replaced per window once `this` is attached
    wc.lpszClassName = kVisualClass;
    RegisterClassExW(&wc);
    wc.lpszClassName = kInputClass;
    RegisterClassExW(&wc);
    done = true;
}

} // namespace

void RefreshCaptureExclusion() { ApplyCapturePolicies(); }

UINT Bar::AppBarNotice()
{
    static const UINT kNotice = RegisterWindowMessageW(L"Atrium.AppBar");
    return kNotice;
}

int Bar::Px(double dip, float scale) { return int(std::lround(dip * scale)); }

Bar::Bar(HINSTANCE instance, bool debugCapturable, std::wstring display)
    : debugCapturable_(debugCapturable),
      displayOverride_(std::move(display)),
      wake_(CreateEventW(nullptr, FALSE, FALSE, nullptr)),
      quit_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
      frame_(CreateEventW(nullptr, FALSE, FALSE, nullptr)),
      captureAck_(CreateEventW(nullptr, FALSE, FALSE, nullptr)),
      captureVisible_(debugCapturable)
{
    RegisterClasses(instance);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory3), reinterpret_cast<IUnknown**>(dwrite_.put()));

    // Visual: never hit-tested (layered + transparent), no redirection bitmap.
    visual_ = CreateWindowExW( // NOLINT(cppcoreguidelines-prefer-member-initializer): after RegisterClasses
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP | WS_EX_TRANSPARENT | WS_EX_LAYERED, kVisualClass, L"Atrium Bar",
        WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
    SetLayeredWindowAttributes(visual_, 0, 255, LWA_ALPHA);
    // Input: draws nothing, so it is invisible; its window region is the shape.
    input_ = CreateWindowExW( // NOLINT(cppcoreguidelines-prefer-member-initializer): after RegisterClasses
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP, kInputClass, L"Atrium Bar Input", WS_POPUP, 0, 0, 1, 1, nullptr,
        nullptr, instance, nullptr);
    SetWindowLongPtrW(input_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    SetWindowLongPtrW(input_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Bar::InputProc));
    SetWindowLongPtrW(visual_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    SetWindowLongPtrW(visual_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Bar::VisualProc));
    // Out of every capture until the render thread has a backdrop that
    // leaves them out by itself (Desktop Duplication).
    if (debugCapturable_)
        Log(L"bar: --debug-capturable, Atrium shows in screenshots (with Windows.Graphics.Capture, the backdrop freezes after the first frame)");
    SetCaptureExclusion(visual_, captureVisible_);
    SetCaptureExclusion(input_, captureVisible_);
    g_capturePolicies[this] = {visual_, input_, debugCapturable_};
    ApplyCapturePolicies();

    // A compact Dock-style label, using the same material and adaptive ink as
    // every other surface. It never takes focus or receives pointer input.
    tooltip_ = std::make_unique<Surface>(instance, L"Atrium Hover Label", false);
    tooltip_->message = [](UINT m, WPARAM, LPARAM, LRESULT& result) { return m == WM_NCHITTEST ? (result = HTTRANSPARENT, true) : false; };
    tooltip_->paint = [this](Canvas& c) {
        auto bounds = D2D1::RectF(0, 0, tooltipSize_.width, tooltipSize_.height);
        if (c.panel)
            c.Callout(bounds, std::min(16.f, tooltipSize_.height / 2), tooltipTailX_, kTooltipTailHalfW, edge_ == BarEdge::Bottom ? kTooltipTailH : 0,
                      kTooltipTailInset, c.theme.panel);
        c.Text(tooltipText_, D2D1::RectF(kTooltipPadX, kTooltipPadY, bounds.right - kTooltipPadX, bounds.bottom - kTooltipPadY), kTooltipFont, c.theme.text,
               DWRITE_FONT_WEIGHT_MEDIUM, 1);
    };

    // Drag and drop needs OLE on this (STA) thread; balanced in the destructor.
    oleReady_ = SUCCEEDED(OleInitialize(nullptr));
    if (oleReady_) {
        dropTarget_ = winrt::make<BarDropTarget>(this);
        if (FAILED(RegisterDragDrop(input_, dropTarget_.get()))) {
            Log(L"bar: RegisterDragDrop failed; dropping files on apps is unavailable");
            dropTarget_ = nullptr;
        }
    }

    // Stacking guard: raised again when covered, hidden behind full-screen apps.
    const DWORD evs[] = {EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_MOVESIZEEND, EVENT_OBJECT_LOCATIONCHANGE};
    for (int i = 0; i < 3; ++i) {
        hooks_[i] = SetWinEventHook(evs[i], evs[i], nullptr, &Bar::WinEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        if (hooks_[i])
            g_guards[hooks_[i]] = this;
    }
    SetTimer(input_, kGuardTimer, kGuardMs, nullptr);

    render_ = std::thread([this] { RenderThread(); });
}

Bar::~Bar()
{
    DetachInput();
    SetEvent(quit_);
    if (render_.joinable())
        render_.join();
    if (dropTarget_)
        RevokeDragDrop(input_);
    dropTarget_ = nullptr;
    tooltip_.reset();
    g_capturePolicies.erase(this);
    ApplyCapturePolicies();
    DestroyWindow(input_);
    DestroyWindow(visual_);
    for (HANDLE h : {wake_, quit_, frame_, captureAck_})
        CloseHandle(h);
    if (oleReady_)
        OleUninitialize();
}

void Bar::DetachInput()
{
    for (HWINEVENTHOOK& h : hooks_) {
        if (h) {
            UnhookWinEvent(h);
            g_guards.erase(h);
            h = nullptr;
        }
    }
    if (input_) {
        for (UINT_PTR id : {kTooltipTimer, kFileHoverTimer, kGuardTimer, kSlideTimer, kPointerTimer, kDisplayTimer, kCoverTimer, kNearTimer})
            KillTimer(input_, id);
        if (GetCapture() == input_)
            ReleaseCapture();
    }
    if (tooltip_)
        tooltip_->Hide(false);
    if (appBar_)
        UpdateAppBar(nullptr, RECT{}, 0);
}

bool Bar::StopRendering(DWORD timeoutMs)
{
    SetEvent(quit_);
    if (!render_.joinable())
        return true;
    // The render thread may wait for this thread (ShowOwnWindowsToCapture, DirectComposition):
    // keep its messages flowing while waiting, never a bare join.
    if (!WaitRenderShutdown(render_.native_handle(), visual_, WM_BAR_CAPTURE, timeoutMs)) {
        Log(L"core: the render thread did not stop in %lu ms", timeoutMs);
        return false;
    }
    render_.join();
    return true;
}

LRESULT CALLBACK Bar::VisualProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    auto* self = reinterpret_cast<Bar*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_DPICHANGED && self) {
        PostMessageW(self->input_, WM_BAR_LAYOUT, 0, 0);
        return 0;
    }
    if (m == WM_BAR_CAPTURE && self) {
        // From the render thread: whether Atrium's windows may show in screenshots.
        bool visible = w != 0;
        g_capturePolicies[self].visible = visible;
        ApplyCapturePolicies();
        SetEvent(self->captureAck_);
        return 0;
    }
    if (m == WM_CLOSE && self) {
        if (self->onCloseRequest)
            self->onCloseRequest(); // the app decides; never destroy just this window
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// The bar's strip of the screen while it is always shown (`reserve` px on `edge_` of
// `monitor`); 0 gives it back. Windows keeps the work area: maximized windows end before the bar.
void Bar::UpdateAppBar(HMONITOR monitor, RECT bounds, int reserve)
{
    APPBARDATA abd{sizeof(abd)};
    abd.hWnd = input_;
    if (reserve <= 0 || !monitor) {
        if (appBar_) {
            AppBarMessage(ABM_REMOVE, abd);
            appBar_ = false;
            appBarReserved_ = 0;
            appBarMonitor_ = nullptr;
            appBarRect_ = RECT{};
            appBarWant_ = RECT{};
            Log(L"bar: screen strip given back");
        }
        return;
    }
    if (!appBar_) {
        abd.uCallbackMessage = AppBarNotice();
        if (!AppBarMessage(ABM_NEW, abd))
            return;
        appBar_ = true;
        appBarRect_ = RECT{};
        appBarWant_ = RECT{};
    }
    const EdgeFrame frame{edge_, bounds};
    const RECT want = frame.ToArea(RECT{0, LONG(frame.Across()) - reserve, LONG(frame.Along()), LONG(frame.Across())});
    if (appBarMonitor_ == monitor && appBarEdge_ == edge_ && EqualRect(&appBarWant_, &want))
        return;
    constexpr UINT kAppBarEdge[] = {ABE_BOTTOM, ABE_TOP, ABE_LEFT, ABE_RIGHT}; // in BarEdge's order
    abd.uEdge = kAppBarEdge[int(edge_)];
    abd.rc = want;
    AppBarMessage(ABM_QUERYPOS, abd); // inside another app bar (the system taskbar, if shown) on the same edge
    RECT rc = frame.FromArea(abd.rc);
    rc.top = rc.bottom - reserve;
    abd.rc = frame.ToArea(rc);
    AppBarMessage(ABM_SETPOS, abd);
    const RECT granted = frame.FromArea(abd.rc);
    appBarRect_ = abd.rc;
    appBarWant_ = want;
    appBarReserved_ = int(std::max(0L, granted.bottom - granted.top));
    appBarMonitor_ = monitor;
    appBarEdge_ = edge_;
    if (!EqualRect(&abd.rc, &want)) {
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(monitor, &mi);
        Log(L"bar: appbar granted (%ld,%ld,%ld,%ld) != want (%ld,%ld,%ld,%ld), monitor (%ld,%ld,%ld,%ld), work (%ld,%ld,%ld,%ld)", abd.rc.left, abd.rc.top,
            abd.rc.right, abd.rc.bottom, want.left, want.top, want.right, want.bottom, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right,
            mi.rcMonitor.bottom, mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom);
    }
    Log(L"bar: keeps %d px of the screen's %s edge (always shown)", appBarReserved_, settings_.barEdge.c_str());
}

void Bar::ReadDisplays()
{
    displays_ = ConnectedDisplays();
    displayDpis_.assign(displays_.size(), 96);
    for (size_t i = 0; i < displays_.size(); ++i)
        if (UINT dx = 96, dy = 96; SUCCEEDED(GetDpiForMonitor(displays_[i].monitor, MDT_EFFECTIVE_DPI, &dx, &dy)))
            displayDpis_[i] = dx;
}

void Bar::Apply(const Settings& s)
{
    const bool changed = settings_.dockDisplayMode != s.dockDisplayMode || settings_.dockDisplay != s.dockDisplay;
    settings_ = s;
    if (displays_.empty())
        ReadDisplays();
    if (changed)
        activeDisplay_.clear();
    summonMonitor_ = nullptr;
    summonGraceUntil_ = 0;
    WatchDisplaysIfNeeded();
    Relayout();
    UpdateCover(); // the hide mode may have changed
    UpdateNearWatch(); // hoverRise may have been turned on or off
}

// Watching the pointer for the follow mode only with another display to follow it to.
void Bar::WatchDisplaysIfNeeded()
{
    if (settings_.dockDisplayMode == L"follow" && displayOverride_.empty() && displays_.size() > 1)
        SetTimer(input_, kDisplayTimer, kPointerMs, nullptr);
    else
        KillTimer(input_, kDisplayTimer);
}

void Bar::RefreshDisplays(bool workAreaOnly)
{
    auto before = std::exchange(displays_, {});
    auto beforeDpis = std::exchange(displayDpis_, {});
    ReadDisplays();
    WatchDisplaysIfNeeded();
    const bool changed = beforeDpis != displayDpis_ || !std::ranges::equal(before, displays_, [](const DisplayInfo& a, const DisplayInfo& b) {
                             return a.monitor == b.monitor && a.id == b.id && EqualRect(&a.bounds, &b.bounds) && a.primary == b.primary;
                         });
    const bool monitorGone = hmonitor_ && std::ranges::none_of(displays_, [&](const DisplayInfo& d) { return d.monitor == hmonitor_; });
    if ((changed && !workAreaOnly) || monitorGone)
        ++displayGeneration_; // HMONITOR can be reused after a mode or adapter change.
    summonMonitor_ = nullptr;
    summonGraceUntil_ = 0;
    if (dragging_)
        EndDrag(false);
    Relayout();
    GuardStacking(L"display");
}

void Bar::WatchDisplays()
{
    // Never move a Dock while its icons, a file drop, or a popup are being used.
    if (!visible_ || dragging_ || pressedLeft_ || !fileHoverKey_.empty() || (GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (keepShown && keepShown()) ||
        (previewOpen && previewOpen())) {
        summonMonitor_ = nullptr;
        return;
    }
    POINT point{};
    GetCursorPos(&point);
    HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONULL);
    auto target = std::ranges::find_if(displays_, [&](const DisplayInfo& d) { return d.monitor == monitor; });
    if (monitor && target == displays_.end()) {
        ReadDisplays();
        target = std::ranges::find_if(displays_, [&](const DisplayInfo& d) { return d.monitor == monitor; });
    }
    // The pointer must stay at the actual edge; screen crossings do not summon the Dock.
    if (target == displays_.end() || monitor == hmonitor_ || !AtDisplayEdge(edge_, point, target->bounds, FollowEdgeMargin(summonMonitor_ == monitor))) {
        summonMonitor_ = nullptr;
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (summonMonitor_ != monitor) {
        summonMonitor_ = monitor;
        summonAt_ = now;
        return;
    }
    if (double(now - summonAt_) < settings_.dockDisplayDelay)
        return;
    activeDisplay_ = target->id;
    summonMonitor_ = nullptr;
    KillTimer(input_, kSlideTimer);
    covered_ = revealed_ = tucked_ = slidOut_ = scene_.tucked = false;
    HideTooltip(), scene_.hoverKey.clear(), scene_.pointerX = scene_.pointerY = -1;
    SetRegion(false);
    Relayout();
    UpdateCover();
    // A summoned Dock stays visible long enough for the pointer to reach it.
    if (!FullScreenInFront(hmonitor_)) {
        revealed_ = true, leftAt_ = now, summonGraceUntil_ = now + 1200;
        Raise();
    }
    UpdateAutoHide();
    UpdateNearWatch();
}

void Bar::SetItems(std::vector<TaskItem> items)
{
    // Title changes (a browser switching tabs) only affect the tooltips; skip
    // the relayout and the redraw unless something visible changed.
    auto visible = [](const std::vector<TaskItem>& v) {
        std::wstring sig;
        for (const auto& t : v)
            sig += t.key + L"|" + std::to_wstring(!t.windows.empty()) + std::to_wstring(t.active) + std::to_wstring(t.attention) + L"|" +
                   std::to_wstring(t.badge) + L"|" + std::to_wstring(t.iconVersion) + L"\n";
        return sig;
    };
    bool same = visible(items) == visible(allItems_);
    allItems_ = std::move(items);
    if (filterTasks_)
        same = false; // monitor membership can change without the group's window count changing
    if (same) {
        items_ = allItems_;
        UpdateTooltips();
    } else {
        Relayout();
    }
}

HMONITOR Bar::ResolveRelayoutMonitor(MONITORINFO& mi)
{
    const std::wstring& id = !displayOverride_.empty()              ? displayOverride_ :
                             settings_.dockDisplayMode == L"fixed"  ? settings_.dockDisplay :
                             settings_.dockDisplayMode == L"follow" ? activeDisplay_ :
                                                                      std::wstring();
    auto valid = [&](HMONITOR m) {
        mi = {sizeof(mi)};
        return m && GetMonitorInfoW(m, &mi) && !IsRectEmpty(&mi.rcMonitor);
    };
    if (HMONITOR mon = ResolveDisplay(displays_, id); valid(mon))
        return mon;
    ReadDisplays();
    if (HMONITOR mon = ResolveDisplay(displays_, id); valid(mon))
        return mon;
    HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    return valid(mon) ? mon : nullptr;
}

void Bar::Relayout()
{
    MONITORINFO mi{sizeof(mi)};
    HMONITOR mon = ResolveRelayoutMonitor(mi);
    if (!mon)
        return;
    const bool primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    filterTasks_ = FilterTasksOnDisplay(settings_.taskbarApps, primary);
    items_ = TaskItemsForDisplay(allItems_, settings_.taskbarApps, primary, mon, GetAncestor(GetForegroundWindow(), GA_ROOTOWNER),
                                 [](HWND window) { return MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST); });
    UINT dpiX = 96, dpiY = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    const float s = float(dpiX) / 96.f;
    const Settings& st = settings_;
    const BarEdge edge = ParseBarEdge(st.barEdge);
    const bool edgeChanged = hmonitor_ && edge != edge_;
    const bool monitorChanged = hmonitor_ && (hmonitor_ != mon || !EqualRect(&monitor_, &mi.rcMonitor) || scale_ != s || edgeChanged);
    if (edgeChanged) {
        if (dragging_) {
            dragging_ = false;
            dragKey_.clear();
        }
        HideTooltip();
        near_.Reset();
        nearX_ = -1;
        nearRise_ = nearMagnify_ = false;
        scene_.hoverKey.clear();
        scene_.pressedKey.clear();
        scene_.pointerX = scene_.pointerY = -1;
        scene_.rippleMs = 0;
        pressed_.clear();
        pressedLeft_ = tracking_ = regionExtended_ = false;
    }
    edge_ = edge;
    const EdgeFrame wa{edge_, mi.rcWork};
    // Resolve per-display visibility without changing the user's individual switches.
    const bool showRight = !st.taskbarRightPrimaryOnly || primary;
    const bool showClock = showRight && st.showClock;
    const bool showTray = showRight && st.showTray;
    const bool showIme = showRight && st.imeStatus;
    const bool showStatus = showRight && st.showStatus;
    const bool showNotifications = showRight && st.showNotifications;
    const bool showDesktop = showRight && st.showDesktop;
    const bool showMusic = showRight && st.showNowPlaying && musicShown_;
    const bool showStacks = showRight && st.showFolderStacks;

    // Metrics (px). Every icon sits centred on a square plate (press and drag
    // feedback); the plate has the same gap `v` to the top and bottom edge,
    // and its corner radius is the bar's minus that gap, so plate and bar
    // corners are concentric.
    const int inset = Px(kInsetDip, s);
    const int barH = Px(st.height, s);
    const int icon = std::clamp(Px(st.iconSize, s), Px(12, s), barH - Px(16, s));
    int plate = std::max(icon + Px(4, s), std::min(barH - Px(12, s), int(std::lround(icon * 1.55))));
    int gap = std::max(Px(4, s), int(std::lround(icon * 0.18)));
    const int sepGap = Px(8, s);
    const float mag = float(std::clamp(st.magnify, 1.0, 2.0));

    // Clock block sized for its widest possible text, or its two stacked lines on a side edge.
    float timePx = float(barH) * 0.25f, datePx = float(barH) * 0.19f;
    int clockW = 0;
    if (showClock) {
        float span = timePx * 1.22f + datePx * 1.25f;
        if (!Vertical(edge_)) {
            std::wstring time, date;
            ClockStrings(time, date, st.showClockSeconds);
            span = std::max(MeasureUiText(dwrite_.get(), WidestDigits(time), timePx, DWRITE_FONT_WEIGHT_SEMI_BOLD),
                            MeasureUiText(dwrite_.get(), WidestDigits(date), datePx, DWRITE_FONT_WEIGHT_NORMAL));
        }
        clockW = int(std::ceil(span)) + Px(22, s);
    }

    // Left to right: [logo] [apps ... (overflow)] | [tray] [input method] [status] [clock] [bell].
    // The logo, the overflow entry, the tray and the bell use the same plates
    // as the apps; so does the status ring (battery or volume, Wi-Fi).
    // The status entry as three icons (statusStyle) is wider than a plate by about an icon.
    const int statusExtra = st.statusStyle == L"icons" ? int(std::lround(icon * 1.3f)) : 0;
    // Show Desktop: a narrow slot at the bar's right end, a thin line in it, as the system taskbar's.
    const int deskW = showDesktop ? std::max(Px(10, s), int(std::lround(icon * 0.45))) : 0;
    const int rightPlates = int(showStacks) + int(showTray) + int(showIme) + int(showStatus) + int(showMusic) + int(showNotifications);
    const int rightCount = rightPlates + int(clockW != 0) + int(showDesktop);
    const int groupGap = rightCount ? sepGap : 0;
    const int total = int(items_.size());
    int n = total;
    bool more = false;
    auto padFor = [&](int p) { return (barH - p) / 2 + Px(4, s); }; // ends: plate gap plus a little air
    auto contentWidth = [&](int count, int p, int g, bool overflow) {
        int slots = count + (overflow ? 1 : 0);
        int w = 2 * padFor(p) + p + 2 * g + (st.showLaunchpad ? p + g : 0); // logo (and Launchpad) and the air after it
        w += slots ? slots * p + (slots - 1) * g : 0;    // apps
        if (rightCount)
            w += 2 * sepGap + p * rightPlates + (showStatus ? statusExtra : 0) + clockW + deskW + (rightCount - 1) * g;
        return std::max(w, barH * 2);
    };
    // The magnified bar stretches by up to (mag - 1) * range on top of its width.
    auto stretchFor = [&](int p, int g) { return mag > 1.001f ? int(std::ceil((mag - 1) * float(p + g) * 2.3f)) : 0; };
    const int screenW = int(wa.Along()) - 2 * inset;
    const int maxW = std::max(barH * 2, std::min(Px(st.width, s), screenW - stretchFor(plate, gap)));
    // Too many apps: tighten the gaps, then the plates; what still does not fit
    // goes behind the overflow entry (never silently dropped).
    while (n > 0 && contentWidth(n, plate, gap, false) > maxW && gap > Px(2, s))
        --gap;
    while (n > 0 && contentWidth(n, plate, gap, false) > maxW && plate > icon + Px(4, s))
        --plate;
    if (contentWidth(n, plate, gap, false) > maxW) {
        more = true;
        while (n > 0 && contentWidth(n, plate, gap, true) > maxW)
            --n;
    }
    hidden_.clear();
    for (int i = n; i < total; ++i)
        hidden_.push_back(items_[size_t(i)].key);
    // Never narrower than what is always there (logo, overflow, tray, status, clock, bell).
    const int barW = std::max(st.autoWidth ? std::min(contentWidth(n, plate, gap, more), maxW) : maxW, std::min(contentWidth(0, plate, gap, more), screenW));
    const int pad = padFor(plate);
    const int statusW = plate + statusExtra;
    const int v = (barH - plate) / 2;
    const int pitch = plate + gap;
    const int radius = std::min(Px(st.cornerRadius, s), std::min(barW, barH) / 2);

    // Room in the windows around the shape: the shadow everywhere; at the
    // sides half of the magnified stretch (the bar grows on both sides of the
    // pointer); above, the raised glass and magnified or hopping icons.
    const float range = float(pitch) * 2.3f;
    const int stretch = stretchFor(plate, gap);
    // The window keeps the width of the widest bar the settings allow, and
    // the bar sits centred in it: when apps come and go only the shape
    // changes, so its width and the icons can glide instead of the window
    // jumping to a new size.
    const int roomW = std::max(barW, maxW);
    const int side = inset + (stretch + 1) / 2 + (stretch ? Px(2, s) : 0);
    const int insetX = side + (roomW - barW) / 2;
    const int lift = std::max(int(std::ceil(float(icon) * (mag - 1))), int(std::ceil(float(icon) * 0.55f)));
    const int glassRise = st.hoverRise ? int(std::ceil(float(icon) * (mag - 1))) : 0;
    const float badgeH = std::max(13.f * s, float(icon) * 0.44f);
    const int badgeHeadroom = int(std::ceil((badgeH * 0.30f + 1.5f * s) * mag));
    const int rise = std::max(glassRise, lift - (barH - icon) / 2 + std::max(Px(6, s), badgeHeadroom));
    // Keep shadow room above the raised glass, including at maximum magnification.
    const int insetTop = std::max(inset + glassRise, rise);

    const int winW = roomW + 2 * side, winH = barH + insetTop + inset;
    const int margin = Px(st.margin, s);
    // "Always shown": the bar keeps its strip of the screen as an app bar (as the system taskbar
    // does), so maximized windows end before it (#40). Its own strip is not part of where it sits.
    const int reserve = visible_ && st.bar && st.autoHide == L"never" && st.barReserve ? margin + barH : 0;
    const int ownReserve = appBar_ && appBarMonitor_ == mon && appBarEdge_ == edge_ ? appBarReserved_ : 0;
    const RECT win = PlaceBarWindow({edge_, mi.rcMonitor, mi.rcWork, winW, winH, barH, margin, insetTop, st.hideTaskbar, ownReserve});

    // Display order: during a drag the dragged app leaves its slot and the
    // others close up around the insertion point.
    int dragIndex = -1;
    const std::vector<int> order = BuildDragOrder(n, dragging_, dragKey_, dragInsert_, items_, dragIndex);

    // Launch feedback is one 480 ms hop.
    const double now = ClockMs();
    for (auto it = bounces_.begin(); it != bounces_.end();)
        it = now - it->second > 480 ? bounces_.erase(it) : std::next(it);

    // Scene in layout coordinates.
    const D2D1_POINT_2F shift =
        hmonitor_ && !edgeChanged ? EdgeFrame{edge_, win}.FromArea(EdgeFrame{edge_, window_}.ToArea(D2D1_POINT_2F{0, 0})) : D2D1_POINT_2F{};
    BarScene scene;
    scene.edge = edge_;
    scene.scale = s;
    scene.bar = D2D1::RectF(float(insetX), float(insetTop), float(insetX + barW), float(insetTop + barH));
    scene.windowW = float(winW);
    scene.windowH = float(winH);
    scene.tucked = tucked_;
    scene.plateRadius = float(std::clamp(radius - v, Px(3, s), plate / 2));
    scene.hoverKey = dragging_ ? std::wstring() : scene_.hoverKey;
    scene.pressedKey = scene_.pressedKey;
    scene.dragKey = dragging_ ? dragKey_ : std::wstring();
    scene.magnify = mag;
    scene.magnifyRange = range;
    scene.magnifyLift = st.hoverRise ? float(icon) * (mag - 1) : 0;
    scene.earlyRise = st.hoverRise && st.riseNear;
    scene.earlyMagnify = st.magnifyNear;
    // Keep the pointer and the ripple; only the geometry changes. The pointer
    // moves with the window if it was rebuilt at another place.
    scene.pointerX = scene_.pointerX < 0 ? -1 : scene_.pointerX + shift.x;
    scene.pointerY = scene_.pointerX < 0 ? -1 : scene_.pointerY + shift.y;
    // The pointer just above the bar: the glass and/or the icons answer it (hoverRise,
    // riseNear, magnifyNear).
    scene.nearX = nearX_ < 0 ? -1 : nearX_ + shift.x;
    scene.nearRise = nearRise_;
    scene.nearMagnify = nearMagnify_;
    scene.alertLight = st.attentionLight;
    scene.alertBounce = st.attentionBounce;
    scene.rippleX = scene_.rippleX + shift.x;
    scene.rippleY = scene_.rippleY + shift.y;
    scene.rippleMs = scene_.rippleMs;
    hits_.clear();
    // Hit cells reach up to the window's top: magnified icons rise above the
    // shape, and the input region then covers that room too.
    const int hitTop = 0, hitBottom = insetTop + barH;
    const float plateTop = float(insetTop + v);
    const float iconTop = float(insetTop) + float(barH - icon) / 2;
    auto entry = [&](const wchar_t* key, int left, const wchar_t* glyph) {
        SceneItem it;
        it.key = key;
        it.plate = D2D1::RectF(float(left), plateTop, float(left + plate), plateTop + float(plate));
        float il = float(left) + float(plate - icon) / 2;
        it.icon = D2D1::RectF(il, iconTop, il + float(icon), iconTop + float(icon));
        it.glyph = glyph;
        return it;
    };
    // Disabled entries consume neither space nor hit targets.
    const int startLeft = insetX + pad;
    int cursor = insetX + barW - pad;
    auto rightSlot = [&](int width, bool enabled) {
        if (!enabled)
            return cursor;
        cursor -= width;
        const int left = cursor;
        cursor -= gap;
        return left;
    };
    // The right-hand group, from the bar's end leftwards.
    std::array<RightEntry, 8> right{{{kDesktopKey, deskW, L"", showDesktop},
                                     {kNotifyKey, plate, L"\uEA8F", showNotifications},
                                     {kClockKey, clockW, L"", clockW != 0},
                                     {kMusicKey, plate, L"", showMusic},
                                     {kStatusKey, statusW, L"", showStatus},
                                     {kImeKey, plate, L"", showIme},
                                     {kTrayKey, plate, L"\uE70E", showTray},
                                     {kStacksKey, plate, L"", showStacks}}};
    for (auto& e : right)
        e.left = rightSlot(e.width, e.shown);
    const int rightLeft = rightCount ? cursor + gap : insetX + barW - pad;
    const int launchLeft = startLeft + plate + gap; // Launchpad, one slot right of the logo
    const int appsMin = (st.showLaunchpad ? launchLeft : startLeft) + plate + 2 * gap;
    const int slots = n + (more ? 1 : 0);
    const int appsW = slots ? slots * plate + (slots - 1) * gap : 0;
    // Apps are centred in the space between the logo and the separator (matters when autoWidth is off).
    const int appsLeft = appsMin + std::max(0, (rightLeft - 2 * groupGap - appsMin - appsW) / 2);
    scene.extras.push_back(entry(kStartKey, startLeft, L""));
    hits_.push_back({kStartKey, RECT{insetX, hitTop, (st.showLaunchpad ? launchLeft : appsMin) - gap / 2, hitBottom}});
    if (st.showLaunchpad) {
        scene.extras.push_back(entry(kLaunchpadKey, launchLeft, L""));
        hits_.push_back({kLaunchpadKey, RECT{launchLeft - gap / 2, hitTop, appsMin - gap / 2, hitBottom}});
    }
    // Icons are rendered for their magnified size, so they stay sharp when they grow.
    const int iconPx = int(std::lround(float(icon) * mag));
    std::map<std::wstring, CachedIcon> keep;
    for (int k = 0; k < n; ++k) {
        const int i = order[size_t(k)];
        const TaskItem& t = items_[size_t(i)];
        SceneItem it;
        it.key = t.key;
        float left = float(appsLeft + k * pitch);
        if (i == dragIndex) // follows the pointer, kept within the app strip
            left = std::clamp(float(dragX_ - grabDx_) - float(plate) / 2, float(appsLeft - gap), float(appsLeft + n * pitch - gap - plate + gap));
        it.plate = D2D1::RectF(left, plateTop, left + float(plate), plateTop + float(plate));
        float iconLeft = left + float(plate - icon) / 2;
        it.icon = D2D1::RectF(iconLeft, iconTop, iconLeft + float(icon), iconTop + float(icon));
        it.running = !t.windows.empty();
        it.active = t.active;
        it.attention = t.attention;
        it.badge = st.barBadges ? t.badge : 0;
        if (auto b = bounces_.find(t.key); b != bounces_.end()) {
            it.bounceMs = b->second;
        }
        // Icons are cached per key until the model reports a new version.
        CachedIcon c;
        if (auto f = icons_.find(t.key); f != icons_.end() && f->second.version == t.iconVersion && f->second.px == iconPx && f->second.image)
            c = f->second;
        else if (iconProvider) {
            IconImage img = iconProvider(t.key, iconPx);
            if (!img.Empty())
                c = {t.iconVersion, iconPx, nextIconId_++, std::make_shared<const IconImage>(std::move(img))};
        }
        if (c.image) {
            it.image = c.image;
            it.imageId = c.id;
            keep[t.key] = c;
        }
        scene.items.push_back(std::move(it));
        // Hit cells split the gaps.
        int hl = appsLeft + k * pitch - gap / 2;
        int hr = appsLeft + k * pitch + plate + (gap - gap / 2);
        hits_.push_back({t.key, RECT{hl, hitTop, hr, hitBottom}});
    }
    if (more) {
        int left = appsLeft + n * pitch;
        scene.extras.push_back(entry(kMoreKey, left, L"\uE712"));
        hits_.push_back({kMoreKey, RECT{left - gap / 2, hitTop, left + plate + gap / 2, hitBottom}});
    }
    icons_ = std::move(keep);
    appsLeft_ = appsLeft;
    pitch_ = pitch;
    plate_ = plate;
    shown_ = n;
    scene.separatorX = rightCount ? float(rightLeft - sepGap) : -1;
    scene.status = status_;
    scene.statusIcons = st.statusStyle == L"icons";
    PlaceRight(scene, right, EntryMetrics{plate, gap, icon, hitTop, hitBottom, insetX + barW, plateTop, iconTop}, timePx, datePx);
    if (!scene.hoverKey.empty() && std::ranges::none_of(hits_, [&](const Hit& h) { return h.key == scene.hoverKey; }))
        scene.hoverKey.clear(); // the hovered app went away
    // The input method glyph is the foreground window's, not layout: it rides through a rebuild.
    scene.imeGlyph = scene_.imeGlyph;
    scene.imePrevGlyph = scene_.imePrevGlyph;
    scene.imeMs = scene_.imeMs;
    scene_ = std::move(scene);

    // The window only changes when the bar's size does (not on every drag move).
    const bool shown = visible_ && !slidOut_;
    if (!dragging_ || !EqualRect(&win, &applied_) || shown != appliedVisible_)
        PlaceWindows(win, shown);
    window_ = win, monitor_ = mi.rcMonitor, hmonitor_ = mon;
    UpdateAppBar(mon, mi.rcMonitor, reserve);
    scale_ = s, insetX_ = insetX, insetTop_ = insetTop, rise_ = rise, radius_ = radius;
    SetRegion(regionExtended_);
    UpdateTooltips();
    Publish();
    if (monitorChanged)
        onMonitorChanged();
    onItemsChanged();
}

void Bar::SetRegion(bool extended)
{
    regionExtended_ = extended;
    const int barW = int(scene_.bar.right - scene_.bar.left), barH = int(scene_.bar.bottom - scene_.bar.top);
    const int ext = extended ? rise_ : 0;
    const int rgnMetrics[7] = {barW, barH, radius_, insetX_, insetTop_, ext, int(edge_)};
    if (std::memcmp(appliedRgn_, rgnMetrics, sizeof(rgnMetrics)) == 0)
        return;
    RECT cr{};
    GetClientRect(input_, &cr);
    const EdgeFrame frame{edge_, cr};
    const RECT shape = frame.ToArea(RECT{insetX_, insetTop_, insetX_ + barW, insetTop_ + barH});
    HRGN rgn = ContinuousRegion(RECT{shape.left, shape.top, shape.right + 1, shape.bottom + 1}, radius_);
    if (ext > 0) {
        // While the pointer is on the bar, the room on its inner side belongs to the
        // raised glass and icons (clicks go to the icon under the pointer).
        const RECT inner = frame.ToArea(RECT{insetX_ + radius_ / 2, insetTop_ - ext, insetX_ + barW - radius_ / 2, insetTop_ + radius_});
        HRGN top = CreateRectRgn(inner.left, inner.top, inner.right, inner.bottom);
        CombineRgn(rgn, rgn, top, RGN_OR);
        DeleteObject(top);
    }
    if (!SetWindowRgn(input_, rgn, FALSE))
        DeleteObject(rgn);
    std::memcpy(appliedRgn_, rgnMetrics, sizeof(rgnMetrics));
}

void Bar::ActivateIndex(int index)
{
    if (index < 0 || index >= int(items_.size()) || !onActivate)
        return;
    const TaskItem& t = items_[size_t(index)];
    Log(L"bar: app %d by the keyboard (%s)", index + 1, t.name.c_str());
    if (t.windows.empty())
        Bounce(t.key);
    onActivate(std::wstring(t.key)); // the copy: activating may change the items
}

void Bar::Bounce(const std::wstring& key)
{
    Log(L"bar: bounce %s", key.c_str());
    double now = ClockMs();
    bounces_[key] = now;
    for (auto& it : scene_.items)
        if (it.key == key)
            it.bounceMs = now;
    Publish();
}

void Bar::SetPanel(const GlassPanel& panel)
{
    if (panel.visible)
        panels_[panel.hwnd] = panel;
    else
        panels_.erase(panel.hwnd);
    Publish(false);
}

void CALLBACK Bar::WinEvent(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD)
{
    auto found = g_guards.find(hook);
    if (found == g_guards.end() || idObject != OBJID_WINDOW || idChild != CHILDID_SELF)
        return;
    Bar* bar = found->second;
    if (event == EVENT_OBJECT_LOCATIONCHANGE) {
        if (bar->filterTasks_ && !bar->layoutPending_) {
            const bool known =
                std::ranges::any_of(bar->allItems_, [&](const TaskItem& item) { return std::ranges::find(item.windows, hwnd) != item.windows.end(); });
            const bool shown =
                std::ranges::any_of(bar->items_, [&](const TaskItem& item) { return std::ranges::find(item.windows, hwnd) != item.windows.end(); });
            if (known && shown != (MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) == bar->hmonitor_)) {
                bar->layoutPending_ = true;
                PostMessageW(bar->input_, WM_BAR_LAYOUT, 0, 0);
            }
        }
        // Windows move many times a second while dragged: the windows of the monitor are walked
        // once for each burst, not for every step.
        if ((hwnd == GetForegroundWindow() || MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL) == bar->hmonitor_) && !bar->coverPending_) {
            bar->coverPending_ = true;
            SetTimer(bar->input_, kCoverTimer, kCoverMs, nullptr);
        }
        return;
    }
    if (bar->settings_.taskbarApps != L"all")
        bar->Relayout();
    bar->GuardStacking(event == EVENT_SYSTEM_FOREGROUND ? L"foreground" : L"move");
}

void Bar::ExpectCover(UINT ms)
{
    expectCoverUntil_ = GetTickCount64() + ms;
    UpdateCover();
    coverPending_ = true;
    SetTimer(input_, kCoverTimer, ms + 20, nullptr); // then by what is really in front
}

void Bar::Reveal()
{
    if (!covered_ || revealed_ || !visible_ || FullScreenInFront(hmonitor_))
        return;
    revealed_ = true;
    leftAt_ = 0;
    UpdateAutoHide();
}

bool Bar::RevealOverGame()
{
    if (!visible_ || !tucked_)
        return false;
    const ULONGLONG now = GetTickCount64();
    revealed_ = true;
    leftAt_ = now;
    summonGraceUntil_ = now + 2500;
    UpdateAutoHide(); // raised without activation (Raise)
    return true;
}

void Bar::UpdateAutoHide()
{
    bool tuck = covered_ && !revealed_ && visible_;
    if (tuck == tucked_)
        return;
    tucked_ = scene_.tucked = tuck;
    onTuckChanged();
    if (tuck) {
        HideTooltip();
        SetRegion(false);
        // The painter's slide out takes kSlideOutMs; then the windows go.
        SetTimer(input_, kSlideTimer, UINT(BarPainter::kSlideOutMs) + 60, nullptr);
        UpdateNearWatch();
        Publish();
        return;
    }
    KillTimer(input_, kSlideTimer);
    if (slidOut_) {
        slidOut_ = false;
        Relayout(); // shows the windows; the painter slides the bar up
    } else {
        Publish();
    }
    UpdateNearWatch();
    if (visible_)
        Raise();
}

void Bar::SetStatus(const SystemStatus& status)
{
    if (status == status_)
        return;
    status_ = status;
    scene_.status = status;
    Publish();
    UpdateTooltips();
}

void Bar::SetIme(const std::wstring& glyph)
{
    if (glyph == scene_.imeGlyph)
        return;
    scene_.imePrevGlyph = scene_.imeGlyph;
    scene_.imeGlyph = glyph;
    scene_.imeMs = ClockMs();
    Publish(); // content only: the glass does not change
}

void Bar::SetUnread(int count)
{
    if (count == unread_)
        return;
    unread_ = count;
    Relayout();
}

void Bar::Publish(bool bar)
{
    {
        std::lock_guard g(lock_);
        snapshot_.settings = settings_;
        snapshot_.scene = scene_;
        snapshot_.window = window_;
        snapshot_.monitor = monitor_;
        snapshot_.hmonitor = hmonitor_;
        snapshot_.displayGeneration = displayGeneration_;
        snapshot_.scale = scale_;
        snapshot_.visible = visible_ && !slidOut_;
        snapshot_.scene.music = music_;
        snapshot_.panels.clear();
        for (const auto& [hwnd, p] : panels_)
            snapshot_.panels.push_back(p);
        if (bar)
            ++snapshot_.generation;
        publishSeq_.store(publishSeq_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }
    SetEvent(wake_);
}

// The bar's time and date, in the system's own formats. `seconds` asks for the
// long time format (which carries seconds); a locale whose long time has none
// gets them added after the minutes, so the switch always shows a difference.
void Bar::ClockStrings(std::wstring& time, std::wstring& date, bool seconds)
{
    wchar_t t[64] = {}, d[64] = {};
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, seconds ? 0 : TIME_NOSECONDS, nullptr, nullptr, t, 64);
    if (seconds) {
        wchar_t plain[64] = {};
        GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, nullptr, nullptr, plain, 64);
        if (std::wstring(t) == plain) {
            wchar_t pattern[64] = {};
            if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_STIMEFORMAT, pattern, ARRAYSIZE(pattern)) > 0) {
                std::wstring withSeconds = pattern;
                const size_t minute = withSeconds.rfind(L'm');
                if (minute != std::wstring::npos) {
                    withSeconds.insert(minute + 1, L":ss");
                    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, nullptr, withSeconds.c_str(), t, ARRAYSIZE(t));
                }
            }
        }
    }
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, nullptr, nullptr, d, 64, nullptr);
    time = t;
    date = d;
}

std::unique_ptr<GlassRenderer> Bar::TryCreatePanelGlass(const Gpu& gpu, HWND hwnd, int width, int height, bool desktop) const
{
    try {
        return std::make_unique<GlassRenderer>(gpu, hwnd, width, height, false, false, desktop);
    } catch (const winrt::hresult_error& e) {
        Log(L"render: popup glass unavailable: 0x%08X", static_cast<unsigned>(e.code()));
        return nullptr;
    }
}

bool Bar::ShowOwnWindowsToCapture(bool visible)
{
    if (WaitForSingleObject(quit_, 0) != WAIT_TIMEOUT)
        return false;
    if (debugCapturable_)
        visible = true;
    if (visible == captureVisible_)
        return true;
    // The windows belong to the UI thread; it answers once they are changed.
    ResetEvent(captureAck_);
    PostMessageW(visual_, WM_BAR_CAPTURE, visible ? 1 : 0, 0);
    if (!WaitCaptureAckOrQuit(quit_, captureAck_, 1000)) {
        if (WaitForSingleObject(quit_, 0) != WAIT_TIMEOUT)
            return false;
        Log(L"render: the UI thread did not confirm the capture exclusion in time");
    }
    captureVisible_ = visible;
    Log(L"render: Atrium %s screenshots", visible ? L"shows in" : L"is kept out of");
    return true;
}

void Bar::RenderThread()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    while (WaitForSingleObject(quit_, 0) == WAIT_TIMEOUT) {
        SessionEnd end = SessionEnd::Retry;
        try {
            end = RenderSession();
        } catch (const winrt::hresult_error& e) {
            Log(L"render: 0x%08X %s; rebuilding in 1 s", static_cast<unsigned>(e.code()), e.message().c_str());
        } catch (const std::exception& e) {
            Log(L"render: %hs; rebuilding in 1 s", e.what());
        }
        if (end == SessionEnd::Exit || (end == SessionEnd::Retry && WaitForSingleObject(quit_, 1000) != WAIT_TIMEOUT))
            break;
    }
    winrt::uninit_apartment();
}

} // namespace atrium
