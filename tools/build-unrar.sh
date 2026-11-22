#!/usr/bin/env bash
# Pinned official UnRAR source (RARLAB), built as a static library for RAR
# extraction with multithreaded unpacking. No downloaded binaries.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
version=7.2.7 # UnRAR 7.23, 27 June 2026
archive="$root/.deps/unrarsrc-$version.tar.gz"
src="$root/.deps/unrar"
mkdir -p "$root/.deps"
if ! test -f "$archive"; then
  curl -fL --retry 3 "https://www.rarlab.com/rar/unrarsrc-$version.tar.gz" -o "$archive"
fi
printf '%s  %s\n' 01d903a7dcf413cb2925696d7796e48e38d471f79bfe7ef3ad2aebf6c12dbefd "$archive" | sha256sum -c -
if ! test -f "$src/dll.cpp"; then
  tar -xzf "$archive" -C "$root/.deps"
fi
python3 "$root/tools/patch-unrar.py"

target=${1:-host}
case "$target" in
  ps5) sdk=${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}; cxx="$sdk/bin/prospero-clang++"; ar="$sdk/bin/prospero-ar"; cpu=-DORBIT_PS5_CPU ;;
  host) cxx=clang++-19; ar=ar; cpu= ;;
  *) echo 'Use host or ps5' >&2; exit 1 ;;
esac
out="$root/.deps/unrar-$target"
rm -rf "$out"
mkdir -p "$out"
# The objects of UnRAR's own "lib" target (makefile), with its library defines.
objects="rar strlist strfn pathfn smallfn global file filefn filcreat archive arcread unicode system
  crypt crc rawread encname resource match timefn rdwrfn consio options errhnd rarvm secpassword
  rijndael getbits sha1 sha256 blake2s hash extinfo extract volume list find unpack headers
  threadpool rs16 cmddata ui largepage filestr scantree dll qopen"
flags=(-O3 -std=c++11 -w -DRARDLL -DRAR_SMP -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE ${cpu:+"$cpu"}
  -ffunction-sections -fdata-sections)
pids=()
for name in $objects; do
  "$cxx" "${flags[@]}" -c "$src/$name.cpp" -o "$out/$name.o" &
  pids+=($!)
  if (( ${#pids[@]} >= ${BUILD_JOBS:-4} )); then wait "${pids[0]}"; pids=("${pids[@]:1}"); fi
done
for pid in "${pids[@]}"; do wait "$pid"; done
"$ar" rcs "$out/liborbitunrar.a" "$out"/*.o
