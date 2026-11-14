// SPDX-License-Identifier: AGPL-3.0-only
#include "stream.h"
#include "hwdec.h"

#include <dlfcn.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>

#include <chiaki/controller.h>
#include <chiaki/ctrl.h>
#include <chiaki/ffmpegdecoder.h>
#include <chiaki/opusdecoder.h>
#include <chiaki/base64.h>
#include <chiaki/regist.h>

#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>

typedef struct {
    ChiakiSession        session;
    ChiakiFfmpegDecoder  decoder;
    ChiakiOpusDecoder    opus;
    ChiakiControllerState ctrl;
    ChiakiLog           *log;

    bool  session_inited;
    bool  started;
    bool  dec_inited;
    bool  opus_inited;
    bool  hwdec_on;

    char  hostname[256];

    /* video */
    SDL_Texture   *tex;
    int            tex_w, tex_h;
    Uint32         tex_fmt;
    struct SwsContext *sws;
    uint8_t       *y, *u, *v;
    uint8_t       *nv12_buf[3];   /* 硬解三缓冲：紧凑 NV12，Y(w*h) + UV(w*h/2)。
                                   * 双缓冲不够：解码快于渲染交换时，连续两帧
                                   * 会选到同一块 back，覆盖正在上传的帧。 */
    int            nv12_w_i;      /* 解码写入槽（仅解码线程读写，免锁） */
    int            nv12_p_i;      /* 最新完成槽（vlock 保护） */
    int            nv12_f_i;      /* 渲染正在上传槽（vlock 保护） */
    unsigned       nv12_seq;      /* 完成帧计数；渲染与 drawn 比较判断新帧 */
    unsigned       nv12_drawn;    /* 已上传纹理的最后一帧序号 */
    int            pend_w, pend_h;  /* pending 帧的实际尺寸 */
    size_t         nv12_cap;        /* 单块缓冲容量（三块同尺寸） */
    int            vw, vh;
    size_t         y_size, u_size;
    SDL_mutex     *vlock;
    bool           vfresh;
    bool           use_argb;
    uint8_t       *argb;
    size_t         argb_size;
    int            view_w, view_h;   /* target render size (ARGB path) */
    int            conv_w, conv_h;   /* size currently produced by sws */
    int            src_w, src_h, src_fmt;

    /* audio */
    SDL_AudioDeviceID adev;
    int               ach;
    int               arate;

    /* events：小环形队列。以前是单槽覆盖式，同一帧内 CONNECTED 紧接
     * PIN_REQUEST 或 QUIT 时先到的会被丢掉，QUIT 丢失会卡在「正在连接…」 */
    SDL_mutex *elock;
    int        evq_type[8];
    int        evq_arg[8];
    int        evq_head, evq_count;

    /* rumble: written by the session thread, applied by the main loop */
    uint8_t    rum_left;
    uint8_t    rum_right;
    bool       rum_dirty;
    int        haptic_peak;      /* 1Hz 汇总用的触觉峰值 */
    Uint32     haptic_log_t0;
    Uint32     rum_until;
    bool       rum_on;
    int        rum_voltage;
    Uint32     pulse_until;  /* 菜单震动测试的到期时间，由主循环驱动 */

    int        frames;
    int        fps;
    Uint32     fps_t0;
    int        fps_n;
    int        fps_log_n;
    double     proc_ms;      /* 每帧转换平均耗时 */
    double     proc_acc;
    double     dec_ms;       /* 每帧解码平均耗时（喂包 + 取帧） */
    double     dec_acc;
    FILE      *dump;
    long       dump_bytes;
    bool       dump_tried;
    uint8_t    regkey[16];
    uint8_t    morning[16];
    bool       have_regkey;
} StreamState;

static StreamState S;

/* 实时码率：video_sample_cb 跑在 session 线程，OSD 在主线程读。
 * 用原子量按 1 秒窗口统计，避免和渲染线程产生数据竞争。 */
static SDL_atomic_t rx_bytes;   /* 当前窗口累计收到的码流字节 */
static SDL_atomic_t rx_kbps;    /* 上一窗口算出的实际码率 kbps */
static SDL_atomic_t rx_t0;      /* 窗口起点 SDL_GetTicks() */

/* OSD 统计由 session 线程写、主线程读。double EMA 与普通 int 跨线程读是
 * 数据竞争（读到写一半的浮点或错值），统一用原子量做发布。 */
static SDL_atomic_t st_fps, st_frames, st_proc_ms, st_dec_ms, st_connected;

static void motor_release(void);   /* 定义在 rumble 段，stream_stop 里要用 */

/* SDL_UpdateNVTexture only exists in SDL >= 2.0.16. The build sysroot has an
 * older SDL2, while the handheld ships 2.30.8, so resolve it at runtime.
 * Verified on the device: opengles2 renderer creates/updates/draws NV12 fine. */
typedef int (*UpdateNVFn)(SDL_Texture *, const SDL_Rect *, const Uint8 *, int,
                          const Uint8 *, int);
static UpdateNVFn get_update_nv(void)
{
    static UpdateNVFn fn = NULL;
    static int tried = 0;
    if (!tried) {
        tried = 1;
        /* RTLD_DEFAULT is not exposed under this toolchain's feature macros;
         * dlopen() returns the already-loaded SDL handle instead. */
        void *h = dlopen("libSDL2-2.0.so.0", RTLD_NOW);
        if (h) {
            fn = (UpdateNVFn)dlsym(h, "SDL_UpdateNVTexture");
            dlclose(h);   /* 只减引用计数，SDL 本体仍被主程序持有 */
        }
        if (!fn) SDL_Log("stream: SDL_UpdateNVTexture unavailable, NV12 path disabled");
    }
    return fn;
}

static void push_event(int type, int arg)
{
    if (!S.elock) return;
    SDL_LockMutex(S.elock);
    if (S.evq_count < 8) {
        int i = (S.evq_head + S.evq_count) % 8;
        S.evq_type[i] = type;
        S.evq_arg[i] = arg;
        S.evq_count++;
    } else if (type == STREAM_EV_QUIT) {
        /* 队列满也不能丢 QUIT：覆盖最后一个槽 */
        int i = (S.evq_head + 7) % 8;
        S.evq_type[i] = type;
        S.evq_arg[i] = arg;
    }
    SDL_UnlockMutex(S.elock);
}

StreamEvent stream_poll_event(int *arg)
{
    if (!S.elock) return STREAM_EV_NONE;
    StreamEvent t = STREAM_EV_NONE;
    SDL_LockMutex(S.elock);
    if (S.evq_count > 0) {
        t = (StreamEvent)S.evq_type[S.evq_head];
        if (arg) *arg = S.evq_arg[S.evq_head];
        S.evq_head = (S.evq_head + 1) % 8;
        S.evq_count--;
    }
    SDL_UnlockMutex(S.elock);
    return t;
}

