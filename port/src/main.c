// SPDX-License-Identifier: AGPL-3.0-only
// Chiaki Brick - a lightweight SDL2 frontend for libchiaki on the TrimUI Brick Pro.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <math.h>

#include <SDL.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/select.h>

#include <chiaki/log.h>
#include <chiaki/discovery.h>
#include <chiaki/discoveryservice.h>
#include <chiaki/controller.h>

#include "ui.h"
#include "stream.h"

#define APP_NAME "Chiaki Brick"

#define SCR_HOSTS    0
#define SCR_PIN      1
#define SCR_STREAM   2
#define SCR_MENU     3
#define SCR_SETTINGS 4
#define SCR_KEYMAP   5
#define SCR_KEYTEST  6
#define SCR_MESSAGE  7
#define SCR_ADD      8

enum {
    ACT_UP = 0, ACT_DOWN, ACT_LEFT, ACT_RIGHT,
    ACT_CONFIRM, ACT_BACK, ACT_START, ACT_SELECT,
    ACT_L1, ACT_R1, ACT_L2, ACT_R2,
    ACT_SQUARE, ACT_TRIANGLE, ACT_PS,
    ACT_L3, ACT_R3, ACT_TOUCHPAD,
    ACT_COUNT
};

static const char *act_names[ACT_COUNT] = {
    "十字键 上", "十字键 下", "十字键 左", "十字键 右",
    "× 键", "○ 键", "OPTIONS 键", "SHARE 键",
    "L1", "R1", "L2", "R2",
    "□ 键", "△ 键", "PS 键 (HOME)",
    "L3 (左摇杆按下)", "R3 (右摇杆按下)", "触摸板"
};

static const SDL_Scancode act_defaults[ACT_COUNT] = {
    SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
    SDL_SCANCODE_RETURN, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_SPACE, SDL_SCANCODE_TAB,
    SDL_SCANCODE_Q, SDL_SCANCODE_E, SDL_SCANCODE_1, SDL_SCANCODE_3,
    SDL_SCANCODE_LSHIFT, SDL_SCANCODE_RSHIFT, SDL_SCANCODE_ESCAPE,
    SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN
};

typedef struct {
    char host[64];
    int  ps5;
    char regkey[40];
    char morning[40];
    char regkey_host[64];
    int  resolution;   /* 1..4 */
    int  fps;          /* 30 / 60 */
    int  bitrate;      /* kbps */
    int  codec;        /* 0/1 */
    int  hwdec;        /* 1 = A133 cedar 硬解；0 = ffmpeg 软解 */
    int  autoconnect;
    int  swap_ab;
    int  swap_xy;
    int  rumble;       /* 震动强度 0..100，0 = 关闭 */
    int  vsync;        /* 1 = 开垂直同步（默认），0 = 关，串流追帧率时关掉 */
    int  osd;          /* 1 = 串流画面显示状态 OSD（默认），0 = 隐藏 */
    int  loglevel;     /* chiaki log bitmask: 7 = info, 15 = +verbose, 31 = +debug */
    int  key[ACT_COUNT];
} Config;

typedef struct {
    char name[64];
    char addr[80];
    int  ps5;
    char regkey[40];
    char morning[40];
    char psn[64];
    int  state;      /* 1 就绪 2 待机 0 未知 */
    int  saved;      /* 1 = 已写入 hosts.txt */
} HostEntry;

#define MAX_HOSTS 24

static struct {
    SDL_Window   *win;
    SDL_Renderer *ren;
    SDL_GameController *pad;
    SDL_Joystick *joy;
    int width, height;

    Config cfg;
    char   base[512];
    char   cfg_path[600];
    char   hosts_path[600];
    FILE  *logfp;

    HostEntry hosts[MAX_HOSTS];
    int       host_count;
    int       host_sel;

    int  screen;
    int  prev_screen;
    bool running;

    ChiakiDiscoveryService discovery;
    bool discovery_on;
    SDL_mutex *host_lock;

    /* pin entry */
    char pin[9];
    int  pin_len;
    int  pin_cursor;
    bool pin_incorrect;
    int  pin_mode;        /* 0 = 连接时 PS5 要的 PIN, 1 = 新主机配对 */
    int  regist_host;     /* 正在配对的主机索引 */

    /* settings */
    int settings_sel;

    /* keymap */
    int keymap_sel;
    int keymap_top;
    int keymap_wait;   /* -1 not waiting, else action index */

    /* message */
    char msg_title[128];
    char msg_body[512];

    SDL_Scancode last_scancode;
    int last_joy_button;

    bool manual_host;
    int  del_pending;      /* 删除确认 */
    int  active_host;      /* 正在串流的主机索引 */
    char add_name[64];
    char add_addr[80];
    char add_psn[64];
    int  add_field;      /* 0 = 名称, 1 = 地址, 2 = PSN ID */
    int  add_shift;
    int  add_ps5;
    int  add_cx, add_cy;   /* 软键盘光标 */
    bool add_wrap_l, add_wrap_r;  /* 长按连发到边界时只环绕一次，松手才复位 */
    int  edit_index;       /* >= 0 表示正在修改已有主机 */

    /* 方向键长按连发 */
    Uint32 rep_t0[5];
    Uint32 rep_last[5];
    Uint32 opt_press_ms;   /* SELECT+START 组合键计时（满 400ms 发 OPTIONS） */
    bool   opt_fired;
    Uint32 sel_press_ms;   /* 串流中 SELECT 按下时刻：250ms 内没等来 START 才发 SHARE */
    bool   sel_suppress;   /* 本回合 SELECT 被认定为组合键前半段，SHARE 不发 */
    Uint32 stats_t0;      /* 周期性把串流统计写进日志 */    Uint32 tap_until;
    uint32_t tap_bits;
    bool stream_quit_pending;
    int  stream_quit_reason;
    Uint32 cancel_hold_t0;   /* 串流「正在连接」界面长按 A 的计时（0=没按） */
    Uint32 osd_hot_t0;       /* L3+R3 同按计时，满 8 秒切换状态 OSD */
    bool   osd_hot_fired;    /* 本次按住已触发过，松手前不再重复切 */
    Uint32 osd_toast_until;  /* OSD 开关反馈条的显示截止时间 */

    /* 串流 OSD 缓存：每帧画几十个字符会吃掉 3-4fps，改成离屏纹理只在内容变化时重绘 */
    SDL_Texture *osd_top;
    SDL_Texture *osd_hint;      /* 底部两行固定提示，同样预渲染成离屏纹理 */
    int          osd_w, osd_hint_w;
    char         osd_key[192];
    char         osd_key_buf[192];   /* 1Hz 采样时构造，避免每帧 snprintf */
    /* OSD 上的统计值降频采样：这些数字每帧都在变，如果直接用实时值，
     * 下面 key 每秒会变 60 次，缓存根本命中不了，等于每帧重建纹理+几十次 draw call。 */
    int          osd_s_fps, osd_s_dec, osd_s_proc, osd_s_rtt, osd_s_lost;
    int          osd_s_kbps;        /* 1Hz 采样的实时码率 */
    char         osd_s_rate[16];    /* 格式化后的 "3369k" */
    int          osd_s_temp;        /* 1Hz 采样的 SoC 温度（摄氏，-1=不可用） */
    bool         osd_s_hwdec;
    Uint32       osd_s_t0;
} app;

/* 每场串流开始前复位 OSD 缓存：否则重连时若分辨率/统计值恰好和上一场
 * 相同，key 比较会命中，短暂显示上一场的数字；osd_top 目标纹理也会常驻显存。 */
static void osd_reset(void)
{
    if (app.osd_top) { SDL_DestroyTexture(app.osd_top); app.osd_top = NULL; }
    app.osd_key[0] = 0;
    app.osd_key_buf[0] = 0;
    app.osd_s_t0 = 0;
    app.osd_s_fps = app.osd_s_dec = app.osd_s_proc = 0;
    app.osd_s_rtt = app.osd_s_lost = app.osd_s_kbps = 0;
    app.osd_s_temp = -1;
    app.osd_s_rate[0] = 0;
    app.osd_w = 0;
}

/* SoC 温度：thermal_zone0 = cpu_thermal_zone（A133 实测 52000 上下 = 52°C）。
 * OSD 每秒才读一次，直接开文件无所谓；读不到返回 -1 隐藏该项。 */
static int read_soc_temp(void)
{
    static const char *paths[] = {
        "/sys/class/thermal/thermal_zone0/temp",
        "/sys/class/sunxi_temperature/soc_temp",
    };
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        FILE *f = fopen(paths[i], "r");
        if (!f) continue;
        long v = 0;
        int ok = (fscanf(f, "%ld", &v) == 1);
        fclose(f);
        if (!ok) continue;
        if (v > 1000) v /= 1000;          /* 毫度 → 度 */
        if (v > -50 && v < 150) return (int)v;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* logging                                                            */
/* ------------------------------------------------------------------ */

static void app_log_cb(ChiakiLogLevel level, const char *msg, void *user)
{
    (void)user;
    char line[2048];
    int n = snprintf(line, sizeof(line), "[%c] %s", chiaki_log_level_char(level), msg);
    if (n > 0) {
        SDL_Log("%s", line);
        if (app.logfp) {
            fputs(line, app.logfp);
            fputc('\n', app.logfp);
            /* 每条都 fflush 会在串流时和解码抢 SD 卡 IO；错误级才立即落盘，
             * 其余靠 stdio 缓冲，SDL_Log 那一路本来就已写进 run.log。 */
            if (chiaki_log_level_char(level) == 'E') fflush(app.logfp);
        }
    }
}

static ChiakiLog g_log;

/* ------------------------------------------------------------------ */
/* config                                                             */
/* ------------------------------------------------------------------ */

static void config_defaults(Config *c)
{
    memset(c, 0, sizeof(*c));
    c->ps5 = 1;
    c->resolution = 3;   /* 720p */
    c->fps = 30;
    c->bitrate = 10000;
    c->codec = 0;
    c->hwdec = 0;       /* 默认软解；config.ini 里 hwdec=1 打开 cedar 硬解 */
    c->autoconnect = 0;
    c->swap_ab = 0;
    c->swap_xy = 0;
    c->rumble = 60;
    c->vsync = 1;
    c->osd = 1;
    c->loglevel = 7;
    for (int i = 0; i < ACT_COUNT; i++)
        c->key[i] = act_defaults[i];
}

static int hex2bin(const char *hex, uint8_t *out, int max)
{
    int n = 0;
    while (hex[0] && hex[1] && n < max) {
        unsigned v;
        if (sscanf(hex, "%2x", &v) != 1) return -1;
        out[n++] = (uint8_t)v;
        hex += 2;
    }
    return n;
}

static void key_to_hex(const uint8_t *in, int n, char *out, size_t outsz)
{
    size_t o = 0;
    for (int i = 0; i < n && o + 3 < outsz; i++)
        o += (size_t)snprintf(out + o, outsz - o, "%02x", in[i]);
}

static void config_load(const char *path)
{
    config_defaults(&app.cfg);
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line;
        char *v = eq + 1;
        while (*v == ' ') v++;
        char *nl = strpbrk(v, "\r\n");
        if (nl) *nl = 0;

        if (!strcmp(k, "host")) snprintf(app.cfg.host, sizeof(app.cfg.host), "%s", v);
        else if (!strcmp(k, "ps5")) app.cfg.ps5 = atoi(v);
        else if (!strcmp(k, "regkey")) snprintf(app.cfg.regkey, sizeof(app.cfg.regkey), "%s", v);
        else if (!strcmp(k, "morning")) snprintf(app.cfg.morning, sizeof(app.cfg.morning), "%s", v);
        else if (!strcmp(k, "regkey_host")) snprintf(app.cfg.regkey_host, sizeof(app.cfg.regkey_host), "%s", v);
        else if (!strcmp(k, "resolution")) app.cfg.resolution = atoi(v);
        else if (!strcmp(k, "fps")) app.cfg.fps = atoi(v);
        else if (!strcmp(k, "bitrate")) app.cfg.bitrate = atoi(v);
        else if (!strcmp(k, "codec")) app.cfg.codec = atoi(v);
        else if (!strcmp(k, "hwdec")) app.cfg.hwdec = atoi(v);
        else if (!strcmp(k, "autoconnect")) app.cfg.autoconnect = atoi(v);
        else if (!strcmp(k, "swap_ab")) app.cfg.swap_ab = atoi(v);
        else if (!strcmp(k, "swap_xy")) app.cfg.swap_xy = atoi(v);
        else if (!strcmp(k, "rumble")) app.cfg.rumble = atoi(v);
        else if (!strcmp(k, "vsync")) app.cfg.vsync = atoi(v);
        else if (!strcmp(k, "osd")) app.cfg.osd = atoi(v);
        else if (!strcmp(k, "loglevel")) app.cfg.loglevel = atoi(v);
        else if (!strncmp(k, "key_", 4)) {
            int idx = atoi(k + 4);
            if (idx >= 0 && idx < ACT_COUNT) app.cfg.key[idx] = atoi(v);
        }
    }
    fclose(f);

    /* config.ini 是纯文本，手改或复用 research/ 测试档时可能写进越界值。
     * 统一 clamp，保证 hwdec_init 和 chiaki_connect_video_profile_preset
     * 看到的是同一份合法配置（后者会把枚举当数组下标用）。 */
    if (app.cfg.resolution < 1 || app.cfg.resolution > 4) app.cfg.resolution = 3;
    if (app.cfg.fps != 30 && app.cfg.fps != 60)             app.cfg.fps = 60;
    if (app.cfg.bitrate < 2000 || app.cfg.bitrate > 50000)   app.cfg.bitrate = 10000;
    if (app.cfg.codec != 0 && app.cfg.codec != 1)            app.cfg.codec = 0;
    if (app.cfg.hwdec != 0 && app.cfg.hwdec != 1)            app.cfg.hwdec = 0;
    if (app.cfg.rumble < 0 || app.cfg.rumble > 100)          app.cfg.rumble = 60;
    if (app.cfg.vsync != 0 && app.cfg.vsync != 1)            app.cfg.vsync = 1;
    if (app.cfg.osd != 0 && app.cfg.osd != 1)                app.cfg.osd = 1;
    if (app.cfg.loglevel < 1 || app.cfg.loglevel > 31)       app.cfg.loglevel = 7;
}

