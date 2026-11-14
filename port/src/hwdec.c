// SPDX-License-Identifier: AGPL-3.0-only
/* Allwinner A133 cedar H.264/H.265 hardware decoder, loaded at runtime so the
 * build does not need any Allwinner headers or link-time libraries.
 *
 * The struct layouts below were verified against libvdecoder.so disassembly on
 * the handheld (sizeof(VConfig) MUST be 208: the library memcpy()s that many
 * bytes, and every field offset was checked against objdump). See
 * research/test_cedar2.c for the standalone probe this module grew out of. */
#include "hwdec.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VIDEO_CODEC_FORMAT_H264 0x115
#define VIDEO_CODEC_FORMAT_H265 0x116
#define FRAME_DECODED           1
#define KEYFRAME_DECODED        3
#define PIXEL_FORMAT_NV12       6

typedef struct {
    int   eCodecFormat;
    int   nWidth, nHeight;
    int   nFrameRate, nFrameDuration;
    int   nAspectRatio;
    int   bIs3DStream;
    int   nCodecSpecificDataLen;
    char *pCodecSpecificData;
    int   bSecureStreamFlag, bSecureStreamFlagLevel1, bIsFramePackage;
    int   h265ReferencePictureNum, bReOpenEngine, bIsFrameCtsTestFlag;
} VideoStreamInfo;

typedef struct {
    int bScaleDownEn, bRotationEn, bSecOutputEn;
    int nHorizonScaleDownRatio, nVerticalScaleDownRatio;
    int nSDWidth, nSDHeight, bAnySizeSD;
    int nSecHorizonScaleDownRatio, nSecVerticalScaleDownRatio;
    int nRotateDegree, bThumbnailMode;
    int eOutputPixelFormat, eSecOutputPixelFormat;
    int bNoBFrames, bDisable3D, bSupportMaf, bDispErrorFrame;
    int nVbvBufferSize, nFrameBufferNum;
    int bSecureosEn, bGpuBufValid, nAlignStride;
    int bIsSoftDecoderFlag, bVirMallocSbm;
    int bSupportPallocBufBeforeDecode;
    int nDeInterlaceHoldingFrameBufferNum, nDisplayHoldingFrameBufferNum;
    int nRotateHoldingFrameBufferNum, nDecodeSmoothFrameBufferNum;
    int bIsTvStream, bAdapteDropFrame;
    int nReserved0, nReserved1;
    void *memops;                       /* 136 */
    int eCtlAfbcMode, eCtlIptvMode;
    void *veOpsS, *pVeOpsSelf;         /* 152, 160 */
    int bConvertVp910bitTo8bit;
    unsigned int nVeFreq;
    int bCalledByOmxFlag;
    int bSetProcInfoEnable, nSetProcInfoFreq, nChannelNum;
    int nSupportMaxWidth, nSupportMaxHeight;
    int commonConfigFlag, bATMFlag;
} VConfig;

typedef struct {
    char     *pData;
    int       nLength;
    long long nPts, nPcr;
    int       bIsFirstPart, bIsLastPart;
    int       nID, nStreamIndex, bValid;
    unsigned int bVideoInfoFlag;
    void     *pVideoInfo;
} VideoStreamDataInfo;

typedef struct {
    int nID, nStreamIndex, ePixelFormat;
    int nWidth, nHeight, nLineStride;
    int nTopOffset, nLeftOffset, nBottomOffset, nRightOffset;
    int nFrameRate, nAspectRatio;
    int bIsProgressive, bTopFieldFirst, bRepeatTopField;
    long long nPts, nPcr;
    char *pData0, *pData1, *pData2, *pData3;
} PicHead;

/* ---- runtime-resolved libvdecoder API ---- */
static void *h_vdec, *h_mem;
static void (*fn_addplugin)(void);
static void *(*fn_create)(void);
static int  (*fn_init)(void *, VideoStreamInfo *, VConfig *);
static int  (*fn_reqbuf)(void *, int, char **, int *, char **, int *, int);
static int  (*fn_submit)(void *, VideoStreamDataInfo *, int);
static int  (*fn_decode)(void *, int, int, int, long long);
static PicHead *(*fn_reqpic)(void *, int);
static int  (*fn_retpic)(void *, PicHead *);
static void (*fn_destroy)(void *);
static void *(*fn_getops)(void);

