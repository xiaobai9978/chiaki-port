#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <time.h>
#include <stddef.h>

typedef struct {
    int eCodecFormat;
    int nWidth, nHeight;
    int nFrameRate, nFrameDuration;
    int nAspectRatio;
    int bIs3DStream;
    int nCodecSpecificDataLen;
    char *pCodecSpecificData;
    int bSecureStreamFlag, bSecureStreamFlagLevel1, bIsFramePackage;
    int h265ReferencePictureNum, bReOpenEngine, bIsFrameCtsTestFlag;
} VideoStreamInfo;

/* VConfig —— 按掌机 libvdecoder.so 反汇编实测的偏移重建（sizeof 必须 = 208）。
 * libvdecoder 会 memcpy(sizeof(VConfig)=208) 整个结构，旧定义只有 184 字节，
 * 尾部 24 字节会读到调用者栈上的随机数据，且 veOpsS/pVeOpsSelf 等字段全部错位。
 * 实测偏移：bThumbnailMode 44 / bSecureosEn 80 / nAlignStride 88 /
 *   nDeInterlaceHolding 104 / nDisplayHolding 108 / nDecodeSmooth 116 /
 *   bIsTvStream 120 / memops 136 / veOpsS 152 / pVeOpsSelf 160 /
 *   nVeFreq 172 / bSetProcInfoEnable 180 / nSetProcInfoFreq 184 / bATMFlag 204。
 */
typedef struct {
    int bScaleDownEn, bRotationEn, bSecOutputEn;                    /* 0,4,8 */
    int nHorizonScaleDownRatio, nVerticalScaleDownRatio;             /* 12,16 */
    int nSDWidth, nSDHeight, bAnySizeSD;                             /* 20,24,28 */
    int nSecHorizonScaleDownRatio, nSecVerticalScaleDownRatio;        /* 32,36 */
    int nRotateDegree, bThumbnailMode;                               /* 40,44 */
    int eOutputPixelFormat, eSecOutputPixelFormat;                    /* 48,52 */
    int bNoBFrames, bDisable3D, bSupportMaf, bDispErrorFrame;         /* 56..68 */
    int nVbvBufferSize, nFrameBufferNum;                              /* 72,76 */
    int bSecureosEn, bGpuBufValid, nAlignStride;                      /* 80,84,88 */
    int bIsSoftDecoderFlag, bVirMallocSbm;                            /* 92,96 */
    int bSupportPallocBufBeforeDecode;                                /* 100 */
    int nDeInterlaceHoldingFrameBufferNum, nDisplayHoldingFrameBufferNum; /* 104,108 */
    int nRotateHoldingFrameBufferNum, nDecodeSmoothFrameBufferNum;    /* 112,116 */
    int bIsTvStream, bAdapteDropFrame;                                /* 120,124 */
    int nReserved0, nReserved1;                                       /* 128,132 (掌机版本多出 8 字节) */
    void *memops;                                                     /* 136 */
    int eCtlAfbcMode, eCtlIptvMode;                                   /* 144,148 */
    void *veOpsS, *pVeOpsSelf;                                        /* 152,160 */
    int bConvertVp910bitTo8bit;                                       /* 168 */
    unsigned int nVeFreq;                                             /* 172 */
    int bCalledByOmxFlag;                                             /* 176 */
    int bSetProcInfoEnable, nSetProcInfoFreq, nChannelNum;             /* 180,184,188 */
    int nSupportMaxWidth, nSupportMaxHeight;                          /* 192,196 */
    int commonConfigFlag, bATMFlag;                                   /* 200,204 */
} VConfig;