/* 掌机是电池设备，随时可能直接断电。先写同目录临时文件再 rename：
 * 同名文件系统内 rename 是原子的，最坏情况保留旧档，不会出现写一半、
 * hosts.txt 里的 regkey 被截断丢失。 */
static FILE *open_atomic(const char *path, char *tmppath, size_t tmpsz)
{
    snprintf(tmppath, tmpsz, "%s.tmp", path);
    return fopen(tmppath, "w");
}

static void finish_atomic(const char *tmppath, const char *path)
{
    fflush(NULL);
    if (rename(tmppath, path) != 0)
        remove(tmppath);
}

static void config_save(void)
{
    char tmp[544];
    FILE *f = open_atomic(app.cfg_path, tmp, sizeof(tmp));
    if (!f) return;
    fprintf(f, "host=%s\n", app.cfg.host);
    fprintf(f, "ps5=%d\n", app.cfg.ps5);
    fprintf(f, "regkey=%s\n", app.cfg.regkey);
    fprintf(f, "morning=%s\n", app.cfg.morning);
    fprintf(f, "regkey_host=%s\n", app.cfg.regkey_host);
    fprintf(f, "resolution=%d\n", app.cfg.resolution);
    fprintf(f, "fps=%d\n", app.cfg.fps);
    fprintf(f, "bitrate=%d\n", app.cfg.bitrate);
    fprintf(f, "codec=%d\n", app.cfg.codec);
    fprintf(f, "hwdec=%d\n", app.cfg.hwdec);
    fprintf(f, "autoconnect=%d\n", app.cfg.autoconnect);
    fprintf(f, "swap_ab=%d\n", app.cfg.swap_ab);
    fprintf(f, "swap_xy=%d\n", app.cfg.swap_xy);
    fprintf(f, "rumble=%d\n", app.cfg.rumble);
    fprintf(f, "vsync=%d\n", app.cfg.vsync);
    fprintf(f, "osd=%d\n", app.cfg.osd);
    fprintf(f, "loglevel=%d\n", app.cfg.loglevel);
    for (int i = 0; i < ACT_COUNT; i++)
        fprintf(f, "key_%d=%d\n", i, app.cfg.key[i]);
    fclose(f);
    finish_atomic(tmp, app.cfg_path);
}

/* ------------------------------------------------------------------ */
/* discovery                                                          */
/* ------------------------------------------------------------------ */

static int host_find(const char *addr)
{
    for (int i = 0; i < app.host_count; i++)
        if (!strcmp(app.hosts[i].addr, addr)) return i;
    return -1;
}

static void hosts_save(void)
{
    char tmp[544];
    FILE *f = open_atomic(app.hosts_path, tmp, sizeof(tmp));
    if (!f) return;
    for (int i = 0; i < app.host_count; i++) {
        HostEntry *h = &app.hosts[i];
        if (!h->saved || !h->addr[0]) continue;
        fprintf(f, "%s|%s|%d|%s|%s|%s\n", h->name, h->addr, h->ps5,
                h->regkey, h->morning, h->psn);
    }
    fclose(f);
    finish_atomic(tmp, app.hosts_path);
}

static void hosts_load(void)
{
    FILE *f = fopen(app.hosts_path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f) && app.host_count < MAX_HOSTS) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = 0;
        if (!line[0] || line[0] == '#') continue;
        char *parts[6] = { 0 };
        int n = 0;
        char *cur = line;
        parts[n++] = cur;
        for (char *q = line; *q && n < 6; q++) {
            if (*q == '|') { *q = 0; parts[n++] = q + 1; }
        }
        if (n < 2 || !parts[1][0]) continue;
        HostEntry *h = &app.hosts[app.host_count];
        memset(h, 0, sizeof(*h));
        snprintf(h->name, sizeof(h->name), "%s", parts[0][0] ? parts[0] : "PlayStation");
        snprintf(h->addr, sizeof(h->addr), "%s", parts[1]);
        h->ps5 = (n > 2) ? atoi(parts[2]) : 1;
        if (n > 3) snprintf(h->regkey, sizeof(h->regkey), "%s", parts[3]);
        if (n > 4) snprintf(h->morning, sizeof(h->morning), "%s", parts[4]);
        if (n > 5) snprintf(h->psn, sizeof(h->psn), "%s", parts[5]);
        h->saved = 1;
        app.host_count++;
    }
    fclose(f);
    SDL_Log("hosts: loaded %d saved hosts", app.host_count);
}

static int host_add(const char *name, const char *addr, int ps5)
{
    /* discovery_cb 在发现线程里也会往 app.hosts 里追加，改 host_count 必须互斥 */
    if (app.host_lock) SDL_LockMutex(app.host_lock);
    if (app.host_count >= MAX_HOSTS) {
        if (app.host_lock) SDL_UnlockMutex(app.host_lock);
        return -1;
    }
    HostEntry *h = &app.hosts[app.host_count];
    memset(h, 0, sizeof(*h));
    snprintf(h->name, sizeof(h->name), "%s", name);
    snprintf(h->addr, sizeof(h->addr), "%s", addr);
    h->ps5 = ps5;
    h->saved = 1;
    int idx = app.host_count++;
    hosts_save();
    if (app.host_lock) SDL_UnlockMutex(app.host_lock);
    return idx;
}

static void discovery_cb(ChiakiDiscoveryHost *hosts, size_t hosts_count, void *user)
{
    (void)user;
    if (!app.host_lock) return;
    SDL_LockMutex(app.host_lock);
    for (size_t i = 0; i < hosts_count; i++) {
        ChiakiDiscoveryHost *d = &hosts[i];
        if (!d->host_addr) continue;
        int idx = host_find(d->host_addr);
        if (idx >= 0) {
            app.hosts[idx].state = (int)d->state;
            app.hosts[idx].ps5 = chiaki_discovery_host_is_ps5(d) ? 1 : 0;
            if (!app.hosts[idx].saved && d->host_name)
                snprintf(app.hosts[idx].name, sizeof(app.hosts[idx].name), "%s", d->host_name);
            continue;
        }
        if (app.host_count >= MAX_HOSTS) continue;
        /* 先填完内容再增加 host_count：读侧（draw_hosts）无锁遍历时
         * 不会看到一个 addr/name 还是空字符串的半成品条目。 */
        HostEntry *h = &app.hosts[app.host_count];
        memset(h, 0, sizeof(*h));
        snprintf(h->addr, sizeof(h->addr), "%s", d->host_addr);
        snprintf(h->name, sizeof(h->name), "%s", d->host_name ? d->host_name : "PlayStation");
        h->ps5 = chiaki_discovery_host_is_ps5(d) ? 1 : 0;
        h->state = (int)d->state;
        h->saved = 0;
        app.host_count++;
    }
    SDL_UnlockMutex(app.host_lock);
}

static void discovery_start(void)
{
    if (app.discovery_on) return;

    ChiakiDiscoveryServiceOptions opt;
    memset(&opt, 0, sizeof(opt));
    opt.hosts_max = MAX_HOSTS;
    opt.host_drop_pings = 3;
    opt.ping_ms = 500;
    opt.ping_initial_ms = 500;
    opt.cb = discovery_cb;
    opt.cb_user = NULL;

    static struct sockaddr_storage send_addr;
    memset(&send_addr, 0, sizeof(send_addr));
    struct sockaddr_in *in = (struct sockaddr_in *)&send_addr;
    in->sin_family = AF_INET;
    in->sin_addr.s_addr = 0xffffffff;
    in->sin_port = htons(987);
    opt.send_addr = &send_addr;
    opt.send_addr_size = sizeof(*in);
    opt.send_host = NULL;
    opt.broadcast_addrs = NULL;
    opt.broadcast_num = 0;

    ChiakiErrorCode err = chiaki_discovery_service_init(&app.discovery, &opt, &g_log);
    if (err != CHIAKI_ERR_SUCCESS) {
        SDL_Log("discovery init failed: %s", chiaki_error_string(err));
        return;
    }
    app.discovery_on = true;
}

static void discovery_stop(void)
{
    if (!app.discovery_on) return;
    chiaki_discovery_service_fini(&app.discovery);
    app.discovery_on = false;
}

/* ------------------------------------------------------------------ */
/* input                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    bool up, down, left, right;
    bool confirm, back, start, select;
    bool l1, r1, l2, r2, square, triangle, ps;
    bool l3, r3, touchpad;
    int  lx, ly, rx, ry;   /* -32768..32767 */
} Input;

/* 自测模式（环境变量 CHIAKI_AUTOTEST=1）：两个摇杆不停转圈，
 * 用来在没有手柄输入的情况下压测「镜头一直转」这种高负载场景。 */
static bool g_autotest = false;
/* 关掉串流 OSD（CHIAKI_NOOSD=1）：用来量化「每帧几十次文字 draw call」的开销 */
static bool g_noosd = false;

static Input in_now, in_prev;

static bool key_active(const Uint8 *ks, int act)
{
    SDL_Scancode sc = (SDL_Scancode)app.cfg.key[act];
    if (sc <= SDL_SCANCODE_UNKNOWN || sc >= SDL_NUM_SCANCODES) return false;
    return ks[sc] != 0;
}

/* Key codes: >=0 SDL scancode | -1..-99 raw joystick button | <=-101 gamecontroller button */
#define CTRL_CODE_OFFSET 100

static bool joy_active(int act)
{
    int code = app.cfg.key[act];
    if (code >= 0) return false;
    if (code <= -(CTRL_CODE_OFFSET + 1)) {
        if (!app.pad) return false;
        int btn = -code - CTRL_CODE_OFFSET - 1;
        return SDL_GameControllerGetButton(app.pad, (SDL_GameControllerButton)btn) != 0;
    }
    if (!app.joy) return false;
    int btn = -code - 1;
    if (btn >= SDL_JoystickNumButtons(app.joy)) return false;
    return SDL_JoystickGetButton(app.joy, btn) != 0;
}

static int axis_dz(int v);

