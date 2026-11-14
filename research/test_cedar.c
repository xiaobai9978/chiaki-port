#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <time.h>

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

typedef struct {
    int bScaleDownEn, bRotationEn, bSecOutputEn;
    int nHorizonScaleDownRatio, nVerticalScaleDownRatio;
    int nSecHorizonScaleDownRatio, nSecVerticalScaleDownRatio;
    int nRotateDegree, bThumbnailMode;
    int eOutputPixelFormat, eSecOutputPixelFormat;
    int bNoBFrames, bDisable3D, bSupportMaf, bDispErrorFrame;
    int nVbvBufferSize, nFrameBufferNum;
    int bSecureosEn, bGpuBufValid, nAlignStride, bIsSoftDecoderFlag, bVirMallocSbm;
    int bSupportPallocBufBeforeDecode;
    int nDeInterlaceHoldingFrameBufferNum, nDisplayHoldingFrameBufferNum;
    int nRotateHoldingFrameBufferNum, nDecodeSmoothFrameBufferNum;
    int bIsTvStream;
    void *memops;
    int eCtlAfbcMode, eCtlIptvMode;
    void *veOpsS, *pVeOpsSelf;
    int bConvertVp910bitTo8bit;
    unsigned int nVeFreq;
    int bCalledByOmxFlag, bSetProcInfoEnable, nSetProcInfoFreq, nChannelNum;
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

#define VDECODE_RESULT_FRAME_DECODED 1
#define VDECODE_RESULT_KEYFRAME_DECODED 3

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { printf("usage: %s <file.h264|file.h265>\n", argv[0]); return 1; }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("cannot open %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *data = malloc((size_t)fsz);
    if (fread(data, 1, (size_t)fsz, f) != (size_t)fsz) { printf("read fail\n"); return 1; }
    fclose(f);
    printf("input %s: %ld bytes\n", argv[1], fsz);

    int is_h265 = strstr(argv[1], "265") != NULL || strstr(argv[1], "hevc") != NULL;

    void *h = dlopen("/usr/lib/libvdecoder.so", RTLD_NOW);
    if (!h) { printf("dlopen FAILED: %s\n", dlerror()); return 1; }

    void (*addplugin)(void) = (void (*)(void))dlsym(h, "AddVDPlugin");
    void *(*create)(void) = (void *(*)(void))dlsym(h, "CreateVideoDecoder");
    int (*init)(void *, VideoStreamInfo *, VConfig *) =
        (int (*)(void *, VideoStreamInfo *, VConfig *))dlsym(h, "InitializeVideoDecoder");
    int (*reqbuf)(void *, int, char **, int *, char **, int *, int) =
        (int (*)(void *, int, char **, int *, char **, int *, int))dlsym(h, "RequestVideoStreamBuffer");
    int (*submit)(void *, VideoStreamDataInfo *, int) =
        (int (*)(void *, VideoStreamDataInfo *, int))dlsym(h, "SubmitVideoStreamData");
    int (*decode)(void *, int, int, int, long long) =
        (int (*)(void *, int, int, int, long long))dlsym(h, "DecodeVideoStream");
    PicHead *(*nextpic)(void *, int) = (PicHead * (*)(void *, int))dlsym(h, "NextPictureInfo");
    int (*retpic)(void *, PicHead *) = (int (*)(void *, PicHead *))dlsym(h, "ReturnPicture");
    printf("nextpic=%p retpic=%p\n", (void *)nextpic, (void *)retpic);

    addplugin();
    void *dec = create();
    if (!dec) { printf("create failed\n"); return 2; }

    VideoStreamInfo info;
    memset(&info, 0, sizeof(info));
    info.eCodecFormat = is_h265 ? 0x116 : 0x115;
    info.nWidth = 1280;
    info.nHeight = 720;
    info.nFrameRate = 60;
    info.nFrameDuration = 16666;

    VConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.eOutputPixelFormat = 1;               /* YUV420P */
    cfg.nFrameBufferNum = 6;
    cfg.nAlignStride = 32;
    cfg.bDispErrorFrame = 1;
    cfg.nDecodeSmoothFrameBufferNum = 4;
    cfg.nDisplayHoldingFrameBufferNum = 2;
    cfg.nDeInterlaceHoldingFrameBufferNum = 2;

    int r = init(dec, &info, &cfg);
    printf("InitializeVideoDecoder = %d\n", r);

    char *buf = NULL, *ring = NULL;
    int bufsz = 0, ringsz = 0;
    int need = (int)fsz + 4096;
    r = reqbuf(dec, need, &buf, &bufsz, &ring, &ringsz, 0);
    printf("RequestVideoStreamBuffer(%d) = %d  buf=%p size=%d\n", need, r, (void *)buf, bufsz);
    if (r != 0 || !buf) return 3;

    memcpy(buf, data, (size_t)fsz);

    VideoStreamDataInfo di;
    memset(&di, 0, sizeof(di));
    di.pData = buf;
    di.nLength = (int)fsz;
    di.bIsFirstPart = 1;
    di.bIsLastPart = 1;
    di.bValid = 1;
    di.nID = 0;
    r = submit(dec, &di, 0);
    printf("SubmitVideoStreamData = %d\n", r);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    int frames = 0;
    for (int i = 0; i < 400; i++) {
        int dr = decode(dec, 0, 0, 0, 0);
        if (dr == VDECODE_RESULT_FRAME_DECODED || dr == VDECODE_RESULT_KEYFRAME_DECODED) {
            PicHead *p = nextpic(dec, 0);
            if (p) {
                frames++;
                if (frames <= 2 || frames % 60 == 0)
                    printf("frame %d: %dx%d stride=%d fmt=%d pts=%lld\n",
                           frames, p->nWidth, p->nHeight, p->nLineStride,
                           p->ePixelFormat, p->nPts);
                if (retpic) retpic(dec, p);
            }
        } else if (dr == 5) {
            printf("no more bitstream at iter %d, frames=%d\n", i, frames);
            break;
        } else if (dr != 0 && dr != 2) {
            if (i > 400) break;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    if (frames > 0)
        printf("PERF: %d frames in %.1f ms -> %.0f fps, %.2f ms/frame\n",
               frames, ms, frames * 1000.0 / ms, ms / frames);

    printf("TOTAL FRAMES = %d\n", frames);
    return 0;
}