static void     *dec;
static void     *memops_handle;   /* struct ScMemOpsS *，退出时要 memops->close() */
static uint8_t  *nvbuf;
static size_t    nvcap;
static int       want_w, want_h;
static long long pts;
static bool      active;
static char      errmsg[256];
static int       n_ok, n_fail;

#define NEED(sym, var, type) do {                                            \
        void *p_ = dlsym(h_vdec, sym);                                       \
        if (!p_) { snprintf(errmsg, sizeof(errmsg), "dlsym %s: %s",        \
                            sym, dlerror()); close_all(); return false; }   \
        var = (type)p_;                                                      \
    } while (0)

/* CdcMemClose() in the SDK is just memops->close(), which is fp[2]
 * (fp[0]=open, fp[1]=open2, fp[2]=close). The demo calls it on exit. */
static void mem_close(void)
{
    if (!memops_handle) return;
    void **fp = (void **)memops_handle;
    if (fp[2]) {
        void (*closefn)(void) = (void (*)(void))fp[2];
        closefn();
    }
    memops_handle = NULL;
}

static void close_all(void)
{
    if (dec && fn_destroy) fn_destroy(dec);
    dec = NULL;
    mem_close();
    free(nvbuf);
    nvbuf = NULL;
    nvcap = 0;
    if (h_vdec) dlclose(h_vdec);
    if (h_mem)  dlclose(h_mem);
    h_vdec = h_mem = NULL;
    active = false;
}

bool hwdec_init(int codec, int w, int h)
{
    if (active) hwdec_fini();
    errmsg[0] = '\0';
    n_ok = n_fail = 0;
    pts = 0;
    want_w = w > 0 ? w : 1280;
    want_h = h > 0 ? h : 720;

    h_vdec = dlopen("/usr/lib/libvdecoder.so", RTLD_NOW);
    if (!h_vdec) { snprintf(errmsg, sizeof(errmsg), "dlopen libvdecoder: %s", dlerror()); return false; }
    h_mem = dlopen("/usr/lib/libMemAdapter.so", RTLD_NOW);
    if (!h_mem) { snprintf(errmsg, sizeof(errmsg), "dlopen libMemAdapter: %s", dlerror()); close_all(); return false; }

    NEED("AddVDPlugin",               fn_addplugin, void (*)(void));
    NEED("CreateVideoDecoder",        fn_create,     void *(*)(void));
    NEED("InitializeVideoDecoder",    fn_init,       int (*)(void *, VideoStreamInfo *, VConfig *));
    NEED("RequestVideoStreamBuffer",  fn_reqbuf,     int (*)(void *, int, char **, int *, char **, int *, int));
    NEED("SubmitVideoStreamData",     fn_submit,     int (*)(void *, VideoStreamDataInfo *, int));
    NEED("DecodeVideoStream",         fn_decode,     int (*)(void *, int, int, int, long long));
    NEED("RequestPicture",            fn_reqpic,     PicHead *(*)(void *, int));
    NEED("ReturnPicture",             fn_retpic,     int (*)(void *, PicHead *));
    fn_destroy = (void (*)(void *))dlsym(h_vdec, "DestroyVideoDecoder");
    fn_getops  = (void *(*)(void))dlsym(h_mem, "MemAdapterGetOpsS");
    if (!fn_getops) { snprintf(errmsg, sizeof(errmsg), "dlsym MemAdapterGetOpsS failed"); close_all(); return false; }

    /* CdcMemOpen is a static inline in the SDK; calling memops->open() directly
     * is exactly what it expands to. libvdecoder calls it again internally. */
    void *ops = fn_getops();
    if (!ops) { snprintf(errmsg, sizeof(errmsg), "MemAdapterGetOpsS returned NULL"); close_all(); return false; }
    memops_handle = ops;
    void **fp = (void **)ops;
    if (fp[0]) {
        int (*openfn)(void) = (int (*)(void))fp[0];
        if (openfn() < 0) { snprintf(errmsg, sizeof(errmsg), "memops->open failed"); close_all(); return false; }
    }

    fn_addplugin();
    dec = fn_create();
    if (!dec) { snprintf(errmsg, sizeof(errmsg), "CreateVideoDecoder failed"); close_all(); return false; }

    VideoStreamInfo info;
    memset(&info, 0, sizeof(info));
    info.eCodecFormat   = (codec == 1) ? VIDEO_CODEC_FORMAT_H265 : VIDEO_CODEC_FORMAT_H264;
    info.nWidth         = want_w;
    info.nHeight        = want_h;
    info.nFrameRate     = 60;
    info.nFrameDuration = 16666;

    VConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.eOutputPixelFormat = PIXEL_FORMAT_NV12;
    cfg.nDisplayHoldingFrameBufferNum = 3;
    cfg.nDecodeSmoothFrameBufferNum  = 0;
    cfg.nDeInterlaceHoldingFrameBufferNum = 0;
    cfg.bDisable3D = 1;
    /* 缩放比例语义（cedar）：0=不缩放、1=1/2、2=1/4。之前填了 1，
     * H.264 插件真的按 1/2 解（960x540 出 480x288，底部 16 行越界未初始化
     * → 屏幕最下方一条绿边，画面也一直是半分辨率）。H.265 插件忽略
     * 这两个字段，所以只有 H.264 出问题。必须显式置 0。 */
    cfg.nHorizonScaleDownRatio  = 0;
    cfg.nVerticalScaleDownRatio = 0;
    cfg.memops = ops;
    cfg.nFrameBufferNum = 6;
    cfg.nAlignStride = 32;
    cfg.bDispErrorFrame = 1;

    if (sizeof(VConfig) != 208) {
        snprintf(errmsg, sizeof(errmsg), "VConfig layout broken: sizeof=%zu", sizeof(VConfig));
        close_all();
        return false;
    }
    if (fn_init(dec, &info, &cfg) != 0) {
        snprintf(errmsg, sizeof(errmsg), "InitializeVideoDecoder failed");
        close_all();
        return false;
    }

    active = true;
    fprintf(stderr, "hwdec: cedar %s init ok (%dx%d)\n",
            codec == 1 ? "H.265" : "H.264", want_w, want_h);
    return true;
}

