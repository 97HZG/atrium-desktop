#include "ui/now_playing_card.h"

#include "bar_edge.h"
#include "clock.h"
#include "localization.h"

#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace atrium {

namespace {

// The card, DIP, long and low as the Dynamic Island playing music: the art and the lines beside
// it, the five bars at the far end; the progress bar between its two times; the three controls.
constexpr float kW = 400, kH = 152, kPad = 20;
constexpr float kArt = 52, kArtRadius = 13;
constexpr float kTextX = kPad + kArt + 13, kBarsW = 24, kBarsY = 36;
constexpr float kTrackY = 93, kTrackH = 5, kTrackHot = 8, kTimeW = 42;
constexpr float kTrackL = kPad + kTimeW + 6, kTrackR = kW - kPad - kTimeW - 6;
constexpr float kControlsY = 125, kControlStep = 62, kControlR = 18, kPlayR = 22;
constexpr double kArtFadeMs = 260;
constexpr UINT_PTR kTickTimer = 0x4E50;
constexpr UINT kTickMs = 250; // the progress bar's pace while nothing else animates

D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) { return D2D1::ColorF(c.r, c.g, c.b, c.a * a); }

D2D1_RECT_F ArtRect() { return D2D1::RectF(kPad, kPad, kPad + kArt, kPad + kArt); }
D2D1_RECT_F TrackRect() { return D2D1::RectF(kTrackL - 6, kTrackY - 11, kTrackR + 6, kTrackY + 11); }
D2D1_RECT_F SessionRect() { return D2D1::RectF(kW - kPad - 44, 55, kW - kPad, 71); }
D2D1_POINT_2F ControlCentre(int i) { return D2D1::Point2F(kW / 2 + float(i) * kControlStep, kControlsY); }

// "1:23", or "1:02:03" past an hour.
std::wstring Clock(int64_t us)
{
    const int64_t s = std::max<int64_t>(0, us / 1000000);
    wchar_t text[32];
    if (s >= 3600)
        swprintf_s(text, L"%lld:%02lld:%02lld", s / 3600, s / 60 % 60, s % 60);
    else
        swprintf_s(text, L"%lld:%02lld", s / 60, s % 60);
    return text;
}

// Rounds a premultiplied picture's corners: each pixel keeps the share of it inside the rounded
// square (`radius` a share of the side).
IconImage RoundCorners(const IconImage& src, float radius)
{
    IconImage out = src;
    const float w = float(src.width), h = float(src.height), r = radius * std::min(w, h);
    for (int y = 0; y < src.height; ++y)
        for (int x = 0; x < src.width; ++x) {
            const float px = float(x) + 0.5f, py = float(y) + 0.5f;
            const float dx = std::max({r - px, 0.f, px - (w - r)}), dy = std::max({r - py, 0.f, py - (h - r)});
            const float cover = std::clamp(r + 0.5f - std::sqrt(dx * dx + dy * dy), 0.f, 1.f);
            if (cover >= 1)
                continue;
            uint8_t* p = &out.bgra[(size_t(y) * size_t(src.width) + size_t(x)) * 4];
            for (int c = 0; c < 4; ++c)
                p[c] = uint8_t(std::lround(float(p[c]) * cover));
        }
    return out;
}

// The picture shrunk to n x n by averaging: stretched across the card it is a soft field of its
// colours.
IconImage Wash(const IconImage& src, int n)
{
    IconImage out{n, n, std::vector<uint8_t>(size_t(n * n * 4))};
    for (int gy = 0; gy < n; ++gy)
        for (int gx = 0; gx < n; ++gx) {
            std::array<uint32_t, 4> sum{};
            uint32_t count = 0;
            for (int y = gy * src.height / n; y < (gy + 1) * src.height / n; ++y)
                for (int x = gx * src.width / n; x < (gx + 1) * src.width / n; ++x, ++count)
                    for (size_t c = 0; c < 4; ++c)
                        sum[c] += src.bgra[(size_t(y) * size_t(src.width) + size_t(x)) * 4 + c];
            for (size_t c = 0; c < 4; ++c)
                out.bgra[(size_t(gy) * size_t(n) + size_t(gx)) * 4 + c] = uint8_t(count ? sum[c] / count : 0);
        }
    return out;
}

