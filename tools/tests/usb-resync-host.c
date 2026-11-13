/* POSIX harness: scripted USB reads go through the real receive loop. Checks that a record whose 8-byte AOA header was
 * taken by another reader (the next read starts with the inner record) is recovered, and that anything else that is not a
 * valid header still ends the session. usb-resync.sh builds and runs this. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

int hook_receive(int fd);
void hook_set_reader_thread(int on);
void hook_audio_fd(int fd, unsigned already);
int hook_audio_is_open(void);
unsigned hook_audio_limit(void);
int hook_receive_live(int fd, unsigned *frames, unsigned *waiting);
void hook_play_reset(int enable);
int hook_play_step(void);
void hook_play_close(void);
void hook_play_prebuffer(unsigned bytes);
unsigned hook_play_dry(void);
unsigned hook_play_queued(void);
int hook_play_failed(void);
int hook_play_open(void);
void hook_lat_reset(void);
unsigned hook_lat_delta(unsigned now, unsigned then);
void hook_lat_key(void);
void hook_lat_frame(unsigned bytes);
void hook_lat_shift(unsigned ms);
unsigned hook_lat_state(unsigned *windows, unsigned *logged);
void hook_play_counts(unsigned *records, unsigned *bytes, unsigned *dropped, unsigned *bad, unsigned *skipped, unsigned *errors);

enum { USB_FD = 7, MAX_CHUNKS = 8192 };
static int mock_errno;
static unsigned char *chunk[MAX_CHUNKS];
static unsigned chunk_len[MAX_CHUNKS];
static int chunks, next_chunk;
static unsigned char saved[8192];
static unsigned saved_len;
static unsigned char audio_saved[1 << 16];
static unsigned audio_len;

static char logbuf[1 << 20];
static unsigned loglen;

long test_write(int fd, const void *b, size_t n)
{
    if (fd == 9) { if (audio_len + n <= sizeof(audio_saved)) { memcpy(audio_saved + audio_len, b, n); audio_len += (unsigned)n; } }
    else if (fd == 2) { if (loglen + n < sizeof(logbuf) - 1) { memcpy(logbuf + loglen, b, n); loglen += (unsigned)n; logbuf[loglen] = 0; } }
    else if (fd == 1) { if (saved_len + n <= sizeof(saved)) { memcpy(saved + saved_len, b, n); saved_len += (unsigned)n; } }
    return (long)n;
}
long test_read(int fd, void *b, size_t n)
{
    (void)fd;
    if (next_chunk >= chunks) { mock_errno = 5; return -1; }
    const unsigned len = chunk_len[next_chunk] < n ? chunk_len[next_chunk] : (unsigned)n;
    memcpy(b, chunk[next_chunk], len);
    ++next_chunk;
    return (long)len;
}
int test_usleep(unsigned us) { (void)us; return 0; }
unsigned test_alarm(unsigned s) { (void)s; return 0; }
int test_open(const char *p, int f, ...) { (void)p; (void)f; return USB_FD; }
int test_close(int fd) { (void)fd; return 0; }
int test_devctl(int fd, int c, void *d, size_t n, int *r) { (void)fd; (void)c; (void)d; (void)n; (void)r; return 0; }
static int real_threads;
int test_pthread_create(unsigned *id, const void *a, void *(*s)(void *), void *g)
{
    (void)a;
    if (!real_threads) { (void)id; (void)s; (void)g; return 99; }
    pthread_t t;
    if (pthread_create(&t, 0, s, g)) return 99;
    pthread_detach(t);
    *id = 1;
    return 0;
}
int test_pthread_join(unsigned id, void **r) { (void)id; (void)r; return 99; }
int *__get_errno_ptr(void) { return &mock_errno; }

static void be32(unsigned char *p, unsigned v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static void add(const unsigned char *data, unsigned len)
{
    chunk[chunks] = malloc(len);
    memcpy(chunk[chunks], data, len);
    chunk_len[chunks++] = len;
}
static void outer(unsigned channel, const unsigned char *body, unsigned len)   /* a complete AOA envelope as ONE read */
{
    unsigned char *buf = malloc(8 + len);
    be32(buf, channel); be32(buf + 4, len); memcpy(buf + 8, body, len);
    add(buf, 8 + len);
    free(buf);
}
static void cmd(unsigned char *out, unsigned *len, unsigned type, const unsigned char *body, unsigned n)
{
    out[0] = n >> 8; out[1] = (unsigned char)n; out[2] = out[3] = 0; be32(out + 4, type);
    memcpy(out + 8, body, n);
    *len = 8 + n;
}

