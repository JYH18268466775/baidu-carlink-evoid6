/* Host harness for usb-ring-impl.c. Checks the ring, backpressure, 32-bit
 * counter wrap, timestamp arithmetic and the continuous-session switches. */
#include <pthread.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void hook_ring_reset(unsigned start, unsigned live, unsigned continuous);
void *hook_writer_entry(void *unused);
void hook_ring_finish(void);
int hook_ring_record(unsigned kind, const void *a, unsigned an, const void *b, unsigned bn);
unsigned hook_ring_failed(void), hook_ring_consumed(void), hook_ring_published(void), hook_ring_size(void);
int hook_video(unsigned preset_frames, unsigned *frames_out, unsigned char *out);

static unsigned char *sink; static size_t sink_len, sink_cap;
static atomic_int block_writes; static atomic_int fail_writes; static atomic_uint alarm_calls, last_alarm;
static pthread_mutex_t sink_lock = PTHREAD_MUTEX_INITIALIZER;

long test_write(int fd, const void *data, size_t size)
{
    if (fd != 1) return (long)size;
    while (atomic_load(&block_writes)) usleep(200);
    if (atomic_load(&fail_writes)) return -1;
    size_t part = size > 70000 ? 70000 : size; /* short writes exercise the retry loop */
    pthread_mutex_lock(&sink_lock);
    if (sink_len + part > sink_cap) { sink_cap = (sink_len + part) * 2; sink = realloc(sink, sink_cap); }
    memcpy(sink + sink_len, data, part); sink_len += part;
    pthread_mutex_unlock(&sink_lock);
    return (long)part;
}
int test_open(const char *p, int f, ...) { (void)p; (void)f; return 3; }
int test_close(int fd) { (void)fd; return 0; }
int test_devctl(int fd, int c, void *b, size_t n, int *o) { (void)fd; (void)c; (void)b; (void)n; (void)o; return 0; }
long test_read(int fd, void *b, size_t n) { (void)fd; (void)b; (void)n; return 0; }
unsigned test_alarm(unsigned s) { atomic_fetch_add(&alarm_calls, 1); atomic_store(&last_alarm, s); return 0; }
int test_usleep(unsigned us) { usleep(us < 200 ? us : 200); return 0; }
int test_pthread_create(unsigned *a, const void *b, void *(*c)(void *), void *d) { (void)a; (void)b; (void)c; (void)d; return 99; }
int test_pthread_join(unsigned a, void **b) { (void)a; (void)b; return 99; }

int *__get_errno_ptr(void) { return &errno; }
static int bad;
static int check(int ok, const char *what) { printf("%s: %s\n", ok ? "PASS" : "FAIL", what); if (!ok) bad = 1; return ok; }

static void fill(unsigned char *p, unsigned n, unsigned seq)
{
    for (unsigned i = 0; i < n; ++i) p[i] = (unsigned char)(seq * 31u + i * 7u);
}

/* Parse the sink as consecutive records (no MDEV prefix since kind 3 only). */
static int verify(unsigned records, unsigned size)
{
    size_t off = 0;
    for (unsigned r = 0; r < records; ++r) {
        if (off + 24 > sink_len) return 0;
        uint32_t kind, len; memcpy(&kind, sink + off + 16, 4); memcpy(&len, sink + off + 20, 4);
        if (kind != 3 || len != size + 8) return 0;
        uint32_t seq; memcpy(&seq, sink + off + 24, 4);
        if (seq != r) return 0;
        unsigned char *want = malloc(size);
        fill(want, size, r);
        const int same = !memcmp(sink + off + 32, want, size);
        free(want);
        if (!same) return 0;
        off += 24 + len;
    }
    return off == sink_len;
}

