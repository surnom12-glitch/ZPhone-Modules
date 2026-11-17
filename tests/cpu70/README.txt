fix70 CPU path test (Linux).
Build a static FFmpeg n8.1 with: --disable-everything --enable-encoder=mjpeg,aac,mpeg4
--enable-decoder=mjpeg,aac,mpeg4 --enable-demuxer=mov --enable-muxer=mp4 --enable-protocol=file
then: g++ -std=c++17 -O2 test70.cpp -I<ff>/include -L<ff>/lib -lavformat -lavcodec -lavutil -lpthread -lm
test70.cpp contains a copy of bgraToJ420 / j420To709 / transcodeH264 from src/module.cpp (h264_mf ->
mpeg4 with a 1/30000 time base, because Media Foundation exists only on Windows).
Result in this environment: colours within 1 level of the BT.709 reference; MJPEG write 90 frames
720x1260 in 0.71 s (2 cores), conversion OK (90 frames, AAC copied, moov before mdat).