static unsigned char idr[40];
static unsigned idr_len;
static unsigned char video_rec[12 + 40];   /* inner video record: length, timestamp, type, Annex B SPS+PPS+IDR */
static unsigned video_len;

static void setup_media(void)
{
    static const unsigned char nal[] = {0, 0, 0, 1, 0x67, 0x42, 0, 0x1f, 0, 0, 0, 1, 0x68, 0xce, 0x3c, 0x80,
                                        0, 0, 0, 1, 0x65, 0x88, 0x84, 0x21};
    idr_len = sizeof(nal);
    memcpy(idr, nal, idr_len);
    be32(video_rec, idr_len); be32(video_rec + 4, 0); be32(video_rec + 8, 0x20001);
    memcpy(video_rec + 12, idr, idr_len);
    video_len = 12 + idr_len;
}

static void handshake(void)   /* the same opening as the existing capture host test: split match, then the encoder init */
{
    unsigned char c[64], zeros[12] = {0};
    unsigned n;
    static const unsigned char match[] = {8, 1};
    cmd(c, &n, 0x10002, match, 2);
    outer(1, c, 8);
    outer(4, zeros, 12);
    outer(1, c + 8, n - 8);
    static const unsigned char init[] = {8, 0x80, 10, 16, 0xd0, 5, 24, 30};
    cmd(c, &n, 0x10008, init, sizeof(init));
    outer(1, c, n);
}

static void reset(void)
{
    for (int i = 0; i < chunks; ++i) free(chunk[i]);
    chunks = next_chunk = 0; saved_len = 0; audio_len = 0; hook_play_reset(0); loglen = 0; logbuf[0] = 0; mock_errno = 0;
}

static int bad;
static void expect(int cond, const char *what) { if (!cond) { printf("FAIL: %s\n", what); bad = 1; } else printf("PASS: %s\n", what); }