// The picture's colour: its pixels weighted by how vivid they are, then lifted to read on glass.
// None for a grey picture.
std::optional<D2D1_COLOR_F> Tint(const IconImage& src)
{
    double r = 0, g = 0, b = 0, weight = 0;
    for (size_t i = 0; i + 3 < src.bgra.size(); i += 16) { // every fourth pixel is plenty
        const float a = float(src.bgra[i + 3]) / 255.f;
        if (a < 0.5f)
            continue;
        const float pb = float(src.bgra[i]) / 255.f / a, pg = float(src.bgra[i + 1]) / 255.f / a, pr = float(src.bgra[i + 2]) / 255.f / a;
        const float hi = std::max({pr, pg, pb}), lo = std::min({pr, pg, pb});
        const float vivid = (hi - lo) * hi; // saturated and bright counts most
        r += pr * vivid;
        g += pg * vivid;
        b += pb * vivid;
        weight += vivid;
    }
    if (weight < 1e-3 * (double(src.bgra.size()) / 16 + 1))
        return std::nullopt;
    const float k = float(weight), top = float(std::max({r, g, b}) / weight);
    const float lift = 0.95f / std::max(top, 0.05f);
    return D2D1::ColorF(std::min(1.f, float(r) / k * lift), std::min(1.f, float(g) / k * lift), std::min(1.f, float(b) / k * lift), 1.f);
}

} // namespace

NowPlayingCard::NowPlayingCard(HINSTANCE instance) : surface_(instance, L"Atrium Now Playing", true)
{
    surface_.paint = [this](Canvas& c) { Paint(c); };
    surface_.message = [this](UINT m, WPARAM w, LPARAM l, LRESULT& r) { return Message(m, w, l, r); };
    surface_.onDismiss = [this] { Close(); };
}

void NowPlayingCard::Open(RECT entry, RECT bar)
{
    entry_ = entry;
    bar_ = bar;
    hover_ = pressed_ = Part::None;
    dragging_ = false;
    hoverAnim_.clear();
    Place(true);
    SetTimer(surface_.Hwnd(), kTickTimer, kTickMs, nullptr);
}

void NowPlayingCard::Close()
{
    if (const HWND h = surface_.Hwnd())
        KillTimer(h, kTickTimer);
    dragging_ = false;
    if (!surface_.Visible())
        return;
    surface_.Hide();
    lastClosed_ = GetTickCount64();
}