/* ---------------- video ---------------- */

static void video_frame_available(ChiakiFfmpegDecoder *dec, void *user)
{
    (void)user;
    int32_t lost = 0;
    ChiakiFfmpegFrame fr = chiaki_ffmpeg_decoder_pull_frame(dec, &lost);
    if (!fr.frame)
        return;

    AVFrame *f = fr.frame;
    int w = f->width, h = f->height;

    if (w <= 0 || h <= 0 || !S.vlock) {
        av_frame_free(&fr.frame);
        return;
    }

    /* 软解路径与硬解路径对齐：像素缓冲 / SwsContext / 统计全部在 vlock 内改动。
     * 以前只在末尾锁一下置 vfresh，渲染线程可能拿着被 realloc 释放的旧指针
     * 上传纹理（自动降分辨率时必现），或看到写了一半的画面。 */
    SDL_LockMutex(S.vlock);

    if (S.vw != w || S.vh != h || !S.y) {
        size_t ysz = (size_t)w * h;
        size_t usz = ysz / 4;
        if ((size_t)w != (size_t)S.vw || (size_t)h != (size_t)S.vh) {
            /* dimensions changed: PTS/messages are reset by the decoder anyway */
        }
        uint8_t *ny = realloc(S.y, ysz);
        uint8_t *nu = realloc(S.u, usz);
        uint8_t *nv = realloc(S.v, usz);
        if (!ny || !nu || !nv) {
            if (ny) S.y = ny;
            if (nu) S.u = nu;
            if (nv) S.v = nv;
            goto out;
        }
        S.y = ny; S.u = nu; S.v = nv;
        S.vw = w; S.vh = h;
        S.y_size = ysz; S.u_size = usz;
        if (S.sws) {
            sws_freeContext(S.sws);
            S.sws = NULL;
        }
    }

    int tw = w, th = h;
    if (S.use_argb && S.view_w > 0 && S.view_h > 0) {
        tw = S.view_w;
        th = S.view_h;
    }

    if (!S.sws || S.src_w != w || S.src_h != h || S.src_fmt != f->format ||
        S.conv_w != tw || S.conv_h != th) {
        if (S.sws)
            sws_freeContext(S.sws);
        S.sws = sws_getContext(w, h, (enum AVPixelFormat)f->format,
                              tw, th, S.use_argb ? AV_PIX_FMT_BGRA : AV_PIX_FMT_YUV420P,
                              S.use_argb ? SWS_BILINEAR : SWS_POINT, NULL, NULL, NULL);
        S.src_w = w; S.src_h = h; S.src_fmt = f->format;
        S.conv_w = tw; S.conv_h = th;
        S.vfresh = true;
        if (!S.sws)
            goto out;
    }

    Uint64 t_conv = SDL_GetPerformanceCounter();

    if (S.use_argb) {
        size_t need = (size_t)tw * (size_t)th * 4;
        if (need > S.argb_size) {
            uint8_t *na = realloc(S.argb, need);
            if (!na) goto out;
            S.argb = na;
            S.argb_size = need;
        }
        uint8_t *dst[4] = { S.argb, NULL, NULL, NULL };
        int dstride[4] = { tw * 4, 0, 0, 0 };
        sws_scale(S.sws, (const uint8_t * const *)f->data, f->linesize, 0, h, dst, dstride);
    } else {
        uint8_t *dst[4] = { S.y, S.u, S.v, NULL };
        int dstride[4] = { S.vw, S.vw / 2, S.vw / 2, 0 };
        sws_scale(S.sws, (const uint8_t * const *)f->data, f->linesize, 0, h, dst, dstride);
    }

    {
        double ms = (double)(SDL_GetPerformanceCounter() - t_conv) * 1000.0
                    / (double)SDL_GetPerformanceFrequency();
        if (ms > 0.0 && ms < 200.0) {
            S.proc_acc = S.proc_acc * 0.9 + ms * 0.1;
            S.proc_ms = S.proc_acc;
            SDL_AtomicSet(&st_proc_ms, (int)(S.proc_ms + 0.5));
        }
    }

    S.frames++;
    SDL_AtomicSet(&st_frames, S.frames);
    S.fps_n++;
    Uint32 now = SDL_GetTicks();
    if (S.fps_t0 == 0)
        S.fps_t0 = now;
    else if (now - S.fps_t0 >= 1000) {
        S.fps = (int)(S.fps_n * 1000 / (now - S.fps_t0));
        SDL_AtomicSet(&st_fps, S.fps);
        S.fps_n = 0;
        S.fps_t0 = now;
    }

    S.vfresh = true;

out:
    SDL_UnlockMutex(S.vlock);
    av_frame_free(&fr.frame);
}

static bool g_use_argb = false;  /* 渲染器是 software 时置位；不能放在 S 里，
                                  * 否则每次 stream_start 的 memset 会把它清掉 */

void stream_set_use_argb(bool on)
{
    g_use_argb = on;
    if (S.vlock) SDL_LockMutex(S.vlock);
    S.use_argb = on;
    if (S.sws) {
        sws_freeContext(S.sws);
        S.sws = NULL;
    }
    if (S.vlock) SDL_UnlockMutex(S.vlock);
}

void stream_get_stats(int *fps, int *frames, int *proc_ms, int *rtt_ms,
                      int *lost, bool *hwdec, int *dec_ms, int *kbps)
{
    if (fps) *fps = SDL_AtomicGet(&st_fps);
    if (frames) *frames = SDL_AtomicGet(&st_frames);
    if (proc_ms) *proc_ms = SDL_AtomicGet(&st_proc_ms);
    if (dec_ms) *dec_ms = SDL_AtomicGet(&st_dec_ms);
    if (rtt_ms) *rtt_ms = S.session_inited ? (int)(S.session.rtt_us / 1000) : -1;
    if (lost) *lost = S.dec_inited ? (int)S.decoder.frames_lost
                                  : (S.hwdec_on ? 0 : -1);
    if (hwdec) {
        *hwdec = S.hwdec_on ||
                 (S.dec_inited && S.decoder.av_codec && S.decoder.av_codec->name
                  && strstr(S.decoder.av_codec->name, "v4l2m2m") != NULL);
    }
    if (kbps) *kbps = SDL_AtomicGet(&rx_kbps);
}