void hwdec_fini(void)
{
    if (!active) return;
    fprintf(stderr, "hwdec: frames ok=%d fail=%d\n", n_ok, n_fail);
    close_all();
}

bool hwdec_active(void) { return active; }
const char *hwdec_error(void) { return errmsg; }

/* Copy cedar's two NV12 planes into one compact buffer. SDL 2.0.16+ can upload
 * NV12 with two independent plane pointers (SDL_UpdateNVTexture), so no
 * per-pixel U/V de-interleave is needed any more: ~3-4ms saved per 720p frame.
 * cedar pads height to a multiple of 32 (720 -> 736); we crop it back. */
static bool crop_into(const PicHead *p, uint8_t *dst, size_t dcap,
                      int *out_w, int *out_h)
{
    int stride = p->nLineStride;
    /* 绝不放大：某插件/边界帧可能给出异常小的 nWidth/nHeight（例如解码器
     * 内部缩放后忘了同步），取 min 后画面只会变小、不会把野内存当像素。 */
    int vw = (want_w < p->nWidth)  ? want_w : p->nWidth;
    int vh = (want_h < p->nHeight) ? want_h : p->nHeight;
    vw &= ~1; vh &= ~1;                     /* NV12 needs even dimensions */
    if (vw < 16 || vh < 16 || stride < vw || !p->pData0 || !p->pData1)
        return false;

    size_t ys = (size_t)vw * (size_t)vh;
    size_t nsz = ys + ys / 2;               /* Y plane + interleaved UV */
    if (nsz > dcap)
        return false;

    const uint8_t *yp  = (const uint8_t *)p->pData0;
    const uint8_t *uvp = (const uint8_t *)p->pData1;
    for (int j = 0; j < vh; j++)
        memcpy(dst + (size_t)j * vw, yp + (size_t)j * stride, (size_t)vw);
    uint8_t *uvdst = dst + ys;
    for (int j = 0; j < vh / 2; j++)
        memcpy(uvdst + (size_t)j * vw, uvp + (size_t)j * stride, (size_t)vw);

    *out_w = vw; *out_h = vh;
    return true;
}

