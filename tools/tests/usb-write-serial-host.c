/* POSIX harness for the serialized USB write path. Mocks stand in for the QNX
 * libc calls the receiver makes; usb-write-serial.sh builds and runs this. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int hook_send_cmd(int fd, unsigned type, const unsigned char *body, unsigned size);
int hook_send_hardkey(int fd, unsigned code);
/* Input-thread hooks. Weak so the baseline build (which lacks them) still links
 * for the cmdbytes comparison. */
void hook_input_reset(int usb_fd, int file_fd) __attribute__((weak));
void *hook_input_entry(void *unused) __attribute__((weak));
void hook_set_ready(unsigned value) __attribute__((weak));
void hook_set_finished(void) __attribute__((weak));
void hook_set_closed(void) __attribute__((weak));
int hook_path_ok(const char *path) __attribute__((weak));
int hook_gate_probe(void) __attribute__((weak));
int hook_main(int argc, char **argv) __attribute__((weak));

enum { USB_FD = 7, INPUT_FD = 8, MAX_REC = 40000, ROUNDS = 300, THREADS = 4 };

struct rec { int tid; size_t n; double ts; unsigned char b[72]; };
static struct rec records[MAX_REC];
static atomic_int next_record;
static atomic_int stderr_lines, errlen;
static char errbuf[16384];  /* everything the receiver logged */
static __thread int tid = -1, mock_errno;
static int fail_call = -1, fail_errno, short_first;  /* injected by the test */
static atomic_int write_calls;

static void pause_us(unsigned us)
{
    struct timespec t = {0, (long)us * 1000L};
    nanosleep(&t, 0);
}

long test_write(int fd, const void *buf, size_t n)
{
    if (fd == 2) {                       /* receiver log lines are not USB data */
        atomic_fetch_add(&stderr_lines, 1);
        const int at = atomic_fetch_add(&errlen, (int)n);
        if ((size_t)at + n < sizeof(errbuf))
            memcpy(errbuf + at, buf, n);
        return (long)n;
    }
    const int call = atomic_fetch_add(&write_calls, 1);
    const int slot = atomic_fetch_add(&next_record, 1);  /* arrival order at the "device" */
    struct timespec arrived;
    clock_gettime(CLOCK_MONOTONIC, &arrived);
    pause_us(30);                        /* widen the window an unlocked writer would race in */
    if (slot < MAX_REC) {
        records[slot].tid = tid;
        records[slot].n = n;
        records[slot].ts = (double)arrived.tv_sec * 1000.0 + (double)arrived.tv_nsec / 1e6;
        memcpy(records[slot].b, buf, n < sizeof(records[slot].b) ? n : sizeof(records[slot].b));
    }
    if (call == fail_call) {
        mock_errno = fail_errno;
        return short_first ? 3 : -1;
    }
    return (long)n;
}

int test_usleep(unsigned us) { pause_us(us); return 0; }
unsigned test_alarm(unsigned s) { (void)s; return 0; }
static atomic_int open_calls, close_calls;
int test_open(const char *p, int f, ...)
{
    (void)f;
    atomic_fetch_add(&open_calls, 1);
    if (!strcmp(p, "/dev/shmem/missing")) {
        mock_errno = 2;
        return -1;
    }
    return !strncmp(p, "/dev/shmem/", 11) ? INPUT_FD : USB_FD;
}
int test_close(int fd) { (void)fd; atomic_fetch_add(&close_calls, 1); return 0; }
int test_devctl(int fd, int c, void *d, size_t n, int *r) { (void)fd; (void)c; (void)d; (void)n; (void)r; return 0; }
/* Scripted contents of the bench input file; served in small chunks. */
static unsigned char feed[8192];
static atomic_int feed_end, feed_pos;
static int chunk_limit = 3, max_request;