void stream_set_view_size(int w, int h)
{
    if (!S.vlock) return;
    /* view 尺寸由渲染线程每帧更新、解码线程读取，必须在同一把锁下。
     * 这里不再 free S.sws：SwsContext 由解码线程在锁内按 conv_w/conv_h
     * 变化自行重建，渲染线程碰它会和 sws_scale 并发 UAF。 */
    SDL_LockMutex(S.vlock);
    S.view_w = w;
    S.view_h = h;
    SDL_UnlockMutex(S.vlock);
}

void stream_dump_samples(bool on)
{
    if (!on) {
        if (S.dump) { fclose(S.dump); S.dump = NULL; }
        return;
    }
    if (!S.dump) {
        S.dump = fopen("/mnt/SDCARD/Apps/Chiaki/sample.h264", "wb");
        if (S.dump) SDL_Log("stream: dumping samples to sample.h264");
    }
}

bool stream_frame_size(int *w, int *h)
{
    if (!S.vlock) return false;
    /* vw/vh 由解码线程在 vlock 内写，这里读也要持同一把锁 */
    SDL_LockMutex(S.vlock);
    bool ok = S.vw > 0 && S.vh > 0;
    if (ok) {
        if (w) *w = S.vw;
        if (h) *h = S.vh;
    }
    SDL_UnlockMutex(S.vlock);
    return ok;
}

void stream_render(SDL_Renderer *ren, SDL_Rect dst)
{
    if (!ren || !S.vlock) return;

    SDL_Texture *tex = NULL;
    SDL_LockMutex(S.vlock);

    int texw = S.use_argb ? S.conv_w : S.vw;
    int texh = S.use_argb ? S.conv_h : S.vh;
    if (S.hwdec_on && !S.use_argb && S.pend_w > 0) {
        texw = S.pend_w;
        texh = S.pend_h;
    }
    Uint32 fmt = S.use_argb ? SDL_PIXELFORMAT_ARGB8888
               : (S.hwdec_on ? SDL_PIXELFORMAT_NV12 : SDL_PIXELFORMAT_IYUV);
    if (texw > 0 && texh > 0) {
        if (!S.tex || S.tex_w != texw || S.tex_h != texh || S.tex_fmt != fmt) {
            if (S.tex) SDL_DestroyTexture(S.tex);
            S.tex = SDL_CreateTexture(ren, fmt, SDL_TEXTUREACCESS_STREAMING, texw, texh);
            S.tex_w = texw;
            S.tex_h = texh;
            S.tex_fmt = fmt;
            S.vfresh = true;
            if (!S.tex)
                SDL_Log("stream: texture create failed (%s): %s",
                        S.hwdec_on ? "NV12" : "IYUV/ARGB", SDL_GetError());
        }
        if (S.tex && S.vfresh) {
            if (S.use_argb) {
                SDL_UpdateTexture(S.tex, NULL, S.argb, texw * 4);
            } else if (S.hwdec_on && S.nv12_seq != S.nv12_drawn) {
                /* 交换到最新完成帧；texw/texh 已在锁外按 pend 尺寸建好 */
                S.nv12_f_i = S.nv12_p_i;
                S.nv12_drawn = S.nv12_seq;
                S.vw = S.pend_w; S.vh = S.pend_h;
                const uint8_t *front = S.nv12_buf[S.nv12_f_i];
                UpdateNVFn upd = get_update_nv();
                if (upd)
                    upd(S.tex, NULL, front, texw,
                        front + (size_t)texw * (size_t)texh, texw);
            } else if (!S.hwdec_on) {
                SDL_UpdateYUVTexture(S.tex, NULL, S.y, S.vw, S.u, S.vw / 2, S.v, S.vw / 2);
            }
            S.vfresh = false;
        }
        tex = S.tex;
    }

    SDL_UnlockMutex(S.vlock);

    /* RenderCopy 不读 S.nv12 / S.y，放到锁外，避免 GPU 提交阻塞
     * 解码线程在 vlock 内做的整帧拷贝。纹理对象只由主线程销毁。 */
    if (tex)
        SDL_RenderCopy(ren, tex, NULL, &dst);
}

/* ---------------- audio ---------------- */

static void audio_settings_cb(uint32_t channels, uint32_t rate, void *user)
{
    (void)user;
    if (S.adev) {
        SDL_CloseAudioDevice(S.adev);
        S.adev = 0;
    }
    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    memset(&have, 0, sizeof(have));
    want.freq = (int)rate;
    want.channels = (Uint8)channels;
    want.format = AUDIO_S16SYS;
    want.samples = 512;
    want.callback = NULL;

    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have,
        SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE);
    if (!dev) {
        SDL_Log("stream: audio open failed: %s", SDL_GetError());
        return;
    }
    S.adev = dev;
    S.ach = have.channels;
    S.arate = have.freq;
    SDL_PauseAudioDevice(dev, 0);
    SDL_Log("stream: audio %d ch @ %d Hz", S.ach, S.arate);
}

static void audio_frame_cb(int16_t *buf, size_t samples_count, void *user)
{
    (void)user;
    if (!S.adev) return;
    if (S.ach <= 0) return;

    /* Keep latency bounded: drop if more than ~120 ms is queued. */
    Uint32 max_bytes = (Uint32)(S.arate * S.ach * sizeof(int16_t) * 120 / 1000);
    if (SDL_GetQueuedAudioSize(S.adev) > max_bytes)
        return;

    Uint32 bytes = (Uint32)(samples_count * (size_t)S.ach * sizeof(int16_t));
    SDL_QueueAudio(S.adev, buf, bytes);
}

/* ---------------- session events ---------------- */

static void cant_display_cb(void *user, bool hdr)
{
    (void)user;
    SDL_Log("stream: console cannot display%s", hdr ? " (HDR requested)" : "");
}

/* PS5 的 HD 触觉以 is_haptics 标记的独立音频流送来（audioreceiver.c 里走
 * session->haptics_sink 而非普通音频 sink）。不设这个 sink，数据直接被丢弃，
 * 表现就是"游戏里有震动、掌机一点反应都没有"。
 * 数据是 int16 PCM，取峰值折算 0..255，复用原有单马达驱动。 */
