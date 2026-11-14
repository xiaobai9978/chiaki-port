// SPDX-License-Identifier: AGPL-3.0-only
// UTF-8 text rendering with an on-demand glyph cache (CJK friendly).
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define NUM_SIZES 3

typedef struct {
    uint32_t      cp;
    SDL_Texture  *tex;
    int           w, h, xoff, yoff, xadv;
} Glyph;

typedef struct {
    int     size;
    float   scale;
    float   ascent;
    float   line_h;
    Glyph  *cache;
    int     n, cap;
    int    *htab;       /* cp -> cache 索引+1 的开放寻址表，0 表示空槽 */
    int     htab_cap;   /* 2 的幂，装载因子保持 <= 0.5 */
    stbtt_fontinfo info;
} Font;

static struct {
    SDL_Renderer  *ren;
    unsigned char *ttf;
    Font           font[NUM_SIZES];
    int            ready;
} g;

static int size_index(int size)
{
    if (size <= 20) return 0;
    if (size <= 28) return 1;
    return 2;
}

static uint32_t utf8_next(const unsigned char **pp)
{
    const unsigned char *p = *pp;
    uint32_t cp = 0;
    if (p[0] < 0x80) {
        cp = p[0];
        *pp = p + 1;
    } else if ((p[0] & 0xE0) == 0xC0 && p[1]) {
        cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        *pp = p + 2;
    } else if ((p[0] & 0xF0) == 0xE0 && p[1] && p[2]) {
        cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        *pp = p + 3;
    } else if ((p[0] & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
        cp = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        *pp = p + 4;
    } else {
        cp = '?';
        *pp = p + 1;
    }
    return cp;
}

/* 字形缓存以前是 O(n) 线性扫描，中文界面缓存上千个码点后，每次
 * ui_text_width + draw_text 都要整串扫两遍。改成开放寻址哈希，O(1) 命中。 */
static uint32_t cp_hash(uint32_t cp)
{
    return cp * 2654435761u;
}

static int htab_find(Font *f, uint32_t cp)
{
    if (!f->htab || !f->htab_cap) return -1;
    uint32_t mask = (uint32_t)f->htab_cap - 1;
    uint32_t h = cp_hash(cp) & mask;
    while (f->htab[h]) {
        int gi = f->htab[h] - 1;
        if (gi < f->n && f->cache[gi].cp == cp) return gi;
        h = (h + 1) & mask;
    }
    return -1;
}

static void htab_put(Font *f, uint32_t cp, int gi)
{
    if (!f->htab || !f->htab_cap) return;
    uint32_t mask = (uint32_t)f->htab_cap - 1;
    uint32_t h = cp_hash(cp) & mask;
    while (f->htab[h]) h = (h + 1) & mask;
    f->htab[h] = gi + 1;
}

static void htab_rebuild(Font *f, int newcap)
{
    free(f->htab);
    f->htab = (int *)calloc((size_t)newcap, sizeof(int));
    f->htab_cap = f->htab ? newcap : 0;
    if (!f->htab) return;
    for (int i = 0; i < f->n; i++)
        htab_put(f, f->cache[i].cp, i);
}

static Glyph *glyph_for(int idx, uint32_t cp)
{
    Font *f = &g.font[idx];
    int gi = htab_find(f, cp);
    /* 哈希表分配失败时退回线性查找，避免同一个码点被反复创建、缓存无限增长 */
    if (gi < 0 && !f->htab) {
        for (int i = 0; i < f->n; i++)
            if (f->cache[i].cp == cp) { gi = i; break; }
    }
    if (gi >= 0)
        return &f->cache[gi];

    if (f->n == f->cap) {
        int cap = f->cap ? f->cap * 2 : 128;
        Glyph *nc = realloc(f->cache, (size_t)cap * sizeof(Glyph));
        if (!nc) return NULL;
        f->cache = nc;
        f->cap = cap;
    }
    /* 新字形插入前先把表扩到装载因子 <= 0.5，rebuild 会重放已有项 */
    if (f->n + 1 > f->htab_cap / 2)
        htab_rebuild(f, f->htab_cap ? f->htab_cap * 2 : 256);

    gi = f->n++;
    Glyph *gl = &f->cache[gi];
    memset(gl, 0, sizeof(*gl));
    gl->cp = cp;
    htab_put(f, cp, gi);

    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetCodepointBitmapBox(&f->info, (int)cp, f->scale, f->scale, &x0, &y0, &x1, &y1);
    int gw = x1 - x0, gh = y1 - y0;
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&f->info, (int)cp, &adv, &lsb);
    gl->w = gw; gl->h = gh;
    gl->xoff = x0; gl->yoff = y0;
    gl->xadv = (int)(adv * f->scale + 0.5f);
    if (gw <= 0 || gh <= 0)
        return gl;

    unsigned char *bmp = (unsigned char *)calloc((size_t)gw * gh, 1);
    if (!bmp) { gl->w = gl->h = 0; return gl; }
    stbtt_MakeCodepointBitmap(&f->info, bmp, gw, gh, gw, f->scale, f->scale, (int)cp);

    unsigned char *rgba = (unsigned char *)malloc((size_t)gw * gh * 4);
    if (!rgba) { free(bmp); gl->w = gl->h = 0; return gl; }
    for (int i = 0; i < gw * gh; i++) {
        rgba[i * 4 + 0] = 255;
        rgba[i * 4 + 1] = 255;
        rgba[i * 4 + 2] = 255;
        rgba[i * 4 + 3] = bmp[i];
    }
    gl->tex = SDL_CreateTexture(g.ren, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_STATIC, gw, gh);
    if (gl->tex) {
        SDL_UpdateTexture(gl->tex, NULL, rgba, gw * 4);
        SDL_SetTextureBlendMode(gl->tex, SDL_BLENDMODE_BLEND);
    }
    free(rgba);
    free(bmp);
    return gl;
}