long test_read(int fd, void *b, size_t n)
{
    if (fd != INPUT_FD)
        return -1;
    if ((int)n > max_request)
        max_request = (int)n;
    const int pos = atomic_load(&feed_pos), avail = atomic_load(&feed_end) - pos;
    if (avail <= 0)
        return 0;  /* end of file: the thread polls and retries */
    size_t take = (size_t)avail < n ? (size_t)avail : n;
    if (take > (size_t)chunk_limit)
        take = (size_t)chunk_limit;
    memcpy(b, feed + pos, take);
    atomic_store(&feed_pos, pos + (int)take);
    return (long)take;
}
int test_pthread_create(unsigned *id, const void *a, void *(*s)(void *), void *g) { (void)id; (void)a; (void)s; (void)g; return 99; }
int test_pthread_join(unsigned id, void **r) { (void)id; (void)r; return 99; }
int *__get_errno_ptr(void) { return &mock_errno; }

static unsigned be32(const unsigned char *p)
{
    return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3];
}

static int check(int condition, const char *what)
{
    if (!condition)
        printf("FAIL: %s\n", what);
    return !condition;
}

/* Per-thread traffic: channel, type, body length. */
static const struct { unsigned channel, type, size; } expected[THREADS] = {
    {1, 0x00018001, 4}, {6, 0x00068008, 2}, {6, 0x00068008, 2}, {1, 0x00018002, 20}
};

static void *worker(void *arg)
{
    const int id = (int)(long)arg;
    static const unsigned char version[] = {8, 1, 16, 0};
    unsigned char filler[20];
    memset(filler, 0xa5, sizeof(filler));
    tid = id;
    for (int i = 0; i < ROUNDS; ++i) {
        int rc = 0;
        if (id == 0) rc = hook_send_cmd(USB_FD, 0x00018001, version, sizeof(version));
        if (id == 1) rc = hook_send_hardkey(USB_FD, 0x14);
        if (id == 2) rc = hook_send_hardkey(USB_FD, 0x0e);
        if (id == 3) rc = hook_send_cmd(USB_FD, 0x00018002, filler, sizeof(filler));
        if (rc)
            abort();
    }
    return 0;
}

static int contention(void)
{
    int bad = 0, interleaved = 0;
    pthread_t threads[THREADS];
    atomic_store(&next_record, 0);
    for (long i = 0; i < THREADS; ++i)
        pthread_create(&threads[i], 0, worker, (void *)i);
    for (int i = 0; i < THREADS; ++i)
        pthread_join(threads[i], 0);

    const int total = atomic_load(&next_record);
    bad |= check(total == THREADS * ROUNDS * 2, "every record is exactly two USB writes");
    int per_thread[THREADS] = {0};
    for (int i = 0; i + 1 < total && i + 1 < MAX_REC; i += 2) {
        const struct rec *outer = &records[i], *inner = &records[i + 1];
        if (outer->tid != inner->tid || outer->n != 8 || inner->n != be32(outer->b + 4)) {
            ++interleaved;
            continue;
        }
        const int t = outer->tid;
        if (t < 0 || t >= THREADS ||
            be32(outer->b) != expected[t].channel ||
            be32(inner->b + 4) != expected[t].type ||
            inner->n != 8u + expected[t].size) {
            ++interleaved;
            continue;
        }
        ++per_thread[t];
    }
    if (interleaved) {
        printf("INTERLEAVED: %d records were split or mixed between writers\n", interleaved);
        return 1;
    }
    for (int t = 0; t < THREADS; ++t)
        bad |= check(per_thread[t] == ROUNDS, "all records from every writer arrived intact");
    if (!bad)
        printf("PASS: %d records from %d concurrent writers stayed outer+inner adjacent\n",
               total / 2, THREADS);
    return bad;
}

