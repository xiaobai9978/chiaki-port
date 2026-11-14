/* Regist lifecycle self-test: include the production stream.c so we exercise
 * the exact stream_register / _poll / _cancel code, not a copy.
 * Build in the CT chroot reusing chiaki_brick's link libraries. */
#include "../port/src/stream.c"

#include <stdio.h>

static void test_log_cb(ChiakiLogLevel level, const char *msg, void *user)
{
    (void)user;
    fprintf(stderr, "[%c] %s\n", chiaki_log_level_char(level), msg);
}

int main(void)
{
    ChiakiLog log;
    chiaki_log_init(&log, 7, test_log_cb, NULL);

    /* 192.0.2.1 is TEST-NET-1: never answers, so the regist thread stays
     * blocked in network I/O -- exactly the case cancel() must interrupt. */
    const char *host = "192.0.2.1";

    printf("T1 register\n");
    int r = stream_register(host, 1, 12345678, NULL, &log);
    printf("T1 r=%d state=%d\n", r, stream_register_state());
    SDL_Delay(1500);
    printf("T1 cancel\n");
    stream_register_cancel();
    printf("T1 after cancel state=%d\n", stream_register_state());

    printf("T2 register again (fresh object)\n");
    r = stream_register(host, 1, 12345678, NULL, &log);
    printf("T2 r=%d state=%d\n", r, stream_register_state());
    SDL_Delay(1500);
    uint8_t k[16], m[16];
    printf("T2 poll=%d\n", stream_register_poll(k, m));
    stream_register_cancel();
    printf("T2 after cancel state=%d\n", stream_register_state());

    printf("T3 back-to-back register (must clean up the previous live one)\n");
    r = stream_register(host, 1, 12345678, NULL, &log);
    printf("T3a r=%d state=%d\n", r, stream_register_state());
    SDL_Delay(300);
    r = stream_register(host, 1, 87654321, NULL, &log);
    printf("T3b r=%d state=%d\n", r, stream_register_state());
    SDL_Delay(1200);
    stream_register_cancel();
    printf("T3 after cancel state=%d\n", stream_register_state());

    printf("LIFECYCLE_OK\n");
    return 0;
}
