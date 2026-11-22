#!/usr/bin/env bash
# Native macOS build of build/orbit_store.elf, without Docker. Reproduces the
# container build (tools/Dockerfile.sdk + Dockerfile + `make payload`) with:
#   - Homebrew llvm@19 and lld@19 (the container's clang-19/lld-19/llvm-19),
#     merged into one bindir so the SDK's prospero-* wrappers find ld.lld;
#   - the pinned PS5 payload SDK v0.43, checksum-verified;
#   - dependency sources from a local source bundle when present (offline).
# Steps already done are skipped, so rerunning it is the normal rebuild.
#
# Environment (all optional):
#   ORBIT_TOOLCHAIN      SDK and LLVM wrapper location (default: ../../toolchain)
#   ORBIT_DEP_SOURCES    unpacked source bundle with the pinned archives (default: ../../src)
#   BUILD_JOBS           parallel jobs (default: CPU count)
#   ORBIT_REBUILD_DEPS=1 rebuild the PS5 libraries (OpenSSL, curl, ...) in the SDK sysroot
#   ORBIT_NPM_CI=1       reinstall node_modules
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
if [ "$(uname -s)" != Darwin ]; then
  echo 'build-macos.sh is for macOS; elsewhere use the Docker build in BUILDING.md.' >&2
  exit 1
fi
parent=$(cd "$root/../.." && pwd)
toolchain=${ORBIT_TOOLCHAIN:-$parent/toolchain}
sources=${ORBIT_DEP_SOURCES:-$parent/src}
export BUILD_JOBS=${BUILD_JOBS:-$(sysctl -n hw.ncpu)}
step() { printf '\n==> %s\n' "$*"; }

step 'Toolchain'
brew_prefix=$(brew --prefix 2>/dev/null || echo /opt/homebrew)
llvm=$brew_prefix/opt/llvm@19
lld=$brew_prefix/opt/lld@19
for need in "$llvm/bin/clang" "$lld/bin/ld.lld" "$brew_prefix/opt/openssl@3/include/openssl/evp.h"; do
  test -e "$need" || { echo "Missing $need. Run: brew install llvm@19 lld@19 openssl@3" >&2; exit 1; }
done
mkdir -p "$toolchain/llvm19/bin"
for tool in "$llvm"/bin/* "$lld"/bin/*; do ln -sf "$tool" "$toolchain/llvm19/bin/"; done
# llvm-config reporting the merged bindir; the SDK runs tools from `--bindir`.
cat > "$toolchain/llvm19/llvm-config" <<EOF
#!/usr/bin/env bash
if [ "\$#" -eq 1 ] && [ "\$1" = "--bindir" ]; then echo "$toolchain/llvm19/bin"; exit 0; fi
exec "$llvm/bin/llvm-config" "\$@"
EOF
chmod +x "$toolchain/llvm19/llvm-config"
sdk=$toolchain/ps5-payload-sdk
if ! test -x "$sdk/bin/prospero-clang"; then
  zip=$toolchain/ps5-payload-sdk-v0.43.zip
  test -f "$zip" || curl -fL --retry 3 https://github.com/ps5-payload-dev/sdk/releases/download/v0.43/ps5-payload-sdk.zip -o "$zip"
  printf '%s  %s\n' a9cc9929f21b2b2c5d5b309f3bab4997067c45281c0622cf4838b1aecba66fcb "$zip" | shasum -a 256 -c -
  unzip -q "$zip" -d "$toolchain"
fi
export LLVM_CONFIG=$toolchain/llvm19/llvm-config
export PS5_PAYLOAD_SDK=$sdk
"$sdk/bin/prospero-llvm-config" --version | grep -q '^19\.' || { echo 'The SDK is not using LLVM 19.' >&2; exit 1; }

step 'Local dependency sources'
mkdir -p .deps/cjson .deps/ps5-deps
seed() { if test -f "$sources/$1" && ! test -f "$2"; then cp "$sources/$1" "$2"; fi; }
for name in cJSON.c cJSON.h LICENSE; do seed "build-inputs/cjson/$name" ".deps/cjson/$name"; done
seed build-inputs/cacert.pem .deps/cacert.pem
seed 7z2603-src.7z .deps/7z2603-src.7z
seed unrarsrc-7.2.7.tar.gz .deps/unrarsrc-7.2.7.tar.gz
seed sqlite-amalgamation-3540100.zip .deps/sqlite-amalgamation-3540100.zip
seed openssl-3.5.2.tar.gz .deps/ps5-deps/openssl.tar.gz
seed curl-8.18.0.tar.xz .deps/ps5-deps/curl.tar.xz
seed libmicrohttpd-1.0.1.tar.gz .deps/ps5-deps/microhttpd.tar.gz
seed zlib-1.3.2.tar.gz .deps/ps5-deps/zlib.tar.gz
seed bzip2-1.0.8.tar.gz .deps/ps5-deps/bzip2.tar.gz
seed xz-5.8.1.tar.gz .deps/ps5-deps/xz.tar.gz
seed libarchive-3.8.1.tar.xz .deps/ps5-deps/libarchive.tar.xz

step 'Host tools'
test -x .venv/bin/python || python3 -m venv .venv
.venv/bin/python tools/bootstrap.py
if [ "${ORBIT_NPM_CI:-0}" = 1 ] || ! test -d node_modules; then npm ci; fi

step 'PS5 libraries'
hb=$sdk/target/user/homebrew/lib
if [ "${ORBIT_REBUILD_DEPS:-0}" = 1 ] || ! test -f "$hb/libarchive.a" || ! test -f "$hb/libcurl.a" || ! test -f "$hb/libmicrohttpd.a"; then
  ORBIT_DEPS_WORK=$root/.deps/ps5-deps bash tools/build-deps.sh
else
  echo "Using the libraries already in $hb (ORBIT_REBUILD_DEPS=1 rebuilds them)."
fi

step 'Browser interface'
npm run build

step 'Payload'
# The Makefile's grouped targets (&:) want GNU make 4.3+; macOS make 3.81 warns
# ("overriding commands for target `&'") but builds. Serial, so that those
# generators never run twice at once.
make_cmd=make
if command -v gmake >/dev/null; then make_cmd=gmake; fi
"$make_cmd" payload PS5_PAYLOAD_SDK="$sdk"
elf=$root/build/orbit_store.elf
printf '\nBuilt %s (%s bytes)\n' "$elf" "$(stat -f %z "$elf")"
shasum -a 256 "$elf"
