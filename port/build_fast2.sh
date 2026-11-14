#!/bin/bash
set -e
export FFMPEG_STATIC_PREFIX=/build/devlibs
mkdir -p /build/out
cd /build/out
cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo ../port
ninja -j 6
echo BUILD_OK
objdump -p chiaki_brick | grep NEEDED
