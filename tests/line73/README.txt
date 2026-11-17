fix73 phone-line test (Linux, no Windows needed)
linetest.cpp drives the real src/phone_line.h (Mixer + Player) with the real PhoneEar bank (FFmpeg
decoding of WAV test tones generated in memory) and a fake WASAPI render device (shim/ = minimal
windows.h / mmdeviceapi.h / audioclient.h with IAudioRenderClient, the fake device is in linetest.cpp:
it consumes frames in real time and records what the line wrote, per channel).
  ./linetest mixer  : phone band 0/1/2 (100 Hz / 1 kHz / 10 kHz tones), voice low-pass, pocket muffle,
                      pitch 2, a short sound ends by itself, stop + stopAll fade (30 ms), smoothed gain
                      changes, age (late start in the middle of the file), unusable / unknown files,
                      96-voice cap (quietest replaced), soft limiter, master 0 + smoothing, NaN / inf
                      arguments, 44.1 / 96 kHz outputs keep the pitch
  ./linetest player : (runs the mixer part first) 48 kHz stereo float, 44.1 kHz 5.1 int16 (centre /
                      LFE / rear stay silent), 96 kHz 24-bit, 48 kHz mono int32: level, left = right,
                      no underruns, ~55 ms queued, close < 100 ms; device lost -> reopened and playing;
                      default device changed -> moved; watchdog (no keep for 3 s -> ends); no device /
                      no Windows audio -> state 2, voices still age, a device appearing later is used;
                      destroyed while playing
Build with the Linux FFmpeg n8.1 (static: wav demuxer + pcm_s16le decoder are enough):
  g++ -std=c++17 -O1 -g -fsanitize=address,undefined -Ishim -I$FF/include linetest.cpp -o linetest \
      $FF/lib/libavformat.a $FF/lib/libavcodec.a $FF/lib/libavutil.a -lpthread -lm
Result of fix73: ALL OK (ASan/UBSan, no reports). band 1: 100 Hz 0.202 / 1 kHz 0.353 / 10 kHz 0.129,
band 2: 0.004 / 0.348 / 0.003 (rms of a 0.354 tone).