int ui_init(SDL_Renderer *ren, const char *font_path)
{
    memset(&g, 0, sizeof(g));
    g.ren = ren;

    FILE *f = fopen(font_path, "rb");
    if (!f) {
        SDL_Log("ui: cannot open font %s", font_path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return -1; }

    g.ttf = (unsigned char *)malloc((size_t)sz);
    if (!g.ttf) { fclose(f); return -1; }
    if (fread(g.ttf, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(g.ttf); g.ttf = NULL; return -1; }
    fclose(f);

    const int sizes[NUM_SIZES] = { UI_SIZE_SMALL, UI_SIZE_NORMAL, UI_SIZE_LARGE };
    for (int s = 0; s < NUM_SIZES; s++) {
        Font *fo = &g.font[s];
        fo->size = sizes[s];
        if (!stbtt_InitFont(&fo->info, g.ttf, stbtt_GetFontOffsetForIndex(g.ttf, 0))) {
            SDL_Log("ui: invalid font %s", font_path);
            free(g.ttf);
            g.ttf = NULL;
            return -1;
        }
        fo->scale = stbtt_ScaleForPixelHeight(&fo->info, (float)sizes[s]);
        int asc = 0, desc = 0, gap = 0;
        stbtt_GetFontVMetrics(&fo->info, &asc, &desc, &gap);
        fo->ascent = asc * fo->scale;
        fo->line_h = (asc - desc + gap) * fo->scale;
    }
    g.ready = 1;
    SDL_Log("ui: font loaded %s (%ld bytes)", font_path, sz);
    return 0;
}

void ui_shutdown(void)
{
    for (int s = 0; s < NUM_SIZES; s++) {
        Font *fo = &g.font[s];
        for (int i = 0; i < fo->n; i++)
            if (fo->cache[i].tex)
                SDL_DestroyTexture(fo->cache[i].tex);
        free(fo->cache);
        free(fo->htab);
        fo->cache = NULL;
        fo->htab = NULL;
        fo->n = fo->cap = fo->htab_cap = 0;
    }
    free(g.ttf);
    g.ttf = NULL;
    g.ready = 0;
}

static int draw_text(int x, int y, int size, SDL_Color c, const char *text)
{
    if (!g.ready || !text) return 0;
    int idx = size_index(size);
    Font *fo = &g.font[idx];

    float fx = (float)x;
    float baseline = (float)y + fo->ascent;

    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        uint32_t cp = utf8_next(&p);
        if (cp == '\n') {
            fx = (float)x;
            baseline += fo->line_h;
            continue;
        }
        Glyph *gl = glyph_for(idx, cp);
        if (!gl) continue;
        if (gl->tex && gl->w > 0 && gl->h > 0) {
            SDL_SetTextureColorMod(gl->tex, c.r, c.g, c.b);
            SDL_SetTextureAlphaMod(gl->tex, c.a);
            SDL_Rect dst;
            dst.x = (int)lroundf(fx) + gl->xoff;
            dst.y = (int)lroundf(baseline) + gl->yoff;
            dst.w = gl->w;
            dst.h = gl->h;
            SDL_RenderCopy(g.ren, gl->tex, NULL, &dst);
        }
        fx += gl->xadv;
    }
    return (int)(fx - x);
}

int ui_text(int x, int y, int size, SDL_Color c, const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return draw_text(x, y, size, c, buf);
}

int ui_text_width(int size, const char *text)
{
    if (!g.ready || !text) return 0;
    int idx = size_index(size);
    Font *fo = &g.font[idx];
    int w = 0;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        uint32_t cp = utf8_next(&p);
        if (cp == '\n') continue;
        Glyph *gl = glyph_for(idx, cp);
        if (gl) w += gl->xadv;
    }
    return w;
}

int ui_text_height(int size)
{
    if (!g.ready) return size;
    return (int)g.font[size_index(size)].line_h;
}

int ui_text_centered(int cx, int y, int size, SDL_Color c, const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    int w = ui_text_width(size, buf);
    return draw_text(cx - w / 2, y, size, c, buf);
}

int ui_text_right(int rx, int y, int size, SDL_Color c, const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    int w = ui_text_width(size, buf);
    return draw_text(rx - w, y, size, c, buf);
}

void ui_rect(int x, int y, int w, int h, SDL_Color c)
{
    SDL_SetRenderDrawBlendMode(g.ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(g.ren, c.r, c.g, c.b, c.a);
    SDL_Rect r = { x, y, w, h };
    SDL_RenderFillRect(g.ren, &r);
}

void ui_frame(int x, int y, int w, int h, SDL_Color c, int t)
{
    SDL_SetRenderDrawBlendMode(g.ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(g.ren, c.r, c.g, c.b, c.a);
    SDL_Rect r = { x, y, w, h };
    SDL_RenderDrawRect(g.ren, &r);
    for (int i = 1; i < t; i++) {
        SDL_Rect rr = { x + i, y + i, w - 2 * i, h - 2 * i };
        if (rr.w <= 0 || rr.h <= 0) break;
        SDL_RenderDrawRect(g.ren, &rr);
    }
}
