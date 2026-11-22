#include "ime_glyph.h"

#include "ime_glyph_data.h"

#include <winrt/base.h>

#include <map>

namespace atrium {

namespace {

// The outline of `g` in em units, as a path of the drawing thread's factory.
ID2D1PathGeometry* Outline(ID2D1DeviceContext* d2d, const ime_glyph::Glyph& g)
{
    winrt::com_ptr<ID2D1Factory> factory;
    d2d->GetFactory(factory.put());
    // The paths per drawing thread; replacing the factory drops the old ones.
    struct Paths {
        winrt::com_ptr<ID2D1Factory> factory;
        std::map<wchar_t, winrt::com_ptr<ID2D1PathGeometry>> byLetter;
    };
    thread_local Paths paths;
    if (paths.factory != factory)
        paths = Paths{factory, {}};
    auto& path = paths.byLetter[g.ch];
    if (path)
        return path.get();
    winrt::com_ptr<ID2D1GeometrySink> sink;
    if (FAILED(factory->CreatePathGeometry(path.put())) || FAILED(path->Open(sink.put()))) {
        path = nullptr;
        return nullptr;
    }
    sink->SetFillMode(D2D1_FILL_MODE_WINDING); // as fonts fill
    bool open = false;
    for (const ime_glyph::Cmd& c : g.cmds) {
        switch (c.op) {
        case ime_glyph::Op::Move:
            if (open)
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->BeginFigure(D2D1::Point2F(c.x1, c.y1), D2D1_FIGURE_BEGIN_FILLED);
            open = true;
            break;
        case ime_glyph::Op::Line: sink->AddLine(D2D1::Point2F(c.x1, c.y1)); break;
        case ime_glyph::Op::Cubic: sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(c.x1, c.y1), D2D1::Point2F(c.x2, c.y2), D2D1::Point2F(c.x3, c.y3))); break;
        case ime_glyph::Op::Close:
            if (open)
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            open = false;
            break;
        }
    }
    if (open)
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close()))
        path = nullptr;
    return path.get();
}

} // namespace

bool HasImeOutline(const std::wstring& text) { return ime_glyph::Find(text) != nullptr; }

void DrawImeGlyph(ID2D1DeviceContext* d2d, IDWriteFactory* dwrite, IDWriteTextFormat* format, const std::wstring& text, D2D1_RECT_F area,
                  ID2D1SolidColorBrush* brush, D2D1_COLOR_F color, float alpha)
{
    if (text.empty() || alpha <= 0.003f || !format)
        return;
    color.a *= alpha;
    brush->SetColor(color);
    const float w = area.right - area.left, h = area.bottom - area.top;
    if (const ime_glyph::Glyph* g = ime_glyph::Find(text)) {
        ID2D1PathGeometry* path = Outline(d2d, *g);
        if (!path)
            return;
        // Em units at the format's size, the ink's centre on the area's.
        const float em = format->GetFontSize();
        D2D1_MATRIX_3X2_F previous;
        d2d->GetTransform(&previous);
        d2d->SetTransform(D2D1::Matrix3x2F::Scale(em, em) * D2D1::Matrix3x2F::Translation(area.left + w / 2, area.top + h / 2) * previous);
        d2d->FillGeometry(path, brush);
        d2d->SetTransform(previous);
        return;
    }
    winrt::com_ptr<IDWriteTextLayout> layout;
    if (FAILED(dwrite->CreateTextLayout(text.c_str(), UINT32(text.size()), format, w, h, layout.put())))
        return;
    // Centred by its ink, as the tray's icons are.
    DWRITE_OVERHANG_METRICS over{};
    layout->GetOverhangMetrics(&over);
    d2d->DrawTextLayout(D2D1::Point2F(area.left, area.top + (over.top - over.bottom) / 2), layout.get(), brush);
}

} // namespace atrium
