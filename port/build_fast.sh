#!/bin/bash
set -e
export FFMPEG_STATIC_PREFIX=/build/ffmpeg6
mkdir -p /build/out
cd /build/out
cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo ../port
ninja -j 6
echo BUILD_OK
ls -la chiaki_brick
