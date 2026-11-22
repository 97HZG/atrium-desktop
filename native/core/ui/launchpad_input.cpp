// Launchpad's input: the pointer (a press launches, a sideways drag turns the pages, a press on the
// empty glass closes), the wheel, the keys and typing.
#include "ui/launchpad.h"

#include "clock.h"

#include <windowsx.h>
#include <imm.h>

#include <algorithm>
#include <cmath>

namespace atrium {

namespace {

constexpr float kDragStart = 8;                                 // DIP sideways before a press turns the pages
constexpr float kRubber = 0.35f;                                // past the first or last page, the sheet follows this much

} // namespace

bool Launchpad::Message(UINT m, WPARAM w, LPARAM l, LRESULT& result)
{
    const POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    switch (m) {
    case WM_TIMER:
        if (w == kCloseTimer)
            Finish();
        else if (w == kCaretTimer && !query_.empty())
            surface_.Invalidate(); // the caret blinks
        return w == kCloseTimer || w == kCaretTimer;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE)
            Close(false);
        return false;
    case WM_MOUSEMOVE: {
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, surface_.Hwnd(), 0};
            tracking_ = TrackMouseEvent(&tme) != FALSE;
        }
        if (down_) {
            Drag(pt);
            return true;
        }
        const int h = closing_ ? kGlass : HitAt(pt);
        const int tile = h >= 0 ? h : -1;
        if (tile != hover_) {
            hover_ = tile;
            surface_.Invalidate();
        }
        return true;
    }
    case WM_MOUSELEAVE:
        tracking_ = false;
        if (hover_ != -1) {
            hover_ = -1;
            surface_.Invalidate();
        }
        return true;
    case WM_LBUTTONDOWN:
        Press(pt);
        SetCapture(surface_.Hwnd());
        return true;
    case WM_LBUTTONUP:
        ReleaseCapture();
        Release(pt);
        return true;
    case WM_MOUSEWHEEL: Wheel(GET_WHEEL_DELTA_WPARAM(w)); return true;
    case WM_MOUSEHWHEEL: Wheel(-GET_WHEEL_DELTA_WPARAM(w)); return true;
    case WM_KEYDOWN: return Key(w);
    case WM_CHAR:
        Type(wchar_t(w));
        result = 0;
        return true;
    case WM_IME_STARTCOMPOSITION: PlaceIme(); return false;
    default: return false;
    }
}

bool Launchpad::Key(WPARAM key)
{
    if (closing_)
        return false;
    const int n = int(Shown().size());
    auto move = [&](int dx, int dy) {
        int page = page_;
        sel_ = MoveSelection(sel_, dx, dy, n, grid_.cols, grid_.rows, page);
        SetPage(page);
    };
    switch (key) {
    case VK_ESCAPE:
        if (query_.empty()) {
            Close(false);
        } else {
            query_.clear(); // a search goes first, then Launchpad
            Filter();
        }
        break;
    case VK_LEFT: move(-1, 0); break;
    case VK_RIGHT: move(1, 0); break;
    case VK_UP: move(0, -1); break;
    case VK_DOWN: move(0, 1); break;
    case VK_PRIOR: SetPage(page_ - 1); break;
    case VK_NEXT: SetPage(page_ + 1); break;
    case VK_HOME: SetPage(0); break;
    case VK_END: SetPage(Pages() - 1); break;
    case VK_RETURN: Launch(sel_); break;
    case VK_BACK:
        if (query_.empty())
            return true;
        query_.pop_back();
        Filter();
        break;
    default: return false;
    }
    surface_.Invalidate();
    return true;
}

// Typing goes to the search, wherever the pointer is (the IME's text arrives here too).
void Launchpad::Type(wchar_t ch)
{
    if (closing_ || ch < 32 || ch == 127 || query_.size() >= 64)
        return;
    query_ += ch;
    Filter();
    surface_.Invalidate();
}

void Launchpad::Press(POINT pt)
{
    if (closing_)
        return;
    down_ = true;
    dragging_ = false;
    downPt_ = pt;
    dragFrom_ = pageShown_;
    dragSpeed_ = 0;
    dragMs_ = ClockMs();
    lastDragX_ = float(pt.x) / surface_.Scale();
    pressed_ = HitAt(pt);
    surface_.Invalidate();
}

