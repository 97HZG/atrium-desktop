// The input method entry's letter: 中, あ, 가, A and a drawn from outlines built into Atrium
// (ime_glyph_data.h, so no font file ships for them); any other letter (LanguageGlyph's) as text
// in the interface face. Either way centred on its ink, so a lowercase a sits in the middle too.
#pragma once

#include <d2d1_1.h>
#include <dwrite.h>

#include <string>

namespace atrium {

// Whether `text` has a built-in outline (no text layout needed).
bool HasImeOutline(const std::wstring& text);

// `text` centred on `area` at `format`'s size, in `color` faded to `alpha`. Sets `brush`'s colour.
void DrawImeGlyph(ID2D1DeviceContext* d2d, IDWriteFactory* dwrite, IDWriteTextFormat* format, const std::wstring& text, D2D1_RECT_F area,
                  ID2D1SolidColorBrush* brush, D2D1_COLOR_F color, float alpha);

} // namespace atrium
