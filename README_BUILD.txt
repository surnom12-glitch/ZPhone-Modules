fix73 (build "fix73-phone-line", line_api 1): the phone line, src/phone_line.h (header only, no new
library). PhoneLine::Mixer mixes the line voices (files from PhoneEar::bank(), shared with the clip
ear) to mono at the output rate: per-voice gain / 2-pole low-pass / pitch smoothed in 15 ms, Hermite
resampling, loops (the bank's loop point), 30 ms stop fade, a late start skips its age + the time it
waited for its file, 96 voices (the quietest goes), phone band (0 full, 1 HD 120-7000 Hz, 2 old
300-3400 Hz, RBJ Butterworth biquads), pocket muffle (900 Hz, 120 ms crossfade), master smoothed in
30 ms, soft limiter. PhoneLine::Player = an own WASAPI shared-mode render stream on the default
playback device (eRender/eConsole, mix format float / PCM 16 / 24 / 32, front L+R only, ~60 ms
queued, 5 ms polling on its own MTA thread; device lost -> reopened every 0.5-2 s; default device
changed -> moved within 2 s; 3 s watchdog without line_gain/keep -> ends by itself; close never
waits on the game thread, finished players are joined later). Lua: line_open() -> 1/0,
line_close(), line_gain(master[, muffle[, band]]) -> alive, line_play(id, key, gain, lowpassHz,
pitch[, loop[, age]]) -> 0 no line / 1 / 2 send the file / 3 unusable, line_set(id, gain, lp, pitch),
line_stop([id]), line_poll() -> {open, state, rate, channels, queuedMs, underruns, reopened, voices,
played, peak, seconds, error}. Recording / clip ear / export are unchanged.
Built with build-info/build_module_cross70.sh unchanged (the fix70 FFmpeg n8.1 SDKs), logs
module-fix73-x64.log / module-fix73-x86.log: 0 warnings; imports identical to fix70.
tests/line73/: Linux test of phone_line.h with a fake WASAPI render device (see its README.txt):
PASS with ASan/UBSan. The WASAPI output was NOT run on Windows / real hardware in this environment.

fix70 (build "fix70-cpu-fallback", encoder_api 2): no H.264 GPU encoder -> CPU path: the frame
(after FXAA) is copied to a staging texture and read back on the encoder worker, BGRA -> YUVJ420P
(BT.601 full range) and encoded with FFmpeg's mjpeg encoder (qscale 3-4, slice threads) into the
partial MP4 with the AAC track; after the stop transcodeH264() decodes it (mjpeg decoder, mov
demuxer), converts to YUV420P BT.709 limited range and encodes h264_mf (hw_encoding=0, rate_control
u_vbr, scenario archive), copies the AAC packets, movflags +faststart. status49() returns 5 values
(4th/5th = converted frames / total, -1 = no conversion), poll() processing/processed/processTotal.
zhphone.encoder(4) = CPU path first. D3D11 device is retried without VIDEO_SUPPORT.
FFmpeg n8.1 rebuilt for both: build-info/build_ffmpeg_cross70.sh (the fix69 configuration +
mjpeg encoder/decoder + mov demuxer, x86 assembly with nasm 2.16.03, without --enable-small,
nv-codec-headers n11.1.5.3 = NVENC API 11.1, NVIDIA driver 471.41+), module:
build-info/build_module_cross70.sh (module-fix70-x64.log / module-fix70-x86.log: 0 warnings).
tests/cpu70/test70.cpp: Linux test of the colour conversion and of transcodeH264 (MJPEG+AAC MP4 ->
MP4 with moov first; h264_mf replaced by FFmpeg's mpeg4 encoder there, Media Foundation exists only
on Windows): colours within 1 level of the BT.709 reference, 90 frames 720x1260 converted, AAC copied.
The CPU path and h264_mf were NOT run on Windows / real hardware in this environment.

fix69 (build "fix69-multigpu"): + h264_amf (AMD; BGRA D3D11 frames like NVENC) and h264_mf (Media
Foundation; BGRA->NV12 with the D3D11 video processor, src/gpu_nv12.h), tried in this order after
h264_nvenc (zhphone.encoder(pref), poll().encoder); + a 32-bit build gmcl_zhphone_win32.dll.
FFmpeg n8.1 rebuilt for both: build-info/build_ffmpeg_cross68.sh (ARCH=x86_64 / i686, the fix63
configuration + --enable-amf --enable-mediafoundation, AMF headers v1.4.36 in
third-party/source/AMF-headers-v1.4.36.tar.gz), module: build-info/build_module_cross68.sh
(logs module-fix69-x64.log / module-fix69-x86.log: 0 warnings). ffmpeg-sdk = x64 (suffix
-zhphone32), ffmpeg-sdk-x86 = 32-bit (suffix -zhphonex86). AMF / Media Foundation / the 32-bit
DLL were NOT run on real hardware in this environment.

Native module fix64-fxaa-prepared (gmcl_zhphone_win64.dll). API3, asynchronous GPU capture
(H.264 NVENC) + clip sound (fix62, src/audio_capture.h) + the phone's own ear (fix63,
src/phone_ear.h) + fix64:
 - FXAA 3.11 (quality preset 39, src/gpu_fxaa.h): D3D11 pixel shader on every recorded frame in
   the encoder thread, right before NVENC (full resolution, same bitrate); shaders compiled at run
   time with d3dcompiler_47.dll (LoadLibrary). Lua: fxaa([mode]) 0 off / 1 quality / 2 strong.
 - prepared session: prepare51(w,h,fps,rate) builds the D3D11 device, NVENC encoder, textures and
   FXAA while the camera is open; start(...) with the same size adopts it (no freeze at the record
   press); cancel_prepare51(), status49() -> ready, done, error. Shared textures are created one per
   frame (initStep), retired sessions are joined + released on the render thread later.
 - cheaper ClearRenderTargetView hook (only while the marker RT is bound);
 - phone ear: files decoded on the bank's own below-normal thread (ear_load returns 4 = busy,
   send again); ear_play(..., loop, age) places a late sound at its real start; 500 ms mix lag.