static void haptics_frame_cb(uint8_t *buf, size_t buf_size, void *user)
{
    (void)user;
    if (!buf || buf_size < 2)
        return;

    const int16_t *s = (const int16_t *)buf;
    size_t n = buf_size / 2;

    /* 用 RMS 而不是峰值：峰值对底噪和单个尖刺太敏感，会把 PS5 UI 那种
     * 极轻微的按键触觉放大成明显震动。RMS 更接近实际"震感"。 */
    double acc = 0.0;
    for (size_t i = 0; i < n; i++) {
        double v = (double)s[i];
        acc += v * v;
    }
    double rms = sqrt(acc / (double)n);

    /* 实测：haptics 的 RMS 绝大多数落在 400~1500，偶尔上万。
     * 线性映射会让普通震动（几百~一千）几乎为 0，所以改用对数：
     * 500→56  1000→87  3000→148  8000→211  16000→255(饱和) */
    const double dead = 300.0;
    int level = 0;
    if (rms > dead) {
        double db = log(1.0 + rms / 400.0) / log(2.0);   /* log2 */
        level = (int)(db / 5.4 * 255.0);
        if (level > 255) level = 255;
        if (level < 0) level = 0;
    }

    /* 触觉帧是毫秒级到达，以前 level 每变一次就打一行，能把 run.log 刷爆。
     * 改成每秒汇总一次峰值，计数放在 S 里由 elock 保护（不再用函数内 static）。 */
    if (S.elock) {
        SDL_LockMutex(S.elock);
        S.rum_left  = (uint8_t)level;
        S.rum_right = (uint8_t)level;
        S.rum_dirty = true;
        if (level > S.haptic_peak) S.haptic_peak = level;
        Uint32 now = SDL_GetTicks();
        int peak = 0;
        if (now - S.haptic_log_t0 >= 1000) {
            peak = S.haptic_peak;
            S.haptic_peak = 0;
            S.haptic_log_t0 = now;
        }
        SDL_UnlockMutex(S.elock);
        if (peak > 0) SDL_Log("stream: HAPTICS 1s peak level=%d", peak);
    }
}

static void session_event_cb(ChiakiEvent *ev, void *user)
{
    (void)user;
    switch (ev->type) {
    case CHIAKI_EVENT_CONNECTED:
        SDL_AtomicSet(&st_connected, 1);
        SDL_Log("stream: CONNECTED");
        push_event(STREAM_EV_CONNECTED, 0);
        break;
    case CHIAKI_EVENT_LOGIN_PIN_REQUEST:
        SDL_Log("stream: PIN requested (incorrect=%d)", ev->login_pin_request.pin_incorrect ? 1 : 0);
        push_event(STREAM_EV_PIN_REQUEST, ev->login_pin_request.pin_incorrect ? 1 : 0);
        break;
    case CHIAKI_EVENT_REGIST:
        memcpy(S.regkey, ev->host.rp_regist_key, sizeof(S.regkey));
        memcpy(S.morning, ev->host.rp_key, sizeof(S.morning));
        S.have_regkey = true;
        SDL_Log("stream: REGIST captured");
        break;
    case CHIAKI_EVENT_RUMBLE:
        /* 只在强度变化时记一行：震动事件来得非常密，全打会淹掉日志 */
        {
            static uint8_t last_l = 0, last_r = 0;
            if (ev->rumble.left != last_l || ev->rumble.right != last_r) {
                SDL_Log("stream: RUMBLE l=%u r=%u",
                        (unsigned)ev->rumble.left, (unsigned)ev->rumble.right);
                last_l = ev->rumble.left;
                last_r = ev->rumble.right;
            }
        }
        /* 回调跑在 session 线程里，只把强度存下来，主循环负责真正驱动马达 */
        if (S.elock) {
            SDL_LockMutex(S.elock);
            S.rum_left = ev->rumble.left;
            S.rum_right = ev->rumble.right;
            S.rum_dirty = true;
            SDL_UnlockMutex(S.elock);
        }
        break;
    case CHIAKI_EVENT_QUIT:
        SDL_AtomicSet(&st_connected, 0);
        SDL_Log("stream: QUIT reason=%d (%s) str=%s", (int)ev->quit.reason,
                chiaki_quit_reason_string(ev->quit.reason),
                ev->quit.reason_str ? ev->quit.reason_str : "");
        push_event(STREAM_EV_QUIT, (int)ev->quit.reason);
        break;
    default:
        break;
    }
}

/* cedar 硬解路径：整块 NV12 直接交给 SDL_UpdateNVTexture，
 * 绕开 ffmpeg 解码线程、sws_scale 和逐像素 U/V 拆分。 */
static void hwdec_update_stats(void)
{
    S.frames++;
    SDL_AtomicSet(&st_frames, S.frames);
    S.fps_n++;
    Uint32 now = SDL_GetTicks();
    if (S.fps_t0 == 0)
        S.fps_t0 = now;
    else if (now - S.fps_t0 >= 1000) {
        S.fps = (int)(S.fps_n * 1000 / (now - S.fps_t0));
        SDL_AtomicSet(&st_fps, S.fps);
        S.fps_n = 0;
        S.fps_t0 = now;
    }
}

/* 三缓冲轮转：解码永远写「既不是渲染在传、也不是待交换」的那一格。
 * 直写 cedar 输出的裁剪目标，省掉 hwdec 内部缓冲到渲染缓冲的第二次
 * 整帧 memcpy（720p ≈ 1.3MB/帧）。双缓冲不够——解码比渲染快时，连续
 * 两帧会选回同一格，覆盖正在 SDL_UpdateNVTexture 的内存。 */
static bool hwdec_video_sample(const uint8_t *buf, size_t len)
{
    if (!S.vlock || !S.nv12_buf[0])
        return true;

    SDL_LockMutex(S.vlock);
    int bi = S.nv12_w_i;
    if (bi == S.nv12_f_i || bi == S.nv12_p_i) {
        /* 只在 f/p 恰好相邻时跳格，保证三格循环 */
        for (int k = 0; k < 3; k++)
            if (k != S.nv12_f_i && k != S.nv12_p_i) { bi = k; break; }
    }
    S.nv12_w_i = bi;
    SDL_UnlockMutex(S.vlock);

    int fw = 0, fh = 0;
    if (!hwdec_decode_into(buf, len, S.nv12_buf[bi], S.nv12_cap, &fw, &fh))
        return true;

    SDL_LockMutex(S.vlock);
    S.nv12_p_i = bi;
    S.pend_w = fw; S.pend_h = fh;
    S.nv12_seq++;         /* 必须最后置：渲染以它判断「有完整的新一帧」 */
    S.vfresh = true;
    SDL_UnlockMutex(S.vlock);

    hwdec_update_stats();
    return true;
}

/* Wrapper around chiaki's decoder callback so we can capture the raw bitstream
 * and measure how long the actual H.264/HEVC decoding takes. */
