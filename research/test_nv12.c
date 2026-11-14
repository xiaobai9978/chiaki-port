/* Probe: can this SDL2/Mali renderer create and draw an NV12 texture?
 * If yes, hwdec could skip the per-pixel UV split and feed cedar's two planes
 * straight into SDL_UpdateNVTexture().
 *
 * NOTE: the CT build sysroot has an old SDL2 without SDL_UpdateNVTexture in
 * its headers/lib (2.0.16+ only), while the handheld ships 2.30.8. Resolve it
 * with dlsym so this probe compiles against the old sysroot. */
#include <SDL.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

typedef int (*UpdateNVFn)(SDL_Texture *, const SDL_Rect *, const Uint8 *, int,
                          const Uint8 *, int);

int main(void)
{
    const int W = 1280, H = 720, STRIDE = 1280, PADH = 736;
    FILE *f = fopen("/tmp/first.nv12", "rb");
    if (!f) { printf("no /tmp/first.nv12\n"); return 1; }
    size_t ysz = (size_t)STRIDE * PADH;
    unsigned char *buf = malloc(ysz + ysz / 2);
    if (fread(buf, 1, ysz + ysz / 2, f) != ysz + ysz / 2) { printf("short read\n"); return 1; }
    fclose(f);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) { printf("SDL_Init: %s\n", SDL_GetError()); return 2; }
    SDL_Window *win = SDL_CreateWindow("nv12probe", 0, 0, 1024, 768, SDL_WINDOW_FULLSCREEN);
    if (!win) { printf("CreateWindow: %s\n", SDL_GetError()); return 2; }
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) { printf("CreateRenderer: %s\n", SDL_GetError()); return 2; }
    SDL_RendererInfo ri;
    if (SDL_GetRendererInfo(ren, &ri) == 0)
        printf("renderer=%s  NV12_texture=%s\n", ri.name,
               (ri.texture_formats && 0) ? "?" : "check below");

    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_NV12,
                                         SDL_TEXTUREACCESS_STREAMING, W, H);
    if (!tex) {
        printf("NV12_CREATE_FAILED: %s\n", SDL_GetError());
        SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
        return 3;
    }
    printf("NV12 texture created OK\n");
    void *sdllib = dlopen("libSDL2-2.0.so.0", RTLD_NOW);
    UpdateNVFn update_nv = sdllib ? (UpdateNVFn)dlsym(sdllib, "SDL_UpdateNVTexture") : NULL;
    printf("SDL_UpdateNVTexture @ %p\n", (void *)update_nv);
    if (!update_nv) {
        printf("NV12_UPDATE_UNAVAILABLE\n");
        SDL_DestroyTexture(tex);
        SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
        return 4;
    }
    int rc = update_nv(tex, NULL, buf, STRIDE, buf + ysz, STRIDE);
    printf("SDL_UpdateNVTexture = %d %s\n", rc, rc ? SDL_GetError() : "");
    SDL_RenderClear(ren);
    rc = SDL_RenderCopy(ren, tex, NULL, NULL);
    printf("SDL_RenderCopy = %d %s\n", rc, rc ? SDL_GetError() : "");
    SDL_RenderPresent(ren);
    SDL_Delay(1500);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    printf("NV12_PROBE_OK\n");
    return 0;
}