fix63 part:
 - microphone: WASAPI shared capture of the default Windows recording device;
 - game sound "what you hear": WASAPI process loopback of the Garry's Mod process tree
   (ActivateAudioInterfaceAsync "VAD\Process_Loopback", Windows 10 2004+, loaded at run time);
   optional fallback: loopback of the default playback device (whole PC);
 - fix63 phone ear: Lua reports every sound the game starts (ear_load(key, fileBytes),
   ear_play(id, key, gain, lowpassHz, pitch[, loop]), ear_set(...), ear_stop(id)); the files are
   decoded once (FFmpeg, memory AVIO: WAV PCM / MS-ADPCM / IMA-ADPCM with "cue " loop points,
   MP3, OGG Vorbis; <= 16 MB per file, 192 MB cache) on the bank thread (fix64) and mixed into the
   clip by the QPC time of the call: per-voice gain / 2-pole low-pass / pitch with 15 ms
   smoothing, loops, 30 ms stop fades, <= 96 voices, a master ear gain (audio(...,ear));
 - mixed to 48 kHz mono by capture timestamps (QPC) against the video clock, encoded with the
   FFmpeg native AAC encoder (AAC-LC 128 kbit/s) into the same MP4, plus a 24 kHz WAV copy
   for in-game playback. Lua API: start(...,audioFlags 1 mic / 2 game / 4 whole PC / 8 ear),
   capture(t,age), audio(mic,game,muffle[,ear]), ear_*, poll() -> audio, audioMic, audioGame,
   audioWav, audioAAC, audioError, earVoices, earPlayed, earPeak, earFiles, earBadFiles,
   earQueued, earMB ...  build = "fix64-fxaa-prepared", audio_api = 2, ear_api = 2, fxaa_api = 1.
No Source SDK interfaces/vtable offsets are used.

BUILD ON WINDOWS: MSVC 2022 Desktop C++, CMake 3.24+, git: run build_windows.bat.
The included ffmpeg-sdk matches the shipped av*-zhphone32 runtime DLLs (fix63 build:
the fix39 FFmpeg n8.1 configuration + --enable-encoder=aac + wav/mp3/ogg demuxers, pcm/adpcm/
mp3/vorbis decoders, mpegaudio/vorbis parsers), see build-info/.
Pinned garrysmod_common and MinHook sources are fetched by CMake; copies in third-party/source.

THE SHIPPED DLL was cross-built on Linux with llvm-mingw 20261006 UCRT, -O2 -g, static C++
runtime: build-info/build_ffmpeg_cross.sh, build-info/build_module_cross.sh,
build-info/module-fix64.log (0 warnings, rebuilt byte for byte), build-info/pe64.json
(SHA-256 + imports).
tests/audio: Linux test of the audio recorder with mocked WASAPI devices (ASan/UBSan PASS, fix64).
tests/ear:   Linux test of the phone ear: decoding + mixing + late sounds (ASan/UBSan PASS, fix64).
Windows/GMod gameplay, real WASAPI devices, NVENC and the FXAA shader on a GPU were NOT executed in this environment
(the FXAA HLSL was validated with DXC for SM 6.0 and its algorithm checked offline).
FFmpeg source, configure settings, NV headers and licenses are included (third-party/).
