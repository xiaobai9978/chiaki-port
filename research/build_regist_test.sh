#!/bin/bash
# Build the regist lifecycle self-test inside the CT chroot, reusing the
# exact same link line as chiaki_brick (captured from build.ninja).
set -e
cd /build
mkdir -p /build/research
cp /build/test_regist_lifecycle.c /build/research/test_regist_lifecycle.c
sed -i 's/\r$//' /build/research/test_regist_lifecycle.c

INC="-I/build/port/src -I/usr/include/SDL2 \
     -I/build/chiaki-ng-1.10.1/lib/include -I/build/out/chiaki/lib/include \
     -I/build/chiaki-ng-1.10.1/third-party/curl/include \
     -I/build/chiaki-ng-1.10.1/third-party/nanopb \
     -I/build/chiaki-ng-1.10.1/third-party/jerasure/include \
     -I/build/chiaki-ng-1.10.1/third-party/gf-complete/include \
     -isystem /build/ffmpeg6/include -isystem /usr/include/json-c"

cd /build/out
gcc -O2 -std=gnu11 -Wno-error -DCURL_STATICLIB -DPB_C99_STATIC_ASSERT $INC \
    -c /build/research/test_regist_lifecycle.c -o /build/out/test_regist.o 2>&1

# Extract the exact link line from ninja instead of hand-copying library paths
LINK_LIBS=$(grep -m1 '^  LINK_LIBRARIES = ' /build/out/build.ninja | sed 's/^  LINK_LIBRARIES = //')
LINK_PATH=$(grep -m1 '^  LINK_PATH = ' /build/out/build.ninja | sed 's/^  LINK_PATH = //')

gcc /build/out/test_regist.o CMakeFiles/chiaki_brick.dir/src/hwdec.c.o \
    -o /build/out/test_regist $LINK_PATH $LINK_LIBS -pthread 2>&1
echo TEST_BUILD_OK
ls -la /build/out/test_regist
