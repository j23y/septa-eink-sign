#!/usr/bin/env python3
"""Regenerates septa_weather/sans_fonts.h: 1-bit bitmap versions of DejaVu Sans
(ASCII 32-126, plus a degree sign stored at code 127).

Usage:  python3 tools/make_fonts.py path/to/fonts/dir septa_weather/sans_fonts.h
Needs:  pip install pillow, and DejaVuSans.ttf + DejaVuSans-Bold.ttf in the fonts dir
        (https://dejavu-fonts.github.io/).
"""
import sys
from PIL import Image, ImageDraw, ImageFont

FONTS = [('SANS17', 'DejaVuSans.ttf', 17), ('SANS20', 'DejaVuSans.ttf', 20), ('SANS22', 'DejaVuSans.ttf', 22),
         ('SANSB22', 'DejaVuSans-Bold.ttf', 22), ('SANSB26', 'DejaVuSans-Bold.ttf', 26),
         ('SANSB30', 'DejaVuSans-Bold.ttf', 30), ('SANSB76', 'DejaVuSans-Bold.ttf', 76)]

def main(font_dir, out_path):
    out = ['// Generated from DejaVu Sans (free font, see dejavu-fonts.github.io). 1-bit glyph bitmaps.',
           '#pragma once', '#include <stdint.h>',
           'struct SansGlyph { uint32_t offset; uint8_t w, h; int8_t xo, yo; uint8_t adv; };',
           'struct SansFont { const uint8_t* bits; const SansGlyph* glyphs; uint8_t ascent, height; };', '']
    for name, ttf, size in FONTS:
        f = ImageFont.truetype(font_dir.rstrip('/') + '/' + ttf, size)
        asc, desc = f.getmetrics()
        data = bytearray(); glyphs = []
        for code in range(32, 128):
            ch = '°' if code == 127 else chr(code)
            x0, y0, x1, y1 = f.getbbox(ch, anchor='ls')
            w, h = max(0, x1 - x0), max(0, y1 - y0)
            adv = round(f.getlength(ch))
            off = len(data)
            if w and h:
                im = Image.new('1', (w, h), 0); d = ImageDraw.Draw(im); d.fontmode = '1'
                d.text((-x0, -y0), ch, font=f, fill=1, anchor='ls')
                px = im.load(); bits = [1 if px[c, r] else 0 for r in range(h) for c in range(w)]
                for i in range(0, len(bits), 8):
                    b = 0
                    for j, v in enumerate(bits[i:i + 8]): b |= v << (7 - j)
                    data.append(b)
            glyphs.append((off, w, h, x0, y0, adv))
        out.append(f'static const uint8_t {name}_bits[] = {{')
        for i in range(0, len(data), 20): out.append('  ' + ','.join(f'0x{b:02x}' for b in data[i:i + 20]) + ',')
        out.append('};')
        out.append(f'static const SansGlyph {name}_glyphs[] = {{')
        for g in glyphs: out.append('  {%d,%d,%d,%d,%d,%d},' % g)
        out.append('};')
        out.append(f'static const SansFont {name} = {{ {name}_bits, {name}_glyphs, {asc}, {asc + desc} }};')
        out.append('')
    open(out_path, 'w').write('\n'.join(out))

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
