#!/bin/bash
# fix69: gmcl_zhphone_win64.dll (ARCH=x86_64, default) and gmcl_zhphone_win32.dll (ARCH=i686),
# Linux, llvm-mingw 20261006 UCRT. $ROOT/gmod = garrysmod_common (third-party/source/gmod.tar.gz),
# $ROOT/minhook = MinHook sources, $FF = output of build_ffmpeg_cross68.sh for the same ARCH
set -euo pipefail
ROOT="${ROOT:-$PWD}"; ARCH="${ARCH:-x86_64}"; SRC="$(cd "$(dirname "$0")/../src" && pwd)"
export PATH="$(echo "$ROOT"/llvm-mingw*/bin):$PATH"
if [ "$ARCH" = i686 ]; then FF="${FF:-$ROOT/ffmpeg-x86}"; SUF=zhphonex86; HDE=hde32; OUTN=gmcl_zhphone_win32.dll
else FF="${FF:-$ROOT/ffmpeg-x64}"; SUF=zhphone32; HDE=hde64; OUTN=gmcl_zhphone_win64.dll; fi
OUT="$ROOT/mod-$ARCH"; mkdir -p "$OUT"; cd "$OUT"
for f in "$ROOT"/minhook/src/buffer.c "$ROOT"/minhook/src/hook.c "$ROOT"/minhook/src/trampoline.c "$ROOT"/minhook/src/hde/$HDE.c; do
 $ARCH-w64-mingw32-clang -O2 -c "$f" -I"$ROOT/minhook/include" -o "$(basename "$f" .c).o"; done
$ARCH-w64-mingw32-ar rcs libminhook.a buffer.o hook.o trampoline.o $HDE.o
$ARCH-w64-mingw32-clang++ -std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter -shared -Wl,--no-insert-timestamp -o $OUTN "$SRC/module.cpp" \
 -I"$ROOT/gmod/include" -I"$ROOT/minhook/include" -I"$FF/include" -L. -lminhook \
 "$FF/lib/libavcodec-$SUF.dll.a" "$FF/lib/libavformat-$SUF.dll.a" "$FF/lib/libavutil-$SUF.dll.a" \
 -ld3d9 -ld3d11 -ldxgi -ldxguid -luser32 -lole32 -loleaut32 -lwindowscodecs -static
echo "DONE: $OUT/$OUTN"
