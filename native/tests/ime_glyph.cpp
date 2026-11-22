#include "render/ime_glyph_data.h"

#include <algorithm>
#include <cmath>
#include <iostream>

using namespace atrium;

namespace {

int g_failures = 0;

void Require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
}

} // namespace

int main()
{
    for (const wchar_t* letter : {L"中", L"あ", L"가", L"A", L"a"}) {
        const ime_glyph::Glyph* g = ime_glyph::Find(letter);
        Require(g != nullptr, "each of the five letters has an outline");
        if (!g)
            continue;
        Require(!g->cmds.empty() && g->cmds.front().op == ime_glyph::Op::Move, "an outline starts with a move");
        float left = 1, right = -1, top = 1, bottom = -1;
        for (const auto& c : g->cmds) {
            if (c.op == ime_glyph::Op::Close)
                continue;
            const float x = c.op == ime_glyph::Op::Cubic ? c.x3 : c.x1, y = c.op == ime_glyph::Op::Cubic ? c.y3 : c.y1;
            left = std::min(left, x);
            right = std::max(right, x);
            top = std::min(top, y);
            bottom = std::max(bottom, y);
        }
        Require(left >= -0.6f && right <= 0.6f && top >= -0.6f && bottom <= 0.6f, "an outline fits in an em");
        Require(std::abs(left + right) < 0.02f && std::abs(top + bottom) < 0.02f, "an outline is centred on its ink");
    }
    Require(ime_glyph::Find(L"中")->cmds.size() > ime_glyph::Find(L"A")->cmds.size(), "each letter has its own outline");
    Require(ime_glyph::Find(L"р") == nullptr, "other letters have none (drawn as text)");
    Require(ime_glyph::Find(L"中文") == nullptr && ime_glyph::Find(L"") == nullptr, "only a single letter has an outline");

    std::cout << (g_failures ? "ime_glyph: FAILED" : "ime_glyph: ok") << '\n';
    return g_failures ? 1 : 0;
}
