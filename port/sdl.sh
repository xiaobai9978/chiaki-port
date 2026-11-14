#!/bin/bash
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get install -y --no-install-recommends libdrm-dev libgbm-dev libasound2-dev > /tmp/sdl_apt.log 2>&1
cd /build
if [ ! -d SDL2-src ]; then
  mkdir -p SDL2-src
  curl -sSL https://codeload.github.com/libsdl-org/SDL/tar.gz/refs/tags/release-2.0.20 \
    | tar xz --strip-components=1 -C SDL2-src
fi
cd SDL2-src
./configure --prefix=/build/sdl2 \
  --disable-x11 --disable-wayland --disable-video-vulkan --disable-video-opengl \
  --enable-video-opengles2 --enable-video-kmsdrm --enable-video-fbdev --enable-video-dummy \
  --disable-pulseaudio --disable-esd --disable-pipewire --disable-jack --disable-libsamplerate \
  --disable-hidapi --disable-dbus --disable-ime --disable-fcitx --disable-ibus \
  --enable-alsa > /tmp/sdl_conf.log 2>&1
make -j 6 > /tmp/sdl_make.log 2>&1
make install > /tmp/sdl_install.log 2>&1
echo SDL_OK
ls -la /build/sdl2/lib/libSDL2-2.0.so.0
