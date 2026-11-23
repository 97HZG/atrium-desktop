#include "bar_extras_painter.h"

#include "continuous_corners.h"

#include <algorithm>
#include <cmath>

namespace atrium {

namespace {

// The square centred in r, `k` of its shorter side.
D2D1_RECT_F Square(D2D1_RECT_F r, float k)
{
    const float w = std::min(r.right - r.left, r.bottom - r.top) * k, cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    return D2D1::RectF(cx - w / 2, cy - w / 2, cx + w / 2, cy + w / 2);
}

} // namespace

bool BarExtrasPainter::Draw(const std::wstring& key, ID2D1DeviceContext2* d2d, D2D1_RECT_F r, ID2D1SolidColorBrush* brush, D2D1_COLOR_F ink, float opacity,
                            const MusicBars& bars)
{
    D2D1_COLOR_F faded = ink;
    faded.a *= opacity;
    if (key == L"#start")
        DrawLogo(d2d, r, opacity);
    else if (key == L"#launchpad")
        DrawLaunchpad(d2d, r, brush, faded);
    else if (key == L"#music")
        DrawMusic(d2d, r, bars, brush, faded);
    else if (key == L"#stacks")
        DrawStacks(d2d, r, brush, faded);
    else
        return false;
    return true;
}

void BarExtrasPainter::DrawLogo(ID2D1DeviceContext2* d2d, D2D1_RECT_F r, float opacity)
{
    const D2D1_RECT_F box = Square(r, 0.80f);
    const float w = box.right - box.left, left = box.left, top = box.top;
    const float gap = w * 0.08f, pane = (w - gap) / 2, corner = pane * 0.10f;
    if (!logoBlue_) {
        D2D1_GRADIENT_STOP stops[] = {{0.f, D2D1::ColorF(0x5CCBFF)}, {0.5f, D2D1::ColorF(0x1A9BF0)}, {1.f, D2D1::ColorF(0x0060C8)}};
        winrt::com_ptr<ID2D1GradientStopCollection> coll;
        if (FAILED(d2d->CreateGradientStopCollection(stops, 3, coll.put())) ||
            FAILED(d2d->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({}, {}), coll.get(), logoBlue_.put())) ||
            FAILED(d2d->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.16f), logoSheen_.put()))) {
            logoBlue_ = nullptr;
            return;
        }
    }
    ID2D1LinearGradientBrush* grad = logoBlue_.get();
    grad->SetStartPoint(D2D1::Point2F(left, top));
    grad->SetEndPoint(D2D1::Point2F(left + w, top + w));
    grad->SetOpacity(opacity);
    ID2D1SolidColorBrush* sheen = logoSheen_.get();
    sheen->SetOpacity(opacity);
    for (int i = 0; i < 4; ++i) {
        const float x = left + float(i % 2) * (pane + gap), y = top + float(i >> 1) * (pane + gap);
        FillContinuousRect(d2d, D2D1::RectF(x, y, x + pane, y + pane), corner, grad);
        // A faint lit edge on top of each pane.
        d2d->FillRectangle(D2D1::RectF(x + corner, y, x + pane - corner, y + std::max(0.6f, pane * 0.06f)), sheen);
    }
}

void BarExtrasPainter::DrawLaunchpad(ID2D1DeviceContext2* d2d, D2D1_RECT_F r, ID2D1SolidColorBrush* brush, D2D1_COLOR_F ink)
{
    // Nine small apps, three by three, in outline: squares of 0.26 with gaps of 0.11.
    const D2D1_RECT_F box = Square(r, 0.86f);
    const float s = box.right - box.left, tile = s * 0.26f, step = s * 0.37f, stroke = std::max(1.f, s * 0.065f);
    const float corner = tile * 0.3f - stroke / 2;
    brush->SetColor(ink);
    for (int i = 0; i < 9; ++i) {
        const float x = box.left + float(i % 3) * step + stroke / 2, y = box.top + float(i / 3) * step + stroke / 2;
        d2d->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + tile - stroke, y + tile - stroke), corner, corner), brush, stroke);
    }
}

void BarExtrasPainter::DrawMusic(ID2D1DeviceContext2* d2d, D2D1_RECT_F r, const MusicBars& bars, ID2D1SolidColorBrush* brush, D2D1_COLOR_F ink)
{
    const D2D1_RECT_F box = Square(r, 0.80f);
    const float side = box.right - box.left, bar = side * 0.11f, gap = side * 0.0925f;
    const float cy = (box.top + box.bottom) / 2;
    float x = (box.left + box.right) / 2 - (float(MusicBars::kCount) * bar + float(MusicBars::kCount - 1) * gap) / 2;
    brush->SetColor(ink);
    for (size_t i = 0; i < MusicBars::kCount; ++i, x += bar + gap) {
        const float h = std::max(bar, side * bars.Height(i));
        d2d->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, cy - h / 2, x + bar, cy + h / 2), bar / 2, bar / 2), brush);
    }
}

void BarExtrasPainter::DrawStacks(ID2D1DeviceContext2* d2d, D2D1_RECT_F r, ID2D1SolidColorBrush* brush, D2D1_COLOR_F ink)
{
    winrt::com_ptr<ID2D1Factory> factory;
    d2d->GetFactory(factory.put());
    const D2D1_RECT_F box = Square(r, 0.74f);
    const float s = box.right - box.left, stroke = std::max(1.f, s * 0.075f);
    auto at = [&](float u, float v) { return D2D1::Point2F(box.left + u * s, box.top + v * s); };
    // The front folder: a body with a tab rising at its top left, rounded at every corner.
    winrt::com_ptr<ID2D1PathGeometry> folder;
    winrt::com_ptr<ID2D1GeometrySink> sink;
    if (FAILED(factory->CreatePathGeometry(folder.put())) || FAILED(folder->Open(sink.put())))
        return;
    const float rr = 0.07f;
    sink->BeginFigure(at(0.f, 0.34f + rr), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(at(0.f, 1.f - rr));
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(at(0.f, 1.f), at(rr, 1.f)));
    sink->AddLine(at(0.86f - rr, 1.f));
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(at(0.86f, 1.f), at(0.86f, 1.f - rr)));
    sink->AddLine(at(0.86f, 0.42f + rr));
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(at(0.86f, 0.42f), at(0.86f - rr, 0.42f)));
    sink->AddLine(at(0.43f, 0.42f));
    sink->AddLine(at(0.34f, 0.26f));
    sink->AddLine(at(rr, 0.26f));
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(at(0.f, 0.26f), at(0.f, 0.26f + rr)));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close()))
        return;
    // The folder behind: only its top and right edges show, up and to the right of the front one.
    winrt::com_ptr<ID2D1PathGeometry> behind;
    sink = nullptr;
    if (FAILED(factory->CreatePathGeometry(behind.put())) || FAILED(behind->Open(sink.put())))
        return;
    sink->BeginFigure(at(0.30f, 0.12f), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine(at(1.f - rr, 0.12f));
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(at(1.f, 0.12f), at(1.f, 0.12f + rr)));
    sink->AddLine(at(1.f, 0.84f));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close()))
        return;
    winrt::com_ptr<ID2D1StrokeStyle> round;
    const auto props = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    if (FAILED(factory->CreateStrokeStyle(props, nullptr, 0, round.put())))
        return;
    brush->SetColor(ink);
    d2d->DrawGeometry(folder.get(), brush, stroke, round.get());
    D2D1_COLOR_F back = ink;
    back.a *= 0.55f;
    brush->SetColor(back);
    d2d->DrawGeometry(behind.get(), brush, stroke, round.get());
}

} // namespace atrium
