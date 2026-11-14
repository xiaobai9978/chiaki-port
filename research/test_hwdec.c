/* Standalone test for port/src/hwdec.c: feed the same Annex-B H.265 stream
 * used by test_cedar2.c through the module that chiaki links, and verify it
 * yields real IYUV frames without needing a live PS5. */
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
    const char *path = argc > 1 ? argv[1] : "/tmp/h265.h265";
    int is_h265 = strstr(path, "265") != NULL;
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *data = malloc((size_t)fsz);
    if (fread(data, 1, (size_t)fsz, f) != (size_t)fsz) return 1;
    fclose(f);
    printf("input %s: %ld bytes\n", path, fsz);

    if (!hwdec_init(is_h265 ? 1 : 0, 1280, 720)) {
        printf("hwdec_init FAILED: %s\n", hwdec_error());
        return 2;
    }
    printf("hwdec_init OK\n");

    long pos = 0, au_start = 0;
    int have_vcl = 0, aus = 0, frames = 0;
    while (pos < fsz) {
        int sc = 0;
        long nal = find_start_code(data, fsz, pos, &sc);
        if (nal < 0) break;
        int type = is_h265 ? ((data[nal] >> 1) & 0x3f) : (data[nal] & 0x1f);
        int is_vcl = is_h265 ? (type <= 31) : (type == 1 || type == 5);

        if (is_vcl && have_vcl) {
            long len = nal - sc - au_start;
            if (len > 0) {
                HwFrame fr;
                aus++;
                if (hwdec_decode(data + au_start, (size_t)len, &fr)) {
                    frames++;
                    if (frames == 1) {
                        printf("FIRST HWFRAME: %dx%d nv12=%p ystride=%d\n",
                               fr.w, fr.h, (void *)fr.nv12, fr.ystride);
                        FILE *fp = fopen("/tmp/hwfirst.nv12", "wb");
                        if (fp) {
                            size_t n = (size_t)fr.w * (size_t)fr.h * 3 / 2;
                            fwrite(fr.nv12, 1, n, fp);
                            fclose(fp);
                            printf("saved /tmp/hwfirst.nv12 (%zu bytes)\n", n);
                        }
                        unsigned int sum = 0;
                        for (int i = 0; i < fr.w * fr.h; i += 97) sum += fr.y[i];
                        printf("Y checksum=%u\n", sum);
                    }
                }
            }
            au_start = nal - sc;
        }
        if (is_vcl) have_vcl = 1;
        pos = nal;
    }
    printf("AUs=%d frames=%d active=%d\n", aus, frames, hwdec_active());
    hwdec_fini();
    return frames > 0 ? 0 : 3;
}