static void input_poll(void)
{
    in_prev = in_now;
    memset(&in_now, 0, sizeof(in_now));

    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    in_now.up = key_active(ks, ACT_UP) || joy_active(ACT_UP);
    in_now.down = key_active(ks, ACT_DOWN) || joy_active(ACT_DOWN);
    in_now.left = key_active(ks, ACT_LEFT) || joy_active(ACT_LEFT);
    in_now.right = key_active(ks, ACT_RIGHT) || joy_active(ACT_RIGHT);
    in_now.confirm = key_active(ks, ACT_CONFIRM) || joy_active(ACT_CONFIRM);
    in_now.back = key_active(ks, ACT_BACK) || joy_active(ACT_BACK);
    in_now.start = key_active(ks, ACT_START) || joy_active(ACT_START);
    in_now.select = key_active(ks, ACT_SELECT) || joy_active(ACT_SELECT);
    in_now.l1 = key_active(ks, ACT_L1) || joy_active(ACT_L1);
    in_now.r1 = key_active(ks, ACT_R1) || joy_active(ACT_R1);
    in_now.l2 = key_active(ks, ACT_L2) || joy_active(ACT_L2);
    in_now.r2 = key_active(ks, ACT_R2) || joy_active(ACT_R2);
    in_now.square = key_active(ks, ACT_SQUARE) || joy_active(ACT_SQUARE);
    in_now.triangle = key_active(ks, ACT_TRIANGLE) || joy_active(ACT_TRIANGLE);
    in_now.ps = key_active(ks, ACT_PS) || joy_active(ACT_PS);

    if (app.pad) {
        int lx = SDL_GameControllerGetAxis(app.pad, SDL_CONTROLLER_AXIS_LEFTX);
        int ly = SDL_GameControllerGetAxis(app.pad, SDL_CONTROLLER_AXIS_LEFTY);
        int rx = SDL_GameControllerGetAxis(app.pad, SDL_CONTROLLER_AXIS_RIGHTX);
        int ry = SDL_GameControllerGetAxis(app.pad, SDL_CONTROLLER_AXIS_RIGHTY);
        in_now.lx = axis_dz(lx); in_now.ly = axis_dz(ly);
        in_now.rx = axis_dz(rx); in_now.ry = axis_dz(ry);

        /* Menu navigation only: while streaming the sticks must stay analog. */
        if (app.screen != SCR_STREAM) {
            if (ly < -12000) in_now.up = true;
            if (ly >  12000) in_now.down = true;
            if (lx < -12000) in_now.left = true;
            if (lx >  12000) in_now.right = true;
        }

        if (SDL_GameControllerGetButton(app.pad, SDL_CONTROLLER_BUTTON_A)) in_now.confirm = true;
        if (SDL_GameControllerGetButton(app.pad, SDL_CONTROLLER_BUTTON_B)) in_now.back = true;
        if (SDL_GameControllerGetButton(app.pad, SDL_CONTROLLER_BUTTON_X)) in_now.square = true;
        if (SDL_GameControllerGetButton(app.pad, SDL_CONTROLLER_BUTTON_Y)) in_now.triangle = true;
        if (SDL_GameControllerGetButton(app.pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) in_now.l1 = true;
        if (SDL_GameControllerGetButton(app.pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) in_now.r1 = true;
        if (SDL_GameControllerGetButton(app.pad, SDL_CONTROLLER_BUTTON_LEFTSTICK)) in_now.l3 = true;
        if (SDL_GameControllerGetButton(app.pad, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) in_now.r3 = true;
        if (SDL_GameControllerGetButton(app.pad, SDL_CONTROLLER_BUTTON_GUIDE)) in_now.ps = true;
        if (SDL_GameControllerGetButton(app.pad, SDL_CONTROLLER_BUTTON_START)) in_now.start = true;
        if (SDL_GameControllerGetButton(app.pad, SDL_CONTROLLER_BUTTON_BACK)) in_now.select = true;
        if (SDL_GameControllerGetButton(app.pad, SDL_CONTROLLER_BUTTON_DPAD_UP)) in_now.up = true;
        if (SDL_GameControllerGetButton(app.pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) in_now.down = true;
        if (SDL_GameControllerGetButton(app.pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) in_now.left = true;
        if (SDL_GameControllerGetButton(app.pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) in_now.right = true;
        if (SDL_GameControllerGetAxis(app.pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8000) in_now.l2 = true;
        if (SDL_GameControllerGetAxis(app.pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8000) in_now.r2 = true;
    } else if (app.joy) {
        int naxes = SDL_JoystickNumAxes(app.joy);
        int lx = naxes > 0 ? SDL_JoystickGetAxis(app.joy, 0) : 0;
        int ly = naxes > 1 ? SDL_JoystickGetAxis(app.joy, 1) : 0;
        int rx = naxes > 2 ? SDL_JoystickGetAxis(app.joy, 2) : 0;
        int ry = naxes > 3 ? SDL_JoystickGetAxis(app.joy, 3) : 0;
        in_now.lx = axis_dz(lx); in_now.ly = axis_dz(ly);
        in_now.rx = axis_dz(rx); in_now.ry = axis_dz(ry);
        if (app.screen != SCR_STREAM) {
            if (ly < -12000) in_now.up = true;
            if (ly >  12000) in_now.down = true;
            if (lx < -12000) in_now.left = true;
            if (lx >  12000) in_now.right = true;
        }
    }

    /* 掌机手柄的 A/B、X/Y 与 PS 布局相反，按配置交换 */
    if (app.cfg.swap_ab) {
        bool t = in_now.confirm; in_now.confirm = in_now.back; in_now.back = t;
    }
    if (app.cfg.swap_xy) {
        bool t = in_now.square; in_now.square = in_now.triangle; in_now.triangle = t;
    }
}

/* small dead zone so a resting stick does not drift the PlayStation cursor */
static int axis_dz(int v)
{
    if (v > -1500 && v < 1500) return 0;
    return v;
}

static bool pressed(bool now, bool prev) { return now && !prev; }

/* ---------------- 方向键长按连发 ----------------
 * 按下立刻响应一次（和原来手感一致），按住超过 delay 后每 rate 毫秒再响应一次。
 * 掌机屏幕小，软键盘上逐格挪太慢，长按必须能快速扫过。 */
#define REP_UP    0
#define REP_DOWN  1
#define REP_LEFT  2
#define REP_RIGHT 3
#define REP_Y     4   /* Y 键退格也用连发 */

static bool rep_fire(bool now, bool prev, int id, Uint32 delay_ms, Uint32 rate_ms)
{
    Uint32 ms = SDL_GetTicks();
    if (id < 0 || id > 4) return false;

    if (!now) {
        app.rep_t0[id] = 0;
        app.rep_last[id] = 0;
        return false;
    }
    if (!prev || app.rep_t0[id] == 0) {
        app.rep_t0[id] = ms;
        app.rep_last[id] = ms;
        return true;
    }
    if ((Uint32)(ms - app.rep_t0[id]) < delay_ms) return false;
    if ((Uint32)(ms - app.rep_last[id]) < rate_ms) return false;
    app.rep_last[id] = ms;
    return true;
}

#define REP_DELAY_MS 380
#define REP_RATE_MS   75

/* ------------------------------------------------------------------ */
/* colors                                                             */
/* ------------------------------------------------------------------ */

static const SDL_Color C_BG      = { 16, 18, 24, 255 };
static const SDL_Color C_PANEL   = { 30, 34, 44, 255 };
static const SDL_Color C_SEL     = { 46, 92, 168, 255 };
static const SDL_Color C_TEXT    = { 232, 236, 244, 255 };
static const SDL_Color C_DIM     = { 140, 148, 162, 255 };
static const SDL_Color C_ACCENT  = { 96, 176, 255, 255 };
static const SDL_Color C_WARN    = { 240, 96, 88, 255 };
static const SDL_Color C_OK      = { 96, 208, 128, 255 };

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static void message(const char *title, const char *body)
{
    snprintf(app.msg_title, sizeof(app.msg_title), "%s", title ? title : "");
    snprintf(app.msg_body, sizeof(app.msg_body), "%s", body ? body : "");
    app.prev_screen = app.screen;
    app.screen = SCR_MESSAGE;
}

static const char *res_name(int r)
{
    switch (r) {
    case 1: return "360p";
    case 2: return "540p";
    case 3: return "720p";
    case 4: return "1080p";
    }
    return "?";
}

static bool port_open(const char *host, int port, int timeout_ms)
{
    struct addrinfo hints, *res = NULL;
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0)
        return false;

    bool ok = false;
    for (struct addrinfo *ai = res; ai && !ok; ai = ai->ai_next) {
        int fd = socket(ai->ai_family, SOCK_STREAM, 0);
        if (fd < 0) continue;
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int r = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (r == 0) {
            ok = true;
        } else if (errno == EINPROGRESS) {
            fd_set wf;
            FD_ZERO(&wf);
            if (fd >= FD_SETSIZE) {
                /* select 的 fd_set 是定长位图，越界会踩栈；直接放弃这次探测 */
                close(fd);
                freeaddrinfo(res);
                return false;
            }
            FD_SET(fd, &wf);
            struct timeval tv;
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            if (select(fd + 1, NULL, &wf, NULL, &tv) > 0) {
                int err = 0;
                socklen_t len = sizeof(err);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0)
                    ok = true;
            }
        }
        close(fd);
    }
    freeaddrinfo(res);
    return ok;
}

/* Build the wakeup credential: the regist key's ASCII prefix parsed as hex. */
static uint64_t wakeup_credential(const uint8_t *key, int klen)
{
    char ascii[9];
    int n = 0;
    for (int i = 0; i < 8 && i < klen; i++) {
        if (key[i] == 0) break;
        if (key[i] < 0x20 || key[i] > 0x7e) return 0;
        ascii[n++] = (char)key[i];
    }
    ascii[n] = 0;
    if (n < 4) return 0;
    return strtoull(ascii, NULL, 16);
}

/* 连接是阻塞的：主机在待机时要等它醒来，最长二十秒。
 * 这段负责在等待期间刷新界面，否则用户看到的就是整个程序卡死。
 * 取消方式是长按 A(○) 0.7 秒（防误触，提示文案已写明）。 */
static void draw_connecting(const char *addr, int secs)
{
    int W = app.width, H = app.height;
    SDL_SetRenderDrawColor(app.ren, C_BG.r, C_BG.g, C_BG.b, 255);
    SDL_RenderClear(app.ren);

    ui_text_centered(W / 2, H / 2 - 90, UI_SIZE_LARGE, C_TEXT, "正在连接");
    ui_text_centered(W / 2, H / 2 - 20, UI_SIZE_NORMAL, C_ACCENT, "%s", addr);
    if (secs >= 0)
        ui_text_centered(W / 2, H / 2 + 34, UI_SIZE_SMALL, C_DIM,
                         "主机暂未响应，已发送唤醒包… 等待 %d 秒", secs);
    else
        ui_text_centered(W / 2, H / 2 + 34, UI_SIZE_SMALL, C_DIM, "正在建立会话…");
    ui_text_centered(W / 2, H - 60, UI_SIZE_SMALL, C_DIM, "长按 A(○) 0.7 秒取消");
    SDL_RenderPresent(app.ren);
}

/* 长按 A(○) 0.7 秒取消：back 键连续按住超过 700ms 才算，松开即归零。
 * 界面上的提示文案与之对应（「长按 A(○) 0.7 秒取消」）。
 * 之所以必须配 50ms 轮询：A 键状态是轮询读的，原来唤醒循环要
 * ~700ms 才轮到一次采样（connect 300ms + SDL_Delay 400ms），
 * 计时忽断忽续，长按 0.7 秒经常凑不满，体感就是按了没反应。 */
static bool back_hold_cancel(void)
{
    static Uint32 hold_from = 0;
    if (!in_now.back) {
        hold_from = 0;
        return false;
    }
    Uint32 now = SDL_GetTicks();
    if (!hold_from) {
        hold_from = now;
    } else if (now - hold_from > 700) {
        hold_from = 0;
        return true;
    }
    return false;
}

static bool wait_tick(const char *addr, int secs)
{
    draw_connecting(addr, secs);
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) {
            app.running = false;
            return false;
        }
    }
    input_poll();
    return !back_hold_cancel();
}

/* 等待用的延时：每 50ms 刷一次输入并检查长按取消，
 * 保证长按 A 的计时连续、0.7 秒内必定响应。 */
static bool cancellable_delay(Uint32 ms)
{
    Uint32 start = SDL_GetTicks();
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                app.running = false;
                return false;
            }
        }
        input_poll();
        if (back_hold_cancel())
            return false;
        if (SDL_GetTicks() - start >= ms)
            return true;
        SDL_Delay(50);
    }
}

static void start_session(void)
{
    /* 把主机条目复制成快照：下面的 wait_tick 最长要等二十几秒，
     * 期间发现回调可能改写 app.hosts[]（例如 ps5 标志、名称）。
     * 以前一直持有指向共享数组的指针，会把中途变化的值带进会话。 */
    HostEntry h;
    if (app.host_lock) SDL_LockMutex(app.host_lock);
    if (app.host_sel < 0 || app.host_sel >= app.host_count) {
        if (app.host_lock) SDL_UnlockMutex(app.host_lock);
        message("没有可用主机", "按 Y(□) 添加主机，或等待自动搜索到 PS5。");
        return;
    }
    h = app.hosts[app.host_sel];
    if (app.host_lock) SDL_UnlockMutex(app.host_lock);

    /* 没有 regkey 说明还没配对。以前这里直接去连，PS5 对未配对的 session
     * 请求根本不回包，表现就是"点了没反应"。现在直接进 PIN 界面。 */
    if (!h.regkey[0]) {
        app.regist_host = app.host_sel;
        app.pin_mode = 1;
        app.pin_len = 0;
        app.pin[0] = 0;
        app.pin_incorrect = false;
        app.pin_cursor = 0;
        app.screen = SCR_PIN;
        return;
    }

    StreamCfg sc;
    sc.resolution = app.cfg.resolution;
    sc.fps = app.cfg.fps;
    sc.bitrate = app.cfg.bitrate;
    sc.codec = app.cfg.codec;
    sc.hwdec = app.cfg.hwdec;

    uint8_t key[16];
    uint8_t morn[16];
    int klen = hex2bin(h.regkey, key, 16);
    int mlen = hex2bin(h.morning, morn, 16);
    const uint8_t *kr = (klen == 16) ? key : NULL;
    const uint8_t *mr = (mlen == 16) ? morn : NULL;

    app.active_host = app.host_sel;
    app.pin_len = 0;
    app.pin[0] = 0;
    app.pin_incorrect = false;
    app.stream_quit_pending = false;
    osd_reset();

    /* 主机在待机或暂时不响应：先唤醒并等它起来 */
    if (!wait_tick(h.addr, -1))
        return;

    if (!port_open(h.addr, 9295, 300)) {
        uint64_t cred = (kr) ? wakeup_credential(key, klen) : 0;
        if (app.discovery_on)
            chiaki_discovery_wakeup(&g_log, &app.discovery.discovery, h.addr, cred,
                                    h.ps5 ? true : false);
        for (int i = 0; i < 26; i++) {
            if (port_open(h.addr, 9295, 300))
                break;
            if (!wait_tick(h.addr, (i + 1) / 2))
                return;
            /* 原来这里是 SDL_Delay(400)：等待期间完全不轮询输入，
             * 一次采样间隔长达 ~700ms，按一下就松开根本采不到。 */
            if (!cancellable_delay(400))
                return;
        }
    }

    if (stream_start(h.addr, h.ps5 ? true : false, kr, mr, h.psn, &sc, &g_log) != 0) {
        message("启动失败", "无法建立串流会话，详情见 chiaki.log。");
        return;
    }
    discovery_stop();
    app.screen = SCR_STREAM;
}

static void stop_session(void)
{
    stream_register_cancel();  /* 配对进行中退出时收掉 regist 线程/pipe/内存 */
    stream_stop_rumble();          /* 先把马达停掉，别让它在菜单里一直转 */
    stream_stop();
    discovery_start();
    app.screen = SCR_HOSTS;
}

/* ------------------------------------------------------------------ */
/* screens                                                            */
/* ------------------------------------------------------------------ */

static const char *host_state_text(const HostEntry *h)
{
    if (!h->regkey[0]) return "未配对";
    if (h->state == 2) return "待机";
    if (h->state == 1) return "在线";
    return "已配对";
}

static void draw_hosts(void)
{
    int W = app.width, H = app.height;
    SDL_SetRenderDrawColor(app.ren, C_BG.r, C_BG.g, C_BG.b, 255);
    SDL_RenderClear(app.ren);

    ui_text(40, 22, UI_SIZE_LARGE, C_TEXT, "Chiaki 串流");
    ui_text(40, 78, UI_SIZE_SMALL, C_DIM, "%s   ·   共 %d 台主机",
            app.discovery_on ? "正在搜索…" : "未搜索", app.host_count);

    int y = 120;
    if (app.host_count == 0) {
        ui_text(40, y + 20, UI_SIZE_NORMAL, C_DIM, "还没有主机");
        ui_text(40, y + 66, UI_SIZE_SMALL, C_DIM, "按  Y  手动添加（输入 IP 或域名）");
        ui_text(40, y + 98, UI_SIZE_SMALL, C_DIM, "或让掌机与 PS5 在同一网络，自动搜索会列出来");
    } else {
        int visible = 5;
        int first = app.host_sel - visible / 2;
        if (first < 0) first = 0;
        if (first > app.host_count - visible) first = app.host_count - visible;
        if (first < 0) first = 0;
        for (int i = first; i < app.host_count && i < first + visible; i++) {
            HostEntry *h = &app.hosts[i];
            SDL_Rect r = { 32, y, W - 64, 92 };
            bool sel = (i == app.host_sel);
            ui_rect(r.x, r.y, r.w, r.h, sel ? C_SEL : C_PANEL);
            ui_text(r.x + 20, r.y + 10, UI_SIZE_NORMAL, C_TEXT, "%s", h->name);
            ui_text(r.x + 20, r.y + 54, UI_SIZE_SMALL, C_DIM, "%s  ·  %s",
                    h->addr, h->ps5 ? "PS5" : "PS4");
            ui_text_right(r.x + r.w - 18, r.y + 16, UI_SIZE_SMALL,
                          h->regkey[0] ? C_OK : C_WARN, "%s", host_state_text(h));
            y += 100;
        }
    }

    if (app.del_pending && app.host_sel >= 0 && app.host_sel < app.host_count) {
        ui_rect(32, H - 150, W - 64, 46, (SDL_Color){ 120, 40, 40, 220 });
        ui_text(52, H - 142, UI_SIZE_SMALL, C_TEXT, "删除「%s」？   B(×) 确认   A(○) 取消",
                app.hosts[app.host_sel].name);
    } else {
        ui_text(40, H - 150, UI_SIZE_SMALL, C_DIM, "当前设置：%s / %d fps / %d kbps / %s",
                res_name(app.cfg.resolution), app.cfg.fps, app.cfg.bitrate,
                app.cfg.codec ? "H.265" : "H.264");
    }

    ui_text(40, H - 100, UI_SIZE_SMALL, C_DIM,
            "B(×) 连接   Y(□) 添加   X(△) 删除   L2 修改   R2 配对");
    ui_text(40, H - 66, UI_SIZE_SMALL, C_DIM,
            "L1 唤醒   R1 设置   SELECT 键位   A(○) 退出");
}

static void draw_pin(void)
{
    int W = app.width, H = app.height;
    SDL_SetRenderDrawColor(app.ren, C_BG.r, C_BG.g, C_BG.b, 255);
    SDL_RenderClear(app.ren);

    ui_text(40, 26, UI_SIZE_LARGE, C_TEXT, "%s",
            app.pin_mode == 1 ? "与 PS5 配对" : "PS5 要求配对码");
    ui_text(40, 74, UI_SIZE_SMALL, C_DIM,
            app.pin_mode == 1 ? "先在 PS5「设置-系统-远程游玩-关联设备-显示 PIN」，再输入那 8 位数字"
                              : "请输入 PS5 电视画面上显示的 8 位数字");

    /* digit boxes */
    int bw = 54, gap = 12;
    int total = 8 * bw + 7 * gap;
    int x0 = (W - total) / 2;
    for (int i = 0; i < 8; i++) {
        SDL_Rect r = { x0 + i * (bw + gap), 120, bw, 72 };
        ui_rect(r.x, r.y, r.w, r.h, C_PANEL);
        if (i < app.pin_len) {
            char s[2] = { app.pin[i], 0 };
            ui_text_centered(r.x + bw / 2, r.y + 22, UI_SIZE_LARGE, C_TEXT, "%s", s);
        }
    }
    if (app.pin_incorrect)
        ui_text(40, 210, UI_SIZE_SMALL, C_WARN, "配对码错误，请重试");

    /* keypad 3x4，贴住屏幕底部 */
    const char *keys[12] = { "1","2","3","4","5","6","7","8","9","DEL","0","OK" };
    int cw = 110, ch = 88, cg = 14;
    int gx = (W - (3 * cw + 2 * cg)) / 2;
    int gy = H - 56 - (4 * ch + 3 * cg);
    if (gy < 240) gy = 240;
    for (int i = 0; i < 12; i++) {
        int col = i % 3, row = i / 3;
        SDL_Rect r = { gx + col * (cw + cg), gy + row * (ch + cg), cw, ch };
        bool sel = (i == app.pin_cursor);
        ui_rect(r.x, r.y, r.w, r.h, sel ? C_SEL : C_PANEL);
        ui_text_centered(r.x + cw / 2, r.y + ch / 2 - 15, UI_SIZE_NORMAL, C_TEXT, "%s", keys[i]);
    }

    ui_text(40, H - 42, UI_SIZE_SMALL, C_DIM, "B(×) 选择   Y(□) 删除   START 取消");
}

static void draw_stream(void)
{
    int W = app.width, H = app.height;
    SDL_SetRenderDrawColor(app.ren, 0, 0, 0, 255);
    SDL_RenderClear(app.ren);

    SDL_Rect dst = { 0, 0, W, H };
    int fw = 0, fh = 0;
    if (stream_frame_size(&fw, &fh) && fw > 0 && fh > 0) {
        float sa = (float)fw / (float)fh;
        float wa = (float)W / (float)H;
        if (sa > wa) {
            dst.w = W;
            dst.h = (int)(W / sa);
        } else {
            dst.h = H;
            dst.w = (int)(H * sa);
        }
        dst.x = (W - dst.w) / 2;
        dst.y = (H - dst.h) / 2;
        stream_set_view_size(dst.w, dst.h);
    }
    stream_render(app.ren, dst);

    /* 16:9 画面在 4:3 屏上留出上下黑边，提示信息优先写在黑边里 */
    int top_h = dst.y;
    int bot_y = dst.y + dst.h;
    int bot_h = H - bot_y;

    /* L3+R3 切换 OSD 的反馈条：必须画在 OSD 开关与连接判断之前，
     * 否则「关到开」看得到、「开到关」或者还没连上时按，都像没生效一样。 */
    if (app.osd_toast_until && SDL_GetTicks() < app.osd_toast_until) {
        SDL_Rect bar = { W / 2 - 150, H - 66, 300, 44 };
        ui_rect(bar.x, bar.y, bar.w, bar.h, (SDL_Color){ 0, 0, 0, 170 });
        ui_frame(bar.x, bar.y, bar.w, bar.h, C_SEL, 2);
        ui_text_centered(W / 2, bar.y + 12, UI_SIZE_SMALL, C_ACCENT,
                         app.cfg.osd ? "状态 OSD：显示" : "状态 OSD：隐藏");
    }

    if (!stream_connected()) {
        /* 连接状态反馈不受 OSD 开关影响——关 OSD 的人更需要看到它卡住了 */
        ui_text_centered(W / 2, dst.y + dst.h / 2 - 16, UI_SIZE_NORMAL, C_TEXT, "正在连接…");
        ui_text_centered(W / 2, dst.y + dst.h / 2 + 22, UI_SIZE_SMALL, C_DIM,
                         "长按 A(○) 0.7 秒取消");
        return;
    }
    /* 提示文字属于黑边信息区，跟随 OSD 开关；CHIAKI_NOOSD=1 是调试用的强制关闭 */
    if (g_noosd || !app.cfg.osd)
        return;
    /* 每秒采样一次统计值；中间帧沿用上次的数字，避免 OSD 疯狂重绘 */
    int frames = 0;   /* stream_get_stats 需要这个出参，界面不再显示 */
    Uint32 now_ms = SDL_GetTicks();
    bool osd_sampled = false;
    if (app.osd_s_t0 == 0 || (Uint32)(now_ms - app.osd_s_t0) >= 1000) {
        stream_get_stats(&app.osd_s_fps, &frames, &app.osd_s_proc,
                         &app.osd_s_rtt, &app.osd_s_lost, &app.osd_s_hwdec,
                         &app.osd_s_dec, &app.osd_s_kbps);
        (void)frames;
        /* 码率统一用 k（用户要求），6Mbps 显示 6000k 也够直观 */
        snprintf(app.osd_s_rate, sizeof(app.osd_s_rate), "%dk",
                 app.osd_s_kbps);
        app.osd_s_temp = read_soc_temp();
        app.osd_s_t0 = now_ms ? now_ms : 1;
        osd_sampled = true;
    }
    int fps = app.osd_s_fps, proc = app.osd_s_proc;
    int rtt = app.osd_s_rtt, lost = app.osd_s_lost, dec = app.osd_s_dec;
    bool hwdec = app.osd_s_hwdec;
    int want_fps = app.cfg.fps;
    const char *rate = app.osd_s_rate;
    char temp_txt[16] = "";
    if (app.osd_s_temp >= 0)
        snprintf(temp_txt, sizeof(temp_txt), "  %d°C", app.osd_s_temp);

    if (top_h >= 34) {
        /* 顶部 OSD 缓存到离屏纹理：字符多，每帧重绘要 30+ 次 draw call */
        /* key 只在 1Hz 采样帧构造：以前每帧都做一次 11 个字段的
         * snprintf，即使内容整整一秒不变。每帧只留一次 strcmp。 */
        if (osd_sampled || app.osd_key_buf[0] == 0)
            snprintf(app.osd_key_buf, sizeof(app.osd_key_buf),
                     "%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%s|%s",
                     fw, fh, app.cfg.codec, (int)hwdec, fps, dec, proc, rtt, lost,
                     want_fps, rate, temp_txt);
        if (app.osd_top && app.osd_w != W) {
            SDL_DestroyTexture(app.osd_top);
            app.osd_top = NULL;
        }
        if (!app.osd_top) {
            app.osd_top = SDL_CreateTexture(app.ren, SDL_PIXELFORMAT_ARGB8888,
                                            SDL_TEXTUREACCESS_TARGET, W, 34);
            if (app.osd_top)
                SDL_SetTextureBlendMode(app.osd_top, SDL_BLENDMODE_BLEND);
            app.osd_w = W;
            app.osd_key[0] = 0;
        }
        if (app.osd_top && strcmp(app.osd_key_buf, app.osd_key) != 0) {
            SDL_SetRenderTarget(app.ren, app.osd_top);
            SDL_SetRenderDrawColor(app.ren, 0, 0, 0, 0);
            SDL_RenderClear(app.ren);
            ui_text(24, 2, UI_SIZE_SMALL, C_ACCENT, "PS5 串流  %dx%d  %s · %s",
                    fw, fh, app.cfg.codec ? "H.265" : "H.264", hwdec ? "硬解" : "软解");
            if (rtt >= 0)
                ui_text_right(W - 24, 2, UI_SIZE_SMALL, C_ACCENT,
                              "%d/%d fps  解 %dms  转 %dms  RTT %dms  丢 %d  码 %s%s",
                              fps, want_fps, dec, proc, rtt, lost, rate, temp_txt);
            else
                ui_text_right(W - 24, 2, UI_SIZE_SMALL, C_ACCENT,
                              "%d/%d fps  解 %dms  转 %dms  丢 %d  码 %s%s",
                              fps, want_fps, dec, proc, lost, rate, temp_txt);
            SDL_SetRenderTarget(app.ren, NULL);
            snprintf(app.osd_key, sizeof(app.osd_key), "%s", app.osd_key_buf);
        }
        if (app.osd_top) {
            SDL_Rect r = { 0, dst.y - 34, W, 34 };
            SDL_RenderCopy(app.ren, app.osd_top, NULL, &r);
        }
    } else {
        ui_text(20, 8, UI_SIZE_SMALL, C_ACCENT,
                "%d/%d fps  解%d 转%d  RTT%d  丢%d  码%s%s",
                fps, want_fps, dec, proc, rtt, lost, rate, temp_txt);
    }

    /* 纹理高度必须容得下第二行文字（y=22 + 字高约 20），34px 会把下半截裁掉 */
    if (bot_h >= 48) {
        /* 两行提示是固定文字，以前每帧要走 45+ 次 glyph 查询和 draw call；
         * 和顶部 OSD 一样预渲染成离屏纹理，之后每帧只 RenderCopy 一次。
         * 顺序按实际按键：PS -> SHARE -> START(触摸板/OPTIONS)，菜单单独一行 */
        if (app.osd_hint && app.osd_hint_w != W) {
            SDL_DestroyTexture(app.osd_hint);
            app.osd_hint = NULL;
        }
        if (!app.osd_hint) {
            app.osd_hint = SDL_CreateTexture(app.ren, SDL_PIXELFORMAT_ARGB8888,
                                             SDL_TEXTUREACCESS_TARGET, W, 48);
            if (app.osd_hint) {
                SDL_SetTextureBlendMode(app.osd_hint, SDL_BLENDMODE_BLEND);
                SDL_SetRenderTarget(app.ren, app.osd_hint);
                SDL_SetRenderDrawColor(app.ren, 0, 0, 0, 0);
                SDL_RenderClear(app.ren);
                    ui_text_centered(W / 2, 2, UI_SIZE_SMALL, C_DIM,
                                     "GUIDE=PS   SELECT=SHARE   START=触摸板   SELECT+START=OPTIONS");
                    ui_text_centered(W / 2, 22, UI_SIZE_SMALL, C_DIM,
                                     "L1+R1+L3+R3=菜单   L3+R3 按住 8 秒=OSD 开关");
                SDL_SetRenderTarget(app.ren, NULL);
                app.osd_hint_w = W;
            }
        }
        if (app.osd_hint) {
            SDL_Rect r = { 0, bot_y, W, 48 };
            SDL_RenderCopy(app.ren, app.osd_hint, NULL, &r);
        }
    }
}

static void draw_menu(void)
{
    int W = app.width, H = app.height;
    SDL_Rect panel = { W / 2 - 220, H / 2 - 160, 440, 320 };
    ui_rect(0, 0, W, H, (SDL_Color){ 0, 0, 0, 150 });
    ui_rect(panel.x, panel.y, panel.w, panel.h, C_PANEL);
    ui_frame(panel.x, panel.y, panel.w, panel.h, C_SEL, 2);

    ui_text_centered(W / 2, panel.y + 22, UI_SIZE_NORMAL, C_TEXT, "串流菜单");

    const char *items[4] = { "继续串流", "返回主机列表", "断开连接", "震动测试" };
    for (int i = 0; i < 4; i++) {
        SDL_Rect r = { panel.x + 24, panel.y + 70 + i * 54, panel.w - 48, 44 };
        bool sel = (i == app.settings_sel);
        if (sel) ui_rect(r.x, r.y, r.w, r.h, C_SEL);
        ui_text(r.x + 16, r.y + 10, UI_SIZE_NORMAL, C_TEXT, "%s", items[i]);
    }
    ui_text_centered(W / 2, panel.y + panel.h - 30, UI_SIZE_SMALL, C_DIM, "B(×) 选择    A(○) 返回");
}

static void draw_settings(void)
{
    int W = app.width, H = app.height;
    SDL_SetRenderDrawColor(app.ren, C_BG.r, C_BG.g, C_BG.b, 255);
    SDL_RenderClear(app.ren);

    ui_text(40, 26, UI_SIZE_LARGE, C_TEXT, "设置");

    char b1[32], b2[32], b3[32], b4[32], b5[32], b6[32], b7[32], b8[32];
    snprintf(b1, sizeof(b1), "%s", res_name(app.cfg.resolution));
    snprintf(b2, sizeof(b2), "%d", app.cfg.fps);
    snprintf(b3, sizeof(b3), "%d kbps", app.cfg.bitrate);
    snprintf(b4, sizeof(b4), "%s", app.cfg.codec ? "H.265" : "H.264");
    if (app.cfg.rumble <= 0)
        snprintf(b5, sizeof(b5), "关闭");
    else
        snprintf(b5, sizeof(b5), "%d%%", app.cfg.rumble);
    snprintf(b6, sizeof(b6), "%s", app.cfg.autoconnect ? "开" : "关");
    snprintf(b7, sizeof(b7), "%s", app.cfg.hwdec ? "硬解" : "软解");
    snprintf(b8, sizeof(b8), "%s", app.cfg.osd ? "显示" : "隐藏");

    const char *labels[8] = { "分辨率", "帧率", "码率", "编码", "震动", "开机直连", "硬解", "状态OSD" };
    const char *values[8] = { b1, b2, b3, b4, b5, b6, b7, b8 };

    int y = 100;
    for (int i = 0; i < 8; i++) {
        SDL_Rect r = { 32, y, W - 64, 64 };
        bool sel = (i == app.settings_sel);
        ui_rect(r.x, r.y, r.w, r.h, sel ? C_SEL : C_PANEL);
        ui_text(r.x + 20, r.y + 17, UI_SIZE_NORMAL, C_TEXT, "%s", labels[i]);
        ui_text_right(r.x + r.w - 20, r.y + 17, UI_SIZE_NORMAL, C_ACCENT, "%s", values[i]);
        y += 72;
    }

    ui_text(40, H - 70, UI_SIZE_SMALL, C_DIM,
            "左右键改数值    B(×) 下一项    A(○) 返回    串流中 L3+R3 按 8 秒切 OSD");
    ui_text(40, H - 42, UI_SIZE_SMALL, C_DIM,
            "主机的名称和地址在主界面按 L2 修改");
}

static void apply_settings_delta(int dir)
{
    switch (app.settings_sel) {
    case 0:
        app.cfg.resolution += dir;
        if (app.cfg.resolution < 1) app.cfg.resolution = 4;
        if (app.cfg.resolution > 4) app.cfg.resolution = 1;
        if (app.cfg.resolution == 4 && app.cfg.fps > 30) app.cfg.fps = 30;
        break;
    case 1:
        app.cfg.fps = (app.cfg.fps == 60) ? 30 : 60;
        break;
    case 2: {
        int step = (app.cfg.bitrate >= 20000) ? 5000 : 2000;
        app.cfg.bitrate += dir * step;
        if (app.cfg.bitrate < 2000) app.cfg.bitrate = 50000;
        if (app.cfg.bitrate > 50000) app.cfg.bitrate = 2000;
        break;
    }
    case 3:
        app.cfg.codec = !app.cfg.codec;
        break;
    case 4: {
        static const int steps[5] = { 0, 25, 50, 75, 100 };
        int cur = 0;
        for (int i = 0; i < 5; i++)
            if (steps[i] == app.cfg.rumble) cur = i;
        cur = (cur + dir + 5) % 5;
        app.cfg.rumble = steps[cur];
        break;
    }
    case 5:
        app.cfg.autoconnect = !app.cfg.autoconnect;
        break;
    case 6:
        app.cfg.hwdec = !app.cfg.hwdec;
        break;
    case 7:
        app.cfg.osd = !app.cfg.osd;
        break;
    default:
        return;
    }
    config_save();
}

static void draw_keymap(void)
{
    int W = app.width, H = app.height;
    SDL_SetRenderDrawColor(app.ren, C_BG.r, C_BG.g, C_BG.b, 255);
    SDL_RenderClear(app.ren);

    ui_text(40, 26, UI_SIZE_LARGE, C_TEXT, "按键映射");
    ui_text(40, 72, UI_SIZE_SMALL, C_DIM,
            app.keymap_wait >= 0 ? "请按下要绑定的键：" : "B(×) 重新绑定    A(○) 返回");
    if (app.keymap_wait >= 0)
        ui_text(40, 96, UI_SIZE_NORMAL, C_ACCENT, "%s", act_names[app.keymap_wait]);

    const int visible = 11;
    int y = 132;
    int first = app.keymap_top;
    if (first > ACT_COUNT - visible) first = ACT_COUNT - visible;
    if (first < 0) first = 0;
    for (int i = first; i < ACT_COUNT && i < first + visible; i++) {
        SDL_Rect r = { 32, y, W - 64, 44 };
        bool sel = (i == app.keymap_sel);
        if (sel) ui_rect(r.x, r.y, r.w, r.h, C_SEL);
        ui_text(r.x + 16, r.y + 8, UI_SIZE_SMALL, C_TEXT, "%s", act_names[i]);
        int code = app.cfg.key[i];
        if (code <= -(CTRL_CODE_OFFSET + 1)) {
            ui_text_right(r.x + r.w - 16, r.y + 8, UI_SIZE_SMALL, C_ACCENT,
                          "PAD B%d", -code - CTRL_CODE_OFFSET - 1);
        } else if (code < 0) {
            ui_text_right(r.x + r.w - 16, r.y + 8, UI_SIZE_SMALL, C_ACCENT,
                          "JOY B%d", -code - 1);
        } else {
            const char *nm = SDL_GetScancodeName((SDL_Scancode)code);
            ui_text_right(r.x + r.w - 16, r.y + 8, UI_SIZE_SMALL, C_ACCENT, "%s",
                          (nm && nm[0]) ? nm : "?");
        }
        y += 48;
    }
    if (ACT_COUNT > visible) {
        ui_text_right(W - 32, 104, UI_SIZE_SMALL, C_DIM, "%d/%d", app.keymap_sel + 1, ACT_COUNT);
    }
    ui_text(40, H - 40, UI_SIZE_SMALL, C_DIM, "上下选择   B(×) 重新绑定   A(○) 返回   ·   SELECT 键=SHARE   GUIDE/Home 键=PS");
}

static void draw_keytest(void)
{
    int W = app.width, H = app.height;
    SDL_SetRenderDrawColor(app.ren, C_BG.r, C_BG.g, C_BG.b, 255);
    SDL_RenderClear(app.ren);

    ui_text(40, 26, UI_SIZE_LARGE, C_TEXT, "按键测试");
    ui_text(40, 80, UI_SIZE_NORMAL, C_TEXT, "最近按键码：%d", (int)app.last_scancode);
    ui_text(40, 116, UI_SIZE_NORMAL, C_ACCENT, "%s",
            SDL_GetScancodeName(app.last_scancode));
    ui_text(40, 170, UI_SIZE_SMALL, C_DIM, "Cross=%d Circle=%d Options=%d Share=%d",
            in_now.confirm, in_now.back, in_now.start, in_now.select);
    ui_text(40, 200, UI_SIZE_SMALL, C_DIM, "L1=%d R1=%d L2=%d R2=%d Sq=%d Tri=%d",
            in_now.l1, in_now.r1, in_now.l2, in_now.r2, in_now.square, in_now.triangle);
    ui_text(40, 230, UI_SIZE_SMALL, C_DIM, "Sticks L(%d,%d) R(%d,%d)",
            in_now.lx, in_now.ly, in_now.rx, in_now.ry);
    if (app.joy)
        ui_text(40, 262, UI_SIZE_SMALL, C_DIM, "Joystick: %s  last button: %d",
                SDL_JoystickName(app.joy), app.last_joy_button);
    ui_text(40, H - 40, UI_SIZE_SMALL, C_DIM, "A(○) 返回");
}

static void draw_message(void)
{
    int W = app.width, H = app.height;
    SDL_SetRenderDrawColor(app.ren, C_BG.r, C_BG.g, C_BG.b, 255);
    SDL_RenderClear(app.ren);

    ui_text_centered(W / 2, H / 2 - 80, UI_SIZE_LARGE, C_WARN, "%s", app.msg_title);

    /* 以前每帧 strdup + strtok 只为按 \n 分行，改成栈上切片，零堆分配 */
    const char *p = app.msg_body;
    int y = H / 2 - 20;
    while (p && *p && y < H - 80) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char line[512];
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = 0;
        ui_text_centered(W / 2, y, UI_SIZE_SMALL, C_TEXT, "%s", line);
        y += 32;
        p = nl ? nl + 1 : NULL;
    }
    ui_text_centered(W / 2, H - 60, UI_SIZE_SMALL, C_DIM, "B / A 继续");
}

