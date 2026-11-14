# FFmpeg, Bink only

Static libraries of FFmpeg n7.1 (LGPL 2.1, COPYING.LGPLv2.1) with only the
Bink demuxer, Bink video and Bink audio decoders and libswresample, built
for i686-w64-mingw32:

    configure --enable-cross-compile --arch=x86 --target-os=mingw32 \
      --cross-prefix=i686-w64-mingw32- --disable-everything --disable-programs \
      --disable-doc --disable-network --disable-autodetect --disable-x86asm \
      --disable-avdevice --disable-avfilter --disable-postproc --disable-swscale \
      --enable-static --disable-shared --enable-decoder=bink,binkaudio_rdft,binkaudio_dct \
      --enable-demuxer=bink --enable-protocol=file --enable-swresample --disable-debug

Used by desktop/src/movie_player.cpp.
