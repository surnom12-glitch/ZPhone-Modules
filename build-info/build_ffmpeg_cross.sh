#!/bin/bash
# fix63: FFmpeg n8.1 for gmcl_zhphone_win64.dll = the fix39 configuration + the native AAC encoder
# (fix62 clip sound) + WAV / MP3 / OGG demuxers and the PCM / ADPCM / MP3 / Vorbis decoders (fix63:
# the phone's ear decodes the game's sound files). Cross-built on Linux with llvm-mingw 20261006 UCRT.
#   ROOT=/path/to/workdir: $ROOT/FFmpeg-n8.1 (third-party/source/FFmpeg-n8.1.tar.gz),
#   $ROOT/llvm-mingw-*/bin, $ROOT/ffdeps = nv-codec-headers installed (make PREFIX=$ROOT/ffdeps install)
set -euo pipefail
ROOT="${ROOT:-$PWD}"
cd "$ROOT/FFmpeg-n8.1"
export PATH="$(echo "$ROOT"/llvm-mingw*/bin):$PATH"
export PKG_CONFIG_PATH="$ROOT/ffdeps/lib/pkgconfig"
./configure --prefix="$ROOT/ffmpeg-minimal" --build-suffix=-zhphone32 --target-os=mingw64 --arch=x86_64 --enable-cross-compile \
 --cross-prefix=x86_64-w64-mingw32- --cc=x86_64-w64-mingw32-clang --cxx=x86_64-w64-mingw32-clang++ \
 --pkg-config=pkg-config --extra-cflags=-I"$ROOT/ffdeps/include" \
 --disable-everything --disable-autodetect --disable-programs --disable-doc --disable-debug --disable-x86asm \
 --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample --disable-static --enable-shared --enable-small \
 --enable-avcodec --enable-avformat --enable-avutil --enable-encoder=h264_nvenc --enable-encoder=aac \
 --enable-demuxer=wav,mp3,ogg --enable-decoder=pcm_u8,pcm_s16le,pcm_s24le,pcm_s32le,pcm_f32le,adpcm_ms,adpcm_ima_wav,mp3float,mp3,vorbis \
 --enable-parser=mpegaudio,vorbis --enable-muxer=mp4 --enable-protocol=file \
 --enable-ffnvcodec --enable-nvenc --enable-d3d11va --disable-vulkan > "$ROOT/ffmpeg-configure.log" 2>&1
make -j"$(nproc)" > "$ROOT/ffmpeg-build.log" 2>&1
make install >> "$ROOT/ffmpeg-build.log" 2>&1
echo "DONE: $ROOT/ffmpeg-minimal/bin"