static int pump(unsigned start, unsigned records, unsigned size)
{
    pthread_t t; sink_len = 0;
    hook_ring_reset(start, 2, 1);
    pthread_create(&t, 0, hook_writer_entry, 0);
    unsigned char *frame = malloc(size);
    for (unsigned r = 0; r < records; ++r) {
        fill(frame, size, r);
        unsigned seq[2] = {r, 0};
        int rc;
        while ((rc = hook_ring_record(3, seq, 8, frame, size)) != 0) { /* ring full: the writer must catch up */
            if (hook_ring_failed()) break;
            usleep(500);
        }
        if (hook_ring_failed()) break;
    }
    while (hook_ring_consumed() != hook_ring_published() && !hook_ring_failed()) usleep(500);
    hook_ring_finish(); pthread_join(t, 0); free(frame);
    return !hook_ring_failed() && verify(records, size);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    /* 1: 40 MiB through the 16 MiB ring: many wraps, odd record size so records straddle the end. */
    check(pump(0, 400, 104729), "40 MiB of records cross the 16 MiB ring intact (record straddles the wrap)");
    /* 2: counters start just below 2^32, so published and consumed wrap mid-run. */
    check(pump(0xffffff00u, 300, 70001), "free-running 32-bit counters wrap past 2^32 without corruption");

    /* 3: backpressure. With writes blocked the ring fills and live_record refuses instead of overwriting. */
    {
        sink_len = 0; atomic_store(&block_writes, 1);
        pthread_t t; hook_ring_reset(0, 2, 1); pthread_create(&t, 0, hook_writer_entry, 0);
        unsigned size = 1 << 20; unsigned char *frame = malloc(size); unsigned accepted = 0; int refused = 0;
        for (unsigned r = 0; r < 40; ++r) {
            fill(frame, size, r); unsigned seq[2] = {r, 0};
            if (hook_ring_record(3, seq, 8, frame, size)) { refused = 1; break; }
            ++accepted;
        }
        /* The writer may hold one chunk in flight (blocked inside write), so the bound is ring + one chunk. */
        check(refused && accepted >= 14 && accepted <= 17, "a stalled pipe makes the ring refuse after about 16 MiB, never overwrite");
        atomic_store(&block_writes, 0);
        while (hook_ring_consumed() != hook_ring_published()) usleep(500);
        hook_ring_finish(); pthread_join(t, 0);
        check(verify(accepted, size), "everything accepted before the refusal is delivered in order");
        free(frame);
    }
    /* 4: a failing pipe is reported, not silent. */
    {
        atomic_store(&fail_writes, 1); sink_len = 0;
        pthread_t t; hook_ring_reset(0, 2, 1); pthread_create(&t, 0, hook_writer_entry, 0);
        unsigned seq[2] = {0, 0}; char data[64] = {0};
        hook_ring_record(3, seq, 8, data, sizeof data);
        pthread_join(t, 0);
        check(hook_ring_failed() == 1, "pipe write failure sets writer_failed");
        atomic_store(&fail_writes, 0);
    }
    /* 5: timestamps equal floor(n*1e6/30) for every n, including past the old 32-bit overflow at n=4295. */
    {
        unsigned samples[] = {2, 3, 29, 30, 899, 4294, 4295, 5000, 128000, 1000000, 3000000};
        int ok = 1;
        for (unsigned i = 0; i < sizeof samples / sizeof *samples; ++i) {
            unsigned n = samples[i], frames; unsigned char rec[40];
            hook_ring_reset(0, 2, 1);
            hook_video(n, &frames, rec);
            uint32_t lo, hi; memcpy(&lo, rec + 24, 4); memcpy(&hi, rec + 28, 4);
            uint64_t want = (uint64_t)n * 1000000ull / 30ull;
            if (((uint64_t)hi << 32 | lo) != want) { printf("  n=%u got %llu want %llu\n", n, (unsigned long long)((uint64_t)hi << 32 | lo), (unsigned long long)want); ok = 0; }
        }
        check(ok, "timestamp = floor(n*1000000/30) with a carried high word");
        /* every n below 143 s of video (4295 frames) equals the original 32-bit formula */
        int same = 1;
        for (unsigned n = 2; n < 4294; n += 1) {
            unsigned old = n * 1000000u / 30u;
            uint64_t m = (uint64_t)n * 33333u + n / 3u;
            if ((uint32_t)m != old || (m >> 32)) { same = 0; break; }
        }
        check(same, "identical to the car-verified 32-bit formula for the whole range it supported");
    }
    /* 6: continuous mode has no 900-frame stop and cancels the startup alarm at the first frame; default mode is unchanged. */
    {
        unsigned frames; unsigned char rec[40]; int rc;
        hook_ring_reset(0, 2, 0); rc = hook_video(899, &frames, rec);
        check(rc == 1 && frames == 900, "default session still stops at the 900th frame");
        hook_ring_reset(0, 2, 1); rc = hook_video(899, &frames, rec);
        check(rc == 0 && frames == 900, "continuous session keeps going past frame 900");
        hook_ring_reset(0, 2, 1); rc = hook_video(5000, &frames, rec);
        check(rc == 0 && frames == 5001, "continuous session keeps going far past the old cap");
        atomic_store(&alarm_calls, 0); atomic_store(&last_alarm, 99);
        hook_ring_reset(0, 2, 1); hook_video(0, &frames, rec);
        check(frames == 1 && atomic_load(&alarm_calls) == 1 && atomic_load(&last_alarm) == 0, "continuous session cancels the handshake alarm at the first frame");
        atomic_store(&alarm_calls, 0);
        hook_ring_reset(0, 2, 0); hook_video(0, &frames, rec);
        check(atomic_load(&alarm_calls) == 0, "default session leaves the alarm armed");
    }
    printf(bad ? "RING TESTS FAILED\n" : "ring tests ok\n");
    return bad;
}