int main(void)
{
    setup_media();
    unsigned char audio[12 + 3584];
    memset(audio, 0x11, sizeof(audio));
    be32(audio, 3584); be32(audio + 4, 0); be32(audio + 8, 0x00030006);

    reset(); handshake(); outer(2, video_rec, video_len);
    int rc = hook_receive(USB_FD);
    expect(rc == 1 && saved_len > 0, "normal video record is captured (control)");
    const unsigned good_len = saved_len;

    reset(); handshake(); add(audio, sizeof(audio)); outer(2, video_rec, video_len);
    rc = hook_receive(USB_FD);
    expect(rc == 1 && saved_len == good_len, "audio record without its AOA header is skipped, the session goes on");
    expect(strstr(logbuf, "without its AOA header") != 0, "  ... and it is logged");

    reset(); handshake(); add(video_rec, video_len);
    rc = hook_receive(USB_FD);
    expect(rc == 1 && saved_len == good_len, "video record without its AOA header is still used");

    reset(); handshake(); unsigned char garbage[3596]; memset(garbage, 0x5a, sizeof(garbage)); add(garbage, sizeof(garbage));
    rc = hook_receive(USB_FD);
    expect(rc < 0 && strstr(logbuf, "Invalid AOA header") != 0 && strstr(logbuf, "USB read failed") != 0,
           "garbage that is not a valid record is dropped; the session ends only because the reads ran out");
    reset(); handshake(); add(garbage, sizeof(garbage)); outer(2, video_rec, video_len);
    rc = hook_receive(USB_FD);
    expect(rc == 1 && saved_len == good_len, "  ... and the next envelope after the garbage read is captured");

    reset(); handshake(); unsigned char shortaudio[sizeof(audio) - 1]; memcpy(shortaudio, audio, sizeof(shortaudio)); add(shortaudio, sizeof(shortaudio));
    rc = hook_receive(USB_FD);
    expect(rc < 0 && saved_len == 0, "a record whose size does not match its length is not accepted");

    reset(); handshake(); unsigned char badtype[sizeof(audio)]; memcpy(badtype, audio, sizeof(badtype)); be32(badtype + 8, 0x00090001); add(badtype, sizeof(badtype));
    rc = hook_receive(USB_FD);
    expect(rc < 0 && saved_len == 0, "an unknown record type is not accepted");

    /* The factory reader took the first read of an envelope body: the inner video header is garbage, but the AOA
     * envelope boundaries are intact, so the envelope is dropped and the next one is used. */
    reset(); handshake();
    unsigned char junk[64]; memset(junk, 0x5a, sizeof(junk));
    outer(2, junk, sizeof(junk)); outer(2, video_rec, video_len);
    rc = hook_receive(USB_FD);
    expect(rc == 1 && saved_len == good_len, "garbage inner video header: envelope dropped, next video record is captured");
    expect(strstr(logbuf, "Inner VIDEO header corrupt") != 0, "  ... and it is logged");

    /* Envelope split over reads: header+first part garbage, rest of the body must be skipped, not re-parsed. */
    reset(); handshake();
    unsigned char big[600]; memset(big, 0x33, sizeof(big));
    unsigned char env[8]; be32(env, 2); be32(env + 4, sizeof(big));
    add(env, 8); add(big, 300); add(big, 300); outer(2, video_rec, video_len);
    rc = hook_receive(USB_FD);
    expect(rc == 1 && saved_len == good_len, "garbage envelope split over reads: tail skipped, session continues");


    /* Audio player: parsed channel-3 records are queued and played through the (fake) car libasound shim. */
    {
        unsigned recs, bytes, dropped, badc, skipped, errors;
        unsigned char a[12 + 3584], b[12 + 3584];
        for (unsigned i = 0; i < 3584; ++i) { a[12 + i] = (unsigned char)i; b[12 + i] = (unsigned char)(255 - i); }
        be32(a, 3584); be32(a + 4, 5); be32(a + 8, 0x00030006);
        be32(b, 3584); be32(b + 4, 6); be32(b + 8, 0x00030006);
        unsigned char hdr[8]; be32(hdr, 3); be32(hdr + 4, sizeof(a));
        const char *out = getenv("FAKE_OUT");
        unsigned char got[65536];

        /* queued, not played yet: video unaffected */
        reset(); hook_play_reset(1); handshake();
        add(hdr, 8); add(a, sizeof(a)); add(hdr, 8); add(b, sizeof(b)); add(hdr, 8); add(a, sizeof(a)); outer(2, video_rec, video_len);
        rc = hook_receive(USB_FD); hook_play_counts(&recs, &bytes, &dropped, &badc, &skipped, &errors);
        expect(rc == 1 && saved_len == good_len && recs == 3 && bytes == 3 * 3584 && hook_play_queued() == 3 * 3584 && !dropped,
               "player: three DATA records are queued as raw PCM, the video is captured as before");
        expect(!hook_play_open(), "  ... and the device is not touched before the first fragment is ready");
        setenv("FAKE_MODE", "", 1);
        int r1 = hook_play_step(), r2 = hook_play_step(), r3 = hook_play_step();
        expect(r1 == 1 && r2 == 1 && r3 == 0 && hook_play_open() && hook_play_queued() == 3 * 3584 - 2 * 4096,
               "player: two full fragments are played, the remainder waits");
        expect(strstr(logbuf, "Audio player: playing 44100 Hz stereo 16 bit") != 0 && strstr(logbuf, "card 0 device 0") != 0,
               "  ... the device was opened and the parameters were accepted (every field at the verified offset)");
        {
            FILE *f = fopen(out, "rb"); unsigned n = f ? (unsigned)fread(got, 1, sizeof(got), f) : 0; if (f) fclose(f);
            int same = n == 8192;
            for (unsigned i = 0; same && i < 3584; ++i) same = got[i] == (unsigned char)i && got[3584 + i] == (unsigned char)(255 - i);
            expect(same, "  ... and the device received exactly the PCM bodies in order (headers stripped)");
        }
        hook_play_close();
        {
            FILE *f = fopen("/tmp/fake_asound_closed", "rb");
            expect(f != 0, "player: closing the player closes the device");
            if (f) { fclose(f); remove("/tmp/fake_asound_closed"); }
        }
        /* not enabled: nothing is queued */
        reset(); hook_play_reset(0); handshake(); add(hdr, 8); add(a, sizeof(a)); outer(2, video_rec, video_len);
        rc = hook_receive(USB_FD);
        expect(rc == 1 && hook_play_queued() == 0, "player: switched off, nothing is queued");
        /* a cut-short record is dropped, not merged into the next */
        reset(); hook_play_reset(1); handshake();
        add(hdr, 8); add(a, 1000); add(hdr, 8); add(b, sizeof(b)); outer(2, video_rec, video_len);
        rc = hook_receive(USB_FD); hook_play_counts(&recs, &bytes, &dropped, &badc, &skipped, &errors);
        expect(rc == 1 && hook_play_queued() == 3584 && recs == 1, "player: a cut-short record is dropped, not merged into the next");
        /* a record split over reads */
        reset(); hook_play_reset(1); handshake();
        add(hdr, 8); add(a, 1000); add(a + 1000, 1000); add(a + 2000, sizeof(a) - 2000); outer(2, video_rec, video_len);
        rc = hook_receive(USB_FD);
        expect(rc == 1 && hook_play_queued() == 3584, "player: a record split over USB reads is reassembled");
        /* a bad length field */
        unsigned char c[sizeof(a)]; memcpy(c, a, sizeof(c)); be32(c, 100);
        reset(); hook_play_reset(1); handshake(); add(hdr, 8); add(c, sizeof(c)); outer(2, video_rec, video_len);
        rc = hook_receive(USB_FD); hook_play_counts(&recs, &bytes, &dropped, &badc, &skipped, &errors);
        expect(rc == 1 && hook_play_queued() == 0 && badc == 1, "player: a record whose length does not add up is dropped and counted");
        /* INIT decides what is played */
        unsigned char ini[12 + 8], ihdr[8]; be32(ihdr, 3); be32(ihdr + 4, sizeof(ini));
        static const unsigned char fmt44[] = {0x08, 0xc4, 0xd8, 0x02, 0x10, 0x02, 0x18, 0x10};
        static const unsigned char fmt48[] = {0x08, 0x80, 0xf7, 0x02, 0x10, 0x02, 0x18, 0x10};
        be32(ini, 8); be32(ini + 4, 0); be32(ini + 8, 0x00030001);
        memcpy(ini + 12, fmt44, 8);
        reset(); hook_play_reset(1); handshake(); add(ihdr, 8); add(ini, sizeof(ini)); add(hdr, 8); add(a, sizeof(a)); outer(2, video_rec, video_len);
        rc = hook_receive(USB_FD);
        expect(rc == 1 && hook_play_queued() == 3584 && strstr(logbuf, "Audio format: 44100 Hz, 2 channel(s), 16 bits") != 0,
               "player: INIT 44.1 kHz stereo 16 bit is logged and played");
        memcpy(ini + 12, fmt48, 8);
        reset(); hook_play_reset(1); handshake(); add(ihdr, 8); add(ini, sizeof(ini)); add(hdr, 8); add(a, sizeof(a)); outer(2, video_rec, video_len);
        rc = hook_receive(USB_FD);
        expect(rc == 1 && hook_play_queued() == 0 && strstr(logbuf, "48000 Hz") != 0 && strstr(logbuf, "not played") != 0,
               "player: INIT with another rate is logged and nothing is queued (no wrong-speed noise)");
        /* the queue is bounded: a stalled device only loses audio */
        reset(); hook_play_reset(1); handshake();
        for (int i = 0; i < 100; ++i) { add(hdr, 8); add(a, sizeof(a)); }
        outer(2, video_rec, video_len);
        rc = hook_receive(USB_FD); hook_play_counts(&recs, &bytes, &dropped, &badc, &skipped, &errors);
        expect(rc == 1 && saved_len == good_len && dropped == 27 && hook_play_queued() == 73 * 3584,
               "player: a full queue drops audio, never blocks, video unaffected");
        /* backlog: the player jumps ahead instead of playing old audio */
        r1 = hook_play_step(); hook_play_counts(&recs, &bytes, &dropped, &badc, &skipped, &errors);
        expect(r1 == 1 && skipped > 0 && hook_play_queued() <= 16384 + 4096, "player: after a stall it skips ahead to about 90 ms of audio");
        hook_play_close();
        remove("/tmp/fake_asound_closed");
        /* jitter buffer: nothing is played until enough is queued; a run-dry waits for a refill instead of playing in dribbles */
        reset(); hook_play_reset(1); hook_play_prebuffer(8192); handshake();
        add(hdr, 8); add(a, sizeof(a)); outer(2, video_rec, video_len);
        rc = hook_receive(USB_FD);
        expect(rc == 1 && hook_play_step() == 0 && !hook_play_open(), "player: below the pre-buffer nothing is played and the device stays closed");
        reset(); hook_play_reset(1); hook_play_prebuffer(8192); handshake();
        for (int i = 0; i < 3; ++i) { add(hdr, 8); add(a, sizeof(a)); }
        outer(2, video_rec, video_len);
        rc = hook_receive(USB_FD);
        r1 = hook_play_step(); r2 = hook_play_step(); r3 = hook_play_step();
        expect(rc == 1 && r1 == 1 && r2 == 1 && r3 == 0 && hook_play_dry() == 1,
               "player: once the pre-buffer is reached it plays; when the queue runs dry it stops and counts it");
        reset(); hook_play_reset(1); hook_play_prebuffer(8192); handshake();
        for (int i = 0; i < 6; ++i) { add(hdr, 8); add(a, sizeof(a)); }
        outer(2, video_rec, video_len);
        rc = hook_receive(USB_FD);
        r1 = hook_play_step(); r2 = hook_play_step(); r3 = hook_play_step();
        expect(rc == 1 && r1 == 1 && r2 == 1 && r3 == 1 && hook_play_dry() == 0, "player: with enough queued it plays on without a stop");
        hook_play_close();
        remove("/tmp/fake_asound_closed");
        /* failures: the capture goes on without sound */
        const char *modes[] = {"open", "params", "prepare"};
        for (unsigned m = 0; m < 3; ++m) {
            reset(); hook_play_reset(1); handshake(); setenv("FAKE_MODE", modes[m], 1);
            add(hdr, 8); add(a, sizeof(a)); add(hdr, 8); add(b, sizeof(b)); outer(2, video_rec, video_len);
            rc = hook_receive(USB_FD);
            int s1 = hook_play_step();
            char what[96]; snprintf(what, sizeof(what), "player: %s refused by the device: audio off, video unaffected", modes[m]);
            expect(rc == 1 && saved_len == good_len && s1 == -1 && hook_play_failed() && !hook_play_open(), what);
            expect(hook_play_step() == -1, "  ... and the player stays off");
        }
        {   /* the shim itself cannot be loaded (seen in the car: a missing dependency): logged with the loader's reason */
            const char *keep = getenv("CARLINK_ASOUND_SHIM");
            char saved_shim[256]; snprintf(saved_shim, sizeof(saved_shim), "%s", keep ? keep : "");
            setenv("CARLINK_ASOUND_SHIM", "/nonexistent/libasound.so", 1);
            reset(); hook_play_reset(1); handshake();
            add(hdr, 8); add(a, sizeof(a)); add(hdr, 8); add(b, sizeof(b)); outer(2, video_rec, video_len);
            rc = hook_receive(USB_FD);
            int s2 = hook_play_step();
            expect(rc == 1 && s2 == -1 && hook_play_failed() && strstr(logbuf, "libasound shim could not be loaded: ") != 0 &&
                   strstr(logbuf, "could not be loaded: (no reason given)") == 0,
                   "player: a shim that cannot be loaded is logged with the loader's reason, audio off, video unaffected");
            setenv("CARLINK_ASOUND_SHIM", saved_shim, 1);
        }
        reset(); hook_play_reset(1); handshake(); setenv("FAKE_MODE", "write", 1);
        add(hdr, 8); add(a, sizeof(a)); add(hdr, 8); add(b, sizeof(b)); outer(2, video_rec, video_len);
        rc = hook_receive(USB_FD);
        int last = 0;
        for (int i = 0; i < 40 && !hook_play_failed(); ++i) last = hook_play_step();
        hook_play_counts(&recs, &bytes, &dropped, &badc, &skipped, &errors);
        expect(rc == 1 && hook_play_failed() && last == -1 && errors == 21 && strstr(logbuf, "too many write errors") != 0,
               "player: repeated write errors switch the audio off after 20 re-prepares");
        remove("/tmp/fake_asound_closed");
        setenv("FAKE_MODE", "", 1);
    }
    /* Endless corruption still ends the session. */
    reset(); handshake();
    for (int i = 0; i < 210; ++i) outer(2, junk, sizeof(junk));
    rc = hook_receive(USB_FD);
    expect(rc < 0, "persistent corruption (over 200 dropped envelopes) still ends the session");

    /* A video record larger than one read whose AOA header was taken: its first read starts with the inner header. */
    {
        enum { BIG = 3000 };
        unsigned char big_rec[12 + BIG];
        memcpy(big_rec + 12, idr, idr_len);
        memset(big_rec + 12 + idr_len, 0x77, BIG - idr_len);
        be32(big_rec, BIG); be32(big_rec + 4, 0); be32(big_rec + 8, 0x20001);
        reset(); handshake();
        add(big_rec, 1000); add(big_rec + 1000, 1000); add(big_rec + 2000, sizeof(big_rec) - 2000);
        rc = hook_receive(USB_FD);
        expect(rc == 1 && saved_len > 0 && strstr(logbuf, "video record start") != 0 && strstr(logbuf, "Invalid") == 0,
               "large video record whose AOA header was taken: continued and captured");
        reset(); handshake();
        add(big_rec, 1000); add(big_rec + 1000, 1000); add(big_rec + 2000, sizeof(big_rec) - 2000); outer(2, video_rec, video_len);
        unsigned long long before = 0; (void)before;
        rc = hook_receive(USB_FD);
        expect(rc == 1 && strstr(logbuf, "Invalid") == 0, "  ... and the next envelope after it parses normally");
        reset(); handshake();
        add(big_rec, 1000); add(big_rec + 1000, 1000);
        unsigned char tail_hdr[8]; be32(tail_hdr, 2); be32(tail_hdr + 4, video_len); add(tail_hdr, 8); add(video_rec, video_len);
        rc = hook_receive(USB_FD);
        expect(strstr(logbuf, "Invalid") == 0, "  ... and a record cut short (next header arrives) is dropped, not fatal");
        /* anything that is not a plausible video start is still fatal */
        reset(); handshake();
        unsigned char bad_rec[1000]; memset(bad_rec, 0x5a, sizeof(bad_rec)); be32(bad_rec, BIG); be32(bad_rec + 8, 0x20009); add(bad_rec, sizeof(bad_rec));
        rc = hook_receive(USB_FD);
        expect(rc < 0 && saved_len == 0 && strstr(logbuf, "Invalid AOA header") != 0,
               "an unknown record type at a read start is dropped, never captured");
        /* The car (RX #473): header of an audio envelope, then ONE read holding the audio body and, behind it, the middle of
         * a video body whose header was stolen. Nothing of the video part is usable; the next IDR must resume the picture. */
        reset(); handshake();
        {
            unsigned char mixed[3596 + 4000], hdr[8];
            memset(mixed, 0x11, sizeof(mixed)); memcpy(mixed, audio, 3596);
            memset(mixed + 3596, 0x6f, 4000);
            be32(hdr, 3); be32(hdr + 4, 3596);
            add(hdr, 8); add(mixed, sizeof(mixed)); outer(2, video_rec, video_len);
            rc = hook_receive(USB_FD);
            expect(rc == 1 && saved_len == good_len && strstr(logbuf, "dropping the rest of this read") != 0,
                   "audio body followed by video-body middle in one read: dropped, picture resumes at the next IDR");
        }
        /* endless headerless reads still end the session */
        reset(); handshake();
        {
            unsigned char mid[4000]; memset(mid, 0x6f, sizeof(mid));
            for (int i = 0; i < 210; ++i) add(mid, sizeof(mid));
            rc = hook_receive(USB_FD);
            expect(rc < 0 && strstr(logbuf, "USB read failed") == 0, "over 200 headerless reads in a row still end the session");
        }
        /* several such reads in a row, then recovery */
        reset(); handshake();
        {
            unsigned char mid[4000]; memset(mid, 0x6f, sizeof(mid));
            for (int i = 0; i < 20; ++i) add(mid, sizeof(mid));
            outer(2, video_rec, video_len);
            rc = hook_receive(USB_FD);
            expect(rc == 1 && saved_len == good_len, "a run of headerless video-body reads is dropped, the next envelope resumes");
        }
    }

    /* Seen in the car: a command envelope's header arrived but its body was taken, then an audio record without its header. */
    reset(); handshake();
    { unsigned char h[8]; be32(h, 1); be32(h + 4, 16); add(h, 8); }
    add(audio, sizeof(audio)); outer(2, video_rec, video_len);
    rc = hook_receive(USB_FD);
    expect(rc == 1 && saved_len == good_len && strstr(logbuf, "Invalid") == 0 && strstr(logbuf, "pending envelope") != 0,
           "pending command envelope whose body was taken, then a headerless audio record: dropped, session goes on");
    /* An audio body that its own header announced is not mistaken for a lost one. */
    reset(); handshake();
    { unsigned char h[8]; be32(h, 3); be32(h + 4, sizeof(audio)); add(h, 8); }
    add(audio, sizeof(audio)); outer(2, video_rec, video_len);
    rc = hook_receive(USB_FD);
    expect(rc == 1 && saved_len == good_len && strstr(logbuf, "pending envelope") == 0 && strstr(logbuf, "without its AOA header") == 0,
           "announced audio body (header + body as two reads) is handled normally, not as a loss");
    /* An announced large video body likewise. */
    {
        unsigned char big_rec2[12 + 3000];
        memcpy(big_rec2 + 12, idr, idr_len); memset(big_rec2 + 12 + idr_len, 0x77, 3000 - idr_len);
        be32(big_rec2, 3000); be32(big_rec2 + 4, 0); be32(big_rec2 + 8, 0x20001);
        reset(); handshake();
        unsigned char h[8]; be32(h, 2); be32(h + 4, sizeof(big_rec2)); add(h, 8);
        add(big_rec2, 1000); add(big_rec2 + 1000, 1000); add(big_rec2 + 2000, sizeof(big_rec2) - 2000);
        rc = hook_receive(USB_FD);
        expect(rc == 1 && saved_len > 0 && strstr(logbuf, "pending envelope") == 0 && strstr(logbuf, "without its AOA header") == 0,
               "announced large video record split over reads is handled normally, not as a loss");
    }

    /* Audio dump (diagnostic): channel-3 envelopes are copied out whole; the video result does not change. */
    reset(); handshake(); hook_audio_fd(9, 0);
    outer(3, audio, sizeof(audio)); outer(3, audio, sizeof(audio)); outer(2, video_rec, video_len);
    rc = hook_receive(USB_FD);
    expect(rc < 0 || rc == 1, "audio dump: session runs");
    expect(audio_len == 2 * (8 + sizeof(audio)) && audio_saved[3] == 3 && memcmp(audio_saved + 8, audio, sizeof(audio)) == 0,
           "audio dump: two channel-3 envelopes (header + body) were copied");
    expect(saved_len == good_len, "audio dump: the video record is still captured");
    reset(); handshake(); hook_audio_fd(-1, 0);
    outer(3, audio, sizeof(audio)); outer(2, video_rec, video_len);
    rc = hook_receive(USB_FD);
    expect(audio_len == 0 && saved_len == good_len, "audio dump off: nothing is written");
    reset(); handshake(); hook_audio_fd(9, hook_audio_limit() - 100);
    outer(3, audio, sizeof(audio)); outer(2, video_rec, video_len);
    rc = hook_receive(USB_FD);
    expect(audio_len <= 8 && !hook_audio_is_open() && saved_len == good_len, "audio dump: stops at the size limit and the video goes on");
    hook_audio_fd(-1, 0);

    /* Stress: a long live stream (IDR with SPS+PPS every 30 frames) where some envelopes lose the start of their body
     * (garbage inner header) or are cut off by a standalone next header. The session must never end on its own, and
     * must lose only frames up to the next IDR. */
    {
        enum { FRAMES = 1500, IDR_EVERY = 30 };
        static const unsigned char sps[] = {0, 0, 0, 1, 0x67, 0x42, 0, 0x1f}, pps[] = {0, 0, 0, 1, 0x68, 0xce, 0x3c, 0x80};
        for (int threaded = 0; threaded < 2; ++threaded)
        for (int mode = 0; mode < 3; ++mode) {
            real_threads = threaded; hook_set_reader_thread(threaded);   /* 0 clean, 1 garbage body start, 2 body replaced by a lost read */
            reset(); handshake();
            unsigned expect_frames = 0, since_idr = 0, dropped_until_idr = 0;
            for (int f = 0; f < FRAMES; ++f) {
                const int is_idr = f % IDR_EVERY == 0;
                unsigned char rec[12 + 600], payload[600];
                unsigned plen = 0;
                if (is_idr) { memcpy(payload, sps, 8); memcpy(payload + 8, pps, 8); plen = 16; }
                static const unsigned char slice[] = {0, 0, 0, 1};
                memcpy(payload + plen, slice, 4); plen += 4;
                payload[plen++] = is_idr ? 0x65 : 0x41;
                memset(payload + plen, 0x77 + (f & 7), 200); plen += 200;
                be32(rec, plen); be32(rec + 4, (unsigned)f); be32(rec + 8, 0x20001);
                memcpy(rec + 12, payload, plen);
                const unsigned rlen = 12 + plen;
                const int hit = mode && f > 2 && f % 11 == 0;
                if (hit) {
                    memset(rec, 0x5a, 12);   /* the first read of the body went to another reader */
                    if (mode == 2) {         /* envelope header alone, body lost; the next envelope header follows */
                        unsigned char h[8]; be32(h, 2); be32(h + 4, rlen); add(h, 8);
                        dropped_until_idr = 1;
                        continue;
                    }
                    dropped_until_idr = 1;
                }
                if (is_idr && !hit) dropped_until_idr = 0;
                if (!dropped_until_idr) ++expect_frames;
                else ++since_idr;
                { unsigned char h[8]; be32(h, 2); be32(h + 4, rlen); add(h, 8); add(rec, rlen); }   /* header and body are separate reads */
            }
            unsigned frames = 0, waiting = 0;
            const int r = hook_receive_live(USB_FD, &frames, &waiting);
            char what[96];
            snprintf(what, sizeof(what), "stress mode %d%s: no fatal stream error, ended only by running out of reads", mode, threaded ? " (reader thread)" : "");
            expect(r < 0 && strstr(logbuf, "USB read failed") != 0 && strstr(logbuf, "Invalid AOA") == 0 &&
                   strstr(logbuf, "Previous RX") == 0, what);
            snprintf(what, sizeof(what), "stress mode %d%s: frames queued %u, expected %u", mode, threaded ? " (reader thread)" : "", frames, expect_frames);
            expect(frames == expect_frames, what); if (frames != expect_frames) { char *p = logbuf; int n = 0; while ((p = strstr(p, "Inner")) && n < 3) { printf("   %.100s\n", p); p += 10; ++n; } printf("   tail: %.300s\n", loglen > 300 ? logbuf + loglen - 300 : logbuf); }
        }
    }

        {   /* key -> video frame latency log */
        unsigned windows = 0, logged = 0;
        loglen = 0; logbuf[0] = 0; hook_lat_reset();
        hook_lat_frame(500);
        expect(strstr(logbuf, "Latency") == 0, "latency: no frame logged before any key");
        hook_lat_key();
        expect(strstr(logbuf, "Latency: key written to the phone at ") != 0 && strstr(logbuf, " UTC") != 0, "latency: the key is logged with a millisecond time");
        { const char *at = strstr(logbuf, "phone at "); expect(at && at[9 + 2] == ':' && at[9 + 5] == ':' && at[9 + 8] == '.', "latency: time is HH:MM:SS.mmm"); }
        hook_lat_frame(1234);
        expect(strstr(logbuf, "Latency: video frame 1234 bytes, ") != 0 && strstr(logbuf, " ms after the key") != 0, "latency: a frame after the key is logged with size and delay");
        hook_lat_key();
        hook_lat_state(&windows, &logged);
        expect(windows == 1, "latency: a second key inside an open window does not open another");
        for (int i = 0; i < 20; ++i) hook_lat_frame(100);
        hook_lat_state(&windows, &logged);
        expect(logged == 12, "latency: at most 12 frames per key are logged");
        expect(hook_lat_state(&windows, &logged) == 0, "latency: the window closes after 12 frames");
        hook_lat_key(); loglen = 0; logbuf[0] = 0;
        hook_lat_shift(700); hook_lat_frame(100);
        expect(strstr(logbuf, "video frame") == 0 && hook_lat_state(&windows, &logged) == 0, "latency: a frame 700 ms after the key closes the window unlogged");
        expect(hook_lat_delta(30, 86399980u) == 50 && hook_lat_delta(1500, 1000) == 500, "latency: delay across midnight UTC is still right");
        hook_lat_reset();
        for (int k = 0; k < 50; ++k) { hook_lat_key(); for (int i = 0; i < 13; ++i) hook_lat_frame(1); }
        hook_lat_state(&windows, &logged);
        expect(windows == 40, "latency: logging stops after 40 keys");
        hook_lat_reset();
    }

    real_threads = 0; hook_set_reader_thread(0);
    return bad;
}