static bool copy_frame(const PicHead *p, HwFrame *out)
{
    /* 容量按对齐后高度估算，一次备足，避免逐帧 realloc */
    size_t need = (size_t)want_w * (size_t)((want_h + 31) & ~31) * 3 / 2;
    if (need > nvcap) {
        uint8_t *n = realloc(nvbuf, need);
        if (!n) return false;
        nvbuf = n;
        nvcap = need;
    }
    int vw, vh;
    if (!crop_into(p, nvbuf, nvcap, &vw, &vh))
        return false;
    out->w = vw; out->h = vh;
    out->y = nvbuf;              /* Y region, for diagnostics */
    out->u = out->v = NULL;      /* no planar U/V any more */
    out->ystride = vw; out->uvstride = vw / 2;
    out->nv12 = nvbuf;
    return true;
}

/* Submit one AU, run the decoder, hand the freshest displayable picture to
 * `consume` while it is still checked out from cedar. */
static bool decode_newest(const uint8_t *data, size_t len,
                          bool (*consume)(const PicHead *p, void *user),
                          void *user)
{
    if (!active || !data || len == 0 || len > (size_t)1u << 20) return false;

    char *buf = NULL, *ring = NULL;
    int bufsz = 0, ringsz = 0;
    if (fn_reqbuf(dec, (int)len, &buf, &bufsz, &ring, &ringsz, 0) != 0 || !buf) {
        n_fail++;
        return false;
    }
    /* SBM is a ring buffer: split the AU when it wraps. */
    if ((int)len <= bufsz) {
        memcpy(buf, data, len);
    } else if (ring && bufsz + ringsz >= (int)len) {
        memcpy(buf, data, (size_t)bufsz);
        memcpy(ring, data + bufsz, len - (size_t)bufsz);
    } else {
        n_fail++;
        return false;
    }

    VideoStreamDataInfo di;
    memset(&di, 0, sizeof(di));
    di.pData = buf;
    di.nLength = (int)len;
    di.bIsFirstPart = 1;
    di.bIsLastPart = 1;
    di.bValid = 1;
    di.nPts = pts++;
    if (fn_submit(dec, &di, 0) != 0) { n_fail++; return false; }

    bool got = false;
    for (int k = 0; k < 16; k++) {
        int dr = fn_decode(dec, 0, 0, 0, 0);
        if (dr != FRAME_DECODED && dr != KEYFRAME_DECODED) {
            if (dr == 0 || dr == 5 || dr == 2 || dr == -1) break;
            continue;
        }
        /* 排空显示队列、只渲染最新一帧。所有借出的帧都必须归还，
         * 否则 cedar 的 fbm 会慢慢耗尽；不设 8 帧上限，只留防异常死循环。 */
        PicHead *newest = NULL, *p;
        int guard = 0;
        while (guard++ < 64 && (p = fn_reqpic(dec, 0)) != NULL) {
            if (newest) fn_retpic(dec, newest);
            newest = p;
        }
        if (newest) {
            if (consume(newest, user)) got = true;
            fn_retpic(dec, newest);
        }
        if (got) break;
    }

    if (got) {
        n_ok++;
    } else {
        n_fail++;
    }
    return got;
}

struct into_ctx {
    uint8_t *dst;
    size_t   dcap;
    int      w, h;
};

static bool consume_into(const PicHead *p, void *user)
{
    struct into_ctx *c = (struct into_ctx *)user;
    if (!crop_into(p, c->dst, c->dcap, &c->w, &c->h))
        return false;
    if ((n_ok % 300) == 0)
        fprintf(stderr, "hwdec: decoded %d frames (%dx%d)\n", n_ok, c->w, c->h);
    return true;
}

bool hwdec_decode_into(const uint8_t *data, size_t len,
                       uint8_t *dst, size_t dcap, int *out_w, int *out_h)
{
    struct into_ctx c = { dst, dcap, 0, 0 };
    if (!decode_newest(data, len, consume_into, &c))
        return false;
    *out_w = c.w;
    *out_h = c.h;
    return true;
}

struct frame_ctx { HwFrame *out; };

static bool consume_frame(const PicHead *p, void *user)
{
    struct frame_ctx *c = (struct frame_ctx *)user;
    if (!copy_frame(p, c->out))
        return false;
    if ((n_ok % 300) == 0)
        fprintf(stderr, "hwdec: decoded %d frames (%dx%d)\n", n_ok,
                c->out->w, c->out->h);
    return true;
}

bool hwdec_decode(const uint8_t *data, size_t len, HwFrame *out)
{
    struct frame_ctx c = { out };
    return decode_newest(data, len, consume_frame, &c);
}
