#!/usr/bin/env python3
"""Convert the bundled CFF font to quadratic outlines for stb's SDF baker.

stbtt_GetGlyphSDF does not support CFF cubic curves. Keep the original OTF
and licence in third_party; the temporary TTF is a build intermediate only.
Requires fonttools==4.66.1. https://fonttools.readthedocs.io/en/latest/pens/cu2quPen.html
"""
import sys
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.cu2quPen import Cu2QuPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTFont

source = TTFont(sys.argv[1])
upem = source['head'].unitsPerEm
builder = FontBuilder(upem, isTTF=True)
builder.setupGlyphOrder(source.getGlyphOrder())
builder.setupCharacterMap(source.getBestCmap())
glyphset = source.getGlyphSet()
glyphs = {}
for name in source.getGlyphOrder():
    pen = TTGlyphPen(glyphset)
    glyphset[name].draw(Cu2QuPen(pen, max_err=upem / 1000, reverse_direction=True))
    glyphs[name] = pen.glyph()
builder.setupGlyf(glyphs)
builder.setupHorizontalMetrics(source['hmtx'].metrics)
builder.setupHorizontalHeader(ascent=source['hhea'].ascent, descent=source['hhea'].descent, lineGap=source['hhea'].lineGap)
builder.setupOS2(sTypoAscender=source['OS/2'].sTypoAscender, sTypoDescender=source['OS/2'].sTypoDescender,
                usWinAscent=source['OS/2'].usWinAscent, usWinDescent=source['OS/2'].usWinDescent)
builder.font['name'] = source['name']
builder.setupPost()
builder.font.recalcTimestamp = False
builder.font['head'].created = source['head'].created
builder.font['head'].modified = source['head'].modified
builder.save(sys.argv[2])
