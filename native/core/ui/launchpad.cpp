#include "ui/launchpad.h"

#include "clock.h"
#include "localization.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace atrium {

namespace launchpad {

constexpr double kOpenMs = 300, kCloseMs = 200;
// The window: about three quarters of the work area, within these bounds (DIP), and 24 DIP clear of its edges.
constexpr float kMinW = 760, kMaxW = 1120, kMinH = 540, kMaxH = 780, kMinRadius = 26, kClear = 24;
constexpr float kSearchW = 280, kSearchH = 34, kSearchTop = 34;
constexpr float kLabel = 12.5f, kLabelH = 18, kLabelGap = 8;
constexpr float kDotsFromBottom = 40, kDotStep = 18, kDot = 4;
constexpr motion::Spring kPageSpring{0.42f, 0.9f};

} // namespace launchpad

using namespace launchpad;

namespace {

D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) { return D2D1::ColorF(c.r, c.g, c.b, c.a * a); }

} // namespace

Launchpad::Launchpad(HINSTANCE instance, AppCatalog& catalog) : surface_(instance, L"Atrium Launchpad", true, 0), catalog_(catalog)
{
    surface_.paint = [this](Canvas& c) { Paint(c); };
    surface_.message = [this](UINT m, WPARAM w, LPARAM l, LRESULT& r) { return Message(m, w, l, r); };
    surface_.onDismiss = [this] { Close(false); };
}

void Launchpad::Open(RECT work, HWND previous)
{
    previous_ = previous;
    apps_ = LaunchableApps();
    query_.clear();
    shown_.clear();
    Size(work);
    const float s = PopupScale(work);
    const LONG cx = (work.left + work.right) / 2, cy = (work.top + work.bottom) / 2;
    const LONG halfW = std::lround(w_ * s / 2), halfH = std::lround(h_ * s / 2);
    const RECT rect{cx - halfW, cy - halfH, cx + halfW, cy + halfH};
    page_ = 0;
    pageShown_ = pageSpeed_ = 0;
    sel_ = hover_ = pressed_ = -1;
    down_ = dragging_ = closing_ = launched_ = false;
    wheel_ = 0;
    hoverAnim_.clear();
    KillTimer(surface_.Hwnd(), kCloseTimer);
    openedMs_ = lastPaint_ = ClockMs();
    surface_.SetRadius(Radius());
    surface_.Show(rect, true, POINT{cx, cy}, false);
    PublishGlass(openedMs_);
    SetTimer(surface_.Hwnd(), kCaretTimer, GetCaretBlinkTime(), nullptr);
    PlaceIme();
}

void Launchpad::Size(RECT work)
{
    const float s = PopupScale(work);
    const float workW = float(work.right - work.left) / s, workH = float(work.bottom - work.top) / s;
    w_ = std::min(std::clamp(workW * 0.74f, kMinW, kMaxW), workW - 2 * kClear);
    h_ = std::min(std::clamp(workH * 0.78f, kMinH, kMaxH), workH - 2 * kClear);
    grid_ = ComputeLaunchpadGrid(w_, h_);
}

std::vector<CatalogApp> Launchpad::LaunchableApps() const
{
    std::vector<CatalogApp> apps = catalog_.Apps();
    std::erase_if(apps, [](const CatalogApp& a) { return !AppCatalog::IsLaunchable(a); });
    return apps;
}

void Launchpad::Prewarm(RECT work)
{
    if (surface_.Visible() || !catalog_.Loaded())
        return;
    const std::vector<CatalogApp> apps = LaunchableApps();
    Size(work);
    const int px = int(std::lround(grid_.iconDip * PopupScale(work)));
    if (prewarmed_ == std::make_pair(px, apps.size()))
        return;
    prewarmed_ = {px, apps.size()};
    std::vector<std::wstring> ids;
    for (size_t i = 0; i < apps.size() && i < size_t(2) * size_t(grid_.perPage); ++i)
        ids.push_back(apps[i].id);
    catalog_.PrefetchIcons(ids, px);
}

void Launchpad::Close(bool launched)
{
    if (!surface_.Visible() || closing_)
        return;
    closing_ = true;
    launched_ = launched;
    if (GetCapture() == surface_.Hwnd())
        ReleaseCapture();
    closedMs_ = ClockMs();
    lastClosed_ = GetTickCount64();
    KillTimer(surface_.Hwnd(), kCaretTimer);
    SetTimer(surface_.Hwnd(), kCloseTimer, UINT(kCloseMs) + 20, nullptr);
    surface_.Invalidate();
}

