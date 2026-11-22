"""Writes the input method entry's letters as outlines, so the bar draws them without shipping a font.

The five letters the entry shows for the common cases (中 あ 가 A a, shell/ime_status.cpp ImeGlyph) are
read from an OFL font, flipped to screen coordinates (y down), centred on their ink and measured in
ems, so a letter drawn at the entry's text size matches what the font would draw. Run once when the
letters or the font change; the outputs are checked in:

    python3 tools/generate-ime-glyphs.py --font NotoSansCJKsc-Regular.otf

Needs fontTools. Font: Noto Sans CJK SC Regular (SIL Open Font License 1.1).
"""

import argparse
import os

from fontTools.pens.basePen import BasePen
from fontTools.pens.boundsPen import BoundsPen
from fontTools.ttLib import TTFont

LETTERS = [('中', 'kZhong'), ('あ', 'kA'), ('가', 'kGa'), ('A', 'kLatinUpper'), ('a', 'kLatinLower')]
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class Outline(BasePen):
    """Moves, lines and cubics (quadratics raised to cubics), in font units."""

    def __init__(self, glyphs):
        super().__init__(glyphs)
        self.cmds = []
        self.at = (0, 0)

    def _moveTo(self, p):
        self.cmds.append(('M', [p]))
        self.at = p

    def _lineTo(self, p):
        self.cmds.append(('L', [p]))
        self.at = p

    def _curveToOne(self, p1, p2, p3):
        self.cmds.append(('C', [p1, p2, p3]))
        self.at = p3

    def _qCurveToOne(self, p1, p2):
        p0 = self.at
        c1 = (p0[0] + 2 / 3 * (p1[0] - p0[0]), p0[1] + 2 / 3 * (p1[1] - p0[1]))
        c2 = (p2[0] + 2 / 3 * (p1[0] - p2[0]), p2[1] + 2 / 3 * (p1[1] - p2[1]))
        self._curveToOne(c1, c2, p2)

    def _closePath(self):
        self.cmds.append(('Z', []))

    def _endPath(self):
        self.cmds.append(('Z', []))


def read(font_path):
    font = TTFont(font_path)
    em = font['head'].unitsPerEm
    cmap = font.getBestCmap()
    glyphs = font.getGlyphSet()
    out = []
    for ch, name in LETTERS:
        glyph = glyphs[cmap[ord(ch)]]
        bounds = BoundsPen(glyphs)
        glyph.draw(bounds)
        x0, y0, x1, y1 = bounds.bounds
        cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
        pen = Outline(glyphs)
        glyph.draw(pen)
        # Em units, centred on the ink, y down.
        cmds = [(op, [((x - cx) / em, (cy - y) / em) for x, y in pts]) for op, pts in pen.cmds]
        out.append((ch, name, cmds))
    family = font['name'].getDebugName(4)
    version = font['name'].getDebugName(5)
    return out, family, version


def num(v):
    s = f'{v:.5f}'.rstrip('0')
    s = '0.' if s in ('-0.', '0.', '') else s
    return s + 'f'


def write_cpp(letters, source):
    lines = [
        '// The input method entry\'s letters as outlines (tools/generate-ime-glyphs.py; do not edit):',
        f'// {source}, SIL Open Font License 1.1. Em units, centred on the ink, y down.',
        '#pragma once',
        '',
        '#include <span>',
        '#include <string_view>',
        '',
        'namespace atrium::ime_glyph {',
        '',
        'enum class Op : unsigned char { Move, Line, Cubic, Close };',
        '',
        '// A step of an outline: Move and Line use (x1, y1); Cubic its two controls and its end.',
        'struct Cmd {',
        '    Op op;',
        '    float x1, y1, x2, y2, x3, y3;',
        '};',
        '',
        'struct Glyph {',
        '    wchar_t ch;',
        '    std::span<const Cmd> cmds;',
        '};',
        '',
        '// clang-format off: one step a line, as generated',
        '// NOLINTBEGIN(modernize-use-std-numbers): outline coordinates, some of which merely resemble constants',
    ]
    ops = {'M': 'Op::Move', 'L': 'Op::Line', 'C': 'Op::Cubic', 'Z': 'Op::Close'}
    for ch, name, cmds in letters:
        lines.append(f'inline constexpr Cmd {name}[] = {{')
        for op, pts in cmds:
            flat = [c for p in pts for c in p] + [0] * (6 - 2 * len(pts))
            lines.append(f'    {{{ops[op]}, ' + ', '.join(num(v) for v in flat) + '},')
        lines.append('};')
        lines.append('')
    lines.append('inline constexpr Glyph kGlyphs[] = {')
    for ch, name, _ in letters:
        lines.append(f"    {{L'{ch}', {name}}},")
    lines += [
        '};',
        '// NOLINTEND(modernize-use-std-numbers)',
        '// clang-format on',
        '',
        '// The outline for `text` when it is one of these letters; nullptr otherwise.',
        'inline const Glyph* Find(std::wstring_view text)',
        '{',
        '    if (text.size() != 1)',
        '        return nullptr;',
        '    for (const Glyph& g : kGlyphs)',
        '        if (g.ch == text[0])',
        '            return &g;',
        '    return nullptr;',
        '}',
        '',
        '} // namespace atrium::ime_glyph',
        '',
    ]
    with open(os.path.join(ROOT, 'native', 'core', 'render', 'ime_glyph_data.h'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines))


def write_cs(letters, source):
    def path(cmds):
        parts = []
        for op, pts in cmds:
            parts.append(op + ' '.join(f'{x:.4f},{y:.4f}' for x, y in pts))
        return ' '.join(parts)

    lines = [
        '// The input method entry\'s letters as outlines (tools/generate-ime-glyphs.py; do not edit):',
        f'// {source}, SIL Open Font License 1.1. Em units, centred on the ink, y down.',
        'namespace Atrium.Settings;',
        '',
        'internal static class ImeGlyphData',
        '{',
        '    // Path markup (the XAML mini-language) for the letter `glyph`, or null.',
        '    public static string? PathFor(string glyph) => glyph switch',
        '    {',
    ]
    for ch, _, cmds in letters:
        # Lines of at most 120 columns: the markup in pieces joined at compile time.
        text, pieces = path(cmds), []
        while text:
            cut = text.rfind(' ', 0, 100) if len(text) > 100 else len(text)
            cut = cut if cut > 0 else len(text)
            pieces.append(text[:cut + 1] if cut < len(text) else text)
            text = text[cut + 1:]
        lines.append(f'        "{ch}" =>')
        for i, piece in enumerate(pieces):
            lines.append(f'            "{piece}"' + (' +' if i < len(pieces) - 1 else ','))
    lines += ['        _ => null,', '    };', '}', '']
    with open(os.path.join(ROOT, 'settings', 'Atrium.Settings', 'ImeGlyphData.cs'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--font', required=True, help='Noto Sans CJK SC Regular (OTF)')
    args = parser.parse_args()
    letters, family, version = read(args.font)
    source = f'{family} {version.split(";")[0]}'
    write_cpp(letters, source)
    write_cs(letters, source)
    print(f'{len(letters)} letters from {source}')


if __name__ == '__main__':
    main()
