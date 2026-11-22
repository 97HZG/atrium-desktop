// The input method entry's letters as outlines (tools/generate-ime-glyphs.py; do not edit):
// Noto Sans CJK SC Light Version 2.004, SIL Open Font License 1.1. Em units, centred on the ink, y down.
#pragma once

#include <span>
#include <string_view>

namespace atrium::ime_glyph {

enum class Op : unsigned char { Move, Line, Cubic, Close };

// A step of an outline: Move and Line use (x1, y1); Cubic its two controls and its end.
struct Cmd {
    Op op;
    float x1, y1, x2, y2, x3, y3;
};

struct Glyph {
    wchar_t ch;
    std::span<const Cmd> cmds;
};

// clang-format off: one step a line, as generated
// NOLINTBEGIN(modernize-use-std-numbers): outline coordinates, some of which merely resemble constants
inline constexpr Cmd kZhong[] = {
    {Op::Move, -0.026f, -0.4535f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, -0.026f, -0.2715f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, -0.397f, -0.2715f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, -0.397f, 0.1855f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, -0.349f, 0.1855f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, -0.349f, 0.1195f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, -0.026f, 0.1195f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, -0.026f, 0.4535f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.024f, 0.4535f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.024f, 0.1195f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.348f, 0.1195f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.348f, 0.1805f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.397f, 0.1805f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.397f, -0.2715f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.024f, -0.2715f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.024f, -0.4535f, 0.f, 0.f, 0.f, 0.f},
    {Op::Close, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f},
    {Op::Move, -0.349f, 0.0725f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, -0.349f, -0.2245f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, -0.026f, -0.2245f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, -0.026f, 0.0725f, 0.f, 0.f, 0.f, 0.f},
    {Op::Close, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f},
    {Op::Move, 0.348f, 0.0725f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.024f, 0.0725f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.024f, -0.2245f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.348f, -0.2245f, 0.f, 0.f, 0.f, 0.f},
    {Op::Close, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f},
};

inline constexpr Cmd kA[] = {
    {Op::Move, 0.123f, -0.083f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, 0.076f, 0.045f, 0.005f, 0.138f, -0.071f, 0.209f},
    {Op::Cubic, -0.084f, 0.146f, -0.093f, 0.08f, -0.093f, 0.017f},
    {Op::Line, -0.092f, -0.047f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, -0.042f, -0.066f, 0.023f, -0.084f, 0.091f, -0.084f},
    {Op::Close, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f},
    {Op::Move, 0.207f, -0.181f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.153f, -0.195f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, 0.152f, -0.181f, 0.148f, -0.165f, 0.145f, -0.153f},
    {Op::Line, 0.136f, -0.123f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, 0.121f, -0.124f, 0.105f, -0.125f, 0.089f, -0.125f},
    {Op::Cubic, 0.038f, -0.125f, -0.03f, -0.114f, -0.09f, -0.094f},
    {Op::Cubic, -0.087f, -0.144f, -0.083f, -0.193f, -0.079f, -0.238f},
    {Op::Cubic, 0.043f, -0.244f, 0.175f, -0.257f, 0.285f, -0.276f},
    {Op::Line, 0.284f, -0.326f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, 0.186f, -0.305f, 0.056f, -0.29f, -0.073f, -0.284f},
    {Op::Cubic, -0.069f, -0.32f, -0.064f, -0.35f, -0.06f, -0.373f},
    {Op::Cubic, -0.058f, -0.386f, -0.054f, -0.404f, -0.05f, -0.412f},
    {Op::Line, -0.106f, -0.414f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, -0.106f, -0.405f, -0.107f, -0.389f, -0.108f, -0.373f},
    {Op::Line, -0.118f, -0.282f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, -0.201f, -0.281f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, -0.233f, -0.281f, -0.32f, -0.286f, -0.355f, -0.293f},
    {Op::Line, -0.353f, -0.242f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, -0.316f, -0.239f, -0.238f, -0.235f, -0.201f, -0.235f},
    {Op::Line, -0.123f, -0.236f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, -0.128f, -0.187f, -0.132f, -0.131f, -0.135f, -0.076f},
    {Op::Cubic, -0.271f, -0.014f, -0.387f, 0.116f, -0.387f, 0.245f},
    {Op::Cubic, -0.387f, 0.32f, -0.341f, 0.358f, -0.279f, 0.358f},
    {Op::Cubic, -0.225f, 0.358f, -0.16f, 0.333f, -0.102f, 0.293f},
    {Op::Cubic, -0.096f, 0.317f, -0.089f, 0.34f, -0.083f, 0.36f},
    {Op::Line, -0.035f, 0.345f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, -0.044f, 0.319f, -0.052f, 0.291f, -0.059f, 0.261f},
    {Op::Cubic, 0.03f, 0.184f, 0.111f, 0.07f, 0.168f, -0.076f},
    {Op::Cubic, 0.277f, -0.051f, 0.337f, 0.027f, 0.337f, 0.113f},
    {Op::Cubic, 0.337f, 0.261f, 0.205f, 0.351f, 0.032f, 0.37f},
    {Op::Line, 0.061f, 0.414f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, 0.285f, 0.377f, 0.387f, 0.26f, 0.387f, 0.116f},
    {Op::Cubic, 0.387f, 0.009f, 0.314f, -0.085f, 0.183f, -0.115f},
    {Op::Line, 0.192f, -0.141f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, 0.197f, -0.154f, 0.202f, -0.17f, 0.207f, -0.181f},
    {Op::Close, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f},
    {Op::Move, -0.137f, -0.027f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, -0.138f, 0.018f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, -0.138f, 0.09f, -0.127f, 0.171f, -0.113f, 0.243f},
    {Op::Cubic, -0.172f, 0.288f, -0.227f, 0.308f, -0.273f, 0.308f},
    {Op::Cubic, -0.313f, 0.308f, -0.337f, 0.285f, -0.337f, 0.24f},
    {Op::Cubic, -0.337f, 0.141f, -0.248f, 0.03f, -0.137f, -0.027f},
    {Op::Close, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f},
};

inline constexpr Cmd kGa[] = {
    {Op::Move, 0.2015f, -0.446f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.2015f, 0.446f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.2555f, 0.446f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.2555f, -0.025f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.4085f, -0.025f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.4085f, -0.071f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.2555f, -0.071f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.2555f, -0.446f, 0.f, 0.f, 0.f, 0.f},
    {Op::Close, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f},
    {Op::Move, -0.3715f, -0.347f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, -0.3715f, -0.301f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, -0.0235f, -0.301f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, -0.0375f, -0.084f, -0.1685f, 0.106f, -0.4085f, 0.226f},
    {Op::Line, -0.3785f, 0.268f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, -0.0955f, 0.127f, 0.0295f, -0.103f, 0.0295f, -0.347f},
    {Op::Close, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f},
};

inline constexpr Cmd kLatinUpper[] = {
    {Op::Move, -0.286f, 0.3645f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, -0.226f, 0.3645f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, -0.143f, 0.1205f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.14f, 0.1205f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.222f, 0.3645f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.286f, 0.3645f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.03f, -0.3645f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, -0.032f, -0.3645f, 0.f, 0.f, 0.f, 0.f},
    {Op::Close, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f},
    {Op::Move, -0.127f, 0.0715f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, -0.082f, -0.0605f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, -0.053f, -0.1465f, -0.028f, -0.2225f, -0.003f, -0.3115f},
    {Op::Line, 0.001f, -0.3115f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, 0.027f, -0.2225f, 0.051f, -0.1465f, 0.08f, -0.0605f},
    {Op::Line, 0.124f, 0.0715f, 0.f, 0.f, 0.f, 0.f},
    {Op::Close, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f},
};

inline constexpr Cmd kLatinLower[] = {
    {Op::Move, -0.044f, 0.28f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, 0.025f, 0.28f, 0.088f, 0.243f, 0.141f, 0.199f},
    {Op::Line, 0.145f, 0.199f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.151f, 0.267f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.199f, 0.267f, 0.f, 0.f, 0.f, 0.f},
    {Op::Line, 0.199f, -0.071f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, 0.199f, -0.189f, 0.154f, -0.28f, 0.026f, -0.28f},
    {Op::Cubic, -0.06f, -0.28f, -0.134f, -0.238f, -0.174f, -0.211f},
    {Op::Line, -0.149f, -0.168f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, -0.111f, -0.195f, -0.051f, -0.229f, 0.019f, -0.229f},
    {Op::Cubic, 0.12f, -0.229f, 0.142f, -0.147f, 0.14f, -0.068f},
    {Op::Cubic, -0.094f, -0.042f, -0.199f, 0.015f, -0.199f, 0.132f},
    {Op::Cubic, -0.199f, 0.232f, -0.13f, 0.28f, -0.044f, 0.28f},
    {Op::Close, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f},
    {Op::Move, -0.032f, 0.23f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, -0.092f, 0.23f, -0.141f, 0.203f, -0.141f, 0.129f},
    {Op::Cubic, -0.141f, 0.048f, -0.07f, -0.002f, 0.14f, -0.025f},
    {Op::Line, 0.14f, 0.148f, 0.f, 0.f, 0.f, 0.f},
    {Op::Cubic, 0.078f, 0.202f, 0.027f, 0.23f, -0.032f, 0.23f},
    {Op::Close, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f},
};

inline constexpr Glyph kGlyphs[] = {
    {L'中', kZhong},
    {L'あ', kA},
    {L'가', kGa},
    {L'A', kLatinUpper},
    {L'a', kLatinLower},
};
// NOLINTEND(modernize-use-std-numbers)
// clang-format on

// The outline for `text` when it is one of these letters; nullptr otherwise.
inline const Glyph* Find(std::wstring_view text)
{
    if (text.size() != 1)
        return nullptr;
    for (const Glyph& g : kGlyphs)
        if (g.ch == text[0])
            return &g;
    return nullptr;
}

} // namespace atrium::ime_glyph