void Launchpad::Finish()
{
    KillTimer(surface_.Hwnd(), kCloseTimer);
    closing_ = false;
    surface_.Hide(false); // the sheet has already faded
    catalog_.SetVisibleIcons(VisibleIconsOwner::Launchpad, {});
    askedIcons_.clear();
    if (!launched_ && previous_ && IsWindow(previous_)) {
        const HWND fg = GetForegroundWindow();
        wchar_t cls[64] = {};
        if (fg && fg != surface_.Hwnd())
            GetClassNameW(fg, cls, int(std::size(cls)));
        if (!fg || fg == surface_.Hwnd() || std::wstring_view(cls) == L"AtriumBarInput")
            SetForegroundWindow(previous_);
    }
}

void Launchpad::Refresh()
{
    if (!surface_.Visible() || closing_)
        return;
    apps_ = LaunchableApps();
    if (!query_.empty())
        Filter();
    page_ = std::min(page_, Pages() - 1);
    surface_.Invalidate();
}

void Launchpad::Filter()
{
    shown_.clear();
    for (const auto& app : apps_)
        if (AppCatalog::Matches(app, query_))
            shown_.push_back(app);
    page_ = 0;
    pageShown_ = pageSpeed_ = 0;
    sel_ = query_.empty() || shown_.empty() ? -1 : 0; // Enter opens the best match
}

int Launchpad::Pages() const { return std::max(1, PageCount(Shown().size(), grid_.perPage)); }

float Launchpad::OpenAmount(double now) const
{
    if (closing_)
        return 1 - motion::EaseOut(float((now - closedMs_) / kCloseMs));
    return motion::EaseOut(float((now - openedMs_) / kOpenMs));
}

D2D1_RECT_F Launchpad::Cell(int slot) const
{
    const int row = slot / grid_.cols, col = slot % grid_.cols;
    const float x = grid_.gridLeft + float(col) * grid_.cellW, y = grid_.gridTop + float(row) * grid_.cellH;
    return D2D1::RectF(x, y, x + grid_.cellW, y + grid_.cellH);
}

D2D1_RECT_F Launchpad::SearchRect() const { return D2D1::RectF(w_ / 2 - kSearchW / 2, kSearchTop, w_ / 2 + kSearchW / 2, kSearchTop + kSearchH); }

float Launchpad::Radius() const { return std::max(radius_, kMinRadius); }

// The window's glass, fading with the opening and the closing.
void Launchpad::PublishGlass(double now)
{
    const float a = OpenAmount(now);
    ShapeDip sheet{D2D1::RectF(0, 0, w_, h_), Radius()};
    sheet.fade = 1 - a;
    surface_.SetShapes({sheet}, a > 0 && a < 1);
}

float Launchpad::Hover(Canvas& c, int id, bool on)
{
    auto& f = hoverAnim_[id];
    const float v = f.Step(on, c.now);
    if (f.moving)
        c.Continue();
    return v;
}

void Launchpad::Paint(Canvas& c)
{
    const Theme& t = c.theme;
    const float dt = float(std::clamp(c.now - lastPaint_, 0.0, 100.0)) / 1000.f;
    lastPaint_ = c.now;
    if (!dragging_) {
        kPageSpring.Step(pageShown_, pageSpeed_, float(page_), dt);
        if (motion::Spring::AtRest(pageShown_, pageSpeed_, float(page_))) {
            pageShown_ = float(page_);
            pageSpeed_ = 0;
        } else {
            c.Continue();
        }
    }
    const float a = OpenAmount(c.now);
    PublishGlass(c.now);
    if (a > 0 && a < 1)
        c.Continue();
    const D2D1_RECT_F all = D2D1::RectF(0, 0, w_, h_);
    if (c.panel)
        c.Fill(all, Radius(), Alpha(t.panel, a));
    // A faint veil over the glass, so the names read on any wallpaper.
    c.Fill(all, Radius(), t.light ? D2D1::ColorF(1, 1, 1, 0.10f * a) : D2D1::ColorF(0, 0, 0, 0.22f * a));
    // The apps settle from a little larger as the sheet fades in, and grow away as it fades out.
    c.BeginGroup(a, 1 + (closing_ ? 0.05f : 0.08f) * (1 - a), w_ / 2, h_ / 2);
    hits_.clear();
    visibleIcons_.clear();
    PaintSearch(c, t, a);
    if (Shown().empty() && !query_.empty())
        c.Text(Tr(L"No matching apps"), D2D1::RectF(0, h_ / 2 - 14, w_, h_ / 2 + 14), 17, t.subtext, DWRITE_FONT_WEIGHT_NORMAL, 1);
    const int first = int(std::floor(pageShown_));
    for (int page = first; page <= first + 1; ++page)
        if (page >= 0 && page < Pages() && std::abs(float(page) - pageShown_) < 1)
            PaintPage(c, t, page, (float(page) - pageShown_) * w_);
    PaintDots(c, t);
    c.EndGroup();
    if (visibleIcons_ != askedIcons_) {
        askedIcons_ = visibleIcons_;
        catalog_.SetVisibleIcons(VisibleIconsOwner::Launchpad, visibleIcons_); // the pages on screen first
    }
}

