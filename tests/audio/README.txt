fix62 audio recorder test (Linux, no Windows needed)
shim/ = minimal mocks of windows.h / mmdeviceapi.h / audioclient.h: a fake microphone (clicks at
known times, 44.1 kHz float) and a fake process loopback (48 kHz sine), driven by the real
src/audio_capture.h. It checks: exact clip length, sound placement against the video clock,
AAC packets, WAV copy, cancel / no-zero / destroy / watchdog paths, MP4 (video + AAC) muxing.
Build with a Linux FFmpeg n8.1 (static, with the aac encoder, mpeg4 encoder and mp4 muxer):
  g++ -std=c++17 -O1 -g -fsanitize=address,undefined -Ishim -I$FF/include test.cpp -o test \
      $FF/lib/libavformat.a $FF/lib/libavcodec.a $FF/lib/libavutil.a -lpthread -lm
  ./test            (main run: prints done=1 ... OK, writes out.wav / out.mp4)
  ./test cancel ; ./test nozero ; ./test destroy ; ./test watchdog
Result of fix62: PASS (ASan/UBSan). The leak report of the main run comes from the test's own
MPEG-4 encoder (not freed at exit), not from audio_capture.h.
Result of fix64 (consume lag 500 ms, the ear bank on its own thread): PASS (ASan/UBSan), all 5 modes.
