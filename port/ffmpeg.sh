#!/bin/bash
set -e
cd /build
if [ ! -d ffmpeg-src ]; then
  mkdir -p ffmpeg-src
  curl -sSL https://codeload.github.com/FFmpeg/FFmpeg/tar.gz/refs/tags/n6.1.2 \
    | tar xz --strip-components=1 -C ffmpeg-src
fi
cd ffmpeg-src
./configure --prefix=/build/ffmpeg6 \
  --arch=aarch64 --target-os=linux \
  --disable-everything --disable-programs --disable-doc --disable-debug \
  --disable-avdevice --disable-avfilter --disable-postproc --disable-network \
  --disable-iconv --disable-autodetect --disable-shared --enable-static --enable-pic \
  --enable-swscale --enable-swresample \
  --enable-decoder=h264,hevc --enable-parser=h264,hevc \
  > /tmp/ff_conf.log 2>&1
make -j 6 > /tmp/ff_make.log 2>&1
make install > /tmp/ff_install.log 2>&1
echo FFMPEG_OK
ls -la /build/ffmpeg6/lib/*.a