void Launchpad::PaintSearch(Canvas& c, const Theme& t, float shown)
{
    const D2D1_RECT_F r = SearchRect();
    const float radius = kSearchH / 2;
    c.Fill(r, radius, Alpha(t.hover, 1.3f));
    c.Stroke(r, radius, Alpha(t.text, 0.10f));
    c.Glyph(L"", D2D1::RectF(r.left + 12, r.top, r.left + 30, r.bottom), 13, t.subtext); // Search
    const D2D1_RECT_F text = D2D1::RectF(r.left + 36, r.top, r.right - 14, r.bottom);
    if (query_.empty()) {
        c.Text(Tr(L"Search"), text, 13.5f, t.subtext);
    } else {
        c.Text(query_, text, 13.5f, t.text);
    }
    // The caret, blinking at the system's pace (kCaretTimer repaints).
    const UINT blink = std::max(1u, GetCaretBlinkTime());
    if (shown >= 1 && blink != INFINITE && (ULONGLONG(c.now) / blink) % 2 == 0) {
        const float x = text.left + std::min(c.TextWidth(query_, 13.5f), text.right - text.left);
        c.Fill(D2D1::RectF(x, r.top + 9, x + 1.5f, r.bottom - 9), 0.75f, t.text);
    }
    hits_.push_back({r, kSearch});
}

void Launchpad::PaintPage(Canvas& c, const Theme& t, int page, float dx)
{
    const LaunchpadSlice slice = PageSlice(Shown().size(), page, grid_.perPage);
    for (size_t i = slice.begin; i < slice.end; ++i) {
        const D2D1_RECT_F cell = Cell(int(i - slice.begin));
        const D2D1_RECT_F at = D2D1::RectF(cell.left + dx, cell.top, cell.right + dx, cell.bottom);
        if (at.right < 0 || at.left > w_)
            continue;
        PaintTile(c, t, int(i), at);
        if (std::abs(dx) < 1)
            hits_.push_back({at, int(i)});
    }
}

void Launchpad::PaintTile(Canvas& c, const Theme& t, int index, D2D1_RECT_F cell)
{
    const CatalogApp& app = Shown()[size_t(index)];
    const float icon = grid_.iconDip, block = icon + kLabelGap + kLabelH;
    const float cx = (cell.left + cell.right) / 2, top = cell.top + (cell.bottom - cell.top - block) / 2;
    const D2D1_RECT_F ir = D2D1::RectF(cx - icon / 2, top, cx + icon / 2, top + icon);
    // Under the pointer or the keys: a soft plate behind the icon and its name.
    const float hv = Hover(c, index, hover_ == index || sel_ == index);
    if (hv > 0.001f) {
        const float half = std::min((cell.right - cell.left) / 2 - 4, icon * 0.5f + 22);
        const D2D1_RECT_F plate = D2D1::RectF(cx - half, top - 12, cx + half, top + block + 10);
        c.Fill(plate, 18, Alpha(sel_ == index && hover_ != index ? t.accent : t.hover, (sel_ == index && hover_ != index ? 0.28f : 1.f) * hv));
    }
    c.BeginGroup(1, pressed_ == index ? 0.92f : 1 + 0.03f * hv, cx, top + icon / 2);
    const int px = int(std::lround(icon * c.scale));
    const std::wstring key = L"lp|" + app.id + L"|" + std::to_wstring(px);
    visibleIcons_.emplace_back(app.id, px);
    if (c.Cached(key)) {
        c.Image(key, IconImage{}, ir);
    } else if (IconImage image = catalog_.Icon(app.id, px); !image.Empty()) {
        c.Image(key, image, ir);
    } else {
        c.Fill(ir, icon * 0.22f, Alpha(t.hover, 1.4f)); // still loading: a quiet square
    }
    c.EndGroup();
    c.Text(app.name, D2D1::RectF(cell.left + 6, ir.bottom + kLabelGap, cell.right - 6, ir.bottom + kLabelGap + kLabelH), kLabel, t.text,
           DWRITE_FONT_WEIGHT_NORMAL, 1);
}

void Launchpad::PaintDots(Canvas& c, const Theme& t)
{
    const int pages = Pages();
    if (pages <= 1)
        return;
    const float y = h_ - kDotsFromBottom, x0 = w_ / 2 - float(pages - 1) * kDotStep / 2;
    for (int p = 0; p < pages; ++p) {
        const float x = x0 + float(p) * kDotStep;
        const float lit = std::max(0.f, 1 - std::abs(pageShown_ - float(p))); // lit as its page comes
        c.Circle(x, y, kDot, Alpha(t.text, 0.28f + 0.62f * lit));
        hits_.push_back({D2D1::RectF(x - kDotStep / 2, y - 12, x + kDotStep / 2, y + 12), -2 - p});
    }
}

} // namespace atrium
