/* Exercise the real replay loop without QNX or a decoder. */
#define read mock_read
#define open mock_open
#define close mock_close
#define getenv mock_getenv
#include "../../build/live-30s-candidate/src/carlife_qnx_short_replay.c"

const unsigned char carlink_gop_start[1] = {0}, carlink_gop_end[1] = {0};
static int scenario, done, position, sent;
static unsigned char packet[37] = {0};
static int error_number;
int *__get_errno_ptr(void) { return &error_number; }
int usleep(unsigned n) { (void)n; return 0; }
char *mock_getenv(const char *name) { (void)name; return "done"; }
int mock_open(const char *name, int flags, ...) {
    (void)name; (void)flags; return done ? 7 : -1;
}
int mock_close(int fd) { (void)fd; return 0; }
int carlink_mdev_arm_open(void) { return 1; }
int carlink_mdev_arm_close(int fd) { (void)fd; return 0; }
int carlink_mdev_arm_sendv(int fd, const void *v, int n) {
    (void)fd; (void)v;
    if (n == 3) { ++sent; if (scenario == 4) return 9; }
    return 0;
}
__PTRDIFF_TYPE__ mock_read(int fd, void *output, __SIZE_TYPE__ size) {
    (void)fd;
    unsigned char *out = output;
    if (position == 37) {
        if (scenario == 1 || scenario == 2 || scenario == 3) done = 1;
        if (scenario == 2) {
            for (__SIZE_TYPE__ i = 0; i < size; ++i) out[i] = 255;
            return (__PTRDIFF_TYPE__)size;
        }
        if (scenario == 3) { out[0] = 0; ++position; return 1; }
        return 0;
    }
    if (position > 37) return 0;
    if (size > (unsigned)(37 - position)) size = (unsigned)(37 - position);
    for (__SIZE_TYPE__ i = 0; i < size; ++i) out[i] = packet[position++];
    return (__PTRDIFF_TYPE__)size;
}
int main(void) {
    unsigned char setup[80] = {0};
    struct carlink_replay replay = {setup, setup + 80, 1280, 720, 30, 900, 1};
    packet[16] = 3; packet[20] = 13; packet[34] = 0; packet[35] = 1; packet[36] = 5;
    const int expected[] = {65, 0, 65, 0, 67};
    for (scenario = 0; scenario < 5; ++scenario) {
        done = position = sent = 0;
        int rc = carlink_play_replay(&replay, 0, 0);
        if (rc != expected[scenario] || sent != 1) return 1 + scenario;
    }
    printf("PASS: timed EOF, partial timed EOF, early EOF, malformed frame and mdev failure\n");
    return 0;
}