// Centred on the entry along the bar, kPopupGapDip off its glass, inside the work area.
void NowPlayingCard::Place(bool show)
{
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromRect(&bar_, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& work = mi.rcWork;
    const float s = PopupScale(bar_);
    const LONG w = std::lround(kW * s), h = std::lround(kH * s), gap = std::lround(kPopupGapDip * s);
    const LONG ex = (entry_.left + entry_.right) / 2, ey = (entry_.top + entry_.bottom) / 2;
    const BarEdge edge = EdgeOf(bar_);
    RECT r{};
    r.left = edge == BarEdge::Left ? bar_.right + gap : edge == BarEdge::Right ? bar_.left - gap - w : ex - w / 2;
    r.top = Vertical(edge) ? ey - h / 2 : edge == BarEdge::Top ? bar_.bottom + gap : bar_.top - gap - h;
    r.left = std::clamp(r.left, work.left + gap, std::max(work.left + gap, work.right - gap - w));
    r.top = std::clamp(r.top, work.top + gap, std::max(work.top + gap, work.bottom - gap - h));
    r.right = r.left + w;
    r.bottom = r.top + h;
    if (show)
        surface_.Show(r, true, POINT{ex, ey});
    else
        surface_.Move(r);
}

void NowPlayingCard::Update(const MediaSnapshot& snapshot)
{
    const double now = ClockMs();
    media_ = snapshot;
    // The session agrees with the press (or never answered): its state shows again.
    if (playingShown_ && (*playingShown_ == (media_.state == MediaPlaybackState::Playing) || now - playingAt_ > 1500))
        playingShown_.reset();
    if (seekTo_ && (std::llabs(media_.timeline.PositionUs(now) - *seekTo_) < 1500000 || now - seekAt_ > 2500))
        seekTo_.reset();
    if (media_.artworkVersion != artVersion_)
        TakeArtwork();
    if (!media_.active)
        Close();
    else
        surface_.Invalidate();
}

void NowPlayingCard::TakeArtwork()
{
    artVersion_ = media_.artworkVersion;
    artMs_ = ClockMs();
    const IconImage* art = media_.artwork.get();
    if (!art || art->Empty() || art->bgra.size() < size_t(art->width) * size_t(art->height) * 4) {
        art_ = wash_ = IconImage{};
        tint_.reset();
        return;
    }
    art_ = RoundCorners(*art, kArtRadius / kArt);
    wash_ = Wash(*art, 6);
    tint_ = Tint(*art);
}

bool NowPlayingCard::Playing() const { return playingShown_ ? *playingShown_ : media_.state == MediaPlaybackState::Playing; }

int64_t NowPlayingCard::ShownPositionUs(double now) const
{
    const MediaTimeline& t = media_.timeline;
    if (dragging_)
        return t.startUs + int64_t(double(t.endUs - t.startUs) * dragFraction_);
    if (seekTo_)
        return *seekTo_;
    return t.PositionUs(now);
}

float NowPlayingCard::TrackFraction(float x) const { return std::clamp((x - kTrackL) / (kTrackR - kTrackL), 0.f, 1.f); }

void NowPlayingCard::Send(MediaCommand command, int64_t positionUs)
{
    if (onCommand)
        onCommand(command, positionUs);
}

float NowPlayingCard::Hover(Canvas& c, Part part, bool on)
{
    auto& f = hoverAnim_[part];
    const float v = f.Step(on, c.now);
    if (f.moving)
        c.Continue();
    return v;
}

void NowPlayingCard::Paint(Canvas& c)
{
    const Theme& t = c.theme;
    const D2D1_RECT_F card = D2D1::RectF(0, 0, kW, kH);
    if (c.panel)
        c.Fill(card, radius_, t.panel);
    // The art's colours, soft and faint, across the glass; cut to the card's shape.
    const float artIn = motion::EaseOut(float((c.now - artMs_) / kArtFadeMs));
    if (artIn < 1)
        c.Continue();
    if (!wash_.Empty()) {
        c.Image(L"np|wash|" + std::to_wstring(artVersion_), wash_, D2D1::RectF(-kW * 0.1f, -kH * 0.2f, kW * 1.1f, kH * 1.2f), 0.20f * artIn);
        c.MaskRounded(card, radius_);
    }
    PaintHeader(c, t);
    PaintProgress(c, t);
    PaintControls(c, t);
}

void NowPlayingCard::PaintHeader(Canvas& c, const Theme& t)
{
    const float artIn = motion::EaseOut(float((c.now - artMs_) / kArtFadeMs));
    const D2D1_RECT_F art = ArtRect();
    if (!art_.Empty()) {
        c.Image(L"np|art|" + std::to_wstring(artVersion_), art_, art, artIn);
    } else {
        c.Fill(art, kArtRadius, Alpha(t.hover, 1.6f));
        c.Glyph(L"", art, 26, t.subtext); // MusicNote
    }
    const float right = kW - kPad - kBarsW - 10;
    const std::wstring title = media_.title.empty() ? std::wstring(Tr(L"Not playing")) : media_.title;
    const bool several = media_.sessionCount > 1;
    c.Text(title, D2D1::RectF(kTextX, 18, right, 38), 15, t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    c.Text(media_.artist, D2D1::RectF(kTextX, 38, several ? SessionRect().left - 6 : right, 55), 12.5f, t.subtext);
    c.Text(media_.appName, D2D1::RectF(kTextX, 55, several ? SessionRect().left - 6 : right, 71), 11, t.faint);
    if (several) {
        const D2D1_RECT_F s = SessionRect();
        const float hv = Hover(c, Part::Session, hover_ == Part::Session);
        c.Fill(s, (s.bottom - s.top) / 2, Alpha(t.hover, 0.5f + 0.5f * hv));
        c.Text(std::to_wstring(media_.sessionIndex + 1) + L"/" + std::to_wstring(media_.sessionCount) + L" ›", s, 11, t.text, DWRITE_FONT_WEIGHT_NORMAL, 1);
    }
    // The five bars, live, in the art's colour.
    const double dt = lastBarsMs_ == 0 ? 16 : c.now - lastBarsMs_;
    lastBarsMs_ = c.now;
    const float peak = feed_ ? feed_->peak.load() : 0.f, intensity = feed_ ? feed_->intensity.load() : 1.f;
    if (bars_.Step(peak, Playing(), intensity, dt))
        c.Continue();
    const float bw = kBarsW / 9, gap = (kBarsW - 5 * bw) / 4, cy = kBarsY;
    const D2D1_COLOR_F ink = tint_ ? *tint_ : t.text;
    for (size_t i = 0; i < MusicBars::kCount; ++i) {
        const float x = kW - kPad - kBarsW + float(i) * (bw + gap), h = std::max(bw, kBarsW * bars_.Height(i));
        c.Fill(D2D1::RectF(x, cy - h / 2, x + bw, cy + h / 2), bw / 2, ink);
    }
}

void NowPlayingCard::PaintProgress(Canvas& c, const Theme& t)
{
    const MediaTimeline& tl = media_.timeline;
    const float hv = Hover(c, Part::Track, hover_ == Part::Track || dragging_);
    if (!tl.Seekable())
        return; // no length known (a stream, or a player that tells none): no track at all
    const float th = kTrackH + (kTrackHot - kTrackH) * hv;
    const D2D1_RECT_F track = D2D1::RectF(kTrackL, kTrackY - th / 2, kTrackR, kTrackY + th / 2);
    c.Fill(track, th / 2, Alpha(t.text, 0.16f));
    const int64_t pos = ShownPositionUs(c.now), length = tl.endUs - tl.startUs;
    const float done = std::clamp(float(double(pos - tl.startUs) / double(length)), 0.f, 1.f);
    if (done > 0)
        c.Fill(D2D1::RectF(track.left, track.top, std::max(track.left + th, track.left + (track.right - track.left) * done), track.bottom), th / 2,
               tint_ && hv > 0.5f ? *tint_ : t.text);
    // The times either side of the bar, as the Dynamic Island has them: the elapsed one right-aligned
    // against its start, the one left against its end.
    c.Text(Clock(pos - tl.startUs), D2D1::RectF(kPad - 4, kTrackY - 9, kTrackL - 8, kTrackY + 9), 11, t.subtext, DWRITE_FONT_WEIGHT_NORMAL, 2);
    c.Text(L"-" + Clock(tl.endUs - pos), D2D1::RectF(kTrackR + 8, kTrackY - 9, kW - kPad + 4, kTrackY + 9), 11, t.subtext);
}

void NowPlayingCard::PaintControls(Canvas& c, const Theme& t)
{
    struct Control {
        Part part;
        int at;
        float r;
        bool enabled;
    };
    const std::array<Control, 3> controls{
        {{Part::Previous, -1, kControlR, media_.canPrevious}, {Part::Play, 0, kPlayR, media_.canPlayPause}, {Part::Next, 1, kControlR, media_.canNext}}};
    for (const auto& k : controls) {
        const D2D1_POINT_2F o = ControlCentre(k.at);
        const float hv = k.enabled ? Hover(c, k.part, hover_ == k.part) : 0.f;
        if (hv > 0.001f)
            c.Circle(o.x, o.y, k.r, Alpha(pressed_ == k.part ? t.press : t.hover, hv));
        c.BeginGroup(k.enabled ? 1.f : 0.35f, pressed_ == k.part ? 0.86f : 1.f, o.x, o.y);
        const D2D1_COLOR_F ink = t.text;
        if (k.part == Part::Play && Playing()) {
            c.Fill(D2D1::RectF(o.x - 9, o.y - 10, o.x - 3, o.y + 10), 2, ink);
            c.Fill(D2D1::RectF(o.x + 3, o.y - 10, o.x + 9, o.y + 10), 2, ink);
        } else if (k.part == Part::Play) {
            const std::array<D2D1_POINT_2F, 3> play{{{o.x - 5.5f, o.y - 9}, {o.x + 9.5f, o.y}, {o.x - 5.5f, o.y + 9}}};
            c.Polygon(play, ink, 2);
        } else {
            // Two triangles pointing the way the track goes.
            const float d = float(k.at);
            for (float base : {-11.f, 0.f}) {
                const std::array<D2D1_POINT_2F, 3> tri{{{o.x + d * base, o.y - 7}, {o.x + d * (base + 10), o.y}, {o.x + d * base, o.y + 7}}};
                c.Polygon(tri, ink, 1.5f);
            }
        }
        c.EndGroup();
    }
}

NowPlayingCard::Part NowPlayingCard::HitAt(POINT pt) const
{
    const float s = surface_.Scale(), x = float(pt.x) / s, y = float(pt.y) / s;
    auto in = [&](const D2D1_RECT_F& r) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; };
    if (media_.timeline.Seekable() && media_.canSeek && in(TrackRect()))
        return Part::Track;
    if (media_.sessionCount > 1 && in(SessionRect()))
        return Part::Session;
    if (in(ArtRect()))
        return Part::Art;
    const std::array<std::pair<Part, int>, 3> controls{{{Part::Previous, -1}, {Part::Play, 0}, {Part::Next, 1}}};
    for (const auto& [part, at] : controls) {
        const D2D1_POINT_2F o = ControlCentre(at);
        if (std::hypot(x - o.x, y - o.y) <= kPlayR + 4)
            return part;
    }
    return Part::None;
}

bool NowPlayingCard::Message(UINT m, WPARAM w, LPARAM l, LRESULT&)
{
    const POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    const float x = float(pt.x) / surface_.Scale();
    switch (m) {
    case WM_TIMER:
        if (w != kTickTimer)
            return false;
        if (Playing())
            surface_.Invalidate(); // the progress bar and the time move on
        return true;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE)
            Close();
        return false;
    case WM_MOUSEMOVE: {
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, surface_.Hwnd(), 0};
            tracking_ = TrackMouseEvent(&tme) != FALSE;
        }
        if (dragging_)
            dragFraction_ = TrackFraction(x);
        hover_ = dragging_ ? Part::Track : HitAt(pt);
        surface_.Invalidate();
        return true;
    }
    case WM_MOUSELEAVE:
        tracking_ = false;
        if (!dragging_)
            hover_ = pressed_ = Part::None;
        surface_.Invalidate();
        return true;
    case WM_LBUTTONDOWN:
        pressed_ = HitAt(pt);
        if (pressed_ == Part::Track) {
            dragging_ = true;
            dragFraction_ = TrackFraction(x);
            SetCapture(surface_.Hwnd());
        }
        surface_.Invalidate();
        return true;
    case WM_LBUTTONUP: {
        const Part was = pressed_;
        const Part at = HitAt(pt);
        pressed_ = Part::None;
        if (dragging_) {
            dragging_ = false;
            ReleaseCapture();
            const MediaTimeline& tl = media_.timeline;
            seekTo_ = tl.startUs + int64_t(double(tl.endUs - tl.startUs) * TrackFraction(x));
            seekAt_ = ClockMs();
            Send(MediaCommand::Seek, *seekTo_ - tl.startUs);
        } else if (was == at && at == Part::Play && media_.canPlayPause) {
            playingShown_ = !Playing();
            playingAt_ = ClockMs();
            Send(MediaCommand::PlayPause);
        } else if (was == at && at == Part::Previous && media_.canPrevious) {
            Send(MediaCommand::Previous);
        } else if (was == at && at == Part::Next && media_.canNext) {
            Send(MediaCommand::Next);
        } else if (was == at && at == Part::Session) {
            Send(MediaCommand::NextSession);
        } else if (was == at && at == Part::Art) {
            Send(MediaCommand::ActivateApp);
            Close();
        }
        surface_.Invalidate();
        return true;
    }
    case WM_KEYDOWN:
        if (w == VK_ESCAPE)
            Close();
        else if (w == VK_SPACE && media_.canPlayPause)
            Send(MediaCommand::PlayPause);
        else
            return false;
        return true;
    default: return false;
    }
}

} // namespace atrium