static bool video_sample_cb(uint8_t *buf, size_t buf_size, int32_t frames_lost, bool frame_recovered, void *user)
{
    /* 实时码率：按 1 秒窗口统计实际收到的码流（kbps = bytes*8/毫秒数） */
    SDL_AtomicAdd(&rx_bytes, (int)buf_size);
    Uint32 now = SDL_GetTicks();
    Uint32 rx_win_t0 = (Uint32)SDL_AtomicGet(&rx_t0);
    if (!rx_win_t0) {
        SDL_AtomicSet(&rx_t0, (int)(now ? now : 1));
    } else if ((Uint32)(now - rx_win_t0) >= 1000) {
        int bytes = SDL_AtomicSet(&rx_bytes, 0);
        Uint32 dt = now - rx_win_t0;
        if (dt > 0)
            SDL_AtomicSet(&rx_kbps, (int)((long long)bytes * 8 / (int)dt));
        SDL_AtomicSet(&rx_t0, (int)(now ? now : 1));
    }

    if (S.dump && S.dump_bytes < (8L << 20)) {
        long before = S.dump_bytes;
        fwrite(buf, 1, buf_size, S.dump);
        S.dump_bytes += (long)buf_size;
        /* 每跨过 1MB 才刷一次盘；以前每个码流包都 fflush，会拖慢解码 */
        if ((before >> 20) != (S.dump_bytes >> 20))
            fflush(S.dump);
    }

    static Uint64 freq = 0;
    if (!freq) freq = SDL_GetPerformanceFrequency();

    Uint64 t0 = SDL_GetPerformanceCounter();
    bool ret = S.hwdec_on
        ? hwdec_video_sample(buf, buf_size)
        : chiaki_ffmpeg_decoder_video_sample_cb(buf, buf_size, frames_lost, frame_recovered, user);
    double ms = (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)freq;
    if (ms > 0.0 && ms < 300.0) {
        S.dec_acc = S.dec_acc * 0.9 + ms * 0.1;
        S.dec_ms = S.dec_acc;
        SDL_AtomicSet(&st_dec_ms, (int)(S.dec_ms + 0.5));
    }
    return ret;
}

/* ---------------- lifecycle ---------------- */

int stream_start(const char *host, bool ps5, const uint8_t *regkey,
                 const uint8_t *morning, const char *psn_id_b64,
                 const StreamCfg *cfg, ChiakiLog *log)
{
    if (S.started) return -1;

    memset(&S, 0, sizeof(S));
    S.log = log;
    S.use_argb = g_use_argb;   /* 恢复渲染器能力标志，别被 memset 冲掉 */
    /* 新会话从零开始统计实时码率，不沿用上一场串流的窗口 */
    SDL_AtomicSet(&rx_bytes, 0);
    SDL_AtomicSet(&rx_kbps, 0);
    SDL_AtomicSet(&rx_t0, 0);
    SDL_AtomicSet(&st_fps, 0);
    SDL_AtomicSet(&st_frames, 0);
    SDL_AtomicSet(&st_proc_ms, 0);
    SDL_AtomicSet(&st_dec_ms, 0);
    SDL_AtomicSet(&st_connected, 0);

    S.vlock = SDL_CreateMutex();
    S.elock = SDL_CreateMutex();
    if (!S.vlock || !S.elock) {
        stream_stop();
        return -1;
    }

    snprintf(S.hostname, sizeof(S.hostname), "%s", host);

    /* 配置在 main.c 的 config_load 里已 clamp，这里再兜一次底，
     * 并保证硬解尺寸与协商 preset 用的是同一份值。 */
    int resolution = (cfg->resolution >= 1 && cfg->resolution <= 4) ? cfg->resolution : 3;
    int fps        = (cfg->fps == 30 || cfg->fps == 60) ? cfg->fps : 60;
    int codec_sel  = (cfg->codec == 1) ? 1 : 0;
    ChiakiCodec codec = codec_sel ? CHIAKI_CODEC_H265 : CHIAKI_CODEC_H264;

    /* chiaki-ng resolution presets: 1=360p 2=540p 3=720p 4=1080p */
    static const int res_w[] = { 0, 640, 960, 1280, 1920 };
    static const int res_h[] = { 0, 360, 540, 720, 1080 };
    int ri = resolution;

    /* 软件渲染器不支持 NV12/YUV 纹理，硬解开了也只会黑屏，直接回退软解 */
    if (cfg->hwdec && !g_use_argb && hwdec_init(codec_sel, res_w[ri], res_h[ri])) {
        S.hwdec_on = true;
        /* 三缓冲按协商尺寸一次分配（crop 只会裁得更小），之后零 realloc */
        S.nv12_cap = (size_t)res_w[ri] * (size_t)res_h[ri] * 3 / 2;
        int ok = 1;
        for (int i = 0; i < 3; i++) {
            S.nv12_buf[i] = malloc(S.nv12_cap);
            if (!S.nv12_buf[i]) ok = 0;
        }
        if (!ok) {
            SDL_Log("stream: hwdec triple buffer alloc failed");
            for (int i = 0; i < 3; i++) { free(S.nv12_buf[i]); S.nv12_buf[i] = NULL; }
            S.nv12_cap = 0;
            hwdec_fini();
            S.hwdec_on = false;
        } else {
            SDL_Log("stream: cedar HARDWARE decoder enabled (%dx%d, %s)",
                    res_w[ri], res_h[ri], codec_sel ? "H.265" : "H.264");
        }
    }
    if (!S.hwdec_on && !S.dec_inited) {
        if (cfg->hwdec)
            SDL_Log("stream: cedar hwdec unavailable (%s), falling back to ffmpeg",
                    g_use_argb ? "software renderer" : hwdec_error());
        if (chiaki_ffmpeg_decoder_init(&S.decoder, log, codec, (unsigned)fps,
                                       NULL, NULL, video_frame_available, NULL) == CHIAKI_ERR_SUCCESS)
            S.dec_inited = true;
        else
            SDL_Log("stream: ffmpeg decoder init failed");
    }

    chiaki_opus_decoder_init(&S.opus, log);
    S.opus_inited = true;
    chiaki_opus_decoder_set_cb(&S.opus, audio_settings_cb, audio_frame_cb, NULL);

    ChiakiConnectInfo info;
    memset(&info, 0, sizeof(info));
    info.ps5 = ps5;
    info.host = S.hostname;
    if (regkey)
        memcpy(info.regist_key, regkey, sizeof(info.regist_key));
    if (morning)
        memcpy(info.morning, morning, sizeof(info.morning));
    if (psn_id_b64 && psn_id_b64[0]) {
        uint8_t acc[32];
        size_t acc_size = sizeof(acc);
        if (chiaki_base64_decode(psn_id_b64, strlen(psn_id_b64), acc, &acc_size) == CHIAKI_ERR_SUCCESS
            && acc_size == CHIAKI_PSN_ACCOUNT_ID_SIZE) {
            memcpy(info.psn_account_id, acc, CHIAKI_PSN_ACCOUNT_ID_SIZE);
            SDL_Log("stream: PSN Account ID set (%zu bytes)", acc_size);
        } else {
            SDL_Log("stream: invalid PSN Account ID (need base64 of %d bytes)", CHIAKI_PSN_ACCOUNT_ID_SIZE);
        }
    }
    chiaki_connect_video_profile_preset(&info.video_profile,
        (ChiakiVideoResolutionPreset)resolution,
        (ChiakiVideoFPSPreset)fps);
    info.video_profile.codec = codec;
    if (cfg->bitrate > 0)
        info.video_profile.bitrate = (unsigned)cfg->bitrate;
    info.video_profile_auto_downgrade = true;
    info.enable_keyboard = false;
    /* 必须声明成 DualSense，PS5 才会下发震动：
     * ctrl_enable_features() 里只有 enable_dualsense 为真时才发
     * CTRL_MESSAGE_TYPE_ENABLE_DUALSENSE_FEATURES。否则 PS5 把客户端当普通
     * DS4，一个 rumble 包都不发（日志里 RUMBLE 计数为 0 就是这个原因）。
     * 该标志在 ctrl.c 中只影响这一条声明，不改变按键上报格式。 */
    info.enable_dualsense = true;
    /* If a frame is lost, ask the console for a fresh keyframe instead of stalling. */
    info.enable_idr_on_fec_failure = true;
    info.packet_loss_max = 0.1;

    ChiakiErrorCode err = chiaki_session_init(&S.session, &info, log);
    if (err != CHIAKI_ERR_SUCCESS) {
        SDL_Log("stream: session init failed: %s", chiaki_error_string(err));
        stream_stop();
        return -1;
    }
    S.session_inited = true;

    chiaki_session_set_event_cb(&S.session, session_event_cb, NULL);
    /* 硬解路径没有 ffmpeg decoder，但 video_sample_cb 仍必须注册，
     * 否则 chiaki 重组出来的码流直接被丢掉（表现为 fps=0、hwdec fail=0）。 */
    if (S.dec_inited || S.hwdec_on)
        chiaki_session_set_video_sample_cb(&S.session, video_sample_cb, &S.decoder);

    ChiakiAudioSink sink;
    memset(&sink, 0, sizeof(sink));
    chiaki_opus_decoder_get_sink(&S.opus, &sink);
    chiaki_session_set_audio_sink(&S.session, &sink);

    /* 触觉流：独立 sink，和音频并行 */
    ChiakiAudioSink hsink;
    memset(&hsink, 0, sizeof(hsink));
    hsink.frame_cb = haptics_frame_cb;
    hsink.user = NULL;
    chiaki_session_set_haptics_sink(&S.session, &hsink);

    ChiakiCtrlDisplaySink dsink;
    memset(&dsink, 0, sizeof(dsink));
    dsink.user = NULL;
    dsink.cantdisplay_cb = cant_display_cb;
    chiaki_session_ctrl_set_display_sink(&S.session, &dsink);

    chiaki_controller_state_set_idle(&S.ctrl);

    err = chiaki_session_start(&S.session);
    if (err != CHIAKI_ERR_SUCCESS) {
        SDL_Log("stream: session start failed: %s", chiaki_error_string(err));
        stream_stop();
        return -1;
    }
    S.started = true;
    SDL_Log("stream: connecting to %s (ps5=%d res=%d fps=%d bitrate=%d codec=%d)",
            host, ps5, cfg->resolution, cfg->fps, cfg->bitrate, cfg->codec);
    return 0;
}