typedef struct {
    char *pData;
    int nLength;
    long long nPts, nPcr;
    int bIsFirstPart, bIsLastPart;
    int nID, nStreamIndex, bValid;
    unsigned int bVideoInfoFlag;
    void *pVideoInfo;
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

#define FRAME_DECODED 1
#define KEYFRAME_DECODED 3

/* 找下一个 Annex-B 起始码，返回 NAL 数据起点（跳过起始码），*sc 返回起始码长度 */
static long find_start_code(const unsigned char *d, long n, long from, int *sc)
{
    for (long i = from; i + 3 < n; i++) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) { *sc = 3; return i + 3; }
        if (i + 4 < n && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 && d[i + 3] == 1) {
            *sc = 4; return i + 4;
        }
    }
    return -1;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { printf("usage: %s <file.h264|.h265>\n", argv[0]); return 1; }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("cannot open %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *data = malloc((size_t)fsz);
    if (fread(data, 1, (size_t)fsz, f) != (size_t)fsz) return 1;
    fclose(f);
    printf("input %s: %ld bytes\n", argv[1], fsz);

    int is_h265 = strstr(argv[1], "265") != NULL;

    void *h = dlopen("/usr/lib/libvdecoder.so", RTLD_NOW);
    if (!h) { printf("dlopen FAILED: %s\n", dlerror()); return 1; }
    void (*addplugin)(void) = (void (*)(void))dlsym(h, "AddVDPlugin");
    void *(*create)(void) = (void *(*)(void))dlsym(h, "CreateVideoDecoder");
    int (*init)(void *, VideoStreamInfo *, VConfig *) = (int (*)(void *, VideoStreamInfo *, VConfig *))dlsym(h, "InitializeVideoDecoder");
    int (*reqbuf)(void *, int, char **, int *, char **, int *, int) = (int (*)(void *, int, char **, int *, char **, int *, int))dlsym(h, "RequestVideoStreamBuffer");
    int (*submit)(void *, VideoStreamDataInfo *, int) = (int (*)(void *, VideoStreamDataInfo *, int))dlsym(h, "SubmitVideoStreamData");
    int (*decode)(void *, int, int, int, long long) = (int (*)(void *, int, int, int, long long))dlsym(h, "DecodeVideoStream");
    PicHead *(*reqpic)(void *, int) = (PicHead * (*)(void *, int))dlsym(h, "RequestPicture");
    int (*retpic)(void *, PicHead *) = (int (*)(void *, PicHead *))dlsym(h, "ReturnPicture");
    int (*relpic)(void *, PicHead *, int) = (int (*)(void *, PicHead *, int))dlsym(h, "ReturnRelasePicture");
    printf("retpic=%p relpic=%p\n", (void *)retpic, (void *)relpic);

    addplugin();
    void *dec = create();
    if (!dec) { printf("create failed\n"); return 2; }

    VideoStreamInfo info;
    memset(&info, 0, sizeof(info));
    info.eCodecFormat = is_h265 ? 0x116 : 0x115;
    info.nWidth = 1280; info.nHeight = 720;
    info.nFrameRate = 60; info.nFrameDuration = 16666;

    /* 照抄 demo：memops 必须拿到并 CdcMemOpen，否则 ion 池不开，帧缓冲用不了 */
    void *hm = dlopen("/usr/lib/libMemAdapter.so", RTLD_NOW);
    void *(*getops)(void) = hm ? (void *(*)(void))dlsym(hm, "MemAdapterGetOpsS") : NULL;
    void *ops = getops ? getops() : NULL;
    void *hc = dlopen("/usr/lib/libcdc_base.so", RTLD_NOW);
    void (*memopen)(void *) = hc ? (void (*)(void *))dlsym(hc, "CdcMemOpen") : NULL;
    printf("memops=%p CdcMemOpen=%p sizeof(VConfig)=%zu\n", ops, (void *)memopen, sizeof(VConfig));
    printf("VConfig offsets: memops=%zu veOpsS=%zu pVeOpsSelf=%zu "
           "bSetProcInfoEnable=%zu bThumbnailMode=%zu size=%zu\n",
           offsetof(VConfig, memops), offsetof(VConfig, veOpsS),
           offsetof(VConfig, pVeOpsSelf), offsetof(VConfig, bSetProcInfoEnable),
           offsetof(VConfig, bThumbnailMode), sizeof(VConfig));
    if (memopen) {
        memopen(ops);
    } else if (ops) {
        /* CdcMemOpen 在设备上是宏，等价于 memops->open()，直接调函数指针 */
        void **fp = (void **)ops;
        if (fp[0]) {
            int (*openfn)(void) = (int (*)(void))fp[0];
            printf("calling memops->open() @%p\n", (void *)openfn);
            printf("memops->open() = %d\n", openfn());
        }
    }

    VConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.eOutputPixelFormat = 6;      /* NV12，demo 用 NV21(5)，这里取成对上面的 */
    cfg.nDisplayHoldingFrameBufferNum = 3;
    cfg.nDecodeSmoothFrameBufferNum = 0;
    cfg.nDeInterlaceHoldingFrameBufferNum = 0;
    cfg.bDisable3D = 1;
    cfg.nHorizonScaleDownRatio = 1;
    cfg.nVerticalScaleDownRatio = 1;
    cfg.memops = ops;
    cfg.nFrameBufferNum = 6;
    cfg.nAlignStride = 32;
    cfg.bDispErrorFrame = 1;

    int r = init(dec, &info, &cfg);
    printf("InitializeVideoDecoder = %d\n", r);

    /* 按 Annex-B 切分访问单元：连续的 SPS/PPS/SEI + 一个 VCL NAL 算一帧 */
    long pos = 0, au_start = 0;
    int have_vcl = 0, aus = 0;
    int frames = 0;
    int dr_cnt[9] = {0};
    long long pts = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    while (pos < fsz) {
        int sc = 0;
        long nal = find_start_code(data, fsz, pos, &sc);
        if (nal < 0) {
            /* 最后一帧 */
            if (have_vcl) {
                int len = (int)(fsz - au_start);
                char *buf = NULL, *ring = NULL;
                int bufsz = 0, ringsz = 0;
                if (reqbuf(dec, len, &buf, &bufsz, &ring, &ringsz, 0) == 0 && buf) {
                    memcpy(buf, data + au_start, (size_t)len);
                    /* bIsFirstPart/bIsLastPart 都要置 NULL，表示这是完整的一帧 */
                    VideoStreamDataInfo di;
                    memset(&di, 0, sizeof(di));
                    di.pData = buf; di.nLength = len;
                    di.bIsFirstPart = 1; di.bIsLastPart = 1;
                    di.bValid = 1; di.nPts = pts++;
                    submit(dec, &di, 0);
                    aus++;
                }
            }
            break;
        }

        int type = is_h265 ? ((data[nal] >> 1) & 0x3f) : (data[nal] & 0x1f);
        int is_vcl = is_h265 ? (type <= 31) : (type == 1 || type == 5);

        if (is_vcl && have_vcl) {
            /* 上一帧在这里结束 */
            if (au_start == 0) au_start = 0;
            int len = (int)(nal - sc - au_start);
            if (len > 0) {
                char *buf = NULL, *ring = NULL;
                int bufsz = 0, ringsz = 0;
                if (reqbuf(dec, len, &buf, &bufsz, &ring, &ringsz, 0) == 0 && buf) {
                    memcpy(buf, data + au_start, (size_t)len);
                    VideoStreamDataInfo di;
                    memset(&di, 0, sizeof(di));
                    di.pData = buf; di.nLength = len;
                    di.bIsFirstPart = 1; di.bIsLastPart = 1;
                    di.bValid = 1; di.nPts = pts++;
                    submit(dec, &di, 0);
                    aus++;
                }
            }
            au_start = nal - sc;
        }
        if (is_vcl) have_vcl = 1;

        /* 提交一帧就立刻尝试取帧。返回 5/2 只表示这次没新帧，不该 break */
        if (aus > 0) {
            for (int k = 0; k < 16; k++) {
                int dr = decode(dec, 0, 0, 0, 0);
                if (dr >= -1 && dr <= 7) dr_cnt[dr + 1]++;
                if (dr == FRAME_DECODED || dr == KEYFRAME_DECODED) {
                    PicHead *p;
                    int drained = 0;
                    while ((p = reqpic(dec, 0)) != NULL) {
                        frames++;
                        drained = 1;
                        if (frames == 1) {
                            printf("FIRST FRAME: %dx%d stride=%d fmt=%d\n",
                                   p->nWidth, p->nHeight, p->nLineStride, p->ePixelFormat);
                            printf("FRAME PTRS: Y=%p UV=%p p2=%p p3=%p\n",
                                   (void *)p->pData0, (void *)p->pData1,
                                   (void *)p->pData2, (void *)p->pData3);
                            if (p->pData0 && p->pData1 && p->nLineStride > 0) {
                                size_t ysz = (size_t)p->nLineStride * (size_t)p->nHeight;
                                size_t uvsz = ysz / 2;
                                unsigned char *yp = (unsigned char *)p->pData0;
                                long acc = 0;
                                for (size_t i = 0; i < ysz; i += 97) acc += yp[i];
                                printf("Y checksum(sample)=%ld first8=%02x %02x %02x %02x\n",
                                       acc, yp[0], yp[1], yp[2], yp[3]);
                                FILE *fp = fopen("/tmp/first.nv12", "wb");
                                if (fp) {
                                    fwrite(p->pData0, 1, ysz, fp);
                                    fwrite(p->pData1, 1, uvsz, fp);
                                    fclose(fp);
                                    printf("saved /tmp/first.nv12 (%zu bytes)\n", ysz + uvsz);
                                }
                            }
                        }
                        if (retpic) retpic(dec, p);
                        if (++drained > 8) break;
                    }
                    if (drained) break;
                } else if (dr == 0) {
                    break;
                }
            }
        }
        pos = nal;
    }

    /* 收尾：bEndOfStream=1 把解码器里缓着的帧刷出来 */
    for (int i = 0; i < 500; i++) {
        int dr = decode(dec, 1, 0, 0, 0);
        if (dr == FRAME_DECODED || dr == KEYFRAME_DECODED) {
            PicHead *p = reqpic(dec, 0);
            if (p) { frames++; if (retpic) retpic(dec, p); }
        } else if (dr == 0) {
            break;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    printf("decode result histogram: ");
    for (int q = 0; q < 9; q++)
        if (dr_cnt[q]) printf("%d:%d  ", q - 1, dr_cnt[q]);
    printf("\n");
    printf("AUs submitted=%d  frames=%d\n", aus, frames);
    if (frames > 0)
        printf("PERF: %.1f ms -> %.0f fps  %.2f ms/frame\n", ms, frames * 1000.0 / ms, ms / frames);
    return 0;
}
