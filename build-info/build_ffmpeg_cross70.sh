#!/bin/bash
# fix70: FFmpeg n8.1 for gmcl_zhphone_win64.dll / gmcl_zhphone_win32.dll = the fix69 configuration
# + mjpeg encoder/decoder + mov demuxer (no-GPU-encoder path: MJPEG while filming, then converted to H.264 by Media Foundation),
# + x86 assembly (nasm 2.16.03 in PATH), no --enable-small (speed of the CPU path), nv-codec-headers n11.1.5.3 (NVIDIA driver 471.41+, Kepler GTX 600/700 too).
# + h264_amf (AMD, AMF headers v1.4.36 = the oldest FFmpeg 8.1 accepts, works with older AMD drivers) + h264_mf (Media Foundation: Intel / AMD / NVIDIA / any GPU).
#   ARCH=x86_64 (default) or ARCH=i686; ROOT=/path/to/workdir: $ROOT/FFmpeg-n8.1, $ROOT/llvm-mingw-*/bin,
#   $ROOT/nasmbin/bin/nasm, $ROOT/ffdeps = nv-codec-headers n11.1.5.3 installed (make PREFIX=$ROOT/ffdeps install) + include/AMF (AMF-headers v1.4.36)
set -euo pipefail
ROOT="${ROOT:-$PWD}"; ARCH="${ARCH:-x86_64}"
if [ "$ARCH" = i686 ]; then FA=x86; TOS=mingw32; SUF=-zhphonex86; OUT="$ROOT/ffmpeg-x86"; else FA=x86_64; TOS=mingw64; SUF=-zhphone32; OUT="$ROOT/ffmpeg-x64"; fi
B="$ROOT/build-$ARCH"; mkdir -p "$B"; cd "$B"
export PATH="$ROOT/nasmbin/bin:$(echo "$ROOT"/llvm-mingw*/bin):$PATH"
export PKG_CONFIG_PATH="$ROOT/ffdeps/lib/pkgconfig"
"$ROOT/FFmpeg-n8.1/configure" --prefix="$OUT" --build-suffix=$SUF --target-os=$TOS --arch=$FA --enable-cross-compile \
 --cross-prefix=$ARCH-w64-mingw32- --cc=$ARCH-w64-mingw32-clang --cxx=$ARCH-w64-mingw32-clang++ \
 --pkg-config=pkg-config --extra-cflags=-I"$ROOT/ffdeps/include" \
 --disable-everything --disable-autodetect --disable-programs --disable-doc --disable-debug \
 --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample --disable-static --enable-shared \
 --enable-avcodec --enable-avformat --enable-avutil --enable-encoder=h264_nvenc,h264_amf,h264_mf,mjpeg --enable-encoder=aac \
 --enable-demuxer=wav,mp3,ogg,mov --enable-decoder=mjpeg --enable-decoder=pcm_u8,pcm_s16le,pcm_s24le,pcm_s32le,pcm_f32le,adpcm_ms,adpcm_ima_wav,mp3float,mp3,vorbis \
 --enable-parser=mpegaudio,vorbis --enable-muxer=mp4 --enable-protocol=file \
 --enable-ffnvcodec --enable-nvenc --enable-amf --enable-mediafoundation --enable-d3d11va --disable-vulkan > "$ROOT/ffmpeg-configure-$ARCH.log" 2>&1
make -j"$(nproc)" > "$ROOT/ffmpeg-build-$ARCH.log" 2>&1
make install >> "$ROOT/ffmpeg-build-$ARCH.log" 2>&1
echo "DONE: $OUT/bin"
