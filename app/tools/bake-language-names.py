#!/usr/bin/env python3
"""Add the few CJK/Arabic language-picker glyphs to every primary UI font.

This keeps every language name readable before a full language font is loaded.
The original Latin glyphs and kerning are preserved. Run after baking the
primary fonts and converting the bundled CFF fonts with bake-language-fonts.sh.
"""
from pathlib import Path
import struct
import subprocess

root = Path(__file__).resolve().parents[1]
header = struct.Struct('<IIHHfffffII')
glyph = struct.Struct('<IHHHHfff')
kern_size = 12


def read(path):
    data = path.read_bytes()
    h = list(header.unpack_from(data))
    records = [list(glyph.unpack_from(data, 40 + i * 24)) for i in range(h[9])]
    offset = 40 + h[9] * 24
    kerns = data[offset:offset + h[10] * kern_size]
    pixels = data[offset + h[10] * kern_size:]
    assert len(pixels) == h[2] * h[3]
    return h, records, kerns, pixels


# Use each regional subset for its own language name.
sets = [
    ('names-jp', root / 'build/host/noto-sans-jp.ttf', set(map(ord, '日本語'))),
    ('names-sc', root / 'build/host/noto-sans-sc.ttf', set(map(ord, '简体中文'))),
    ('names-tc', root / 'build/host/noto-sans-tc.ttf', set(map(ord, '繁體中文'))),
    ('names-kr', root / 'build/host/noto-sans-kr.ttf', set(map(ord, '한국어'))),
    ('names-arabic', root / 'third_party/fonts/NotoSansArabic-Regular.ttf',
     set(map(ord, 'العربية')) | set(range(0xFE70, 0xFF00))),
]
# Sora lacks some European scripts; preserve its own glyphs and fill only gaps.
extras = [read(root / 'assets/fonts/inter-regular.huifont')]
for name, source, codepoints in sets:
    points = root / 'build/host' / (name + '.txt')
    points.write_text('\n'.join(map(str, sorted(codepoints))) + '\n')
    baked = root / 'build/host' / (name + '.huifont')
    subprocess.run([str(root / 'build/host/bake_font'), str(source), str(baked),
                    '56', '8', '2048', 'file:' + str(points)], check=True)
    extras.append(read(baked))

for target in sorted((root / 'assets/fonts').glob('*.huifont')):
    h, records, kerns, pixels = read(target)
    if h[4] != 56 or h[2] != 2048:
        raise ValueError(f'Unexpected primary font layout: {target}')
    present = {g[0] for g in records}
    for eh, more, _, epixels in extras:
        additions = [g.copy() for g in more if g[0] not in present]
        if not additions:
            continue
        for g in additions:
            g[2] += h[3]
            present.add(g[0])
        records.extend(additions)
        pixels += epixels
        h[3] += eh[3]
    h[9] = len(records)
    target.write_bytes(header.pack(*h) + b''.join(glyph.pack(*g) for g in sorted(records)) + kerns + pixels)
    print(f'{target.name}: language names available, atlas {h[2]}x{h[3]}')