void Launchpad::Drag(POINT pt)
{
    const float s = surface_.Scale(), x = float(pt.x) / s, dx = float(pt.x - downPt_.x) / s;
    if (!dragging_ && std::abs(dx) < kDragStart)
        return;
    if (!dragging_) {
        dragging_ = true;
        pressed_ = -1; // a drag launches nothing and closes nothing
    }
    // The sheet follows the pointer; past the ends it gives only a little, as a rubber band.
    const float raw = dragFrom_ - dx / w_, end = float(Pages() - 1);
    const float held = std::clamp(raw, 0.f, end);
    pageShown_ = held + (raw - held) * kRubber;
    const double now = ClockMs();
    if (now > dragMs_) {
        const float speed = (x - lastDragX_) / w_ / float((now - dragMs_) / 1000.0);
        dragSpeed_ = dragSpeed_ * 0.6f + speed * 0.4f; // pointer pages/s, the last moves weighing most
    }
    dragMs_ = now;
    lastDragX_ = x;
    surface_.Invalidate();
}

void Launchpad::Release(POINT pt)
{
    if (!down_)
        return;
    down_ = false;
    const int was = pressed_;
    pressed_ = -1;
    if (dragging_) {
        dragging_ = false;
        const float moved = float(pt.x - downPt_.x) / surface_.Scale() / w_;
        page_ = SettlePageDrag(page_, Pages(), moved, ClockMs() - dragMs_ > 120 ? 0.f : dragSpeed_);
        pageSpeed_ = -dragSpeed_; // the glide goes on from the drag's pace
        surface_.Invalidate();
        return;
    }
    const int at = HitAt(pt);
    if (was >= 0 && at == was)
        Launch(was);
    else if (was <= -2 && was != kGlass && at == was)
        SetPage(-2 - was);
    else if (was == kGlass && at == kGlass)
        Close(false); // the empty glass
    surface_.Invalidate();
}

void Launchpad::Wheel(int delta)
{
    if (closing_)
        return;
    wheel_ += delta;
    // A notch is a page; precision touchpads add up to one.
    while (wheel_ >= WHEEL_DELTA) {
        wheel_ -= WHEEL_DELTA;
        SetPage(page_ - 1);
    }
    while (wheel_ <= -WHEEL_DELTA) {
        wheel_ += WHEEL_DELTA;
        SetPage(page_ + 1);
    }
}

// What is under a content point (px): a tile's index, kSearch, -2 - n for page dot n, or kGlass.
int Launchpad::HitAt(POINT pt) const
{
    const float s = surface_.Scale(), x = float(pt.x) / s, y = float(pt.y) / s;
    for (const auto& hit : hits_)
        if (x >= hit.rect.left && x < hit.rect.right && y >= hit.rect.top && y < hit.rect.bottom)
            return hit.index;
    return kGlass;
}

void Launchpad::SetPage(int page)
{
    page_ = std::clamp(page, 0, Pages() - 1);
    surface_.Invalidate();
}

void Launchpad::Launch(int index)
{
    if (index < 0 || size_t(index) >= Shown().size())
        return;
    const CatalogApp app = Shown()[size_t(index)];
    Close(true);
    AppCatalog::Launch(app);
}

// The IME's composition window under the search field's text.
void Launchpad::PlaceIme()
{
    const HWND hwnd = surface_.Hwnd();
    if (HIMC imc = ImmGetContext(hwnd)) {
        RECT win{};
        GetWindowRect(hwnd, &win);
        const RECT content = surface_.Rect();
        const D2D1_RECT_F field = SearchRect();
        const float s = surface_.Scale(), x = field.left + 36, y = field.bottom + 4;
        COMPOSITIONFORM cf{};
        cf.dwStyle = CFS_POINT;
        cf.ptCurrentPos = POINT{content.left - win.left + LONG(x * s), content.top - win.top + LONG(y * s)};
        ImmSetCompositionWindow(imc, &cf);
        ImmReleaseContext(hwnd, imc);
    }
}

} // namespace atrium