/* ------------------------------------------------------------------ */
/* per-screen update                                                  */
/* ------------------------------------------------------------------ */

static void add_begin(int idx);   /* 打开添加/修改界面：idx >= 0 表示修改已有主机 */

static void host_delete(int idx)
{
    if (app.host_lock) SDL_LockMutex(app.host_lock);
    if (idx >= 0 && idx < app.host_count) {
        for (int i = idx; i < app.host_count - 1; i++)
            app.hosts[i] = app.hosts[i + 1];
        app.host_count--;
        if (app.host_sel >= app.host_count)
            app.host_sel = app.host_count > 0 ? app.host_count - 1 : 0;
        hosts_save();
    }
    if (app.host_lock) SDL_UnlockMutex(app.host_lock);
}

static void host_wake(int idx)
{
    if (idx < 0 || idx >= app.host_count) return;
    HostEntry *h = &app.hosts[idx];
    uint8_t key[16];
    int klen = hex2bin(h->regkey, key, 16);
    uint64_t cred = (klen == 16) ? wakeup_credential(key, klen) : 0;
    if (app.discovery_on)
        chiaki_discovery_wakeup(&g_log, &app.discovery.discovery, h->addr, cred,
                                h->ps5 ? true : false);
    message("已发送唤醒", "已向主机发送唤醒包，若它在待机会自动开机（约 10 秒）。");
}

