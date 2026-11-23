// Launchpad, as on macOS: every app, oldest installed first, on one window of the bar's glass (or that
// glass under a black veil: launchpadMaterial tint) in the middle of the display, in pages of a grid
// (launchpad_model.h) with the page dots below and a search field above. It opens from its entry next to
// the start logo: the glass fades in and the apps settle from a little larger, all at once when every
// icon is read (a spinner meanwhile; they are read ahead at start), and it leaves the same way. Typing searches (by name or initials, as the start menu does); the wheel, a
// drag or the arrows turn the pages with a spring; a click launches, a click on the empty glass or
// Esc closes (Esc clears a search first), and so does a click outside it.
#pragma once

#include "motion.h"
#include "shell/app_catalog.h"
#include "ui/launchpad_model.h"
#include "ui/surface.h"

#include <windows.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace atrium {

class Launchpad {
public:
    Launchpad(HINSTANCE instance, AppCatalog& catalog);

    // work: the display's work area (screen px), the window centred in it; previous: the window to
    // give the focus back to when closed without launching.
    void Open(RECT work, HWND previous);
    void SetRadius(float dip) { radius_ = dip; }
    void SetTint(bool tint) { tint_ = tint; }
    void Close(bool launched);
    bool Visible() const { return surface_.Visible(); }
    ULONGLONG LastClosed() const { return lastClosed_; }
    void Refresh(); // the catalog changed: the apps, an icon
    // Reads every app's icon ahead, at the size a window on `work` would show them.
    void Prewarm(RECT work);
    HWND Hwnd() const { return surface_.Hwnd(); }

private:
    static constexpr UINT_PTR kCloseTimer = 0x4C50, kCaretTimer = 0x4C51;
    static constexpr int kSearch = -1, kGlass = -1000; // what HitAt finds besides a tile (its index) and page dot n (-2 - n)
    struct Hit {
        D2D1_RECT_F rect;
        int index; // into Shown(), kSearch, or -2 - n for page dot n
    };

    // Painting (launchpad.cpp).
    void Paint(Canvas& c);
    void PaintSearch(Canvas& c, const Theme& t, float shown);
    void PaintPage(Canvas& c, const Theme& t, int page, float dx);
    void PaintTile(Canvas& c, const Theme& t, int index, D2D1_RECT_F cell);
    void PaintDots(Canvas& c, const Theme& t);
    void PaintSpinner(Canvas& c, const Theme& t, float shown);
    std::vector<std::wstring> Ids() const; // apps_' ids
    int IconPx(RECT work) const;
    void PublishGlass(double now);
    float Hover(Canvas& c, int id, bool on);
    D2D1_RECT_F Cell(int slot) const; // a slot of a page, its page at x 0
    D2D1_RECT_F SearchRect() const;
    float OpenAmount(double now) const; // 0 hidden .. 1 open, through the opening and the closing
    void Size(RECT work);               // w_, h_ and grid_ for a window on `work`
    std::vector<CatalogApp> LaunchableApps() const;
    float Radius() const;

    // Input (launchpad_input.cpp).
    bool Message(UINT m, WPARAM w, LPARAM l, LRESULT& result);
    bool Key(WPARAM key);
    void Type(wchar_t ch);
    void Press(POINT pt);
    void Drag(POINT pt);
    void Release(POINT pt);
    void Wheel(int delta);
    int HitAt(POINT pt) const;
    void SetPage(int page);
    void Launch(int index);
    void PlaceIme();

    void Finish();          // the closing has played: the window goes, the focus goes back
    void Filter();          // shown_ from apps_ and query_
    int Pages() const;
    const std::vector<CatalogApp>& Shown() const { return query_.empty() ? apps_ : shown_; }

    Surface surface_;
    AppCatalog& catalog_;
    std::vector<CatalogApp> apps_, shown_;
    std::wstring query_;
    LaunchpadGrid grid_;
    float w_ = 0, h_ = 0; // the window, DIP
    float radius_ = 24;   // the bar's popups' corner
    bool tint_ = false;   // launchpadMaterial tint: a black veil over the glass, light ink
    int iconPx_ = 0;      // the icons' size on the display it opened on
    bool loading_ = false;  // icons still being read: a spinner instead of the apps
    double revealedMs_ = 0; // when the apps began to settle in
    int page_ = 0;
    float pageShown_ = 0, pageSpeed_ = 0; // the page on screen (fractional while it glides), pages/s
    int sel_ = -1, hover_ = -1, pressed_ = -1;
    // A press: on a tile or the glass; dragged sideways past a few DIP, it turns the pages.
    bool down_ = false, dragging_ = false;
    POINT downPt_{};
    float dragFrom_ = 0;     // pageShown_ when the drag began
    float dragSpeed_ = 0;    // pages/s, of the last moves
    double dragMs_ = 0;
    float lastDragX_ = 0;
    int wheel_ = 0;          // wheel notches not yet turned into a page
    bool tracking_ = false;
    HWND previous_ = nullptr;
    double openedMs_ = 0, closedMs_ = 0, lastPaint_ = 0;
    bool closing_ = false, launched_ = false;
    ULONGLONG lastClosed_ = 0;
    std::pair<int, size_t> prewarmed_; // icon px and the apps' count last read ahead
    std::vector<Hit> hits_;
    std::vector<std::pair<std::wstring, int>> visibleIcons_, askedIcons_;
    std::map<int, motion::Follow> hoverAnim_;
};

} // namespace atrium
