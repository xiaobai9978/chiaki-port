/* Repeated init/fini in one process: mimics disconnect/reconnect. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hwdec.h"

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/tmp/h265.h265";
    FILE *f = fopen(path, "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *data = malloc((size_t)fsz);
    if (fread(data, 1, (size_t)fsz, f) != (size_t)fsz) return 1;
    fclose(f);

    for (int cycle = 0; cycle < 3; cycle++) {
        if (!hwdec_init(1, 1280, 720)) {
            printf("cycle %d init FAILED: %s\n", cycle, hwdec_error());
            return 2;
        }
        int frames = 0;
        /* Feed the first 40 access units of the stream (start codes are 3 or 4 bytes). */
        long prev = -1, pos = 0, fed = 0;
        while (pos + 4 < fsz && fed < 40) {
            int sc = 0;
            if (data[pos] == 0 && data[pos+1] == 0 && data[pos+2] == 1) sc = 3;
            else if (data[pos] == 0 && data[pos+1] == 0 && data[pos+2] == 0 && data[pos+3] == 1) sc = 4;
            if (sc) {
                if (prev >= 0) {
                    HwFrame fr;
                    if (hwdec_decode(data + prev, (size_t)(pos - prev), &fr)) frames++;
                    fed++;
                }
                prev = pos;
                pos += sc;
            } else pos++;
        }
        printf("cycle %d: frames=%d active=%d\n", cycle, frames, hwdec_active());
        hwdec_fini();
        printf("cycle %d: after fini active=%d\n", cycle, hwdec_active());
    }
    printf("CYCLE_TEST_OK\n");
    return 0;
}
