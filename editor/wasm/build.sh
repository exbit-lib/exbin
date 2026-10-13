#!/usr/bin/env sh
# Rebuilds editor/exbin.wasm from the C sources.
# Needs: clang (with the wasm32 target), wasm-ld, and a WASI sysroot:
#   https://github.com/WebAssembly/wasi-sdk/releases  (wasi-sysroot-XX.0.tar.gz)
#   export WASI_SYSROOT=/path/to/wasi-sysroot-24.0
set -eu
: "${WASI_SYSROOT:?set WASI_SYSROOT to the wasi-sysroot directory}"
HERE=$(cd "$(dirname "$0")" && pwd); ROOT="$HERE/../.."; OUT=$(mktemp -d)
CF="--target=wasm32-wasi --sysroot=$WASI_SYSROOT -O2 -std=gnu11 -I$ROOT/include -I$ROOT/src"
for f in object chunk scanner compiler vm serialize regex natives toolkit api; do clang $CF -c "$ROOT/src/$f.c" -o "$OUT/$f.o"; done
clang $CF -c "$HERE/glue.c" -o "$OUT/glue.o"
clang $CF -c "$HERE/rt.c"   -o "$OUT/rt.o"
wasm-ld --no-entry --export-memory -o "$HERE/../exbin.wasm" "$OUT"/*.o \
  -L"$WASI_SYSROOT/lib/wasm32-wasi" -lc -lm -lwasi-emulated-process-clocks \
  --strip-all --gc-sections -z stack-size=1048576 --initial-memory=16777216
rm -rf "$OUT"; ls -l "$HERE/../exbin.wasm"