void stream_stop(void)
{
    /* 抓流文件不关掉的话，每次重连都泄漏一个句柄（stream_start 会 memset 掉指针） */
    if (S.dump) {
        fclose(S.dump);
        S.dump = NULL;
        S.dump_bytes = 0;
    }
    if (S.started) {
        chiaki_session_stop(&S.session);
        chiaki_session_join(&S.session);
        S.started = false;
    }
    if (S.session_inited) {
        chiaki_session_fini(&S.session);
        S.session_inited = false;
    }
    if (S.opus_inited) {
        chiaki_opus_decoder_fini(&S.opus);
        S.opus_inited = false;
    }
    if (S.dec_inited) {
        chiaki_ffmpeg_decoder_fini(&S.decoder);
        S.dec_inited = false;
    }
    if (S.hwdec_on) {
        hwdec_fini();
        S.hwdec_on = false;
    }
    if (S.adev) {
        SDL_CloseAudioDevice(S.adev);
        S.adev = 0;
    }
    if (S.sws) {
        sws_freeContext(S.sws);
        S.sws = NULL;
    }
    if (S.tex) {
        SDL_DestroyTexture(S.tex);
        S.tex = NULL;
        S.tex_w = S.tex_h = 0;
    }
    free(S.argb); S.argb = NULL; S.argb_size = 0;
    free(S.y); S.y = NULL;
    free(S.u); S.u = NULL;
    free(S.v); S.v = NULL;
    for (int i = 0; i < 3; i++) { free(S.nv12_buf[i]); S.nv12_buf[i] = NULL; }
    S.nv12_cap = 0;
    S.pend_w = S.pend_h = 0;
    S.vw = S.vh = 0;

    S.rum_left = 0;
    S.rum_right = 0;
    S.rum_dirty = false;
    S.rum_until = 0;
    S.rum_on = false;
    S.rum_voltage = 0;
    S.pulse_until = 0;        /* 防止退出后主循环 tick 又去写已关闭的 fd */
    motor_release();   /* 关掉 sysfs fd，下次串流会重新打开 */

    if (S.vlock) { SDL_DestroyMutex(S.vlock); S.vlock = NULL; }
    if (S.elock) { SDL_DestroyMutex(S.elock); S.elock = NULL; }
    SDL_AtomicSet(&st_connected, 0);
}

bool stream_running(void) { return S.started; }
bool stream_connected(void) { return SDL_AtomicGet(&st_connected) != 0; }

void stream_submit_pin(const char *pin)
{
    if (!S.session_inited || !pin) return;
    chiaki_session_set_login_pin(&S.session, (const uint8_t *)pin, strlen(pin));
}

void stream_send_controller(ChiakiControllerState *st)
{
    if (!S.started) return;
    chiaki_session_set_controller_state(&S.session, st);
}

/* ---------------- rumble ---------------- */

/* 掌机的马达是 sysfs 控制的，跟 SDL 手柄的 FF 无关：
 *   /sys/class/motor/voltage      强度，微伏，hardwareservice 只用 2.5V/3.0V/3.3V 三档
 *   /sys/class/gpio/gpio227/value 马达使能（PH3），拉高转、拉低停
 * 注意 gpio227 在 /usr/trimui/bin/runtrimui.sh 里被导出并初始化为 out/0；
 * gpio243 是 DIP 拨码开关（PH19，输入态），千万别动它。 */