static int failures(void)
{
    int bad = 0;
    const unsigned char one[] = {8, 1};
    tid = 9;

    atomic_store(&write_calls, 0);
    fail_call = 0; fail_errno = 5; short_first = 0;
    bad |= check(hook_send_hardkey(USB_FD, 0x14) == -1, "failed first write reports failure");
    bad |= check(atomic_load(&write_calls) == 1, "no body is sent after a failed header");

    atomic_store(&write_calls, 0);
    fail_call = 1;
    bad |= check(hook_send_hardkey(USB_FD, 0x14) == -1, "failed second write reports failure");

    atomic_store(&write_calls, 0);
    fail_call = 0; short_first = 1;
    bad |= check(hook_send_hardkey(USB_FD, 0x14) == -1, "short header write reports failure");
    bad |= check(atomic_load(&write_calls) == 1, "no body is sent after a short header");

    fail_call = -1; short_first = 0;
    atomic_store(&write_calls, 0);
    bad |= check(hook_send_hardkey(USB_FD, 0x14) == 0, "writes work again, so the lock was released");
    bad |= check(atomic_load(&write_calls) == 2, "recovered record is two writes");

    atomic_store(&write_calls, 0);
    unsigned char big[57] = {0};
    bad |= check(hook_send_cmd(USB_FD, 1, big, 57) == -1, "oversize body is refused");
    bad |= check(atomic_load(&write_calls) == 0, "oversize body writes nothing");
    bad |= check(hook_send_cmd(USB_FD, 1, one, 2) == 0, "lock is free after a refused record");
    if (!bad)
        printf("PASS: header/body failures, short writes and oversize bodies release the lock\n");
    return bad;
}

static void feed_append(const void *data, size_t n)
{
    const int end = atomic_load(&feed_end);
    memcpy(feed + end, data, n);
    atomic_store(&feed_end, end + (int)n);  /* publish after the bytes are in place */
}

static int wait_until(atomic_int *value, int target, int limit_ms)
{
    for (int waited = 0; waited < limit_ms; waited += 2) {
        if (atomic_load(value) >= target)
            return 1;
        pause_us(2000);
    }
    return atomic_load(value) >= target;
}

static pthread_t start_input(void)
{
    pthread_t thread;
    atomic_store(&feed_end, 0);
    atomic_store(&feed_pos, 0);
    atomic_store(&write_calls, 0);
    atomic_store(&next_record, 0);
    max_request = 0;
    fail_call = -1;
    hook_input_reset(USB_FD, INPUT_FD);
    pthread_create(&thread, 0, hook_input_entry, 0);
    return thread;
}

static int keys_match(const unsigned char *codes, int count)
{
    for (int k = 0; k < count; ++k) {
        const struct rec *outer = &records[2 * k], *inner = &records[2 * k + 1];
        if (outer->n != 8 || be32(outer->b) != 6 || be32(outer->b + 4) != 10 ||
            inner->n != 10 || be32(inner->b + 4) != 0x00068008 ||
            inner->b[8] != 0x08 || inner->b[9] != codes[k])
            return 0;
    }
    return 1;
}

