#include "bar_edge.h"

#include <iostream>
#include <stdexcept>

using namespace atrium;

namespace {
const RECT kScreen{-1920, 0, 0, 1080}; // a display left of the primary one

void Require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

bool Same(const RECT& a, const RECT& b) { return EqualRect(&a, &b) != FALSE; }

// A 400 x 64 bar centred along the edge, its outer side on the edge.
RECT Strip(const EdgeFrame& f) { return f.ToArea(RECT{LONG(f.Along() / 2) - 200, LONG(f.Across()) - 64, LONG(f.Along() / 2) + 200, LONG(f.Across())}); }

void CheckPlacement()
{
    Require(Same(Strip({BarEdge::Bottom, kScreen}), {-1160, 1016, -760, 1080}), "bottom: the strip sits on the bottom edge");
    Require(Same(Strip({BarEdge::Top, kScreen}), {-1160, 0, -760, 64}), "top: the strip sits on the top edge");
    Require(Same(Strip({BarEdge::Left, kScreen}), {-1920, 340, -1856, 740}), "left: the strip runs down the left edge");
    Require(Same(Strip({BarEdge::Right, kScreen}), {-64, 340, 0, 740}), "right: the strip runs down the right edge");
}

void CheckRoundTrip()
{
    for (BarEdge edge : {BarEdge::Bottom, BarEdge::Top, BarEdge::Left, BarEdge::Right}) {
        const EdgeFrame f{edge, kScreen};
        const POINT p{-700, 333};
        const POINT back = f.ToArea(f.FromArea(p));
        Require(back.x == p.x && back.y == p.y, "a point comes back to where it was");
        const RECT r{-1500, 100, -1400, 180};
        Require(Same(f.ToArea(f.FromArea(r)), r), "a rect comes back to where it was");
    }
}

void CheckAxes()
{
    // u grows left to right on horizontal edges and top to bottom on vertical ones; v grows toward the edge.
    const EdgeFrame left{BarEdge::Left, kScreen}, top{BarEdge::Top, kScreen};
    Require(left.Along() == 1080 && left.Across() == 1920, "a side edge runs along the screen's height");
    Require(left.FromArea(POINT{-1920, 10}).x == 10 && left.FromArea(POINT{-1920, 10}).y == 1920, "left: the edge is the far end of v");
    Require(top.FromArea(POINT{-1910, 0}).y == 1080, "top: the edge is the far end of v");
    Require(ParseBarEdge(L"right") == BarEdge::Right && ParseBarEdge(L"") == BarEdge::Bottom, "setting names map to edges");
}

void CheckEdgeOf()
{
    // The primary display, whatever its size: each strip is told by its shape and side.
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    for (BarEdge edge : {BarEdge::Bottom, BarEdge::Top, BarEdge::Left, BarEdge::Right})
        Require(EdgeOf(Strip({edge, mi.rcMonitor})) == edge, "a strip's edge is read back from its rect");
}

RECT GlassBand(BarEdge edge, const RECT& win, int winW, int insetTop, int barH)
{
    return EdgeFrame{edge, win}.ToArea(RECT{0, insetTop, winW, insetTop + barH});
}

RECT ShortenEdge(RECT r, BarEdge edge, int by)
{
    switch (edge) {
    case BarEdge::Bottom: r.bottom -= by; break;
    case BarEdge::Top: r.top += by; break;
    case BarEdge::Left: r.left += by; break;
    case BarEdge::Right: r.right -= by; break;
    }
    return r;
}

LONG OuterEdge(const RECT& r, BarEdge edge)
{
    switch (edge) {
    case BarEdge::Bottom: return r.bottom;
    case BarEdge::Top: return r.top;
    case BarEdge::Left: return r.left;
    case BarEdge::Right: return r.right;
    }
    return 0;
}

void CheckBarWindowPlacement()
{
    const int winW = 600, barH = 60, insetTop = 18, inset = 14, winH = barH + insetTop + inset;

    // #141: secondary display at positive or negative coordinates, hidden taskbar, stale work-area deduction.
    for (const RECT& mon : {RECT{1920, 0, 3840, 1080}, RECT{-1920, -200, 0, 880}}) {
        for (int shortBy : {48, 60, 72, barH}) {
            RECT work = ShortenEdge(mon, BarEdge::Bottom, shortBy);
            work.left += 80; // other axis keeps using work area
            const RECT win = PlaceBarWindow({BarEdge::Bottom, mon, work, winW, winH, barH, 0, insetTop, true, 0});
            const RECT glass = GlassBand(BarEdge::Bottom, win, winW, insetTop, barH);
            Require(glass.bottom == mon.bottom, "#141: hidden taskbar anchors glass to monitor bottom");
            Require((win.left + win.right) == (work.left + work.right), "other axis stays centred in work area");
        }
    }

    // Coexisting with a visible system taskbar: work area + own reserve, clamped to monitor.
    const RECT primary{0, 0, 1920, 1080};
    {
        const RECT work{0, 0, 1920, 1080 - 48 - barH};
        const RECT win = PlaceBarWindow({BarEdge::Bottom, primary, work, winW, winH, barH, 0, insetTop, false, barH});
        const RECT glass = GlassBand(BarEdge::Bottom, win, winW, insetTop, barH);
        Require(glass.bottom == 1080 - 48, "coexisting: glass sits above the 48 px system taskbar");
    }
    for (BarEdge edge : {BarEdge::Bottom, BarEdge::Top, BarEdge::Left, BarEdge::Right}) {
        const RECT win = PlaceBarWindow({edge, primary, primary, winW, winH, barH, 0, insetTop, false, barH});
        const RECT glass = GlassBand(edge, win, winW, insetTop, barH);
        Require(OuterEdge(glass, edge) == OuterEdge(primary, edge), "coexisting: own reserve is clamped inside monitor");
    }

    // All four edges with hidden taskbar and margin 0 and 10.
    for (int margin : {0, 10}) {
        for (BarEdge edge : {BarEdge::Bottom, BarEdge::Top, BarEdge::Left, BarEdge::Right}) {
            const RECT work = ShortenEdge(kScreen, edge, 48);
            const RECT win = PlaceBarWindow({edge, kScreen, work, winW, winH, barH, margin, insetTop, true, 0});
            const RECT glass = GlassBand(edge, win, winW, insetTop, barH);
            const LONG sign = (edge == BarEdge::Top || edge == BarEdge::Left) ? 1 : -1;
            Require(OuterEdge(glass, edge) == OuterEdge(kScreen, edge) + sign * margin, "four edges: glass sits margin px from monitor edge");
        }
    }
}
} // namespace

int main()
{
    try {
        CheckPlacement();
        CheckRoundTrip();
        CheckAxes();
        CheckEdgeOf();
        CheckBarWindowPlacement();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    std::cout << "bar edge: all checks passed\n";
    return 0;
}