#define MOTOR_GPIO_DIR  "/sys/class/gpio/gpio227/direction"
#define MOTOR_GPIO_VAL  "/sys/class/gpio/gpio227/value"

static int motor_fd = -1;
static int motor_gpio_fd = -1;
static bool motor_ready = false;

static void motor_release(void)
{
    if (motor_gpio_fd >= 0) { (void)!write(motor_gpio_fd, "0", 1); close(motor_gpio_fd); motor_gpio_fd = -1; }
    if (motor_fd >= 0) { close(motor_fd); motor_fd = -1; }
    motor_ready = false;
}

static void motor_init(void)
{
    if (motor_ready) return;
    motor_ready = true;

    if (access("/sys/class/gpio/gpio227", F_OK) != 0) {
        int fd = open("/sys/class/gpio/export", O_WRONLY);
        if (fd >= 0) { (void)!write(fd, "227", 3); close(fd); }
    }
    int fd = open(MOTOR_GPIO_DIR, O_WRONLY);
    if (fd >= 0) { (void)!write(fd, "out", 3); close(fd); }

    motor_gpio_fd = open(MOTOR_GPIO_VAL, O_WRONLY);
    motor_fd = open("/sys/class/motor/voltage", O_WRONLY);
    if (motor_gpio_fd < 0 || motor_fd < 0)
        SDL_Log("stream: motor sysfs not available (gpio=%d motor=%d)", motor_gpio_fd, motor_fd);
    else
        SDL_Log("stream: motor ready (gpio227 + motor/voltage)");
}

static void motor_write(int uv, bool on)
{
    char b[16];
    if (on && motor_fd >= 0 && uv > 0) {
        int n = snprintf(b, sizeof(b), "%d", uv);
        (void)!write(motor_fd, b, (size_t)n);
    }
    if (motor_gpio_fd >= 0)
        (void)!write(motor_gpio_fd, on ? "1" : "0", 1);
}

static int motor_uv_for(int level)
{
    /* 电压档位保留（马达有启动门槛，太低转不动），
     * 真正的强度分级交给下面的占空比调制。 */
    if (level < 60) return 2500000;    /* 2.0V 起不动，最低仍给 2.5V */
    if (level < 140) return 2500000;
    if (level < 210) return 3000000;
    return 3300000;
}

/* 单马达做强度分级只能靠占空比：低强度时让它间歇断电。
 * 只靠 2.5V/3.0V/3.3V 三档，25% 和 100% 体感几乎一样（2.5V 已接近满速）。 */
static bool rumble_duty_on(int level)
{
    if (level >= 235)
        return true;                 /* 满强度常开 */

    static Uint32 frame = 0;
    frame++;

    /* 12 帧一个周期（apply 每帧调用，约 200ms），占空比 = frac^1.5。
     * 上一版 8 帧周期（133ms）下用户反馈 25% 档还是偏大：低档时
     * on_frames 已经贴着「保底 1 帧」，曲线再陡也降不下去，
     * 唯一的余量就是把保底脉冲摊薄——周期 8→12 后 25% 档常见
     * level≈63 从 1/8 降到 1/12（体感再轻 ~1/3），
     * 而 level≥160 的中高档计算结果和原来基本持平。
     * level 很小时仍保底 1 帧通电，否则马达永远转不起来（之前踩过的坑）。 */
    const Uint32 period = 12;
    double frac = (double)level / 255.0;
    int on_frames = (int)(period * frac * sqrt(frac) + 0.35);
    if (on_frames == 0)
        on_frames = 1;
    return (frame % period) < (Uint32)on_frames;
}

void stream_motor_pulse(int pct, int ms)
{
    motor_init();
    if (pct <= 0 || motor_gpio_fd < 0) {
        motor_write(0, false);
        S.rum_on = false;
        S.pulse_until = 0;
        return;
    }
    int level = 255 * (pct > 100 ? 100 : pct) / 100;
    motor_write(motor_uv_for(level), true);
    S.rum_on = true;
    /* 不再起 SDL_AddTimer 线程：timer 回调会和 stream_stop/motor_release
     * 并发写 sysfs fd 与 S.rum_on。改由主循环 stream_tick_motor() 到期收尾。 */
    S.pulse_until = SDL_GetTicks() + (Uint32)ms;
}

/* 每帧由主循环调用（所有界面），负责结束菜单里的震动测试脉冲。 */
void stream_tick_motor(void)
{
    if (!S.pulse_until) return;
    if ((Sint32)(SDL_GetTicks() - S.pulse_until) >= 0) {
        motor_write(0, false);
        S.rum_on = false;
        S.pulse_until = 0;
    }
}

void stream_apply_rumble(int intensity_pct)
{
    motor_init();
    if (S.pulse_until) return;   /* 菜单震动测试脉冲期间不干预 */

    uint8_t l = 0, r = 0;
    bool dirty = false;
    if (S.elock) {
        SDL_LockMutex(S.elock);
        l = S.rum_left;
        r = S.rum_right;
        dirty = S.rum_dirty;
        S.rum_dirty = false;
        SDL_UnlockMutex(S.elock);
    }

    Uint32 now = SDL_GetTicks();
    int level = (l > r) ? (int)l : (int)r;   /* 单马达，取强的那个通道 */
    if (intensity_pct <= 0) {
        level = 0;
    } else if (intensity_pct < 100) {
        /* 强度 = 门限，不再是等比缩小（用户反馈等比缩小后要么整体发麻、
         * 要么高档没劲）。低于门限的小震动直接不播；过了门限的重新拉回
         * 全量程——强度档只决定「多大的震动才值得震」。
         *   门限 t = (1-intensity)²：t = 1-intensity 时 25% 档要
         *   level>191 才震，实测把攻击重击也滤掉了（重击 RMS ~2000-5000
         *   → level 140~190）。改平方后 25% 档静默线 = 255·0.5625 ≈ 143：
         *   走路/UI（实测 level ≤ 90）依旧安静，攻击重击（≥150）能过线，
         *   爆炸（level 210+）满量程。
         *   gamma = 0.5+0.5·intensity：低强度档过门限后爬升更陡。
         *   intensity→100 时 t→0、gamma→1，完全退化为原线性行为。 */
        double f = level / 255.0;
        double q = (100 - intensity_pct) / 100.0;
        double t = q * q;
        if (f <= t) {
            level = 0;
        } else {
            double x = (f - t) / (1.0 - t);
            double gamma = 0.5 + 0.5 * intensity_pct / 100.0;
            level = (int)(pow(x, gamma) * 255.0);
        }
    }

    if (level <= 0) {
        if (S.rum_on) {
            motor_write(0, false);
            S.rum_on = false;
        }
        return;
    }

    if (dirty)
        S.rum_until = now + 300;    /* 有新事件就续期 */

    /* 超过保持时间就停，否则 PS5 停止发事件后马达会一直转 */
    if ((Sint32)(now - S.rum_until) > 0) {
        if (S.rum_on) {
            motor_write(0, false);
            S.rum_on = false;
        }
        return;
    }

    /* 每帧按占空比决定通断 */
    int uv = motor_uv_for(level);
    if (rumble_duty_on(level)) {
        if (!S.rum_on || uv != S.rum_voltage) {
            motor_write(uv, true);
            S.rum_on = true;
            S.rum_voltage = uv;
        }
    } else if (S.rum_on) {
        motor_write(0, false);
        S.rum_on = false;
    }
}

