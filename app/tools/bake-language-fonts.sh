#!/usr/bin/env bash
# Orbit Store TV app - Bake the fonts of languages beyond Inter's scripts.
#
# One font per language, each the whole character set of its encoding
# (tools/charsets.py) at 40 pixels, from the Noto Sans regional subsets: the
# app loads only the chosen language's font and keeps it on the GPU alone
# (src/main.cpp). Stored as .huifontz, "HUFZ", the size, then zlib
# (gfx::inflate_font). Arabic has few glyphs and bakes at Inter's 56 pixels.

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
python=${PYTHON:-python3}
cxx=$(command -v "${HOST_CXX:-clang++}")
mkdir -p "$root/build/host" "$root/assets/fonts"
"$cxx" -std=c++20 -O2 -w "$root/tools/font-baker/bake_font.cpp" -o "$root/build/host/bake_font"
"$python" "$root/tools/charsets.py" "$root/build/host/charsets"
pack() {
    "$python" - "$1" "$2" <<'PY'
import struct, sys, zlib
data = open(sys.argv[1], "rb").read()
open(sys.argv[2], "wb").write(b"HUFZ" + struct.pack("<I", len(data)) + zlib.compress(data, 9))
PY
}
while read -r source output size atlas set; do
    [[ -n $source ]] || continue
    font="$root/third_party/fonts/$source"
    if [[ $source == *.otf ]]; then
        font="$root/build/host/$output.ttf"
        "$python" "$root/tools/quadratic-font.py" "$root/third_party/fonts/$source" "$font"
    fi
    "$root/build/host/bake_font" "$font" "$root/build/host/$output.huifont" \
        "$size" 8 "$atlas" "file:$root/build/host/charsets/$set.txt"
    pack "$root/build/host/$output.huifont" "$root/assets/fonts/$output.huifontz"
done <<'FONTS'
NotoSansJP-Regular.otf noto-sans-jp 40 8192 ja
NotoSansSC-Regular.otf noto-sans-sc 40 8192 zh-Hans
NotoSansTC-Regular.otf noto-sans-tc 40 8192 zh-Hant
NotoSansKR-Regular.otf noto-sans-kr 40 8192 ko
NotoSansArabic-Regular.ttf noto-sans-arabic 56 2048 ar
FONTS
cp "$root"/third_party/fonts/NotoSans*-LICENSE.txt "$root/assets/fonts/"
