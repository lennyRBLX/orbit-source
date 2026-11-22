#!/usr/bin/env python3
"""Writes the character sets the language fonts are baked with, one decimal
code point per line, to the folder given (tools/bake-fonts.sh).

The standard sets of each language's own encoding, so anything written in
it shows: JIS X 0208 for Japanese, GB 2312 for Simplified Chinese, Big5 for
Traditional Chinese, and every Hangul syllable with KS X 1001's symbols for
Korean. Arabic is its block and the presentation forms the TV app joins
letters into (src/gfx/script.cpp).
"""
import json
import sys
from pathlib import Path


def two_byte(codec, leads, trails):
    out = set()
    for a in leads:
        for b in trails:
            try:
                ch = bytes([a, b]).decode(codec)
            except UnicodeDecodeError:
                continue
            if len(ch) == 1 and ord(ch) > 0x7F:
                out.add(ord(ch))
    return out


def ideograph(c):
    return 0x4E00 <= c <= 0x9FFF or 0xF900 <= c <= 0xFAFF


sets = {
    "ja": two_byte("euc_jp", range(0xA1, 0xF5), range(0xA1, 0xFF)),
    "zh-Hans": two_byte("gb2312", range(0xA1, 0xF8), range(0xA1, 0xFF)),
    "zh-Hant": two_byte("big5", range(0xA1, 0xFA), [*range(0x40, 0x7F), *range(0xA1, 0xFF)]),
    "ko": {c for c in two_byte("euc_kr", range(0xA1, 0xFE), range(0xA1, 0xFF)) if not ideograph(c)}
    | set(range(0xAC00, 0xD7A4)),
    "ar": set(range(0x0600, 0x0700)) | set(range(0xFE70, 0xFF00)),
}
folder = Path(sys.argv[1])
folder.mkdir(parents=True, exist_ok=True)
for code, characters in sets.items():
    # Include UI punctuation and characters outside the legacy encodings, too.
    phrases = json.loads((Path(__file__).resolve().parents[2] / 'i18n' / (code + '.json')).read_text())
    for value in phrases.values():
        for text in ([value] if isinstance(value, str) else value.values()):
            characters.update(ord(c) for c in text if ord(c) > 127)
    (folder / f"{code}.txt").write_text("\n".join(str(c) for c in sorted(characters)) + "\n")