static void update_hosts(void)
{
    if (pressed(in_now.up, in_prev.up) && app.host_count > 0) {
        app.host_sel = (app.host_sel + app.host_count - 1) % app.host_count;
        app.del_pending = 0;
    }
    if (pressed(in_now.down, in_prev.down) && app.host_count > 0) {
        app.host_sel = (app.host_sel + 1) % app.host_count;
        app.del_pending = 0;
    }

    if (pressed(in_now.back, in_prev.back)) {
        if (app.del_pending) { app.del_pending = 0; return; }
        app.running = false;
        return;
    }
    if (pressed(in_now.confirm, in_prev.confirm)) {
        if (app.del_pending) {
            host_delete(app.host_sel);
            app.del_pending = 0;
        } else {
            start_session();
        }
        return;
    }
    if (pressed(in_now.square, in_prev.square)) {          /* 物理 Y：添加主机 */
        add_begin(-1);
        return;
    }
    if (pressed(in_now.triangle, in_prev.triangle)) {      /* 物理 X：删除 */
        if (app.host_count > 0) app.del_pending = 1;
    }
    if (pressed(in_now.l2, in_prev.l2)) {                  /* L2：修改当前主机 */
        if (app.host_count > 0) {
            add_begin(app.host_sel);
        } else {
            add_begin(-1);
        }
        return;
    }
    if (pressed(in_now.l1, in_prev.l1))                    /* 唤醒 */
        host_wake(app.host_sel);
    if (pressed(in_now.r2, in_prev.r2)) {                  /* R2：PIN 配对（走 regist） */
        if (app.host_count > 0) {
            app.regist_host = app.host_sel;
            app.pin_mode = 1;
            app.pin_len = 0;
            app.pin[0] = 0;
            app.pin_incorrect = false;
            app.pin_cursor = 0;
            app.screen = SCR_PIN;
        }
        return;
    }
    if (pressed(in_now.r1, in_prev.r1)) {                  /* 设置 */
        app.settings_sel = 0;
        app.screen = SCR_SETTINGS;
    }
    if (pressed(in_now.select, in_prev.select)) {          /* 按键映射 */
        app.keymap_sel = 0;
        app.keymap_top = 0;
        app.keymap_wait = -1;
        app.screen = SCR_KEYMAP;
    }
}

