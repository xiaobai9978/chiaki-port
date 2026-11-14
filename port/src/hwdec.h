// SPDX-License-Identifier: AGPL-3.0-only
#ifndef CHIAKI_BRICK_HWDEC_H
#define CHIAKI_BRICK_HWDEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Allwinner A133 cedar (libvdecoder.so) hardware decoder.
 * Output is converted from the decoder's NV12 frame into planar IYUV so the
 * existing SDL_UpdateYUVTexture path in stream.c can stay unchanged. */
typedef struct {
    int            w, h;          /* visible (cropped) size */
    const uint8_t *y, *u, *v;     /* planar IYUV buffers owned by hwdec (fallback) */
    int            ystride;       /* == w */
    int            uvstride;      /* == w / 2 */
    const uint8_t *nv12;         /* compact NV12: Y(w*h) then UV(w*h/2), row pitch w */
} HwFrame;

/* codec: 0 = H.264, 1 = H.265. want_w/want_h is the negotiated display size;
 * cedar pads height to a multiple of 32 (720 -> 736), we crop it back. */
bool        hwdec_init(int codec, int want_w, int want_h);
void        hwdec_fini(void);
bool        hwdec_active(void);
/* Feed one Annex-B access unit. Returns true when out contains a fresh frame.
 * The frame lives in hwdec's own buffer (offline tests / fallback). */
bool        hwdec_decode(const uint8_t *data, size_t len, HwFrame *out);
/* Zero-copy variant: the cropped NV12 frame is written straight into dst
 * (row pitch = returned *out_w, capacity dcap >= w*h*3/2), skipping hwdec's
 * internal buffer entirely. dst must NOT be realloc'd or freed while another
 * thread could still be uploading a previous frame from it. Returns false
 * (writing nothing) when dcap is too small — caller grows the buffer. */
bool        hwdec_decode_into(const uint8_t *data, size_t len,
                              uint8_t *dst, size_t dcap, int *out_w, int *out_h);
const char *hwdec_error(void);

#endif
