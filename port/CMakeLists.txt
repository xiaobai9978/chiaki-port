cmake_minimum_required(VERSION 3.13)
project(chiaki_brick C)

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# gcc 10 in this sysroot is noisy about strncpy; do not let warnings fail the build
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-error")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-error")

# --- configure chiaki-ng: library only, no Qt GUI / CLI / Switch frontend ---
set(CHIAKI_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_CLI OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_GUI OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_BOREALIS OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_STEAMDECK_NATIVE OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_SETSU OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_RUDP OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_SPEEX OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_STEAM_SHORTCUT OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_FFMPEG_DECODER ON CACHE BOOL "" FORCE)
set(CHIAKI_USE_SYSTEM_JERASURE OFF CACHE STRING "" FORCE)
set(CHIAKI_USE_SYSTEM_NANOPB OFF CACHE STRING "" FORCE)
set(CHIAKI_USE_SYSTEM_CURL OFF CACHE STRING "" FORCE)

# keep the bundled libcurl small: no gssapi/krb5/psl/zstd/brotli/ssh2/ldap
set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
set(CURL_USE_GSSAPI OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
set(CURL_USE_KERBEROS OFF CACHE BOOL "" FORCE)
set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
set(USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_LDAPS ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_SMB ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_TELNET ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_DICT ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_GOPHER ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_FTP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_IMAP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_POP3 ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_SMTP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_RTSP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_PROXY OFF CACHE BOOL "" FORCE)

# Self-built static FFmpeg 6.1: must precede the chiaki subdirectory so that
# its find_package(FFMPEG) resolves to these libs and headers.
if(DEFINED ENV{FFMPEG_STATIC_PREFIX})
    set(ENV{PKG_CONFIG_PATH} "$ENV{FFMPEG_STATIC_PREFIX}/lib/pkgconfig")
    link_directories("$ENV{FFMPEG_STATIC_PREFIX}/lib")
    set(FFMPEG_ABS_LIBS "")
    file(GLOB _avc "$ENV{FFMPEG_STATIC_PREFIX}/libavcodec.so.6*"
                    "$ENV{FFMPEG_STATIC_PREFIX}/libavutil.so.5*"
                    "$ENV{FFMPEG_STATIC_PREFIX}/libavformat.so.6*"
                    "$ENV{FFMPEG_STATIC_PREFIX}/libswscale.so.7*"
                    "$ENV{FFMPEG_STATIC_PREFIX}/libswresample.so.4*")
    set(FFMPEG_ABS_LIBS ${_avc})
    message(STATUS "Using FFmpeg from $ENV{FFMPEG_STATIC_PREFIX}: ${FFMPEG_ABS_LIBS}")
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../chiaki-ng-1.10.1
                 ${CMAKE_BINARY_DIR}/chiaki EXCLUDE_FROM_ALL)

find_package(PkgConfig REQUIRED)

pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(SWSCALE REQUIRED libswscale)
pkg_check_modules(AVUTIL REQUIRED libavutil)

add_executable(chiaki_brick
    src/main.c
    src/stream.c
    src/hwdec.c
    src/ui.c
)

target_include_directories(chiaki_brick PRIVATE
    ${SDL2_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/../chiaki-ng-1.10.1/lib/include
)
target_link_directories(chiaki_brick PRIVATE
    ${SDL2_LIBRARY_DIRS}
    ${SWSCALE_LIBRARY_DIRS}
    ${AVUTIL_LIBRARY_DIRS}
)
target_link_libraries(chiaki_brick
    ${FFMPEG_ABS_LIBS}
    chiaki-lib
    ${SDL2_LIBRARIES}
    ${SWSCALE_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    m pthread dl
)
