#include "ui/genie_minimize.h"

#include "clock.h"
#include "log.h"

#include <dwmapi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace atrium {

namespace {

constexpr UINT WM_GENIE_PICTURE = WM_APP + 0x5E;
constexpr UINT_PTR kWatchTimer = 0x4751, kIdleTimer = 0x4752;
constexpr UINT kWatchMs = 300;
constexpr ULONGLONG kDrawnMs = 700;      // a window this long in front has drawn itself
constexpr ULONGLONG kNearMs = 1500;      // the picture's age while the pointer is near the caption buttons
constexpr ULONGLONG kFarMs = 20000;      // elsewhere: PrintWindow makes the app draw itself whole
constexpr int kLargeMax = 1600;          // the picture, at most this many px either way
constexpr double kFlightMs = 560;
constexpr int kRows = 64;                // the picture poured as this many slices

GenieMinimize* g_genie = nullptr; // the one effect, for its hook

void CALLBACK OnMinimizeStart(HWINEVENTHOOK, DWORD, HWND h, LONG object, LONG, DWORD, DWORD)
{
    if (object == OBJID_WINDOW && g_genie)
        g_genie->Minimizing(h);
}

void Transitions(HWND h, bool on)
{
    const BOOL off = on ? FALSE : TRUE;
    DwmSetWindowAttribute(h, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof(off));
}

RECT Frame(HWND h)
{
    RECT r{};
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))))
        GetWindowRect(h, &r);
    return r;
}

// A window worth the effect: a visible, top-level, captioned one that minimizes.
bool Minimizable(HWND h)
{
    const LONG_PTR style = GetWindowLongPtrW(h, GWL_STYLE);
    return h && IsWindowVisible(h) && !IsIconic(h) && (style & WS_MINIMIZEBOX) && (style & WS_CAPTION) == WS_CAPTION && !GetWindow(h, GW_OWNER);
}

float SmoothStep(float a, float b, float x)
{
    const float t = std::clamp((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3 - 2 * t);
}

float Lerp(float a, float b, float t) { return a + (b - a) * t; }

} // namespace

GenieMinimize::GenieMinimize(HINSTANCE instance, std::function<std::optional<RECT>(HWND)> target, std::function<bool(HWND)> skip)
    : surface_(instance, L"Atrium Genie", false, 0), snapshots_(surface_.Hwnd(), WM_GENIE_PICTURE), target_(std::move(target)), skip_(std::move(skip))
{
    surface_.SetGlassless(true);
    surface_.paint = [this](Canvas& c) { Paint(c); };
    surface_.message = [this](UINT m, WPARAM w, LPARAM, LRESULT&) {
        if (m == WM_TIMER && w == kWatchTimer) {
            Watch();
            return true;
        }
        if (m == WM_TIMER && w == kIdleTimer) {
            KillTimer(surface_.Hwnd(), kIdleTimer);
            if (flights_.empty())
                surface_.Hide(false); // idle for a while: its buffers go
            return true;
        }
        if (m != WM_GENIE_PICTURE)
            return false;
        for (WindowSnapshots::Result& r : snapshots_.Take())
            if (r.large.ok && pictures_.contains(r.window))
                pictures_[r.window].large = std::make_shared<const WindowPicture>(std::move(r.large));
        return true;
    };
}

GenieMinimize::~GenieMinimize() { SetEnabled(false); }

void GenieMinimize::SetEnabled(bool on)
{
    if (on == enabled_)
        return;
    enabled_ = on;
    if (on) {
        g_genie = this;
        hook_ = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZESTART, nullptr, OnMinimizeStart, 0, 0,
                                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        SetTimer(surface_.Hwnd(), kWatchTimer, kWatchMs, nullptr);
    } else {
        if (hook_)
            UnhookWinEvent(hook_);
        hook_ = nullptr;
        KillTimer(surface_.Hwnd(), kWatchTimer);
        Release(held_);
        pictures_.clear();
        front_ = nullptr;
    }
    Log(L"genie: %s", on ? L"on" : L"off");
}

void GenieMinimize::Release(HWND window)
{
    if (window && window == held_) {
        if (IsWindow(window))
            Transitions(window, true);
        held_ = nullptr;
    }
}