void stream_stop_rumble(void)
{
    if (S.elock) SDL_LockMutex(S.elock);
    if (motor_gpio_fd >= 0)
        motor_write(0, false);
    S.rum_left = 0;
    S.rum_right = 0;
    S.rum_dirty = false;
    S.rum_until = 0;
    S.rum_on = false;
    S.rum_voltage = 0;
    S.pulse_until = 0;
    if (S.elock) SDL_UnlockMutex(S.elock);
}

bool stream_get_regist_key(uint8_t out[16])
{
    if (!S.have_regkey) return false;
    memcpy(out, S.regkey, 16);
    return true;
}

bool stream_get_morning(uint8_t out[16])
{
    if (!S.have_regkey) return false;
    memcpy(out, S.morning, 16);
    return true;
}

/* ---------------- 独立注册（PIN 配对） ---------------- */

static ChiakiRegist g_regist;
static volatile int g_reg_state = 0;      /* 0 idle, 1 running, 2 ok, 3 fail */
static uint8_t g_reg_key[16];
static uint8_t g_reg_morning[16];   /* rp_key —— session 的 Ctrl 握手要它 */
static bool g_regist_live = false;       /* chiaki_regist_start 成功，需 stop+fini */
static volatile int g_reg_cancel = 0;    /* 已取消：忽略迟到的回调 */

/* chiaki_regist_start() 会 strdup(host) + 建 stop_pipe + 起线程，
 * 必须 chiaki_regist_stop() + chiaki_regist_fini() 配对回收，否则每次配对
 * 泄漏一份内存/两个 fd/一个线程。官方 regist.c:39/83-94。 */
static void regist_cleanup(void)
{
    if (!g_regist_live) return;
    /* stop 会让线程发 CANCELED 回调；不屏蔽的话它会把 g_reg_state 写成 3，
     * 打印一行莫名的 "regist FAILED"，甚至污染紧接着发起的新注册。 */
    g_reg_cancel = 1;
    chiaki_regist_stop(&g_regist);
    chiaki_regist_fini(&g_regist);
    g_regist_live = false;
}

static void regist_event_cb(ChiakiRegistEvent *ev, void *user)
{
    (void)user;
    if (g_reg_cancel) return;   /* 用户已取消，迟到的成功/失败回调一律丢弃 */
    if (ev->type == CHIAKI_REGIST_EVENT_TYPE_FINISHED_SUCCESS && ev->registered_host) {
        memcpy(g_reg_key, ev->registered_host->rp_regist_key, sizeof(g_reg_key));
        memcpy(g_reg_morning, ev->registered_host->rp_key, sizeof(g_reg_morning));
        g_reg_state = 2;
        SDL_Log("stream: regist SUCCESS (regist_key + rp_key captured)");
    } else {
        g_reg_state = 3;
        SDL_Log("stream: regist FAILED (type=%d)", (int)ev->type);
    }
}

int stream_register(const char *host, int ps5, unsigned int pin,
                    const char *psn_b64, ChiakiLog *log)
{
    /* 上一次配对若还在跑，先 stop+join 再覆盖 g_regist，否则会踩到正在
     * 被线程使用的 stop_pipe / info（UAF），并泄漏上一份资源。 */
    if (g_reg_state == 1 || g_regist_live)
        regist_cleanup();
    g_reg_state = 0;
    g_reg_cancel = 0;
    memset(&g_regist, 0, sizeof(g_regist));
    ChiakiRegistInfo info;
    memset(&info, 0, sizeof(info));
    info.host     = host;
    info.target   = ps5 ? CHIAKI_TARGET_PS5_1 : CHIAKI_TARGET_PS4_10;
    info.broadcast = false;
    info.pin         = pin;
    info.console_pin = pin;
    info.holepunch_info = NULL;
    info.rudp = NULL;

    if (psn_b64 && psn_b64[0]) {
        uint8_t acc[32];
        size_t sz = sizeof(acc);
        if (chiaki_base64_decode(psn_b64, strlen(psn_b64), acc, &sz) == CHIAKI_ERR_SUCCESS
            && sz == CHIAKI_PSN_ACCOUNT_ID_SIZE) {
            memcpy(info.psn_account_id, acc, CHIAKI_PSN_ACCOUNT_ID_SIZE);
            SDL_Log("stream: regist using PSN Account ID (%zu bytes)", sz);
        } else {
            SDL_Log("stream: regist PSN Account ID invalid (need base64 of %d bytes)",
                    CHIAKI_PSN_ACCOUNT_ID_SIZE);
        }
    }

    g_reg_state = 1;
    ChiakiErrorCode err = chiaki_regist_start(&g_regist, log, &info, regist_event_cb, NULL);
    if (err != CHIAKI_ERR_SUCCESS) {
        SDL_Log("stream: chiaki_regist_start failed: %s", chiaki_error_string(err));
        g_reg_state = 3;
        return -1;
    }
    g_regist_live = true;
    SDL_Log("stream: regist started (target=%d pin=%u)", (int)info.target, pin);
    return 0;
}

int stream_register_poll(uint8_t out_key[16], uint8_t out_morning[16])
{
    if (g_reg_state == 2) {
        if (out_key)
            memcpy(out_key, g_reg_key, 16);
        if (out_morning)
            memcpy(out_morning, g_reg_morning, 16);
        g_reg_state = 0;
        regist_cleanup();
        return 1;
    }
    if (g_reg_state == 3) {
        g_reg_state = 0;
        regist_cleanup();
        return -1;
    }
    return 0;
}

int stream_register_state(void)
{
    return g_reg_state == 1 ? 1 : 0;
}

void stream_register_cancel(void)
{
    g_reg_cancel = 1;
    regist_cleanup();
    g_reg_state = 0;
}
