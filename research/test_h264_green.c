/* Reproduce the "green bar at the bottom" on H.264 hardware decode:
 * feed an Annex-B H.264 file through hwdec_decode and dump the full NV12
 * output plus per-row statistics for the bottom rows, so we can see exactly
 * where (and in which plane) the invalid chroma comes from. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hwdec.h"

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
    const char *path = argc > 1 ? argv[1] : "/tmp/t540.h264";
    int want_w = argc > 2 ? atoi(argv[2]) : 960;
    int want_h = argc > 3 ? atoi(argv[3]) : 540;
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *data = malloc((size_t)fsz);
    if (fread(data, 1, (size_t)fsz, f) != (size_t)fsz) return 1;
    fclose(f);
    printf("input %s: %ld bytes, want %dx%d\n", path, fsz, want_w, want_h);

    if (!hwdec_init(0, want_w, want_h)) {
        printf("hwdec_init FAILED: %s\n", hwdec_error());
        return 2;
    }

    long pos = 0, au_start = 0;
    int have_vcl = 0, frames = 0;
    int fr_w = 0, fr_h = 0;
    while (pos < fsz) {
        int sc = 0;
        long nal = find_start_code(data, fsz, pos, &sc);
        if (nal < 0) break;
        int type = data[nal] & 0x1f;
        int is_vcl = (type == 1 || type == 5);
        if (is_vcl && have_vcl) {
            long len = nal - sc - au_start;
            if (len > 0) {
                HwFrame fr;
                if (hwdec_decode(data + au_start, (size_t)len, &fr)) {
                    frames++;
                    if (frames == 30) {   /* a few frames in, decoder settled */
                        fr_w = fr.w; fr_h = fr.h;
                        size_t n = (size_t)fr.w * (size_t)fr.h * 3 / 2;
                        FILE *fp = fopen("/tmp/green.nv12", "wb");
                        if (fp) { fwrite(fr.nv12, 1, n, fp); fclose(fp); }
                        break;
                    }
                }
            }
            au_start = nal - sc;
        }
        if (is_vcl) have_vcl = 1;
        pos = nal;
    }
    if (!fr_w) { printf("no frame at 30\n"); hwdec_fini(); return 3; }
    printf("HWFRAME %dx%d (want %dx%d)\n", fr_w, fr_h, want_w, want_h);

    /* per-row averages for the last 24 luma rows and last 12 chroma rows */
    unsigned char *buf = malloc((size_t)fr_w * (size_t)fr_h * 3 / 2);
    FILE *fp = fopen("/tmp/green.nv12", "rb");
    if (!fp) { hwdec_fini(); return 4; }
    fread(buf, 1, (size_t)fr_w * (size_t)fr_h * 3 / 2, fp);
    fclose(fp);
    printf("--- last luma rows (y avg) ---\n");
    for (int j = fr_h - 24; j < fr_h; j++) {
        long s = 0;
        for (int i = 0; i < fr_w; i++) s += buf[(size_t)j * fr_w + i];
        printf("row %d: %ld\n", j, s / fr_w);
    }
    printf("--- last chroma rows (u v avg) ---\n");
    unsigned char *uv = buf + (size_t)fr_w * fr_h;
    for (int j = fr_h / 2 - 12; j < fr_h / 2; j++) {
        long su = 0, sv = 0;
        for (int i = 0; i < fr_w / 2; i++) {
            su += uv[(size_t)j * fr_w + i * 2];
            sv += uv[(size_t)j * fr_w + i * 2 + 1];
        }
        printf("crow %d: u=%ld v=%ld\n", j, su / (fr_w / 2), sv / (fr_w / 2));
    }
    hwdec_fini();
    return 0;
}