// The window in front: its picture kept fresh, and its own minimize animation off once there is one.
void GenieMinimize::Watch()
{
    HWND fg = GetForegroundWindow();
    HWND root = fg ? GetAncestor(fg, GA_ROOTOWNER) : nullptr;
    DWORD pid = 0;
    if (root)
        GetWindowThreadProcessId(root, &pid);
    if (pid == GetCurrentProcessId() || !Minimizable(root) || (skip_ && skip_(root)))
        root = nullptr;
    const ULONGLONG now = GetTickCount64();
    if (root != front_) {
        Release(held_);
        front_ = root;
        frontSince_ = now;
    }
    std::erase_if(pictures_, [&](const auto& kv) { return kv.first != root && !IsWindow(kv.first); });
    if (!root)
        return;
    Picture& p = pictures_[root];
    p.frame = Frame(root);
    if (p.large && held_ != root) {
        Transitions(root, false);
        held_ = root;
    }
    POINT pt{};
    GetCursorPos(&pt);
    const float s = float(GetDpiForWindow(root)) / 96.f;
    const RECT& r = p.frame;
    const bool atButtons = PtInRect(&r, pt) && pt.y < r.top + LONG(56 * s) && pt.x > r.right - LONG(200 * s);
    if (now - frontSince_ < kDrawnMs || now - p.taken < (atButtons ? kNearMs : kFarMs))
        return;
    p.taken = now;
    snapshots_.Request(root, 64, 64, 0, kLargeMax);
}

void GenieMinimize::Minimizing(HWND window)
{
    if (!enabled_ || window != held_)
        return; // not prepared for: Windows' own animation played
    const auto it = pictures_.find(window);
    const std::optional<RECT> to = target_ ? target_(window) : std::nullopt;
    Release(window); // down now: restoring it is Windows' own
    front_ = nullptr;
    if (it == pictures_.end() || !it->second.large || !to)
        return;
    Flight f;
    f.picture = it->second.large;
    f.key = L"genie|" + std::to_wstring(++flightGen_);
    f.from = it->second.frame;
    f.to = *to;
    f.startMs = ClockMs();
    flights_.push_back(std::move(f));
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromRect(&it->second.frame, MONITOR_DEFAULTTONEAREST), &mi);
    KillTimer(surface_.Hwnd(), kIdleTimer);
    if (!surface_.Visible())
        surface_.Show(mi.rcMonitor, false, POINT{mi.rcMonitor.left, mi.rcMonitor.top}, false);
    else
        surface_.Move(mi.rcMonitor);
    surface_.Invalidate();
}

void GenieMinimize::Paint(Canvas& c)
{
    for (auto it = flights_.begin(); it != flights_.end();) {
        const float t = float(std::clamp((c.now - it->startMs) / kFlightMs, 0.0, 1.0));
        PaintFlight(c, *it, t);
        if (t >= 1) {
            surface_.ForgetImage(it->key);
            it = flights_.erase(it);
        } else {
            ++it;
        }
    }
    if (!flights_.empty())
        c.Continue();
    else
        SetTimer(surface_.Hwnd(), kIdleTimer, 3000, nullptr);
}

