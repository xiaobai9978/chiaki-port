// SPDX-License-Identifier: AGPL-3.0-only
#ifndef CHIAKI_BRICK_STREAM_H
#define CHIAKI_BRICK_STREAM_H

#include <stdbool.h>
#include <stdint.h>
#include <SDL.h>

#include <chiaki/log.h>
#include <chiaki/session.h>

typedef struct {
    int resolution;   /* ChiakiVideoResolutionPreset */
    int fps;          /* 30 / 60 */
    int bitrate;      /* kbps */
    int codec;        /* 0 = H.264, 1 = H.265 */
    int hwdec;        /* 1 = A133 cedar 硬解，0 = ffmpeg 软解（默认） */
} StreamCfg;

typedef enum {
    STREAM_EV_NONE = 0,
    STREAM_EV_PIN_REQUEST,
    STREAM_EV_CONNECTED,
    STREAM_EV_QUIT,
} StreamEvent;

int  stream_start(const char *host, bool ps5, const uint8_t *regkey,
                  const uint8_t *morning, const char *psn_id_b64,
                  const StreamCfg *cfg, ChiakiLog *log);
void stream_stop(void);
bool stream_running(void);
bool stream_connected(void);

/* Returns the first pending event, clearing it. arg receives event payload. */
StreamEvent stream_poll_event(int *arg);
void stream_submit_pin(const char *pin);

/* ---- 独立注册流程（PS5 的 PIN 配对必须走这条，session 里不会触发）----
 * stream_register() 是异步的：内部开线程做 regist。
 * 主线程用 stream_register_poll() 轮询结果：
 *   1  = 成功，out_key 收到 16 字节 regkey
 *   -1 = 失败
 *   0  = 还在进行
 */
int  stream_register(const char *host, int ps5, unsigned int pin,
                     const char *psn_b64, ChiakiLog *log);
int  stream_register_poll(uint8_t out_key[16], uint8_t out_morning[16]);
int  stream_register_state(void);   /* 0 idle / 1 running */
/* 取消进行中的配对并回收线程/pipe/strdup 内存；退出与按 START 返回时调用 */
void stream_register_cancel(void);

/* Use ARGB conversion for software renderers (no YUV texture support). */
void stream_set_use_argb(bool on);
void stream_set_view_size(int w, int h);
void stream_get_stats(int *fps, int *frames, int *proc_ms, int *rtt_ms,
                      int *lost, bool *hwdec, int *dec_ms, int *kbps);
void stream_dump_samples(bool on);

/* Video: uploads + draws the latest frame into dst (letterboxed by caller). */
bool stream_frame_size(int *w, int *h);
void stream_render(SDL_Renderer *ren, SDL_Rect dst);

void stream_send_controller(ChiakiControllerState *st);

/* 震动：这台机器的马达不走手柄 FF（虚拟 X360 手柄没有 FF 能力），
 * 而是 sysfs：/sys/class/motor/voltage 设强度，gpio243 当开关。
 * session 线程只记录强度，主循环调用 apply 真正写 sysfs。 */
void stream_apply_rumble(int intensity_pct);
void stream_stop_rumble(void);
void stream_motor_pulse(int pct, int ms);
void stream_tick_motor(void);   /* 每帧主循环调用，收尾震动测试脉冲 */

/* Valid after CHIAKI_EVENT_REGIST. */
bool stream_get_regist_key(uint8_t out[16]);
bool stream_get_morning(uint8_t out[16]);

#endif