static int input_tests(void)
{
    int bad = 0;
    pthread_t thread;
    tid = 5;
    chunk_limit = 3;  /* split every line across several reads */

    /* Gate: lines read before the first video frame are discarded. */
    thread = start_input();
    feed_append("OK\nBACK\n", 8);
    bad |= check(wait_until(&feed_pos, 8, 2000), "input file is consumed");
    pause_us(150000);
    bad |= check(atomic_load(&write_calls) == 0, "keys before the first frame are not sent");

    /* Accepted actions, in the order written, as separate outer+inner writes. */
    hook_set_ready(1);
    static const char actions[] = "OK\nBACK\n+2\n-1\nUP\nDOWN\nLEFT\nRIGHT\n";
    static const unsigned char codes[] = {0x14, 0x0e, 0x06, 0x06, 0x07, 0x17, 0x18, 0x15, 0x16};
    feed_append(actions, sizeof(actions) - 1);
    bad |= check(wait_until(&write_calls, 18, 4000), "all nine keys are sent");
    pause_us(100000);
    bad |= check(atomic_load(&write_calls) == 18, "no extra keys are sent");
    bad |= check(keys_match(codes, 9), "keys arrive in order as channel-6 hard keys");
    bad |= check(strstr(errbuf, "Input sent: OK\n") && strstr(errbuf, "Input sent: BACK\n") &&
                 strstr(errbuf, "Input sent: +2 rotary steps\n") &&
                 strstr(errbuf, "Input sent: -1 rotary steps\n") && strstr(errbuf, "Input sent: RIGHT\n"),
                 "each sent action is logged");

    /* Rejected input: unknown words, out-of-range steps, blank and overlong
     * lines, and a last line with no newline. Only the CRLF 'OK' is sent. */
    char junk[400];
    size_t n = 0;
    static const char head[] = "MENU\n+33\n++1\n+\n-\n+0\n\n";
    memcpy(junk, head, sizeof(head) - 1);
    n += sizeof(head) - 1;
    memset(junk + n, 'x', 100);
    n += 100;
    static const char tail[] = "\nOK\r\nOK";
    memcpy(junk + n, tail, sizeof(tail) - 1);
    n += sizeof(tail) - 1;
    feed_append(junk, n);
    bad |= check(wait_until(&write_calls, 20, 4000), "the one valid CRLF line is sent");
    pause_us(200000);
    bad |= check(atomic_load(&write_calls) == 20, "rejected lines and the unterminated line send nothing");
    bad |= check(max_request == 64, "each poll reads at most 64 bytes");
    bad |= check(strstr(errbuf, "Ignored unrecognized input line\n") != 0, "rejected lines are logged");

    /* Rate bound: one read holding four rotary steps must still go out spaced. */
    chunk_limit = 64;
    feed_append("x\n+4\n", 5);  /* 'x' ends the unterminated "OK" above as the unknown line "OKx" */
    bad |= check(wait_until(&write_calls, 28, 4000), "four rotary steps are sent");
    double least_gap = 1e9;
    for (int k = 0; k < 3; ++k) {
        const double gap = records[20 + 2 * (k + 1)].ts - records[20 + 2 * k].ts;
        if (gap < least_gap)
            least_gap = gap;
    }
    bad |= check(least_gap >= 4.5, "consecutive keys are at least about 5 ms apart");

    hook_set_finished();
    pthread_join(thread, 0);  /* a stuck thread is caught by the alarm */

    /* A closed USB descriptor stops the thread before it reads or writes. */
    thread = start_input();
    hook_set_ready(1);
    hook_set_closed();
    feed_append("OK\n", 3);
    pthread_join(thread, 0);
    bad |= check(atomic_load(&write_calls) == 0, "nothing is written after the descriptor is closed");
    bad |= check(hook_send_hardkey(USB_FD, 0x14) == -1 && atomic_load(&write_calls) == 0,
                 "a late writer is refused after close");

    /* A failed USB write ends the input thread; later lines are not sent. */
    thread = start_input();
    hook_set_ready(1);
    fail_call = 0; fail_errno = 5; short_first = 0;
    feed_append("OK\nOK\nOK\n", 9);
    pthread_join(thread, 0);
    fail_call = -1;
    bad |= check(atomic_load(&write_calls) == 1, "input stops after the first failed write");

    /* The gate opens exactly when the first live frame is queued. */
    bad |= check(hook_gate_probe() == 6, "input_ready stays 0 for parameter sets and becomes 1 with the first frame");
    bad |= check(strstr(errbuf, "Key input enabled: first video frame queued\n") != 0, "opening the gate is logged");

    /* Only a plain name under /dev/shmem/ may be used as the input file. */
    char long48[80] = "/dev/shmem/", long49[80] = "/dev/shmem/";
    memset(long48 + 11, 'a', 48); long48[59] = 0;
    memset(long49 + 11, 'a', 49); long49[60] = 0;
    bad |= check(hook_path_ok("/dev/shmem/carlife-keys.txt") && hook_path_ok(long48) &&
                 hook_path_ok("/dev/shmem/.keys"), "valid input file names are accepted");
    bad |= check(!hook_path_ok("/dev/shmem/") && !hook_path_ok("/dev/shmem/..") &&
                 !hook_path_ok("/dev/shmem/.") && !hook_path_ok("/dev/shmem/a/b") &&
                 !hook_path_ok("/dev/shmem/../etc") && !hook_path_ok("/tmp/keys") &&
                 !hook_path_ok("/dev/shmemx/keys") && !hook_path_ok("/dev/shmem/a b") &&
                 !hook_path_ok(long49), "paths outside /dev/shmem, with separators or too long are refused");

    if (!bad)
        printf("PASS: input gate, parsing, rate bound, closed-descriptor, write-failure and path rules\n");
    return bad;
}