// The picture in kRows slices across the way to the icon ("along"). In the frame where the icon lies
// ahead, a slice at v (0 the far side, 1 the near) slides from its place to the icon's line, the near
// ones first; at any point of the way the two sides bend from the window's towards the icon's, more as
// the pour goes on, along an S curve.
void GenieMinimize::PaintFlight(Canvas& c, const Flight& f, float t)
{
    const WindowPicture& pic = *f.picture;
    const RECT origin = surface_.Rect();
    auto dip = [&](LONG v, LONG o) { return float(v - o) / c.scale; };
    const D2D1_RECT_F w = D2D1::RectF(dip(f.from.left, origin.left), dip(f.from.top, origin.top), dip(f.from.right, origin.left), dip(f.from.bottom, origin.top));
    const D2D1_RECT_F ic = D2D1::RectF(dip(f.to.left, origin.left), dip(f.to.top, origin.top), dip(f.to.right, origin.left), dip(f.to.bottom, origin.top));
    const float wcx = (w.left + w.right) / 2, wcy = (w.top + w.bottom) / 2, icx = (ic.left + ic.right) / 2, icy = (ic.top + ic.bottom) / 2;
    // Which way: towards the bar's side the icon lies on.
    const bool vertical = std::abs(icy - wcy) * (w.right - w.left) >= std::abs(icx - wcx) * (w.bottom - w.top);
    const float sign = vertical ? (icy >= wcy ? 1.f : -1.f) : (icx >= wcx ? 1.f : -1.f);
    // The frame: a along the way (growing towards the icon), b across it.
    const float a0 = vertical ? (sign > 0 ? w.top : -w.bottom) : (sign > 0 ? w.left : -w.right);
    const float a1 = vertical ? (sign > 0 ? w.bottom : -w.top) : (sign > 0 ? w.right : -w.left);
    const float b0 = vertical ? w.left : w.top, b1 = vertical ? w.right : w.bottom;
    const float aEnd = vertical ? sign * icy : sign * icx;
    const float iconHalf = 0.35f * (vertical ? ic.right - ic.left : ic.bottom - ic.top), iconMid = vertical ? icx : icy;
    const float bend = SmoothStep(0.f, 0.42f, t), slide = SmoothStep(0.12f, 1.f, t);
    auto along = [&](float v) {
        constexpr float kLead = 0.9f; // how far ahead the near rows go
        const float s = std::clamp(slide * (1 + kLead) - (1 - v) * kLead, 0.f, 1.f);
        return Lerp(a0 + v * (a1 - a0), aEnd, s * s * (3 - 2 * s));
    };
    auto sides = [&](float a, float& lo, float& hi) {
        const float u = std::clamp((a - a0) / std::max(1.f, aEnd - a0), 0.f, 1.f), k = bend * SmoothStep(0.f, 1.f, u);
        lo = Lerp(b0, iconMid - iconHalf, k);
        hi = Lerp(b1, iconMid + iconHalf, k);
    };
    const float opacity = 1 - SmoothStep(0.86f, 1.f, t);
    const IconImage& image = pic.image;
    const float pw = float(pic.width), ph = float(pic.height), pad = float(kPicturePad);
    for (int i = 0; i < kRows; ++i) {
        const float v0 = float(i) / kRows, v1 = float(i + 1) / kRows;
        const float p0 = along(v0), p1 = along(v1) + 0.5f; // a hair over the next: no seams
        if (p1 - p0 < 0.55f)
            continue;
        float lo0 = 0, hi0 = 0, lo1 = 0, hi1 = 0;
        sides(p0, lo0, hi0);
        sides(p1, lo1, hi1);
        // Back to the screen: the slice's corners for its source's top left, top right, bottom right, bottom
        // left; its source rows (far side v = 0) in the picture.
        const float s0 = sign > 0 ? v0 : 1 - v1, s1 = sign > 0 ? v1 : 1 - v0;
        const float y0 = sign * p0, y1 = sign * p1; // the far end and the near end, on the screen's axis
        std::array<D2D1_POINT_2F, 4> quad{};
        D2D1_RECT_F src{};
        if (vertical) {
            // The source's top is the slice's far end when the icon is below, its near end when above.
            quad = sign > 0 ? std::array{D2D1::Point2F(lo0, y0), D2D1::Point2F(hi0, y0), D2D1::Point2F(hi1, y1), D2D1::Point2F(lo1, y1)}
                            : std::array{D2D1::Point2F(lo1, y1), D2D1::Point2F(hi1, y1), D2D1::Point2F(hi0, y0), D2D1::Point2F(lo0, y0)};
            src = D2D1::RectF(pad, pad + s0 * ph, pad + pw, pad + s1 * ph);
        } else {
            quad = sign > 0 ? std::array{D2D1::Point2F(y0, lo0), D2D1::Point2F(y1, lo1), D2D1::Point2F(y1, hi1), D2D1::Point2F(y0, hi0)}
                            : std::array{D2D1::Point2F(y1, lo1), D2D1::Point2F(y0, lo0), D2D1::Point2F(y0, hi0), D2D1::Point2F(y1, hi1)};
            src = D2D1::RectF(pad + s0 * pw, pad, pad + s1 * pw, pad + ph);
        }
        c.ImageQuad(f.key, image, src, quad, opacity);
    }
}

} // namespace atrium