/* ---------------- 添加主机（软键盘） ---------------- */

static const char *kb_rows[4] = {
    "1234567890",
    "qwertyuiop",
    "asdfghjkl.",
    "zxcvbnm-/:"
};

/* 底部功能键：把「上一项 / 下一项」放进键盘里，用户不用去够 L1/R1 */
#define KB_ACT_SHIFT  0
#define KB_ACT_PREV   1
#define KB_ACT_NEXT   2
#define KB_ACT_TYPE   3
#define KB_ACT_PLUS   4
#define KB_ACT_EQ     5
#define KB_ACT_DEL    6
#define KB_ACT_OK     7
#define KB_ACT_COUNT  8

static const char *kb_actions[KB_ACT_COUNT] = {
    "SHIFT", "上一项", "下一项", "PS5/PS4", "+", "=", "删除", "确定"
};

#define ADD_F_NAME 0
#define ADD_F_ADDR 1
#define ADD_F_PSN  2
#define ADD_F_COUNT 3

static const char *add_field_label[ADD_F_COUNT] = {
    "名称 (可留空)", "地址 (IP / 域名)", "PSN ID (base64)"
};

static char *add_field_buf(void)
{
    switch (app.add_field) {
    case ADD_F_NAME: return app.add_name;
    case ADD_F_PSN:  return app.add_psn;
    default:         return app.add_addr;
    }
}

static size_t add_field_cap(void)
{
    switch (app.add_field) {
    case ADD_F_NAME: return sizeof(app.add_name);
    case ADD_F_PSN:  return sizeof(app.add_psn);
    default:         return sizeof(app.add_addr);
    }
}

/* 键盘固定在屏幕底部：底边距留一行提示文字 */
static void kb_layout(int *y0, int *cw, int *ch, int *x0, int *by)
{
    int W = app.width, H = app.height;
    const int gap = 8, fh = 58, fgap = 10;
    int key_h = 70;

    /* 4 行字符 + 1 行功能键，整体贴住底边 */
    int total = 4 * (key_h + fgap) + gap + fh;
    int bot = H - 56;
    *y0 = bot - total;
    if (*y0 < 260) {              /* 屏幕太矮时退化为均匀排布 */
        *y0 = 260;
        key_h = (H - 56 - 260 - 4 * fgap - gap - fh) / 4;
        if (key_h < 44) key_h = 44;
    }

    *cw = 90;
    *ch = key_h;
    *x0 = (W - (10 * *cw + 9 * gap)) / 2;
    *by = *y0 + 4 * (key_h + fgap) + gap;
}

/* 输入太长时从左边截断：光标在尾部，末尾那段才是用户正在看的内容。
 * 先按平均字宽粗估要丢掉几个字，再逐字修正——直接在循环里反复测量整串是 O(n²)，
 * 七十几字符的 IPv6 地址每帧都量一遍会明显掉帧。 */
static const char *next_utf8(const char *p)
{
    const unsigned char *u = (const unsigned char *)p;
    if (!*u) return p;
    if (*u < 0x80) return p + 1;
    if ((*u & 0xE0) == 0xC0 && u[1]) return p + 2;
    if ((*u & 0xF0) == 0xE0 && u[1] && u[2]) return p + 3;
    if ((*u & 0xF8) == 0xF0 && u[1] && u[2] && u[3]) return p + 4;
    return p + 1;
}

static const char *tail_fit(int size, int max_w, const char *s)
{
    if (!s || max_w <= 0) return s;
    int total = ui_text_width(size, s);
    if (total <= max_w) return s;

    int avg = ui_text_width(size, "0123456789") / 10;
    if (avg < 1) avg = 1;
    int drop = (total - max_w) / avg;
    if (drop < 0) drop = 0;

    const char *p = s;
    for (int i = 0; i < drop && *p; i++)
        p = next_utf8(p);

    while (*p && ui_text_width(size, p) > max_w) {
        const char *n = next_utf8(p);
        if (!*n) break;
        p = n;
    }
    return p;
}

static void draw_add(void)
{
    int W = app.width, H = app.height;
    SDL_SetRenderDrawColor(app.ren, C_BG.r, C_BG.g, C_BG.b, 255);
    SDL_RenderClear(app.ren);

    ui_text(40, 14, UI_SIZE_LARGE, C_TEXT, "%s",
            app.edit_index >= 0 ? "修改主机" : "添加主机");

    /* 三个输入项：名称 / 地址 / PSN ID */
    int fy = 70, fh = 54, fstep = 60;
    for (int f = 0; f < ADD_F_COUNT; f++) {
        SDL_Rect b = { 40, fy + f * fstep, W - 80, fh };
        bool active = (app.add_field == f);
        ui_rect(b.x, b.y, b.w, b.h, active ? C_SEL : C_PANEL);
        if (active) ui_frame(b.x, b.y, b.w, b.h, C_ACCENT, 2);
        ui_text(b.x + 14, b.y + 14, UI_SIZE_SMALL, active ? C_TEXT : C_DIM,
                "%s", add_field_label[f]);

        const char *text = (f == ADD_F_NAME) ? app.add_name
                         : (f == ADD_F_ADDR) ? app.add_addr : app.add_psn;
        const char *shown = text[0] ? text : (f == ADD_F_ADDR ? "必填" : "可留空");
        shown = tail_fit(UI_SIZE_NORMAL, b.w - 280, shown);
        ui_text(b.x + 260, b.y + 10, UI_SIZE_NORMAL, C_TEXT, "%s%s",
                shown, active ? "_" : "");
    }

    ui_text(40, fy + ADD_F_COUNT * fstep + 4, UI_SIZE_SMALL, C_DIM,
            "L1 / R1 或键盘「上一项 / 下一项」切换    ·    类型：%s",
            app.add_ps5 ? "PS5" : "PS4");

    /* 字符键 */
    int y0, cw, ch, x0, by;
    kb_layout(&y0, &cw, &ch, &x0, &by);
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 10; c++) {
            SDL_Rect k = { x0 + c * (cw + 8), y0 + r * (ch + 10), cw, ch };
            bool sel = (app.add_cy == r && app.add_cx == c);
            ui_rect(k.x, k.y, k.w, k.h, sel ? C_SEL : C_PANEL);
            char chs[2] = { kb_rows[r][c], 0 };
            if (app.add_shift && chs[0] >= 'a' && chs[0] <= 'z')
                chs[0] = (char)(chs[0] - 'a' + 'A');
            ui_text_centered(k.x + cw / 2, k.y + (ch - 30) / 2, UI_SIZE_NORMAL, C_TEXT, "%s", chs);
        }
    }

    /* 功能键 */
    int bw = 116, bg = 6;
    int bx0 = (W - (KB_ACT_COUNT * bw + (KB_ACT_COUNT - 1) * bg)) / 2;
    for (int c = 0; c < KB_ACT_COUNT; c++) {
        SDL_Rect k = { bx0 + c * (bw + bg), by, bw, 58 };
        bool sel = (app.add_cy == 4 && app.add_cx == c);
        ui_rect(k.x, k.y, k.w, k.h, sel ? C_SEL : C_PANEL);
        ui_text_centered(k.x + bw / 2, k.y + 14, UI_SIZE_SMALL, C_TEXT, "%s", kb_actions[c]);
    }

    ui_text(40, H - 40, UI_SIZE_SMALL, C_DIM,
            "B(×) 输入/确定   Y(□) 退格   A(○) 返回   START 返回   （PSN ID 多数情况留空）");
}

static void add_begin(int idx)
{
    if (idx >= 0 && idx < app.host_count) {
        HostEntry *h = &app.hosts[idx];
        snprintf(app.add_name, sizeof(app.add_name), "%s", h->name);
        snprintf(app.add_addr, sizeof(app.add_addr), "%s", h->addr);
        snprintf(app.add_psn, sizeof(app.add_psn), "%s", h->psn);
        app.add_ps5 = h->ps5 ? 1 : 0;
        app.edit_index = idx;
    } else {
        app.add_name[0] = 0;
        app.add_addr[0] = 0;
        app.add_psn[0] = 0;
        app.add_ps5 = 1;
        app.edit_index = -1;
    }
    app.add_field = ADD_F_ADDR;   /* 名称可留空，直接停在地址上省一步 */
    app.add_shift = 0;
    app.add_cx = 0;
    app.add_cy = 0;
    app.add_wrap_l = app.add_wrap_r = false;
    app.del_pending = 0;
    app.screen = SCR_ADD;
}