static int entry_tests(void)
{
    int bad = 0;
    char *usb = "/dev/oap1", *prog = "capture";
    struct { int argc; char *argv[6]; int status, opens, closes; const char *what; } cases[] = {
        {4, {prog, "--stream-visible", usb, "--input-file"}, 2, 0, 0, "four arguments are refused"},
        {5, {prog, "--stream-visible", usb, "--bogus", "/dev/shmem/k"}, 2, 0, 0, "unknown option is refused"},
        {5, {prog, "--switch", "/dev/android5", "--input-file", "/dev/shmem/k"}, 2, 0, 0, "input file is refused outside streaming modes"},
        {5, {prog, "--capture", usb, "--input-file", "/dev/shmem/k"}, 2, 0, 0, "input file is refused for plain capture"},
        {5, {prog, "--stream-visible", usb, "--input-file", "/tmp/keys"}, 2, 0, 0, "input file outside /dev/shmem is refused"},
        {5, {prog, "--stream-visible", usb, "--input-file", "/dev/shmem/missing"}, 14, 1, 0, "missing input file exits before opening USB"},
        {5, {prog, "--stream-visible", usb, "--input-file", "/dev/shmem/keys"}, 13, 2, 2, "writer startup failure closes both descriptors"},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        atomic_store(&open_calls, 0);
        atomic_store(&close_calls, 0);
        const int status = hook_main(cases[i].argc, cases[i].argv);
        if (status != cases[i].status || atomic_load(&open_calls) != cases[i].opens ||
            atomic_load(&close_calls) != cases[i].closes) {
            printf("FAIL: %s (status %d, opens %d, closes %d)\n", cases[i].what, status,
                   atomic_load(&open_calls), atomic_load(&close_calls));
            bad = 1;
        }
    }
    if (!bad)
        printf("PASS: entry arguments and early-failure cleanup\n");
    return bad;
}

static void identity(void)
{
    /* Printed for the runner to compare with carlife_idrive_keys.cjs. */
    atomic_store(&next_record, 0);
    tid = 0;
    hook_send_hardkey(USB_FD, 0x14);
    hook_send_hardkey(USB_FD, 0x0e);
    for (int i = 0; i < atomic_load(&next_record); ++i) {
        printf("HEX:");
        for (size_t k = 0; k < records[i].n; ++k)
            printf("%02x", records[i].b[k]);
        printf("\n");
    }
}

static void command_bytes(void)
{
    /* Every legal body size, so the runner can diff against the verified source. */
    atomic_store(&next_record, 0);
    tid = 0;
    for (unsigned size = 0; size <= 56; ++size) {
        unsigned char body[56];
        for (unsigned i = 0; i < size; ++i)
            body[i] = (unsigned char)(i * 7 + size);
        hook_send_cmd(USB_FD, 0x00010000u + size, body, size);
    }
    for (int i = 0; i < atomic_load(&next_record); ++i) {
        printf("HEX:");
        for (size_t k = 0; k < records[i].n; ++k)
            printf("%02x", records[i].b[k]);
        printf("\n");
    }
}

int main(int argc, char **argv)
{
    alarm(20);  /* a leaked lock would spin forever; fail instead of hanging */
    if (argc > 1 && !strcmp(argv[1], "cmdbytes")) {
        command_bytes();
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "identity")) {
        identity();
        return 0;
    }
    int bad = contention();
    if (bad)
        return 1;
    if (failures())
        return 1;
    if (!hook_input_entry)
        return 0;
    return input_tests() || entry_tests() ? 1 : 0;
}
