/* Host-test wrapper: compiles the real QNX receiver source with its libc I/O renamed to the harness mocks (see
 * usb-resync.sh) and exposes the receive loop. */
#include "carlife_qnx_usb_capture.c"

/* Runs the real receive() on a non-live capture: it returns 1 once the first IDR has been saved, <0 on a fatal error. */
int hook_receive(int fd)
{
    struct capture capture = {0};
    capture.live = 0;
    return receive(fd, &capture);
}

/* Live (visible) mode, continuous: returns the receive() result; frames queued and recoveries are reported back. */
int hook_receive_live(int fd, unsigned *frames, unsigned *waiting)
{
    struct capture capture = {0};
    capture.live = 2;
    capture.continuous = 1;
    const int rc = receive(fd, &capture);
    *frames = capture.frames;
    *waiting = capture.waiting_for_idr;
    return rc;
}

void hook_audio_fd(int fd, unsigned already) { audio_dump_fd = fd; audio_dump_bytes = already; }
int hook_audio_is_open(void) { return audio_dump_fd >= 0; }
unsigned hook_audio_limit(void) { return AUDIO_DUMP_MAX; }

void hook_set_reader_thread(int on) { use_rx_thread = on; }

/* Audio player hooks: the harness drives the playback step itself (no thread) and reads the counters. */
void hook_play_reset(int enable)
{
    play_enabled = enable; play_failed = 0; play_pcm = 0; play_head = play_tail = 0;
    play_skipped = play_writes = play_errors = play_in_row = 0;
    play_prebuffer = 0; play_primed = 0; play_dry = 0; play_empty_cur = 0; play_empty_max = 0; play_reprime = 0; play_wait_ticks = 0; play_events_logged = 0; play_lead_min = 0xffffffffu; play_lead_max = 0;
    audio_env_len = audio_env_used = 0;
    audio_rate = 44100; audio_channels = 2; audio_bits = 16;
    pcm_records = pcm_bytes = pcm_dropped = pcm_bad = 0;
}
int hook_play_step(void) { return play_step(); }
void hook_play_close(void) { play_close(); }
unsigned hook_play_queued(void) { return play_head - play_tail; }
int hook_play_failed(void) { return play_failed; }
int hook_play_open(void) { return play_pcm != 0; }
void hook_play_counts(unsigned *records, unsigned *bytes, unsigned *dropped, unsigned *bad, unsigned *skipped, unsigned *errors)
{
    *records = pcm_records; *bytes = pcm_bytes; *dropped = pcm_dropped; *bad = pcm_bad; *skipped = play_skipped; *errors = play_errors;
}
void hook_play_prebuffer(unsigned bytes) { play_prebuffer = bytes; }
unsigned hook_play_dry(void) { return play_dry; }

/* Latency log hooks: open a window as the input thread does after a key, then report frames as the receive loop does. */
void hook_lat_reset(void) { lat_active = 0; lat_windows = 0; lat_frames_logged = 0; lat_key_ms = 0; }
void hook_lat_key(void) { lat_key(); }
void hook_lat_frame(unsigned bytes) { lat_frame(bytes); }
void hook_lat_shift(unsigned ms) { lat_key_ms = lat_key_ms >= ms ? lat_key_ms - ms : lat_key_ms + 86400000u - ms; }
unsigned hook_lat_state(unsigned *windows, unsigned *logged) { *windows = lat_windows; *logged = lat_frames_logged; return lat_active; }
unsigned hook_lat_delta(unsigned now, unsigned then) { return lat_delta(now, then); }
