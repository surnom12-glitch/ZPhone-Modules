#!/bin/bash
# fix63/fix64: the distributed gmcl_zhphone_win64.dll was built like this (Linux, llvm-mingw 20261006 UCRT).
#   $ROOT/gmod = garrysmod_common (third-party/source/gmod.tar.gz), $ROOT/minhook = MinHook sources,
#   $ROOT/ffmpeg-minimal = output of build_ffmpeg_cross.sh (or ../ffmpeg-sdk)
set -euo pipefail
ROOT="${ROOT:-$PWD}"; SRC="$(cd "$(dirname "$0")/../src" && pwd)"
export PATH="$(echo "$ROOT"/llvm-mingw*/bin):$PATH"
FF="$ROOT/ffmpeg-minimal"; mkdir -p "$ROOT/mod"; cd "$ROOT/mod"
for f in "$ROOT"/minhook/src/buffer.c "$ROOT"/minhook/src/hook.c "$ROOT"/minhook/src/trampoline.c "$ROOT"/minhook/src/hde/hde64.c; do
 x86_64-w64-mingw32-clang -O2 -c "$f" -I"$ROOT/minhook/include" -o "$(basename "$f" .c).o"; done
x86_64-w64-mingw32-ar rcs libminhook.a buffer.o hook.o trampoline.o hde64.o
x86_64-w64-mingw32-clang++ -std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter -shared -Wl,--no-insert-timestamp -o gmcl_zhphone_win64.dll "$SRC/module.cpp" \
 -I"$ROOT/gmod/include" -I"$ROOT/minhook/include" -I"$FF/include" -L. -lminhook \
 "$FF/lib/libavcodec-zhphone32.dll.a" "$FF/lib/libavformat-zhphone32.dll.a" "$FF/lib/libavutil-zhphone32.dll.a" \
 -ld3d9 -ld3d11 -ldxgi -ldxguid -luser32 -lole32 -loleaut32 -lwindowscodecs -static
echo "DONE: $ROOT/mod/gmcl_zhphone_win64.dll"
