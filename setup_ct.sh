#!/bin/bash
# 在 PVE 容器（Debian 13，privileged + apparmor unconfined）里重建整套 aarch64 构建环境。
# 用法：把本脚本和 port/ 目录一起放到容器 /root/ 下，然后 bash setup_ct.sh
set -e

CHROOT=/opt/arm64
BUILD=$CHROOT/build
SUBMODULES_NANOPB=160d4f09e5fabb2b66aa2dea32d4f38ace2c4b3f
SUBMODULES_JERASURE=505ccb4bc69eee8d7ea40a6089a056b99671134f
SUBMODULES_GFCOMPLETE=fa54a4670a5705c84abf6c24b92b0cd479625478
SUBMODULES_CURL=75a2079d5c28debb2eaa848ca9430f1fe0d7844c

echo "=== 1/7 安装宿主工具 ==="
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq qemu-user-static binfmt-support debootstrap curl git \
  cmake ninja-build pkg-config file python3 openssh-server

echo "=== 2/7 注册 qemu binfmt（容器需要 apparmor unconfined） ==="
mkdir -p /proc/sys/fs/binfmt_misc || true
mountpoint -q /proc/sys/fs/binfmt_misc || mount -t binfmt_misc binfmt_misc /proc/sys/fs/binfmt_misc
systemctl start systemd-binfmt || true

echo "=== 3/7 debootstrap arm64 bullseye ==="
if [ ! -d "$CHROOT/usr" ]; then
    printf 'deb https://mirrors.ustc.edu.cn/debian bullseye main contrib non-free\ndeb https://mirrors.ustc.edu.cn/debian bullseye-updates main contrib non-free\n' \
        > /tmp/bullseye.list
    debootstrap --arch=arm64 --foreign --variant=minbase \
        --include=ca-certificates bullseye "$CHROOT" https://mirrors.ustc.edu.cn/debian
    cp -f /usr/bin/qemu-aarch64-static "$CHROOT/usr/bin/" || true
    printf 'deb https://mirrors.ustc.edu.cn/debian bullseye main contrib non-free\ndeb https://mirrors.ustc.edu.cn/debian bullseye-updates main contrib non-free\n' \
        > "$CHROOT/etc/apt/sources.list"
    chroot "$CHROOT" /debootstrap/debootstrap --second-stage
fi
mountpoint -q "$CHROOT/proc" || mount -t proc proc "$CHROOT/proc"
mountpoint -q "$CHROOT/dev"  || mount --bind /dev "$CHROOT/dev"

echo "=== 4/7 chroot 里装编译依赖 ==="
DEBIAN_FRONTEND=noninteractive chroot "$CHROOT" apt-get update -qq
DEBIAN_FRONTEND=noninteractive chroot "$CHROOT" apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config git ca-certificates \
    python3 python3-protobuf protobuf-compiler \
    libsdl2-dev libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev \
    libopus-dev libssl-dev libudev-dev libasound2-dev libjson-c-dev libspeexdsp-dev \
    libevent-dev libcurl4-openssl-dev libminiupnpc-dev fonts-dejavu-core

echo "=== 5/7 chiaki-ng v1.10.1 + 子模块 ==="
mkdir -p "$BUILD"
if [ ! -d "$BUILD/chiaki-ng-1.10.1" ]; then
    curl -sSL https://codeload.github.com/chiaki-ng/chiaki-ng/tar.gz/refs/tags/v1.10.1 \
        | tar xz -C "$BUILD"
    T="$BUILD/chiaki-ng-1.10.1/third-party"
    curl -sSL https://codeload.github.com/nanopb/nanopb/tar.gz/$SUBMODULES_NANOPB        | tar xz --strip-components=1 -C "$T/nanopb"
    curl -sSL https://codeload.github.com/streetpea/jerasure/tar.gz/$SUBMODULES_JERASURE  | tar xz --strip-components=1 -C "$T/jerasure"
    curl -sSL https://codeload.github.com/streetpea/gf-complete/tar.gz/$SUBMODULES_GFCOMPLETE | tar xz --strip-components=1 -C "$T/gf-complete"
    curl -sSL https://codeload.github.com/curl/curl/tar.gz/$SUBMODULES_CURL            | tar xz --strip-components=1 -C "$T/curl"
fi

echo "=== 6/7 静态 FFmpeg 6.1（只带 h264/hevc + swscale） ==="
if [ ! -f "$BUILD/ffmpeg6/lib/libavcodec.a" ]; then
    mkdir -p "$BUILD/ffmpeg-src"
    curl -sSL https://codeload.github.com/FFmpeg/FFmpeg/tar.gz/refs/tags/n6.1.2 \
        | tar xz --strip-components=1 -C "$BUILD/ffmpeg-src"
    ( cd "$BUILD/ffmpeg-src" && \
      ./configure --prefix=/build/ffmpeg6 --arch=aarch64 --target-os=linux \
        --disable-everything --disable-programs --disable-doc --disable-debug \
        --disable-avdevice --disable-avfilter --disable-postproc --disable-network \
        --disable-iconv --disable-autodetect --disable-shared --enable-static --enable-pic \
        --enable-swscale --enable-swresample \
        --enable-decoder=h264,hevc --enable-parser=h264,hevc && \
      make -j6 && make install )
fi

echo "=== 7/7 编译掌机前端 ==="
mkdir -p "$BUILD/port"
cp -r /root/port/* "$BUILD/port/" 2>/dev/null || true
cp /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf "$BUILD/port/assets/font.ttf" 2>/dev/null || true
chroot "$CHROOT" /bin/bash /build/port/build.sh

echo
echo "完成，产物：$BUILD/out/chiaki_brick"
echo "取出来： cp $BUILD/out/chiaki_brick /root/  然后 scp"