static void update_add(void)
{
    bool mu = rep_fire(in_now.up, in_prev.up, REP_UP, REP_DELAY_MS, REP_RATE_MS);
    bool md = rep_fire(in_now.down, in_prev.down, REP_DOWN, REP_DELAY_MS, REP_RATE_MS);
    bool ml = rep_fire(in_now.left, in_prev.left, REP_LEFT, REP_DELAY_MS, REP_RATE_MS);
    bool mr = rep_fire(in_now.right, in_prev.right, REP_RIGHT, REP_DELAY_MS, REP_RATE_MS);

    int maxc = (app.add_cy == 4) ? KB_ACT_COUNT - 1 : 9;
    if (mu && app.add_cy > 0) app.add_cy--;
    if (md && app.add_cy < 4) app.add_cy++;
    maxc = (app.add_cy == 4) ? KB_ACT_COUNT - 1 : 9;
    /* 左右到边后绕回。长按连发时每跨一次边界只绕一次，松手才复位，
     * 否则按住不放会在行首行尾之间来回跳，根本停不下来。 */
    if (ml) {
        if (app.add_cx > 0) { app.add_cx--; app.add_wrap_l = false; }
        else if (!app.add_wrap_l) { app.add_cx = maxc; app.add_wrap_l = true; }
    } else {
        app.add_wrap_l = false;
    }
    if (mr) {
        if (app.add_cx < maxc) { app.add_cx++; app.add_wrap_r = false; }
        else if (!app.add_wrap_r) { app.add_cx = 0; app.add_wrap_r = true; }
    } else {
        app.add_wrap_r = false;
    }
    if (app.add_cx > maxc) app.add_cx = maxc;

    /* 切换输入项：L1 上一项 / R1 下一项，键盘里也有对应按钮 */
    if (pressed(in_now.l1, in_prev.l1))
        app.add_field = (app.add_field + ADD_F_COUNT - 1) % ADD_F_COUNT;
    if (pressed(in_now.r1, in_prev.r1))
        app.add_field = (app.add_field + 1) % ADD_F_COUNT;

    char *buf = add_field_buf();
    size_t cap = add_field_cap();
    size_t len = strlen(buf);

    if (pressed(in_now.start, in_prev.start)) { app.screen = SCR_HOSTS; return; }
    /* 掌机 Y（= PS5 方块 □）：唯一退格键，支持长按连发快速删 */
    bool ydel = rep_fire(in_now.square, in_prev.square, REP_Y, REP_DELAY_MS, REP_RATE_MS);
    if (ydel) {
        if (len > 0) buf[len - 1] = 0;
        return;
    }
    /* A 只负责回列表，不再删字符 */
    if (pressed(in_now.back, in_prev.back)) {
        app.screen = SCR_HOSTS;
        return;
    }

    if (!pressed(in_now.confirm, in_prev.confirm)) return;

    if (app.add_cy < 4) {
        char ch = kb_rows[app.add_cy][app.add_cx];
        if (app.add_shift && ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
        if (len + 1 < cap) { buf[len] = ch; buf[len + 1] = 0; }
        return;
    }

    switch (app.add_cx) {
    case KB_ACT_SHIFT:
        app.add_shift = !app.add_shift;
        break;
    case KB_ACT_PREV:
        app.add_field = (app.add_field + ADD_F_COUNT - 1) % ADD_F_COUNT;
        break;
    case KB_ACT_NEXT:
        app.add_field = (app.add_field + 1) % ADD_F_COUNT;
        break;
    case KB_ACT_TYPE:
        app.add_ps5 = !app.add_ps5;
        break;
    case KB_ACT_PLUS:
    case KB_ACT_EQ: {
        char ch = (app.add_cx == KB_ACT_PLUS) ? '+' : '=';
        if (len + 1 < cap) { buf[len] = ch; buf[len + 1] = 0; }
        break;
    }
    case KB_ACT_DEL:
        if (len > 0) buf[len - 1] = 0;
        break;
    case KB_ACT_OK: {
        if (!app.add_addr[0]) {
            message("地址为空", "请先在「地址」里输入 PS5 的 IP 或域名。");
            return;
        }
        char auto_name[96];
        snprintf(auto_name, sizeof(auto_name), "%s (%s)",
                 app.add_ps5 ? "PS5" : "PS4", app.add_addr);
        const char *nm = app.add_name[0] ? app.add_name : auto_name;

        /* 修改已有主机：保留 regkey / morning，换了 IP 也不用重新配对 */
        if (app.edit_index >= 0 && app.edit_index < app.host_count) {
            HostEntry *h = &app.hosts[app.edit_index];
            snprintf(h->name, sizeof(h->name), "%s", nm);
            snprintf(h->addr, sizeof(h->addr), "%s", app.add_addr);
            snprintf(h->psn, sizeof(h->psn), "%s", app.add_psn);
            h->ps5 = app.add_ps5;
            h->saved = 1;
            hosts_save();
            app.host_sel = app.edit_index;
            app.edit_index = -1;
            app.screen = SCR_HOSTS;
            return;
        }

        int idx = host_find(app.add_addr);
        if (idx >= 0) {
            /* 这个地址已经在列表里（自动发现的常见就在），更新它而不是重复添加 */
            HostEntry *h = &app.hosts[idx];
            snprintf(h->name, sizeof(h->name), "%s", nm);
            snprintf(h->psn, sizeof(h->psn), "%s", app.add_psn);
            h->ps5 = app.add_ps5;
            h->saved = 1;
            hosts_save();
            app.host_sel = idx;
            start_session();
            return;
        }

        int nidx = host_add(nm, app.add_addr, app.add_ps5);
        if (nidx < 0) {
            message("无法添加", "主机数量已达上限。");
            return;
        }
        snprintf(app.hosts[nidx].psn, sizeof(app.hosts[nidx].psn), "%s", app.add_psn);
        hosts_save();
        app.host_sel = nidx;
        start_session();
        break;
    }
    }
}

static void pin_press(int key)
{
    /* 键盘布局 keys[12] = { "1".."9", "DEL", "0", "OK" }
     *   索引 0..8 -> '1'..'9'，9 -> DEL，10 -> '0'，11 -> OK
     * 之前直接 '0' + key，导致屏幕上的"1"输入成 '0'，"0"(索引10) 还会落进
     * OK 分支直接提交。这里按索引语义严格分开处理。 */
    char ch = 0;
    if (key >= 0 && key <= 8)
        ch = (char)('1' + key);
    else if (key == 10)
        ch = '0';

    if (ch) {
        if (app.pin_len < 8) {
            app.pin[app.pin_len++] = ch;
            app.pin[app.pin_len] = 0;
        }
        return;
    }

    if (key == 9) {              /* DEL */
        if (app.pin_len > 0)
            app.pin[--app.pin_len] = 0;
        return;
    }

    if (key != 11)               /* 只认 OK，其余忽略 */
        return;

    if (app.pin_len != 8)
        return;

    if (app.pin_mode == 1) {
        /* 配对：本机生成 regkey，成功后写回 hosts.txt */
        if (app.host_sel >= 0 && app.host_sel < app.host_count) {
            HostEntry *h = &app.hosts[app.host_sel];
            unsigned int pin = (unsigned int)strtoul(app.pin, NULL, 10);
            if (stream_register(h->addr, h->ps5, pin, h->psn, &g_log) != 0)
                message("配对失败", "无法启动配对，请检查地址。");
        }
    } else {
        stream_submit_pin(app.pin);
    }
    app.pin_len = 0;
    app.pin[0] = 0;
}

/* 配对是异步的（regist 内部有线程），主循环轮询结果 */
static void poll_register(void)
{
    uint8_t key[16], morn[16];
    int r = stream_register_poll(key, morn);
    if (r == 0)
        return;

    /* 用户已离开配对界面（按 START 取消 / 返回列表）：成功和失败都丢掉。
     * 失败结果同样要判——否则用户取消后regist超时迟到的 FAILED
     * 会弹「配对失败」并把界面强行拉回 PIN。 */
    if (app.screen != SCR_PIN || app.pin_mode != 1)
        return;

    int idx = (app.regist_host >= 0 && app.regist_host < app.host_count)
              ? app.regist_host : app.host_sel;
    if (r == 1 && idx >= 0 && idx < app.host_count) {
        HostEntry *h = &app.hosts[idx];
        key_to_hex(key, 16, h->regkey, sizeof(h->regkey));
        key_to_hex(morn, 16, h->morning, sizeof(h->morning));
        SDL_Log("main: pairing idx=%d addr=%s regkey=[%s] morning=[%s]",
                idx, h->addr, h->regkey, h->morning);
        hosts_save();
        SDL_Log("main: pairing done, auto-connecting to %s", h->addr);

        /* 配对成功后直接开始串流，不用再回列表按一次 B */
        app.host_sel = idx;
        app.pin_mode = 0;
        app.regist_host = -1;
        app.screen = SCR_HOSTS;
        start_session();
    } else {
        message("配对失败", "请确认：\n· PS5 上「显示 PIN」还开着（5 分钟内有效）\n· PIN 输入正确");
        app.regist_host = -1;
        app.screen = SCR_PIN;
    }
}

static void update_pin(void)
{
    bool mu = rep_fire(in_now.up, in_prev.up, REP_UP, REP_DELAY_MS, REP_RATE_MS);
    bool md = rep_fire(in_now.down, in_prev.down, REP_DOWN, REP_DELAY_MS, REP_RATE_MS);
    bool ml = rep_fire(in_now.left, in_prev.left, REP_LEFT, REP_DELAY_MS, REP_RATE_MS);
    bool mr = rep_fire(in_now.right, in_prev.right, REP_RIGHT, REP_DELAY_MS, REP_RATE_MS);

    int col = app.pin_cursor % 3, row = app.pin_cursor / 3;
    if (ml && col > 0) col--;
    if (mr && col < 2) col++;
    if (mu && row > 0) row--;
    if (md && row < 3) row++;
    app.pin_cursor = row * 3 + col;

    if (pressed(in_now.confirm, in_prev.confirm))
        pin_press(app.pin_cursor);

    /* A 不再当退格，退格统一走 Y（键盘上也有 DEL 键），支持长按连发 */
    if (rep_fire(in_now.square, in_prev.square, REP_Y, REP_DELAY_MS, REP_RATE_MS))
        pin_press(9);

    if (pressed(in_now.start, in_prev.start)) {
        stop_session();
    }
}

static void update_stream(void)
{
    Uint32 now = SDL_GetTicks();

    /* 会话还没建立时（黑屏「正在连接…」）长按 A(○) 0.7 秒取消，回主机列表。
     * 之前只有 wait_tick（端口探测阶段）有取消，stream_start 成功后等 ctrl
     * 握手这段没有任何出口——TCP 通但会话起不来（PS5 被别的设备占着、
     * 跨网段打洞半通）就永远卡在这，按什么键都只会发给 PS5。
     * 已连上之后长按 A 仍然是正常的 ○ 键，不能拦。 */
    if (!stream_connected() && !app.stream_quit_pending) {
        if (!in_now.back) {
            app.cancel_hold_t0 = 0;
        } else {
            if (!app.cancel_hold_t0) {
                app.cancel_hold_t0 = now;
            } else if (now - app.cancel_hold_t0 > 700) {
                app.cancel_hold_t0 = 0;
                SDL_Log("stream: user cancelled connect (long-press A)");
                stream_stop_rumble();
                stream_stop();
                discovery_start();
                osd_reset();
                message("已取消", "连接超时或被取消。可尝试唤醒主机（L1）后重连，\n"
                        "或确认 PS5 没有其他设备正在远程游玩。");
                return;
            }
        }
    } else {
        app.cancel_hold_t0 = 0;
    }

    /* START = 真实按住触摸板（短按=点击，长按=长按，游戏里长按触摸板
     * 的需求从此满足——之前 START 是 120ms 模拟点击，永远按不住）。
     * OPTIONS = SELECT+START 同按 0.4 秒。组合期间抑制触摸板按下，
     * 免得呼出系统菜单前先给游戏糊了 0.4 秒触摸板。
     * SELECT 单独按下延迟 250ms 才发 SHARE：给组合键留出识别窗口，
     * 期间等来 START 就整回合不发 SHARE/触摸板。 */
    if (in_now.select) {
        if (!app.sel_press_ms) {
            app.sel_press_ms = now;
            app.sel_suppress = true;
        } else if (in_now.start) {
            app.sel_suppress = true;   /* 组合键路线：SHARE 一直压着 */
        } else if (app.sel_suppress && now - app.sel_press_ms >= 250) {
            app.sel_suppress = false;  /* 没等到 START，正常发 SHARE */
        }
    } else {
        if (app.sel_press_ms && app.sel_suppress && !app.opt_press_ms) {
            /* SELECT 在 250ms 内快速点按、且没碰 START：
             * 补发一次 120ms 的 SHARE 点击，不然这次按键就丢了 */
            app.tap_bits = CHIAKI_CONTROLLER_BUTTON_SHARE;
            app.tap_until = now + 120;
        }
        app.sel_press_ms = 0;
        app.sel_suppress = false;
    }
    if (in_now.select && in_now.start) {
        if (!app.opt_press_ms) {
            app.opt_press_ms = now;
        } else if (!app.opt_fired && (now - app.opt_press_ms) >= 400) {
            app.opt_fired = true;
            app.tap_bits = CHIAKI_CONTROLLER_BUTTON_OPTIONS;
            app.tap_until = now + 120;
        }
    } else {
        app.opt_press_ms = 0;
        app.opt_fired = false;
    }

    /* feed controller state */
    ChiakiControllerState st;
    memset(&st, 0, sizeof(st));
    st.buttons = 0;
    if (in_now.up) st.buttons |= CHIAKI_CONTROLLER_BUTTON_DPAD_UP;
    if (in_now.down) st.buttons |= CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN;
    if (in_now.left) st.buttons |= CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT;
    if (in_now.right) st.buttons |= CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT;
    if (in_now.confirm) st.buttons |= CHIAKI_CONTROLLER_BUTTON_CROSS;
    if (in_now.back) st.buttons |= CHIAKI_CONTROLLER_BUTTON_MOON;
    if (in_now.square) st.buttons |= CHIAKI_CONTROLLER_BUTTON_BOX;
    if (in_now.triangle) st.buttons |= CHIAKI_CONTROLLER_BUTTON_PYRAMID;
    if (in_now.l1) st.buttons |= CHIAKI_CONTROLLER_BUTTON_L1;
    if (in_now.r1) st.buttons |= CHIAKI_CONTROLLER_BUTTON_R1;
    if (in_now.l2) st.l2_state = 255;
    if (in_now.r2) st.r2_state = 255;
    if (in_now.l3) st.buttons |= CHIAKI_CONTROLLER_BUTTON_L3;
    if (in_now.r3) st.buttons |= CHIAKI_CONTROLLER_BUTTON_R3;
    if (in_now.touchpad) st.buttons |= CHIAKI_CONTROLLER_BUTTON_TOUCHPAD;
    /* START 按住期间触摸板保持按下（松手即抬起，短按就是普通点击）；
     * 与 SELECT 同按视为 OPTIONS 组合键，此时不发触摸板 */
    if (in_now.start && !in_now.select) st.buttons |= CHIAKI_CONTROLLER_BUTTON_TOUCHPAD;
    if (app.tap_until > now) st.buttons |= app.tap_bits;
    if (in_now.select && !app.sel_suppress) st.buttons |= CHIAKI_CONTROLLER_BUTTON_SHARE;
    if (in_now.ps) st.buttons |= CHIAKI_CONTROLLER_BUTTON_PS;
    st.left_x = (int16_t)in_now.lx;
    st.left_y = (int16_t)in_now.ly;
    st.right_x = (int16_t)in_now.rx;
    st.right_y = (int16_t)in_now.ry;

    if (g_autotest) {
        /* 两个摇杆各转一圈约 2.4 秒，幅度给满，让游戏里的镜头持续扫动 */
        double t = (double)now / 1000.0 * 2.6;
        int16_t sx = (int16_t)(28000.0 * sin(t));
        int16_t sy = (int16_t)(28000.0 * cos(t));
        st.right_x = sx; st.right_y = sy;
        st.left_x = (int16_t)(-sx / 2); st.left_y = (int16_t)(-sy / 2);
    }
    st.touch_id_next = 0;
    for (int i = 0; i < CHIAKI_CONTROLLER_TOUCHES_MAX; i++) {
        st.touches[i].id = -1;
        st.touches[i].x = 0;
        st.touches[i].y = 0;
    }
    stream_send_controller(&st);

    /* 震动：session 线程只记录强度，这里每帧负责真正驱动掌机马达（sysfs） */
    stream_apply_rumble(app.cfg.rumble);

    /* 快捷键：L3+R3（两个摇杆同时按下）保持 8 秒切换状态 OSD。
     * 与串流菜单组合键 L1+R1+L3+R3 不冲突（那组要求 L1/R1 也按着）。
     * 期间照常把摇杆按下发给 PS5（L3/R3 是合法游戏键），不做拦截。 */
    if (in_now.l3 && in_now.r3) {
        if (!app.osd_hot_fired) {
            if (!app.osd_hot_t0)
                app.osd_hot_t0 = now;
            else if (now - app.osd_hot_t0 >= 8000) {
                app.osd_hot_fired = true;
                app.cfg.osd = !app.cfg.osd;
                config_save();
                app.osd_toast_until = now + 2000;
                SDL_Log("stream: OSD toggled to %d by L3+R3 hold 8s", app.cfg.osd);
            }
        }
    } else {
        app.osd_hot_t0 = 0;
        app.osd_hot_fired = false;
    }

    /* 每 5 秒把统计写进 chiaki.log：调帧率/码率时不用盯着黑边看，事后翻日志就行 */
    if (app.stats_t0 == 0) {
        app.stats_t0 = now;
    } else if (now - app.stats_t0 >= 5000) {
        app.stats_t0 = now;
        int sf = 0, sn = 0, sp = 0, sr = -1, sl = -1, sd = 0, sk = 0;
        bool sh = false;
        stream_get_stats(&sf, &sn, &sp, &sr, &sl, &sh, &sd, &sk);
        SDL_Log("stats: fps=%d total=%d dec=%dms conv=%dms rtt=%dms lost=%d hwdec=%d res=%s@%d br=%d rx=%dk",
                sf, sn, sd, sp, sr, sl, (int)sh, res_name(app.cfg.resolution),
                app.cfg.fps, app.cfg.bitrate, sk);
    }

    /* chiaki 默认的串流菜单组合键：L1+R1+L3+R3 */
    {
        bool combo = in_now.l1 && in_now.r1 && in_now.l3 && in_now.r3;
        bool combo_prev = in_prev.l1 && in_prev.r1 && in_prev.l3 && in_prev.r3;
        if (combo && !combo_prev) {
            app.settings_sel = 0;
            app.screen = SCR_MENU;
        }
    }

    int arg = 0;
    int ev = stream_poll_event(&arg);
    if (ev == STREAM_EV_PIN_REQUEST) {
        app.pin_incorrect = arg != 0;
        app.pin_len = 0;
        app.pin[0] = 0;
        app.pin_cursor = 0;
        app.screen = SCR_PIN;
    } else if (ev == STREAM_EV_CONNECTED) {
        uint8_t rk[16], mo[16];
        if (stream_get_regist_key(rk) && app.active_host >= 0 && app.active_host < app.host_count) {
            HostEntry *hh = &app.hosts[app.active_host];
            key_to_hex(rk, 16, hh->regkey, sizeof(hh->regkey));
            if (stream_get_morning(mo))
                key_to_hex(mo, 16, hh->morning, sizeof(hh->morning));
            hh->saved = 1;
            hosts_save();
            SDL_Log("hosts: saved keys for %s", hh->addr);
        }
    } else if (ev == STREAM_EV_QUIT) {
        app.stream_quit_pending = true;
        app.stream_quit_reason = arg;
    }

    if (app.stream_quit_pending) {
        char body[256];
        snprintf(body, sizeof(body), "%s\n\nSee chiaki.log for details.",
                 chiaki_quit_reason_string((ChiakiQuitReason)app.stream_quit_reason));
        stream_stop_rumble();
        stream_stop();
        discovery_start();
        osd_reset();
        message("串流结束", body);
        app.stream_quit_pending = false;
    }
}

static void update_menu(void)
{
    if (pressed(in_now.up, in_prev.up))
        app.settings_sel = (app.settings_sel + 3) % 4;
    if (pressed(in_now.down, in_prev.down))
        app.settings_sel = (app.settings_sel + 1) % 4;

    if (pressed(in_now.back, in_prev.back) || pressed(in_now.start, in_prev.start)) {
        app.screen = SCR_STREAM;
        return;
    }
    if (pressed(in_now.confirm, in_prev.confirm)) {
        if (app.settings_sel == 0) {
            app.screen = SCR_STREAM;
        } else if (app.settings_sel == 3) {
            /* 震动测试：直接走 sysfs（虚拟手柄没有 FF 能力，SDL 那条路不通） */
            stream_motor_pulse(100, 700);
            SDL_Log("rumble test: 700ms full via sysfs");
        } else {
            stop_session();
        }
    }
}

static void update_settings(void)
{
    if (pressed(in_now.up, in_prev.up))
        app.settings_sel = (app.settings_sel + 7) % 8;
    if (pressed(in_now.down, in_prev.down))
        app.settings_sel = (app.settings_sel + 1) % 8;
    if (pressed(in_now.left, in_prev.left))
        apply_settings_delta(-1);
    if (pressed(in_now.right, in_prev.right))
        apply_settings_delta(1);
    if (pressed(in_now.confirm, in_prev.confirm))
        app.settings_sel = (app.settings_sel + 1) % 8;
    if (pressed(in_now.back, in_prev.back))
        app.screen = SCR_HOSTS;
}

static void update_keymap(void)
{
    if (app.keymap_wait >= 0)
        return;

    if (pressed(in_now.up, in_prev.up))
        app.keymap_sel = (app.keymap_sel + ACT_COUNT - 1) % ACT_COUNT;
    if (pressed(in_now.down, in_prev.down))
        app.keymap_sel = (app.keymap_sel + 1) % ACT_COUNT;
    if (app.keymap_sel < app.keymap_top)
        app.keymap_top = app.keymap_sel;
    if (app.keymap_sel > app.keymap_top + 10)
        app.keymap_top = app.keymap_sel - 10;
    if (pressed(in_now.confirm, in_prev.confirm))
        app.keymap_wait = app.keymap_sel;
    if (pressed(in_now.back, in_prev.back))
        app.screen = SCR_HOSTS;
}

static void update_message(void)
{
    if (pressed(in_now.confirm, in_prev.confirm) || pressed(in_now.back, in_prev.back)) {
        /* 回到弹消息之前的界面：从添加界面报错就回添加界面，别把输入的内容丢了。
         * 串流相关的消息不能回 SCR_STREAM（画面已经没了），一律回列表。 */
        int back = app.prev_screen;
        if (back == SCR_STREAM || back == SCR_MENU || back == SCR_PIN || back == SCR_MESSAGE)
            back = SCR_HOSTS;
        app.screen = back;
    }
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

static void cleanup(void)
{
    /* 不要用 stop_session()：它会顺手 discovery_start()，退出时反而把 UDP 服务拉起来 */
    stream_register_cancel();
    stream_stop_rumble();
    stream_stop();
    discovery_stop();
    if (app.pad) SDL_GameControllerClose(app.pad);
    if (app.joy) SDL_JoystickClose(app.joy);
    if (app.osd_top) { SDL_DestroyTexture(app.osd_top); app.osd_top = NULL; }
    if (app.osd_hint) { SDL_DestroyTexture(app.osd_hint); app.osd_hint = NULL; }
    ui_shutdown();
    if (app.ren) SDL_DestroyRenderer(app.ren);
    if (app.win) SDL_DestroyWindow(app.win);
    if (app.host_lock) SDL_DestroyMutex(app.host_lock);
    if (app.logfp) fclose(app.logfp);
    SDL_Quit();
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    const char *at = getenv("CHIAKI_AUTOTEST");
    g_autotest = (at && at[0] && at[0] != '0');
    const char *no = getenv("CHIAKI_NOOSD");
    g_noosd = (no && no[0] && no[0] != '0');

    memset(&app, 0, sizeof(app));
    app.screen = SCR_HOSTS;
    app.running = true;
    app.keymap_wait = -1;
    app.host_sel = 0;
    app.width = 1024;
    app.height = 768;

    const char *base = SDL_GetBasePath();
    if (!base) base = "./";
    snprintf(app.base, sizeof(app.base), "%s", base);
    snprintf(app.cfg_path, sizeof(app.cfg_path), "%sconfig.ini", app.base);
    snprintf(app.hosts_path, sizeof(app.hosts_path), "%shosts.txt", app.base);

    char logpath[600];
    snprintf(logpath, sizeof(logpath), "%schiaki.log", app.base);
    {
        /* 只追加不截断的话，跑久了能把 SD 卡吃满 */
        struct stat st;
        if (stat(logpath, &st) == 0 && st.st_size > (2L << 20))
            remove(logpath);
    }
    app.logfp = fopen(logpath, "a");

    /* 配置要在建渲染器之前读：vsync 是建渲染器时的选项 */
    config_load(app.cfg_path);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL init failed: %s\n", SDL_GetError());
        return 1;
    }
    Uint32 winflags = SDL_WINDOW_SHOWN;
    app.win = SDL_CreateWindow(APP_NAME, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               app.width, app.height, winflags);
    if (!app.win) {
        fprintf(stderr, "window failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    Uint32 rflags = SDL_RENDERER_ACCELERATED;
    if (app.cfg.vsync) rflags |= SDL_RENDERER_PRESENTVSYNC;
    app.ren = SDL_CreateRenderer(app.win, -1, rflags);
    if (!app.ren) {
        app.ren = SDL_CreateRenderer(app.win, -1, SDL_RENDERER_SOFTWARE);
    }
    SDL_Log("renderer vsync=%d", app.cfg.vsync);
    if (!app.ren) {
        fprintf(stderr, "renderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(app.win);
        SDL_Quit();
        return 1;
    }
    SDL_RendererInfo rinfo;
    if (SDL_GetRendererInfo(app.ren, &rinfo) == 0) {
        SDL_Log("renderer: %s", rinfo.name);
        if (!strcmp(rinfo.name, "software"))
            stream_set_use_argb(true);
    }

    SDL_GetRendererOutputSize(app.ren, &app.width, &app.height);

    char fontpath[600];
    snprintf(fontpath, sizeof(fontpath), "%sassets/font.ttf", app.base);
    if (ui_init(app.ren, "/usr/trimui/res/regular.ttf") != 0 &&
        ui_init(app.ren, fontpath) != 0 &&
        ui_init(app.ren, "/mnt/SDCARD/Apps/BrickTools/regular.ttf") != 0) {
        fprintf(stderr, "font load failed\n");
    }

    chiaki_log_init(&g_log, (ChiakiLogLevel)app.cfg.loglevel, app_log_cb, NULL);

    app.regist_host = -1;
    app.host_lock = SDL_CreateMutex();

    hosts_load();
    if (app.host_count == 0) SDL_Log("hosts: none saved, waiting for discovery or manual add");

    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            app.pad = SDL_GameControllerOpen(i);
            if (app.pad) {
                SDL_Log("gamecontroller: %s", SDL_GameControllerName(app.pad));
                break;
            }
        }
    }

    discovery_start();

    /* Kiosk-style boot straight into the session. */
    if (getenv("CHIAKI_DUMP"))
        stream_dump_samples(true);

    if (app.cfg.autoconnect && app.host_count > 0) {
        app.host_sel = 0;
        start_session();
    }

    while (app.running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                app.running = false;
            } else if (e.type == SDL_KEYDOWN) {
                app.last_scancode = e.key.keysym.scancode;
                if (app.keymap_wait >= 0) {
                    app.cfg.key[app.keymap_wait] = (int)e.key.keysym.scancode;
                    app.keymap_wait = -1;
                    config_save();
                }
            } else if (e.type == SDL_CONTROLLERBUTTONDOWN) {
                /* 只在按键映射界面打日志：串流时每次按键一行会白刷 run.log */
                if (app.keymap_wait >= 0) {
                    SDL_Log("pad button %d down", (int)e.cbutton.button);
                    app.cfg.key[app.keymap_wait] = -(int)e.cbutton.button - 1 - CTRL_CODE_OFFSET;
                    app.keymap_wait = -1;
                    config_save();
                }
            } else if (e.type == SDL_JOYBUTTONDOWN) {
                app.last_joy_button = e.jbutton.button;
                if (app.keymap_wait >= 0) {
                    app.cfg.key[app.keymap_wait] = -(int)e.jbutton.button - 1;
                    app.keymap_wait = -1;
                    config_save();
                }
            } else if (e.type == SDL_CONTROLLERDEVICEADDED) {
                if (!app.pad)
                    app.pad = SDL_GameControllerOpen(e.cdevice.which);
            }
        }

        input_poll();

        poll_register();   /* 配对是异步的，每帧看一眼结果 */
        stream_tick_motor();  /* 震动测试脉冲的到期收尾，不再起 timer 线程 */
        switch (app.screen) {
        case SCR_HOSTS:    update_hosts();    break;
        case SCR_PIN:      update_pin();      break;
        case SCR_STREAM:   update_stream();   break;
        case SCR_MENU:     update_menu();     break;
        case SCR_SETTINGS: update_settings(); break;
        case SCR_KEYMAP:   update_keymap();   break;
        case SCR_ADD:      update_add();      break;
        case SCR_KEYTEST:
            if (pressed(in_now.back, in_prev.back)) app.screen = SCR_HOSTS;
            break;
        case SCR_MESSAGE:  update_message();  break;
        }

        switch (app.screen) {
        case SCR_HOSTS:    draw_hosts();    break;
        case SCR_PIN:      draw_pin();      break;
        case SCR_STREAM:   draw_stream();   break;
        case SCR_MENU:
            draw_stream();
            draw_menu();
            break;
        case SCR_SETTINGS: draw_settings(); break;
        case SCR_KEYMAP:   draw_keymap();   break;
        case SCR_ADD:      draw_add();      break;
        case SCR_KEYTEST:  draw_keytest();  break;
        case SCR_MESSAGE:  draw_message();  break;
        }

        SDL_RenderPresent(app.ren);

        /* 渲染器已经开了 vsync，再叠 16ms 延时会把菜单压到 30fps，按键反馈发钝 */
        if (app.screen != SCR_STREAM && app.screen != SCR_MENU)
            SDL_Delay(8);
    }

    cleanup();
    return 0;
}
