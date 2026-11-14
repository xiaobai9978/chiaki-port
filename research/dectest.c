#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    const AVCodec *c = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!c) { printf("no h264 decoder\n"); return 1; }
    AVCodecContext *ctx = avcodec_alloc_context3(c);
    if (avcodec_open2(ctx, c, NULL) < 0) { printf("open failed\n"); return 1; }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("no file\n"); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc(n); fread(buf, 1, n, f); fclose(f);

    AVCodecParserContext *ps = av_parser_init(AV_CODEC_ID_H264);
    AVPacket *pkt = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    int frames = 0, packets = 0;
    unsigned char *p = buf; long left = n;
    while (left > 0) {
        uint8_t *out = NULL; int outsz = 0;
        int used = av_parser_parse2(ps, ctx, &out, &outsz, p, (int)left, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (used < 0) break;
        p += used; left -= used;
        if (outsz > 0) {
            packets++;
            AVPacket p2 = { .data = out, .size = outsz };
            if (avcodec_send_packet(ctx, &p2) == 0)
                while (avcodec_receive_frame(ctx, fr) == 0) { frames++; av_frame_unref(fr); }
        }
    }
    avcodec_send_packet(ctx, NULL);
    while (avcodec_receive_frame(ctx, fr) == 0) { frames++; av_frame_unref(fr); }
    printf("packets=%d frames=%d %dx%d\n", packets, frames, ctx->width, ctx->height);
    return frames > 0 ? 0 : 2;
}
