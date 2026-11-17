fix63/fix64 phone-ear test (Linux, no Windows needed)
eartest.cpp drives the real src/phone_ear.h + src/audio_capture.h with no audio devices (the shim's
CoCreateInstance fails, so only the Ear source is mixed):
  ./eartest bank : decodes click.wav (PCM 16-bit), loop.wav (PCM + "cue " loop point at sample 1000),
                   adpcm.wav (MS-ADPCM), tone.mp3, tone.ogg (Vorbis); 100 bytes of junk -> refused (3)
  ./eartest      : a 3.8 s clip with voices started at known times: click placement against the video
                   clock, ADPCM, a file still decoding when it is started, a 500 Hz low-pass, a loop
                   until ear_stop, pitch 2 (twice as fast), master ear gain 0 (silent); writes ear.wav
                   (24 kHz copy) and checks the AAC packets.
Test sounds: generated with FFmpeg (sine tones / single-sample clicks); loop.wav got a "cue " chunk
by hand (dwSampleOffset 1000). They are tiny and included here.
Build with a Linux FFmpeg n8.1 (static: aac encoder, wav/mp3/ogg demuxers, pcm/adpcm/mp3/vorbis
decoders, mpegaudio/vorbis parsers, mp4 muxer):
  g++ -std=c++17 -O1 -g -fsanitize=address,undefined -Ishim -I$FF/include eartest.cpp -o eartest \
      $FF/lib/libavformat.a $FF/lib/libavcodec.a $FF/lib/libavutil.a -lpthread -lm
Result of fix63: PASS (ASan/UBSan). Onsets in ear.wav: 0.3161 / 0.8146 / 1.4093 / 2.6146 s =
exactly the printed play times.
fix64: the bank decodes on its own thread (the test waits until nothing is decoding) and earPlay
gets an age (seconds since the game started the sound): play7 with age 0.25 s must start 0.25 s
before its call. Result of fix64: PASS (ASan/UBSan): onsets 0.3028 / 1.4126 / 2.6030 / 2.6585 s =
play1 / play3 (file still decoding) / play5 / play7 - 0.25 s exactly; master gain 0 silent.
