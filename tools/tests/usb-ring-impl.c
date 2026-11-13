/* Host-test wrapper for the live spool ring and continuous-session logic of the
 * real receiver source (see usb-ring.sh). */
#include "carlife_qnx_usb_capture.c"

static struct capture ring_cap;

void hook_ring_reset(unsigned start, unsigned live, unsigned continuous)
{
    ring_cap = (struct capture){0};
    ring_cap.live = live;
    ring_cap.continuous = continuous;
    ring_cap.published = ring_cap.consumed = start;
}
void *hook_writer_entry(void *unused) { (void)unused; return live_writer(&ring_cap); }
void hook_ring_finish(void) { __atomic_store_n(&ring_cap.finished, 1, __ATOMIC_RELEASE); }
int hook_ring_record(unsigned kind, const void *a, unsigned an, const void *b, unsigned bn)
{
    return live_record(&ring_cap, kind, a, an, b, bn);
}
unsigned hook_ring_failed(void) { return ring_cap.writer_failed; }
unsigned hook_ring_consumed(void) { return __atomic_load_n(&ring_cap.consumed, __ATOMIC_ACQUIRE); }
unsigned hook_ring_published(void) { return ring_cap.published; }
unsigned hook_ring_size(void) { return LIVE_RING; }

/* Feeds parameter sets, then an IDR with frames preset (preset > 1 skips the
 * first-frame setup). Returns capture_video's result; the last 40 bytes the
 * ring received (the video record: 24 header + 8 timestamp + 8 data) go to out. */
int hook_video(unsigned preset_frames, unsigned *frames_out, unsigned char *out)
{
    static const unsigned char params[] = {
        0, 0, 0, 1, 0x67, 0x42, 0x00, 0x1f, 0, 0, 0, 1, 0x68, 0xce, 0x3c, 0x80
    };
    static const unsigned char idr[] = {0, 0, 0, 1, 0x65, 0x88, 0x84, 0x21};
    ring_cap.width = 1280; ring_cap.height = 720; ring_cap.fps = 30;
    if (capture_video(&ring_cap, params, sizeof(params)) < 0)
        return -100;
    if (preset_frames > 1)
        ring_cap.frames = preset_frames;
    int rc = capture_video(&ring_cap, idr, sizeof(idr));
    *frames_out = ring_cap.frames;
    for (unsigned i = 0; i < 40; ++i)
        out[i] = live_spool[(ring_cap.published - 40 + i) & LIVE_MASK];
    return rc;
}
