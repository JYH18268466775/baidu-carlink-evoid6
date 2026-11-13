/* One-shot, plaintext CarLife USB receiver for the supervised QNX bench. */
#include "carlife_qnx_short_replay.h"

typedef __SIZE_TYPE__ size_t;
typedef __PTRDIFF_TYPE__ ssize_t;

int open(const char *, int, ...);
int close(int);
int devctl(int, int, void *, size_t, int *);
ssize_t read(int, void *, size_t);
ssize_t write(int, const void *, size_t);
unsigned alarm(unsigned);
char *getenv(const char *);
int usleep(unsigned);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, size_t);
int snprintf(char *, size_t, const char *, ...);
void *memcpy(void *, const void *, size_t);
int memcmp(const void *, const void *, size_t);
int *__get_errno_ptr(void);
int pthread_create(unsigned *, const void *, void *(*)(void *), void *);
int pthread_join(unsigned, void **);
long time(long *);
int clock_gettime(int, void *);
int poll(void *, unsigned, int);

enum {
    AOA_GET_MODE = 0x40040501,
    AOA_GET_PROTOCOL = 0x40040504,
    USB_BUFFER_SIZE = 65536,
    MAX_VIDEO_SIZE = 2 * 1024 * 1024,
    VERSION_MATCH = 0x00010002,
    VIDEO_INIT_DONE = 0x00010008,
    MD_AUTH_RESULT = 0x0001004b,
    FEATURE_REQUEST = 0x00010051,
    RSA_KEY_REQUEST = 0x0001006a,
    VIDEO_DATA = 0x00020001,
    VIDEO_HEARTBEAT = 0x00020002
};
#define AOA_CONFIG_SWITCH 0xc6040503u

/* Layout and devctls from the matching NBT-AAD usb_aoap.hpp.
 * Accessory identity from Baidu's AOAHostSetup.java. */
static const struct {
    int protocol;
    char manufacturer[256], model[256], description[256];
    char version[256], url[256], serial[256];
} identity = {1, "Baidu", "CarLife", "Baidu CarLife", "1.0.0",
              "http://carlife.baidu.com/", "0720SerialNo."};
_Static_assert(sizeof(identity) == 1540, "aoap-switch payload size");

static void log_line(const char *message)
{
    char line[160];
    int count = snprintf(line, sizeof(line), "%s\n", message);
    if (count > 0 && (size_t)count < sizeof(line))
        (void)write(2, line, (size_t)count);
}

static int failure(const char *stage, int code)
{
    char line[160];
    snprintf(line, sizeof(line), "%s: %d", stage, code);
    log_line(line);
    return -1;
}

static int device_path(const char *path, const char *prefix, size_t length)
{
    if (strncmp(path, prefix, length) != 0)
        return 0;
    path += length;
    if (!*path)
        return 0;
    for (; *path; ++path)
        if (*path < '0' || *path > '9')
            return 0;
    return 1;
}

static int switch_phone(int fd)
{
    int mode = -1;
    int rc = devctl(fd, AOA_GET_MODE, &mode, sizeof(mode), 0);
    if (rc != 0 || mode != 0)
        return failure("AOA mode unavailable or another client already switched",
                       rc ? rc : mode);
    int protocol = 0;
    rc = devctl(fd, AOA_GET_PROTOCOL, &protocol, sizeof(protocol), 0);
    if (rc != 0)
        return failure("AOA_GET_PROTOCOL", rc);
    if (protocol != 1 && protocol != 2)
        return failure("Unsupported USB AOA version", protocol);

    unsigned char payload[sizeof(identity)];
    memcpy(payload, &identity, sizeof(payload));
    rc = devctl(fd, (int)AOA_CONFIG_SWITCH, payload, sizeof(payload), 0);
    if (rc != 0)
        return failure("AOA_CONFIG_SWITCH", rc);
    log_line("AOA switch requested with Baidu CarLife identity");
    return 0;
}

static unsigned get_be32(const unsigned char *p)
{
    return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 |
           (unsigned)p[2] << 8 | p[3];
}

static void put_be32(unsigned char *p, unsigned value)
{
    p[0] = (unsigned char)(value >> 24);
    p[1] = (unsigned char)(value >> 16);
    p[2] = (unsigned char)(value >> 8);
    p[3] = (unsigned char)value;
}

/* Serializes USB bulk-OUT records. A record is an AOA outer header and a body
 * sent as two separate transfers, so no other writer may land between them.
 * Atomic builtins avoid importing a mutex the head unit's libc stub lacks. */
static int usb_write_lock;
/* Set under the lock just before the USB descriptor is closed, so a late
 * writer cannot touch a closed or reused descriptor. */
static int usb_closed;
/* Wi-Fi mode (--stream-wifi): one TCP socket per CarLife channel instead of the AOA envelopes; index = AOA channel 1..6. */
enum { WIFI_CHANNELS = 6, WIFI_LOCAL_BASE = 50400 };
static int wifi_mode;
static int wifi_fd[WIFI_CHANNELS + 1] = {-1, -1, -1, -1, -1, -1, -1};
/* Optional (CARLINK_READ_THREAD=1): a dedicated thread keeps a USB read pending while the main loop parses and writes, so
 * the factory reader thread has less opportunity to take a transfer. The main loop consumes the transfers in order. */
enum { RX_SLOTS = 48 };
static struct rx_slot { unsigned char data[USB_BUFFER_SIZE]; int count, err; } rx_slot[RX_SLOTS];
static unsigned rx_head, rx_tail, rx_fd_value;
static int use_rx_thread;
/* Diagnostic, off unless CARLINK_AUDIO_DUMP=/dev/shmem/NAME names an existing empty file: the raw channel-3 (media audio)
 * AOA envelopes are written there, at most AUDIO_DUMP_MAX bytes, so the format can be read with src/carlife_media_pcm.cjs on
 * a PC. Nothing is played and the video path does not depend on it. */
enum { AUDIO_DUMP_MAX = 6 * 1024 * 1024 };
static int audio_dump_fd = -1;
static unsigned audio_dump_bytes;

static void audio_dump(const void *data, unsigned size)
{
    if (audio_dump_fd < 0)
        return;
    if (size > AUDIO_DUMP_MAX - audio_dump_bytes || write(audio_dump_fd, data, size) != (int)size) {
        close(audio_dump_fd);
        audio_dump_fd = -1;
        return;
    }
    audio_dump_bytes += size;
}

/* Optional (CARLINK_AUDIO_PLAY=1): play the phone's media audio on the car. Every channel-3 AOA envelope carries one inner
 * record (length, timestamp, type, body); DATA bodies (type 0x00030006) are queued as raw PCM in a ring buffer and a playback
 * thread writes them to the car's browser audio device through the head unit's own libasound shim, the way the upstream
 * daemon does. INIT (0x00030001) carries the format as a protobuf (1 = rate, 2 = channels, 3 = bits); only 44100 Hz stereo
 * 16 bit (what the phone sends, and what that device takes) is played, anything else is logged and dropped. The queue never
 * blocks the receiver: when it is full the audio is dropped. Records that do not add up are dropped. The device and its
 * audio route (audioconn, started by the supervisor) are only touched when audio actually arrives. */
void *dlopen(const char *, int);
void *dlsym(void *, const char *);
int dlclose(void *);
const char *dlerror(void);
enum { AUDIO_ENV_MAX = 16384, PLAY_RING = 1 << 18, PLAY_FRAG = 4096, PLAY_FRAGS = 5, PLAY_PARAMS = 1024,
       PLAY_MAX_LEAD = 65536, PLAY_KEEP_LEAD = 16384, PLAY_MAX_ERRORS = 20 };
static unsigned char audio_env[AUDIO_ENV_MAX];
static unsigned audio_env_len, audio_env_used;
static unsigned audio_rate = 44100, audio_channels = 2, audio_bits = 16;
static unsigned pcm_records, pcm_bytes, pcm_dropped, pcm_bad;
static int play_enabled, play_stop, play_failed;
static unsigned play_tid, play_started;
static unsigned char play_ring[PLAY_RING];
static unsigned play_head, play_tail, play_skipped, play_writes, play_errors, play_in_row;
/* Jitter buffer: playback starts once play_prebuffer bytes are queued (default 600 ms; the device itself swallows about 115 ms, 150 ms was
 * too little and made the queue run dry six times a second) and, if the queue ever runs dry, waits
 * for it to fill again; a stall of the receiver (the panel hand-over at MENU took about that long) is then not heard as a
 * glitch. CARLINK_AUDIO_PREBUFFER_MS changes it (0 = off). */
static unsigned play_prebuffer = 105600, play_reprime = 52800, play_primed, play_dry, play_lead_min = 0xffffffffu, play_lead_max,
    play_empty_cur, play_empty_max, play_wait_ticks, play_events_logged;
/* After the queue ran dry it plays on again at play_reprime bytes (half of the first prebuffer): waiting for the whole
 * prebuffer again made every stall a half second longer. Each of the first 60 dry spells is logged with its wall clock
 * time (UTC, like the supervisor's log) so it can be matched with what was done at that moment. */
/* Wall clock (UTC, like the supervisor's log) to the millisecond. timespec is two longs on the head unit (32-bit ARM). */
static unsigned now_ms_of_day(void)
{
    long ts[2] = {0, 0};
    if (clock_gettime(0, ts) != 0) /* CLOCK_REALTIME */
        return 0;
    return ((unsigned)ts[0] % 86400u) * 1000u + (unsigned)ts[1] / 1000000u;
}
static void ms_stamp(unsigned ms, char *out) /* out: at least 13 bytes */
{
    snprintf(out, 13, "%02u:%02u:%02u.%03u", ms / 3600000u, ms / 60000u % 60u, ms / 1000u % 60u, ms % 1000u);
}
static void play_stamp(char *out)
{
    ms_stamp(now_ms_of_day(), out);
}
/* Where the delay of a key goes: when a key reaches the phone, the arrival time and size of the next video frames are logged
 * (the first LAT_FRAMES within LAT_WINDOW_MS of the key, for at most LAT_WINDOWS keys), so the phone's reaction can be told
 * apart from the head unit's own display delay. The input thread opens a window, the receive loop reads it. */
enum { LAT_WINDOW_MS = 600, LAT_FRAMES = 12, LAT_WINDOWS = 40 };
static unsigned lat_key_ms, lat_active, lat_windows, lat_frames_logged;
static void lat_key(void)
{
    if (__atomic_load_n(&lat_active, __ATOMIC_ACQUIRE) || lat_windows >= LAT_WINDOWS)
        return;
    char line[64], when[16];
    ++lat_windows;
    lat_frames_logged = 0;
    lat_key_ms = now_ms_of_day();
    __atomic_store_n(&lat_active, 1, __ATOMIC_RELEASE);
    ms_stamp(lat_key_ms, when);
    snprintf(line, sizeof(line), "Latency: key written to the phone at %s UTC", when);
    log_line(line);
}
static unsigned lat_delta(unsigned now, unsigned then) /* both ms of the UTC day; the key may be before midnight */
{
    return now >= then ? now - then : now + 86400000u - then;
}
static void lat_frame(unsigned bytes)
{
    if (!__atomic_load_n(&lat_active, __ATOMIC_ACQUIRE))
        return;
    const unsigned now = now_ms_of_day();
    const unsigned delta = lat_delta(now, lat_key_ms);
    if (delta > LAT_WINDOW_MS || lat_frames_logged >= LAT_FRAMES) {
        __atomic_store_n(&lat_active, 0, __ATOMIC_RELEASE);
        return;
    }
    char line[80];
    ++lat_frames_logged;
    snprintf(line, sizeof(line), "Latency: video frame %u bytes, %u ms after the key", bytes, delta);
    log_line(line);
}
static void *play_pcm;
static int (*asound_open)(void **, int *, int *, int);
static int (*asound_params)(void *, void *);
static int (*asound_prepare)(void *, int);
static int (*asound_write)(void *, const void *, unsigned);
static int (*asound_close)(void *);
static int (*asound_flush)(void *, int);

static void audio_pcm_log(void)
{
    char line[140];
    snprintf(line, sizeof(line), "Audio: %u PCM records, %u bytes queued, %u dropped (queue full), %u bad; "
             "player: %u writes, %u errors, %u skipped bytes, ran dry %u times",
             pcm_records, pcm_bytes, pcm_dropped, pcm_bad, play_writes, play_errors, play_skipped, play_dry);
    log_line(line);
}

static void audio_begin(unsigned envelope_length)
{
    audio_env_used = 0;
    audio_env_len = envelope_length >= 12 && envelope_length <= AUDIO_ENV_MAX ? envelope_length : 0;
}

static unsigned varint(const unsigned char *p, unsigned n, unsigned *used)
{
    unsigned value = 0, shift = 0, i = 0;
    for (; i < n && i < 5; ++i) {
        value |= (unsigned)(p[i] & 0x7f) << shift;
        shift += 7;
        if (!(p[i] & 0x80)) {
            *used = i + 1;
            return value;
        }
    }
    *used = 0;
    return 0;
}

static void play_put(const unsigned char *data, unsigned size)
{
    const unsigned head = __atomic_load_n(&play_head, __ATOMIC_RELAXED);
    if (size > PLAY_RING - (head - __atomic_load_n(&play_tail, __ATOMIC_ACQUIRE))) {
        ++pcm_dropped;
        return;
    }
    for (unsigned i = 0; i < size; ++i)
        play_ring[(head + i) % PLAY_RING] = data[i];
    __atomic_store_n(&play_head, head + size, __ATOMIC_RELEASE);
    pcm_bytes += size;
}

static void audio_record(void)
{
    const unsigned body = get_be32(audio_env), type = get_be32(audio_env + 8);
    if (body != audio_env_len - 12) {
        ++pcm_bad;
        return;
    }
    const unsigned char *data = audio_env + 12;
    if (type == 0x00030001) {
        unsigned rate = 0, channels = 0, bits = 0;
        for (unsigned i = 0; i < body;) {
            const unsigned tag = data[i++], field = tag >> 3;
            unsigned used = 0;
            if ((tag & 7) != 0)
                break;
            const unsigned value = varint(data + i, body - i, &used);
            if (!used)
                break;
            i += used;
            if (field == 1) rate = value;
            else if (field == 2) channels = value;
            else if (field == 3) bits = value;
        }
        if (rate && channels && bits) {
            char line[100];
            audio_rate = rate; audio_channels = channels; audio_bits = bits;
            snprintf(line, sizeof(line), "Audio format: %u Hz, %u channel(s), %u bits%s", rate, channels, bits,
                     rate == 44100 && channels == 2 && bits == 16 ? "" : " (not played)");
            log_line(line);
        }
        return;
    }
    if (type != 0x00030006 || !body || audio_rate != 44100 || audio_channels != 2 || audio_bits != 16)
        return;
    ++pcm_records;
    if (play_enabled && !play_failed)
        play_put(data, body);
}

static void audio_collect(const unsigned char *data, unsigned size)
{
    if (!audio_env_len || size > audio_env_len - audio_env_used) {
        audio_env_len = 0;
        return;
    }
    memcpy(audio_env + audio_env_used, data, size);
    audio_env_used += size;
    if (audio_env_used == audio_env_len) {
        audio_record();
        audio_env_len = 0;
    }
}

/* The shim hands the parameters to the car's A2I client, which refuses anything but a power-of-two fragment of 32+ bytes.
 * snd_pcm_channel_params_t is filled by byte offset, each one read off the head unit's own libasound (snd_pcm_plugin_params):
 * 0 channel, 4 mode, 0x18 format.interleave, 0x1c format.format, 0x20 rate, 0x24 voices, 0x1d8 start_mode, 0x1dc stop_mode,
 * 0x1e8 frag_size, 0x1ec frags_min, 0x1f0 frags_max. The rest stays zero, as the QNX header demands. */
static void put32(unsigned char *base, unsigned offset, unsigned value)
{
    base[offset] = (unsigned char)value;
    base[offset + 1] = (unsigned char)(value >> 8);
    base[offset + 2] = (unsigned char)(value >> 16);
    base[offset + 3] = (unsigned char)(value >> 24);
}

static int play_open(void)
{
    /* The shim needs liba2i_client.so, which lives in /opt/mm/lib, outside our library path: load it first by its full path
     * (global, so the shim's dependency on it is found). Not fatal here: the shim's own load below reports what is missing. */
    const char *a2i = getenv("CARLINK_A2I_LIB");
    if (!dlopen(a2i && *a2i ? a2i : "/opt/mm/lib/liba2i_client.so", 0x102)) {
        const char *why = dlerror();
        char note[200];
        snprintf(note, sizeof(note), "Audio player: liba2i_client.so was not preloaded: %s", why ? why : "(no reason given)");
        log_line(note);
    }
    const char *shim = getenv("CARLINK_ASOUND_SHIM");
    void *lib = dlopen(shim && *shim ? shim : "/opt/mm/lib/libasound/libasound.so", 2);
    if (!lib) {
        const char *why = dlerror();
        char line[200];
        snprintf(line, sizeof(line), "Audio player: libasound shim could not be loaded: %s", why ? why : "(no reason given)");
        log_line(line);
        return -1;
    }
    asound_open = (int (*)(void **, int *, int *, int))dlsym(lib, "snd_pcm_open_preferred");
    asound_params = (int (*)(void *, void *))dlsym(lib, "snd_pcm_plugin_params");
    asound_prepare = (int (*)(void *, int))dlsym(lib, "snd_pcm_plugin_prepare");
    asound_write = (int (*)(void *, const void *, unsigned))dlsym(lib, "snd_pcm_plugin_write");
    asound_close = (int (*)(void *))dlsym(lib, "snd_pcm_close");
    asound_flush = (int (*)(void *, int))dlsym(lib, "snd_pcm_plugin_flush");
    if (!asound_open || !asound_params || !asound_prepare || !asound_write || !asound_close) {
        log_line("Audio player: the shim lacks a needed function");
        return -1;
    }
    void *pcm = 0;
    int card = -1, device = -1;
    int rc = asound_open(&pcm, &card, &device, 1); /* SND_PCM_OPEN_PLAYBACK */
    char line[120];
    if (rc < 0 || !pcm) {
        snprintf(line, sizeof(line), "Audio player: PCM open failed rc=%d", rc);
        log_line(line);
        return -1;
    }
    snprintf(line, sizeof(line), "Audio player: PCM open card %d device %d", card, device);
    log_line(line);
    static unsigned char params[PLAY_PARAMS] __attribute__((aligned(8)));
    for (unsigned i = 0; i < sizeof(params); ++i)
        params[i] = 0;
    put32(params, 0x00, 0);      /* SND_PCM_CHANNEL_PLAYBACK */
    put32(params, 0x04, 1);      /* SND_PCM_MODE_BLOCK */
    put32(params, 0x18, 1);      /* interleaved */
    put32(params, 0x1c, 4);      /* SND_PCM_SFMT_S16_LE */
    put32(params, 0x20, 44100);
    put32(params, 0x24, 2);
    put32(params, 0x1d8, 2);     /* SND_PCM_START_FULL */
    put32(params, 0x1dc, 1);     /* SND_PCM_STOP_STOP */
    put32(params, 0x1e8, PLAY_FRAG);
    put32(params, 0x1ec, 1);
    put32(params, 0x1f0, PLAY_FRAGS);
    rc = asound_params(pcm, params);
    if (rc < 0) {
        snprintf(line, sizeof(line), "Audio player: snd_pcm_plugin_params failed rc=%d (see sloginfo: check_parameter)", rc);
        log_line(line);
        asound_close(pcm);
        return -1;
    }
    rc = asound_prepare(pcm, 0); /* SND_PCM_CHANNEL_PLAYBACK */
    if (rc < 0) {
        snprintf(line, sizeof(line), "Audio player: snd_pcm_plugin_prepare failed rc=%d", rc);
        log_line(line);
        asound_close(pcm);
        return -1;
    }
    play_pcm = pcm;
    log_line("Audio player: playing 44100 Hz stereo 16 bit");
    return 0;
}

static void play_close(void)
{
    if (!play_pcm)
        return;
    if (asound_flush)
        asound_flush(play_pcm, 0);
    asound_close(play_pcm);
    play_pcm = 0;
    log_line("Audio player: PCM closed");
}

/* One step of the playback thread: returns 1 when a fragment was played, 0 when there was nothing to do, -1 once the
 * player has given up (the capture goes on without sound). */
static int play_step(void)
{
    if (play_failed)
        return -1;
    const unsigned head = __atomic_load_n(&play_head, __ATOMIC_ACQUIRE);
    unsigned tail = __atomic_load_n(&play_tail, __ATOMIC_RELAXED);
    unsigned avail = head - tail;
    if (play_primed && avail < PLAY_FRAG) { /* the queue ran dry: let it fill again before playing on */
        ++play_dry;
        play_primed = 0;
        play_wait_ticks = 1;
        if (play_events_logged < 60) {
            char line[100], when[16];
            play_stamp(when);
            snprintf(line, sizeof(line), "Audio player: queue ran dry at %s UTC (dry spell %u)", when, play_dry);
            log_line(line);
        }
        return 0;
    }
    if (!play_primed) {
        if (play_wait_ticks)
            ++play_wait_ticks;
        /* how long the queue stays empty after running dry, in polls of 5 ms (a lower bound) */
        if (avail < PLAY_FRAG) {
            if (play_dry)
                ++play_empty_cur;
        } else if (play_empty_cur) {
            if (play_empty_cur > play_empty_max) play_empty_max = play_empty_cur;
            play_empty_cur = 0;
        }
        if (avail < PLAY_FRAG || avail < (play_dry ? play_reprime : play_prebuffer))
            return 0;
        play_primed = 1;
        if (play_wait_ticks && play_events_logged < 60) {
            char line[110], when[16];
            ++play_events_logged;
            play_stamp(when);
            snprintf(line, sizeof(line), "Audio player: playing again at %s UTC, about %u ms or more after it ran dry", when,
                     play_wait_ticks * 5);
            log_line(line);
        }
        play_wait_ticks = 0;
    }
    if (avail < play_lead_min) play_lead_min = avail;
    if (avail > play_lead_max) play_lead_max = avail;
    if (avail > PLAY_MAX_LEAD + play_prebuffer) { /* the device fell behind: jump ahead rather than play old audio */
        play_skipped += avail - (PLAY_KEEP_LEAD + play_prebuffer);
        tail = head - (PLAY_KEEP_LEAD + play_prebuffer);
        __atomic_store_n(&play_tail, tail, __ATOMIC_RELEASE);
    }
    if (!play_pcm && play_open()) {
        play_failed = 1;
        return -1;
    }
    unsigned char block[PLAY_FRAG];
    for (unsigned i = 0; i < PLAY_FRAG; ++i)
        block[i] = play_ring[(tail + i) % PLAY_RING];
    const int written = asound_write(play_pcm, block, PLAY_FRAG);
    ++play_writes;
    if (written == PLAY_FRAG) {
        play_in_row = 0;
        __atomic_store_n(&play_tail, tail + PLAY_FRAG, __ATOMIC_RELEASE);
        if (play_writes % 430 == 0) { /* about every 10 s */
            char line[170];
            snprintf(line, sizeof(line),
                     "Audio player: queue %u..%u bytes, ran dry %u times (longest empty >= %u ms), %u write errors, %u bytes skipped",
                     play_lead_min, play_lead_max, play_dry, play_empty_max * 5, play_errors, play_skipped);
            log_line(line);
            play_lead_min = 0xffffffffu; play_lead_max = 0; play_empty_max = 0;
        }
        return 1;
    }
    /* A short write is an underrun or a stopped channel: prepare again and go on. */
    ++play_errors;
    asound_prepare(play_pcm, 0);
    if (written > 0)
        __atomic_store_n(&play_tail, tail + PLAY_FRAG, __ATOMIC_RELEASE);
    if (++play_in_row > PLAY_MAX_ERRORS) {
        log_line("Audio player: too many write errors, audio switched off");
        play_close();
        play_failed = 1;
        return -1;
    }
    return 0;
}

static void *play_thread(void *unused)
{
    (void)unused;
    while (!__atomic_load_n(&play_stop, __ATOMIC_ACQUIRE)) {
        if (play_step() <= 0)
            usleep(5000);
    }
    play_close();
    return 0;
}

static void play_start(void)
{
    const char *ms = getenv("CARLINK_AUDIO_PREBUFFER_MS");
    if (ms && *ms) {
        unsigned value = 0;
        for (const char *p = ms; *p >= '0' && *p <= '9' && value < 2000; ++p)
            value = value * 10 + (unsigned)(*p - '0');
        play_prebuffer = value * 176 > 120000 ? 120000 : value * 176;
        play_reprime = play_prebuffer / 2;
    }
    if (pthread_create(&play_tid, 0, play_thread, 0)) {
        log_line("Audio player: thread could not be started");
        play_enabled = 0;
        return;
    }
    play_started = 1;
    log_line("Audio player enabled (starts when audio arrives)");
}

static void play_shutdown(void)
{
    if (!play_started)
        return;
    __atomic_store_n(&play_stop, 1, __ATOMIC_RELEASE);
    pthread_join(play_tid, 0);
    play_started = 0;
}

static void lock_usb_writes(void)
{
    while (__atomic_exchange_n(&usb_write_lock, 1, __ATOMIC_ACQUIRE))
        usleep(200);
}

static void unlock_usb_writes(void)
{
    __atomic_store_n(&usb_write_lock, 0, __ATOMIC_RELEASE);
}

static int send_record(int fd, unsigned channel, unsigned type,
                       const unsigned char *body, unsigned size)
{
    unsigned char outer[8], inner[64] = {0};
    if (size > sizeof(inner) - 8)
        return -1;
    put_be32(outer, channel);
    put_be32(outer + 4, 8 + size);
    inner[1] = (unsigned char)size;
    put_be32(inner + 4, type);
    if (size)
        memcpy(inner + 8, body, size);
    /* The phone reads the outer header and body in separate bulk transfers. */
    lock_usb_writes();
    if (usb_closed) {
        unlock_usb_writes();
        return -1;
    }
    /* Wi-Fi: the record goes to its channel's socket, without the AOA header. */
    const int target = !wifi_mode ? fd : channel >= 1 && channel <= WIFI_CHANNELS ? wifi_fd[channel] : -1;
    const int bad = target < 0 ||
                    (!wifi_mode && write(target, outer, sizeof(outer)) != (ssize_t)sizeof(outer)) ||
                    write(target, inner, 8 + size) != (ssize_t)(8 + size);
    const int error = bad ? *__get_errno_ptr() : 0;
    unlock_usb_writes();
    return bad ? failure("USB command write failed, errno", error) : 0;
}

static int send_cmd(int fd, unsigned type, const unsigned char *body, unsigned size)
{
    return send_record(fd, 1, type, body, size);
}

/* CarLife hard key on AOA channel 6; matches src/carlife_idrive_keys.cjs. */
static int send_hardkey(int fd, unsigned char code)
{
    const unsigned char body[] = {0x08, code};
    return send_record(fd, 6, 0x00068008, body, sizeof(body));
}

static int read_varint(const unsigned char **data, unsigned *size, unsigned *value)
{
    *value = 0;
    for (unsigned shift = 0; *size && shift <= 28; shift += 7) {
        unsigned byte = *(*data)++;
        --*size;
        if (shift == 28 && byte > 15)
            return -1;
        *value |= (byte & 127) << shift;
        if (byte < 128)
            return 0;
    }
    return -1;
}

/* Read the varint fields used by MATCH_STATUS and VIDEO_INIT_DONE. */
static int read_fields(const unsigned char *data, unsigned size,
                       unsigned *values, unsigned count)
{
    unsigned seen = 0;
    while (size) {
        unsigned key, value;
        if (read_varint(&data, &size, &key) || !(key >> 3))
            return -1;
        unsigned field = key >> 3;
        if (!(key & 7)) {
            if (read_varint(&data, &size, &value))
                return -1;
            if (field <= count) {
                values[field - 1] = value;
                seen |= 1u << (field - 1);
            }
            continue;
        }
        switch (key & 7) {
        case 1: value = 8; break;
        case 2: if (read_varint(&data, &size, &value)) return -1; break;
        case 5: value = 4; break;
        default: return -1;
        }
        if (value > size)
            return -1;
        data += value;
        size -= value;
    }
    return seen == ((1u << count) - 1) ? 0 : -1;
}

enum session_stage { WAIT_MATCH, WAIT_VIDEO_INIT, WAIT_IDR };

struct input_args;

struct capture {
    unsigned live, frames;
    unsigned width, height, fps;
    unsigned waiting_for_idr;
    unsigned published, consumed, finished, writer_failed, continuous;
    unsigned input_ready; /* set once the first live frame is queued */
    struct input_args *input; /* bench input, started only after the first frame */
    unsigned input_tid, input_started;
};

static void start_input_thread(struct capture *capture);

static int handle_cmd(int fd, enum session_stage *stage, unsigned type,
                      const unsigned char *body, unsigned size, struct capture *capture)
{
    static const unsigned char stats[] = {
        0x0a, 0, 0x12, 3, '1', '.', '0', 0x18, 1, 0x22, 0,
        0x28, 0, 0x30, 0, 0x38, 0
    };
    static const unsigned char hu_info[] = {
        0x0a, 3, 'Q', 'N', 'X', 0x22, 3, 'B', 'M', 'W',
        0x72, 7, 'N', 'B', 'T', ' ', 'E', 'V', 'O'
    };
    static const unsigned char video_init[] = {8, 0x80, 10, 16, 0xe0, 3, 24, 30};
    static const unsigned char module_control[] = {8, 5, 16, 2};
    static const unsigned char no_features[] = {8, 0};
    char line[96];
    snprintf(line, sizeof(line), "CarLife CMD RX 0x%08x (%u bytes)", type, size);
    log_line(line);

    switch (type) {
    case VERSION_MATCH: {
        unsigned status = 0;
        if (*stage != WAIT_MATCH)
            return 0;
        if (read_fields(body, size, &status, 1) || status != 1)
            return failure("CarLife 1.0 version rejected or invalid", (int)status);
        log_line("CarLife 1.0 accepted; requesting 1280x480 @ 30 fps");
        if (send_cmd(fd, 0x00018027, stats, sizeof(stats)) ||
            send_cmd(fd, 0x00018003, hu_info, sizeof(hu_info)) ||
            send_cmd(fd, 0x00018007, video_init, sizeof(video_init)) ||
            send_cmd(fd, 0x00018028, module_control, sizeof(module_control)))
            return -1;
        *stage = WAIT_VIDEO_INIT;
        return 0;
    }
    case VIDEO_INIT_DONE: {
        unsigned video[3];
        if (*stage != WAIT_VIDEO_INIT)
            return 0;
        if (read_fields(body, size, video, 3) || !video[0] || !video[1] ||
            video[0] > 8192 || video[1] > 8192 || video[2] > 240)
            return failure("Invalid VIDEO_ENCODER_INIT_DONE", 0);
        snprintf(line, sizeof(line), "Phone encoder: %ux%u @ %u fps",
                 video[0], video[1], video[2]);
        log_line(line);
        capture->width = video[0];
        capture->height = video[1];
        capture->fps = video[2] ? video[2] : 30;
        if (capture->live && capture->fps != 30)
            return failure("Live bench requires negotiated 30 fps", (int)capture->fps);
        if (send_cmd(fd, 0x00018009, 0, 0) || send_cmd(fd, 0x0001801d, 0, 0))
            return -1;
        *stage = WAIT_IDR;
        log_line("VIDEO_ENCODER_START sent; waiting for SPS, PPS and IDR");
        return 0;
    }
    case FEATURE_REQUEST:
        return send_cmd(fd, 0x00018052, no_features, sizeof(no_features));
    case MD_AUTH_RESULT: {
        unsigned accepted = 0;
        if (read_fields(body, size, &accepted, 1) || accepted != 1)
            return failure("Phone rejected HU authentication", (int)accepted);
        return 0;
    }
    case RSA_KEY_REQUEST:
        return failure("Phone requires CarLife encryption; plaintext capture stopped", 0);
    default:
        return 0;
    }
}

static unsigned nal_prefix(const unsigned char *p, unsigned size)
{
    if (size < 4 || p[0] || p[1])
        return 0;
    if (p[2] == 1)
        return 3;
    return size >= 5 && !p[2] && p[3] == 1 ? 4 : 0;
}

static int save_bytes(const unsigned char *data, unsigned size)
{
    while (size) {
        ssize_t count = write(1, data, size);
        if (count <= 0)
            return failure("Capture file write failed, errno", *__get_errno_ptr());
        data += count;
        size -= (unsigned)count;
    }
    return 0;
}

/* Power-of-two ring between the USB reader and the pipe writer. The counters
 * are free-running 32-bit byte counts; the ring index is the count masked.
 * The 30-second bench never wraps it. A continuous session may, and fails
 * only if the writer falls a whole ring behind. */
enum { LIVE_RING = 16 * 1024 * 1024, LIVE_MASK = LIVE_RING - 1 };
static unsigned char live_spool[LIVE_RING];

static void *live_writer(void *arg)
{
    struct capture *capture = arg;
    unsigned sent = __atomic_load_n(&capture->consumed, __ATOMIC_ACQUIRE);
    for (;;) {
        unsigned finished = __atomic_load_n(&capture->finished, __ATOMIC_ACQUIRE);
        unsigned available = __atomic_load_n(&capture->published, __ATOMIC_ACQUIRE);
        if (sent != available) {
            unsigned chunk = available - sent;
            const unsigned index = sent & LIVE_MASK;
            if (chunk > LIVE_RING - index)
                chunk = LIVE_RING - index;
            if (save_bytes(live_spool + index, chunk)) {
                __atomic_store_n(&capture->writer_failed, 1, __ATOMIC_RELEASE);
                return 0;
            }
            sent += chunk;
            __atomic_store_n(&capture->consumed, sent, __ATOMIC_RELEASE);
        } else if (finished) {
            return 0;
        } else {
            usleep(1000);
        }
    }
}

static void ring_put(unsigned offset, const void *data, unsigned size)
{
    const unsigned index = offset & LIVE_MASK;
    unsigned first = LIVE_RING - index;
    if (first > size)
        first = size;
    memcpy(live_spool + index, data, first);
    if (size > first)
        memcpy(live_spool, (const unsigned char *)data + first, size - first);
}

/* ARMv7 QNX and mdev use little endian words. Publish only complete records. */
static int live_record(struct capture *capture, unsigned kind,
                       const void *first, unsigned first_size,
                       const void *second, unsigned second_size)
{
    const unsigned header[6] = {0, 0, 0, 0, kind, first_size + second_size};
    unsigned offset = __atomic_load_n(&capture->published, __ATOMIC_RELAXED);
    const unsigned prefix_size = kind == 5 ? 8 : 0;
    const unsigned size = prefix_size + sizeof(header) + first_size + second_size;
    const unsigned pending = offset - __atomic_load_n(&capture->consumed, __ATOMIC_ACQUIRE);
    if (size > LIVE_RING - pending)
        return failure("Live video spool full after accepted USB frames", (int)capture->frames);
    if (prefix_size) {
        ring_put(offset, "MDEVv1\r\n", prefix_size);
        offset += prefix_size;
    }
    ring_put(offset, header, sizeof(header));
    offset += sizeof(header);
    ring_put(offset, first, first_size);
    offset += first_size;
    if (second_size) {
        ring_put(offset, second, second_size);
        offset += second_size;
    }
    __atomic_store_n(&capture->published, offset, __ATOMIC_RELEASE);
    return 0;
}

static int capture_video(struct capture *capture, const unsigned char *data, unsigned size)
{
    static unsigned char sps[4096], pps[4096];
    static unsigned sps_size, pps_size;
    unsigned picture = 0;
    unsigned idr_offset = size;
    for (unsigned offset = 0; offset < size;) {
        unsigned prefix = nal_prefix(data + offset, size - offset);
        if (!prefix)
            return failure("Video payload has no Annex B start code", 0);
        unsigned type = data[offset + prefix] & 31;
        unsigned end = offset + prefix + 1;
        while (end < size && !nal_prefix(data + end, size - end))
            ++end;
        if (type == 7 || type == 8) {
            unsigned count = end - offset;
            if (count > sizeof(sps))
                return failure("H.264 parameter set exceeds capture limit", (int)count);
            unsigned char *stored = type == 7 ? sps : pps;
            unsigned *stored_size = type == 7 ? &sps_size : &pps_size;
            if (capture->frames) {
                const unsigned old_prefix = nal_prefix(stored, *stored_size);
                if (count - prefix != *stored_size - old_prefix ||
                    memcmp(stored + old_prefix, data + offset + prefix, count - prefix) != 0)
                    return failure("H.264 parameter set changed during live capture", (int)type);
            }
            memcpy(stored, data + offset, count);
            *stored_size = count;
        }
        picture |= capture->frames && type >= 1 && type <= 5;
        if (type == 5 && idr_offset == size)
            idr_offset = offset;
        if (!capture->frames && type == 5 && sps_size && pps_size) {
            if (!capture->live) {
                if (save_bytes(sps, sps_size) || save_bytes(pps, pps_size) ||
                    save_bytes(data + offset, size - offset))
                    return -1;
                log_line("Captured Annex B SPS + PPS + first IDR");
                return 1;
            }
            const unsigned sync = 1;
            const unsigned format[8] = {2, 0x48323634, capture->width, capture->height,
                                        16, capture->fps, 0, 0};
            if (live_record(capture, 5, &sync, sizeof(sync), 0, 0) ||
                live_record(capture, 2, format, sizeof(format), 0, 0) ||
                live_record(capture, 4, sps, sps_size, pps, pps_size))
                return -1;
            log_line("Captured Annex B SPS + PPS + first IDR");
            data += offset;
            size -= offset;
            idr_offset = 0;
            picture = 1;
            break;
        }
        offset = end;
    }
    if (capture->waiting_for_idr) {
        if (idr_offset == size || !sps_size || !pps_size)
            return 0;
        data += idr_offset;
        size -= idr_offset;
        capture->waiting_for_idr = 0;
        log_line("Recovered video at an IDR after USB transfer loss");
    }
    if (capture->live && picture) {
        /* n * 1000000 / 30 without a 64-bit divide: n*33333 + n/3 (exact floor). */
        const unsigned long long micros = (unsigned long long)capture->frames * 33333u +
                                          capture->frames / 3u;
        const unsigned timestamp[2] = {(unsigned)micros, (unsigned)(micros >> 32)};
        if (live_record(capture, 3, timestamp, sizeof(timestamp), data, size))
            return -1;
        ++capture->frames;
        lat_frame(size);
        if (capture->frames == 1) { /* keys are accepted only after the first frame is queued */
            __atomic_store_n(&capture->input_ready, 1, __ATOMIC_RELEASE);
            start_input_thread(capture); /* never runs during the handshake */
            log_line("Key input enabled: first video frame queued");
        }
        if (capture->frames == 1 && capture->continuous)
            alarm(0); /* the startup alarm only guards the handshake */
        if (!capture->continuous &&
            capture->frames == (capture->live == 2 ? CARLINK_VISIBLE_FRAMES : CARLINK_LIVE_FRAMES)) {
            char line[64];
            snprintf(line, sizeof(line), "Queued %u live USB frames", capture->frames);
            log_line(line);
            return 1;
        }
        if (capture->live == 2 && capture->frames % 60 == 0) {
            char line[64];
            snprintf(line, sizeof(line), "USB video progress: %u frames queued", capture->frames);
            log_line(line);
        }
    }
    return 0;
}

struct stream {
    unsigned char *data;
    unsigned capacity, header_size, used, wanted;
};

/* Bench input: poll a plain text file (one action per line, appended with
 * `echo OK >> FILE`) and forward actions as CarLife hard keys. A plain file
 * is polled instead of a FIFO so the thread never blocks and needs only
 * open/read/usleep, all already used by this program. Actions match
 * src/carlife_idrive_keys.cjs: OK BACK LEFT RIGHT UP DOWN, and +N / -N
 * rotary steps. Lines read before the first video frame, overlong lines,
 * unknown words and unterminated last lines are never sent. */
enum { INPUT_LINE_MAX = 32, INPUT_READ_MAX = 64, INPUT_MAX_STEPS = 32,
       INPUT_POLL_US = 20000, INPUT_KEY_GAP_US = 5000 };

struct input_args {
    struct capture *capture;
    int usb_fd, file_fd;
};

static int input_path_ok(const char *path)
{
    static const char prefix[] = "/dev/shmem/";
    if (strncmp(path, prefix, sizeof(prefix) - 1) != 0)
        return 0;
    path += sizeof(prefix) - 1;
    unsigned length = 0;
    for (; path[length]; ++length) {
        const char c = path[length];
        if (length >= 48 ||
            !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') || c == '.' || c == '_' || c == '-'))
            return 0;
    }
    return length && strcmp(path, ".") != 0 && strcmp(path, "..") != 0;
}

static int input_send(const struct input_args *input, unsigned char code)
{
    const int rc = send_hardkey(input->usb_fd, code);
    if (!rc)
        lat_key();
    usleep(INPUT_KEY_GAP_US); /* bounds the key rate whatever the file holds */
    return rc;
}

static int input_dispatch(const struct input_args *input, const char *line)
{
    static const struct { const char *name; unsigned char code; } keys[] = {
        {"OK", 0x14}, {"BACK", 0x0e}, {"LEFT", 0x15},
        {"RIGHT", 0x16}, {"UP", 0x17}, {"DOWN", 0x18}
    };
    char note[48];
    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i)
        if (strcmp(line, keys[i].name) == 0) {
            const int rc = input_send(input, keys[i].code);
            if (!rc) {
                snprintf(note, sizeof(note), "Input sent: %s", keys[i].name);
                log_line(note);
            }
            return rc;
        }
    if ((line[0] == '+' || line[0] == '-') && line[1]) {
        unsigned steps = 0;
        for (const char *p = line + 1; *p; ++p) {
            if (*p < '0' || *p > '9' || steps > INPUT_MAX_STEPS) {
                steps = INPUT_MAX_STEPS + 1;
                break;
            }
            steps = steps * 10 + (unsigned)(*p - '0');
        }
        if (steps && steps <= INPUT_MAX_STEPS) {
            /* Candidate direction mapping, as in the offline encoder. */
            const unsigned char code = line[0] == '+' ? 0x06 : 0x07;
            for (unsigned step = 0; step < steps; ++step)
                if (input_send(input, code))
                    return -1;
            snprintf(note, sizeof(note), "Input sent: %s%u rotary steps", line[0] == '+' ? "+" : "-", steps);
            log_line(note);
            return 0;
        }
    }
    log_line("Ignored unrecognized input line");
    return 0;
}

static void *input_thread(void *arg)
{
    const struct input_args *input = arg;
    char line[INPUT_LINE_MAX + 1], chunk[INPUT_READ_MAX];
    unsigned used = 0, skipping = 0;
    for (;;) {
        if (__atomic_load_n(&input->capture->finished, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&usb_closed, __ATOMIC_ACQUIRE))
            return 0;
        const ssize_t count = read(input->file_fd, chunk, sizeof(chunk));
        if (count < 0) {
            failure("Input file read failed, errno", *__get_errno_ptr());
            return 0;
        }
        for (ssize_t i = 0; i < count; ++i) {
            const char c = chunk[i];
            if (c != '\n') {
                if (skipping)
                    continue;
                if (used < INPUT_LINE_MAX)
                    line[used++] = c;
                else
                    skipping = 1, used = 0;
                continue;
            }
            if (used && line[used - 1] == '\r')
                --used;
            line[used] = 0;
            const int send_it = used && !skipping &&
                __atomic_load_n(&input->capture->input_ready, __ATOMIC_ACQUIRE);
            used = skipping = 0;
            if (send_it && input_dispatch(input, line))
                return 0; /* USB write failed or the session closed */
        }
        usleep(INPUT_POLL_US);
    }
}

static void start_input_thread(struct capture *capture)
{
    if (!capture->input || capture->input_started)
        return;
    const int rc = pthread_create(&capture->input_tid, 0, input_thread, capture->input);
    if (rc)
        failure("Input thread startup failed", rc); /* display continues without keys */
    else
        capture->input_started = 1;
}

static int is_phone_command(unsigned type)
{
    return type > 0x00010000 && type <= 0x000100ff;
}

/* consume() result: a video record header is garbage (the factory reader took the first read of an envelope body).
 * The AOA envelope boundaries are still trustworthy, so the caller drops this envelope and waits for the next IDR. */
#define INNER_RESYNC (-2)

/* Outer payloads can carry a full inner record or slices of one record.
 * Keep independent command and video state so channels may interleave. */
static int consume(int fd, enum session_stage *stage, struct stream *stream,
                   const unsigned char *data, unsigned size, struct capture *capture)
{
    while (size) {
        unsigned count = stream->wanted - stream->used;
        if (count > size)
            count = size;
        memcpy(stream->data + stream->used, data, count);
        stream->used += count;
        data += count;
        size -= count;
        if (stream->used != stream->wanted)
            continue;

        int command = stream->header_size == 8;
        if (stream->wanted == stream->header_size) {
            const unsigned type = get_be32(stream->data + stream->header_size - 4);
            unsigned body_size = command
                ? (unsigned)stream->data[0] << 8 | stream->data[1]
                : get_be32(stream->data);
            if (body_size > stream->capacity - stream->header_size ||
                (command ? !is_phone_command(type) :
                    (type != VIDEO_DATA && type != VIDEO_HEARTBEAT))) {
                char line[120];
                snprintf(line, sizeof(line), "Invalid CarLife inner %s header: length=%08x "
                         "type=%08x after %u video frames", command ? "CMD" : "VIDEO",
                         body_size, type, capture->frames);
                log_line(line);
                return command ? -1 : INNER_RESYNC;
            }
            stream->wanted += body_size;
            if (body_size)
                continue;
        }
        unsigned type = get_be32(stream->data + stream->header_size - 4);
        unsigned body_size = stream->wanted - stream->header_size;
        const unsigned char *body = stream->data + stream->header_size;
        int result = 0;
        if (command)
            result = handle_cmd(fd, stage, type, body, body_size, capture);
        else if (type == VIDEO_DATA && body_size && *stage == WAIT_IDR)
            result = capture_video(capture, body, body_size);
        stream->used = 0;
        stream->wanted = stream->header_size;
        if (result)
            return result;
    }
    return 0;
}

static int is_unwrapped_command(const unsigned char *data, unsigned size)
{
    if (size < 8 || data[2] || data[3])
        return 0;
    const unsigned length = (unsigned)data[0] << 8 | data[1];
    const unsigned type = get_be32(data + 4);
    return length == size - 8 && is_phone_command(type);
}

static int valid_envelope(unsigned channel, unsigned length)
{
    return channel >= 1 && channel <= 6 && length && length <= MAX_VIDEO_SIZE + 12;
}

static void *rx_reader(void *unused)
{
    (void)unused;
    for (;;) {
        const unsigned head = __atomic_load_n(&rx_head, __ATOMIC_RELAXED);
        if (head - __atomic_load_n(&rx_tail, __ATOMIC_ACQUIRE) >= RX_SLOTS) {
            usleep(200); /* the consumer is behind: wait rather than overwrite */
            continue;
        }
        struct rx_slot *slot = &rx_slot[head % RX_SLOTS];
        const int count = (int)read((int)rx_fd_value, slot->data, USB_BUFFER_SIZE);
        if (!count) {
            usleep(1000); /* devc-serusb-aoap reports a zero-length packet as read() == 0 */
            continue;
        }
        slot->count = count;
        slot->err = count < 0 ? *__get_errno_ptr() : 0;
        __atomic_store_n(&rx_head, head + 1, __ATOMIC_RELEASE);
        if (count < 0)
            return 0;
    }
}

/* One USB transfer: straight from the device, or the next one the reader thread queued. 0 = nothing yet. */
static long rx_read(int fd, unsigned char *buffer, const struct capture *capture)
{
    if (!use_rx_thread)
        return (long)read(fd, buffer, USB_BUFFER_SIZE);
    for (;;) {
        const unsigned tail = __atomic_load_n(&rx_tail, __ATOMIC_RELAXED);
        if (tail != __atomic_load_n(&rx_head, __ATOMIC_ACQUIRE)) {
            const struct rx_slot *slot = &rx_slot[tail % RX_SLOTS];
            const int count = slot->count;
            if (count < 0) {
                *__get_errno_ptr() = slot->err;
                return -1;
            }
            memcpy(buffer, slot->data, (unsigned)count);
            __atomic_store_n(&rx_tail, tail + 1, __ATOMIC_RELEASE);
            return count;
        }
        if (__atomic_load_n((const unsigned *)&capture->writer_failed, __ATOMIC_ACQUIRE))
            return 0;
        usleep(300);
    }
}

static int receive(int fd, struct capture *capture)
{
    /* Commands carry a 16-bit body length and may span several USB reads. */
    static unsigned char usb[USB_BUFFER_SIZE], cmd[8 + 0xffff], video[MAX_VIDEO_SIZE + 12];
    struct stream streams[] = {{cmd, sizeof(cmd), 8, 0, 8},
                               {video, sizeof(video), 12, 0, 12}};
    enum session_stage stage = WAIT_MATCH;
    const unsigned char version[] = {8, 1, 16, 0};
    unsigned char header[8];
    unsigned header_used = 0, channel = 0, remaining = 0;
    unsigned transfer = 0, previous_count = 0, envelope_length = 0;
    unsigned header_transfer = 0, header_offset = 0;
    unsigned discard = 0, inner_resyncs = 0; /* discard: rest of this envelope is unusable */
    unsigned aoa_resyncs = 0;                /* reads dropped after an invalid AOA header */

    usleep(1000000);
    if (send_cmd(fd, 0x00018001, version, sizeof(version)))
        return -1;
    log_line("HU_PROTOCOL_VERSION 1.0 sent");
    if (use_rx_thread) {
        unsigned tid = 0;
        rx_fd_value = (unsigned)fd;
        rx_head = rx_tail = 0;
        if (pthread_create(&tid, 0, rx_reader, 0)) {
            use_rx_thread = 0;
            log_line("USB reader thread could not be started: reading directly");
        } else
            log_line("USB reads run on a dedicated thread");
    }
    for (;;) {
        if (__atomic_load_n(&capture->writer_failed, __ATOMIC_ACQUIRE))
            return -1;
        ssize_t count = rx_read(fd, usb, capture);
        if (count < 0)
            return failure("USB read failed, errno", *__get_errno_ptr());
        if (!count) {
            /* devc-serusb-aoap reports a USB zero-length packet as read() == 0. */
            usleep(1000);
            continue;
        }
        ++transfer;
        if (capture->live == 2 && transfer % 512 == 0) {
            char line[112];
            snprintf(line, sizeof(line), "USB receive progress: %u reads, %u frames queued, waiting for IDR=%u",
                     transfer, capture->frames, capture->waiting_for_idr);
            log_line(line);
        }
        /* devc-serusb-aoap returns individual bulk transfers. A standalone
         * AOA header at the next read boundary is not the previous envelope's
         * missing body. Discard that incomplete record before accepting it. */
        if (remaining && count == 8 && valid_envelope(get_be32(usb), get_be32(usb + 4))) {
            char line[120];
            snprintf(line, sizeof(line), "Missing AOA payload at RX #%u: channel=%u "
                     "remaining=%u; resuming at next header", transfer, channel, remaining);
            log_line(line);
            if (channel <= 2) {
                struct stream *interrupted = &streams[channel - 1];
                interrupted->used = 0;
                interrupted->wanted = interrupted->header_size;
            }
            if (channel == 2)
                capture->waiting_for_idr = 1;
            remaining = 0;
            discard = 0;
        }
        /* The bench phone delivered MD_AUTHEN_RESULT as a whole USB read
         * without its AOA header. Only accept an exact command at a read
         * boundary, with no unfinished outer envelope or command. */
        if (!remaining && !header_used && !streams[0].used &&
            is_unwrapped_command(usb, (unsigned)count)) {
            log_line("Complete CarLife CMD received without AOA header");
            const int rc = consume(fd, &stage, &streams[0], usb, (unsigned)count, capture);
            if (rc)
                return rc;
            previous_count = (unsigned)count;
            continue;
        }
        /* The factory reader thread can take an AOA header (or the whole body that followed a header), so a read may start
         * with an inner record that nothing announced (seen in the car: "Invalid AOA header ... matches complete media
         * record=1", a large video record's first read, and an audio record arriving while a command envelope's body was
         * missing). Accept only an exact match: it is not a valid AOA header, it starts with a media record header (audio
         * with the read being that one record, or video: the record, or its first read when it is longer), and it is not
         * simply the body the pending envelope header announced. An envelope that was still waiting for its body lost it
         * and is dropped. Anything else stays fatal. */
        if (count >= 12) {
            const unsigned body_length = get_be32(usb);
            const unsigned record_type = get_be32(usb + 8);
            const int audio = record_type >= 0x00030001 && record_type <= 0x00030006;
            const int whole = body_length == (unsigned)count - 12;
            const int video_whole = whole && record_type == VIDEO_DATA;
            const int video_start = record_type == VIDEO_DATA && body_length > (unsigned)count - 12 &&
                                    body_length <= MAX_VIDEO_SIZE;
            const int announced = remaining == envelope_length && envelope_length == body_length + 12 &&
                                  ((channel == 2 && !audio) || (channel == 3 && audio));
            if (!announced && !valid_envelope(body_length, get_be32(usb + 4)) &&
                ((audio && whole) || video_whole || video_start)) {
                char line[136];
                if (remaining || header_used) {
                    snprintf(line, sizeof(line), "RX #%u: pending envelope (channel %u, %u bytes missing) dropped: the next "
                             "record arrived without its AOA header", transfer, channel, remaining);
                    log_line(line);
                    if (channel >= 1 && channel <= 2) {
                        streams[channel - 1].used = 0;
                        streams[channel - 1].wanted = streams[channel - 1].header_size;
                    }
                    if (channel == 2)
                        capture->waiting_for_idr = 1;
                    remaining = 0;
                    header_used = 0;
                    discard = 0;
                }
                if (!audio) { /* a video record in progress belonged to the lost envelope */
                    streams[1].used = 0;
                    streams[1].wanted = streams[1].header_size;
                }
                if (audio) {
                    snprintf(line, sizeof(line), "RX #%u: record 0x%08x (%u bytes) arrived without its AOA header: skipped",
                             transfer, record_type, (unsigned)count);
                    log_line(line);
                } else {
                    snprintf(line, sizeof(line), "RX #%u: video record %s (%u of %u bytes) arrived without its AOA header: %s",
                             transfer, video_whole ? "whole" : "start", (unsigned)count, body_length + 12,
                             video_whole ? "used" : "continuing it");
                    log_line(line);
                    if (video_start) {
                        channel = 2;
                        envelope_length = body_length + 12;
                        remaining = envelope_length - (unsigned)count;
                    }
                    const int rc = consume(fd, &stage, &streams[1], usb, (unsigned)count, capture);
                    if (rc == INNER_RESYNC) {
                        streams[1].used = 0;
                        streams[1].wanted = streams[1].header_size;
                        capture->waiting_for_idr = 1;
                        discard = remaining != 0;
                    } else if (rc)
                        return rc;
                }
                previous_count = (unsigned)count;
                continue;
            }
        }
        for (unsigned offset = 0; offset < (unsigned)count;) {
            if (!remaining) {
                if (!header_used) {
                    header_transfer = transfer;
                    header_offset = offset;
                }
                header[header_used++] = usb[offset++];
                if (header_used != sizeof(header))
                    continue;
                const unsigned next_channel = get_be32(header);
                const unsigned next_length = get_be32(header + 4);
                header_used = 0;
                if (!valid_envelope(next_channel, next_length)) {
                    char line[144];
                    /* The factory reader can take an AOA header and the start of a video body, so a read may carry the middle
                     * of an envelope: seen in the car as 3596 bytes of audio followed by 12788 bytes of video body. Nothing
                     * of it can be used; drop the rest of this read and wait for the next read that starts with a header.
                     * Video restarts at the next IDR. Give up only if this keeps happening. */
                    if (++aoa_resyncs <= 10) {
                        snprintf(line, sizeof(line),
                                 "Invalid AOA header %08x %08x at RX #%u+%u; "
                                 "finished in RX #%u (%u bytes): dropping the rest of this read",
                                 next_channel, next_length, header_transfer, header_offset,
                                 transfer, (unsigned)count);
                        log_line(line);
                        snprintf(line, sizeof(line),
                                 "Previous RX size=%u; prior AOA channel=%u length=%u; "
                                 "inner pending cmd=%u video=%u",
                                 previous_count, channel, envelope_length,
                                 streams[0].used, streams[1].used);
                        log_line(line);
                    }
                    if (aoa_resyncs > 200)
                        return -1;
                    for (unsigned i = 0; i < 2; ++i) {
                        streams[i].used = 0;
                        streams[i].wanted = streams[i].header_size;
                    }
                    capture->waiting_for_idr = 1;
                    remaining = envelope_length = header_used = discard = 0;
                    break;
                }
                channel = next_channel;
                remaining = envelope_length = next_length;
                if (channel == 3) {
                    audio_dump(header, sizeof(header));
                    audio_begin(next_length);
                }
                continue;
            }
            unsigned chunk = (unsigned)count - offset;
            if (chunk > remaining)
                chunk = remaining;
            if (channel == 3) {
                audio_dump(usb + offset, chunk);
                audio_collect(usb + offset, chunk);
            }
            if (channel <= 2 && !discard) {
                int rc = consume(fd, &stage, &streams[channel - 1], usb + offset, chunk, capture);
                if (rc == INNER_RESYNC) {
                    char line[120];
                    snprintf(line, sizeof(line), "Inner VIDEO header corrupt at RX #%u (envelope length=%u, %u left): "
                             "dropping the rest of it, waiting for IDR", transfer, envelope_length, remaining);
                    log_line(line);
                    streams[1].used = 0;
                    streams[1].wanted = streams[1].header_size;
                    capture->waiting_for_idr = 1;
                    discard = 1;
                    if (++inner_resyncs > 200)
                        return -1;
                    rc = 0;
                }
                if (rc < 0) {
                    char line[124];
                    snprintf(line, sizeof(line), "RX #%u offset=%u size=%u AOA channel=%u "
                             "length=%u left=%u", transfer, offset, (unsigned)count,
                             channel, envelope_length, remaining);
                    log_line(line);
                }
                if (rc)
                    return rc;
            }
            offset += chunk;
            remaining -= chunk;
            if (!remaining)
                discard = 0;
        }
        previous_count = (unsigned)count;
    }
}

/* ---- Wi-Fi transport (candidate; how Android CarLife+ serves WLAN mode is not yet confirmed in the car) ----
 * Apollo's head unit, CONNECTED_BY_WIFI: the phone serves six TCP ports, 7240 commands, 8240 video, 9240 media audio,
 * 9241 TTS, 9242 voice, 9340 touch/keys. Each socket carries the same inner records as the AOA envelopes (command: 8-byte
 * header, video and audio: 12-byte header) with no AOA header. The head unit's firewall passes outbound TCP only from
 * local ports 49512 and up, so each socket is bound to one of those first. */
static const unsigned short wifi_ports[WIFI_CHANNELS] = {7240, 8240, 9240, 9241, 9242, 9340};
typedef int (*wifi_socket_fn)(int, int, int);
typedef int (*wifi_addr_fn)(int, const void *, unsigned);
typedef int (*wifi_setopt_fn)(int, int, int, const void *, unsigned);
struct wifi_pollfd { int fd; short events, revents; };

static int wifi_ip(const char *text, unsigned char *ip)
{
    unsigned part = 0, digits = 0, index = 0;
    for (;; ++text) {
        if (*text >= '0' && *text <= '9') {
            part = part * 10 + (unsigned)(*text - '0');
            if (++digits > 3 || part > 255)
                return 0;
        } else if ((*text == '.' || !*text) && digits && index < 4) {
            ip[index++] = (unsigned char)part;
            part = digits = 0;
            if (!*text)
                break;
        } else
            return 0;
    }
#ifdef CARLINK_HOST_TEST
    if (index == 4 && ip[0] == 127)
        return 1;
#endif
    /* only a phone on the head unit's own hotspot */
    return index == 4 && ip[0] == 172 && ip[1] == 16 && ip[2] == 224 && ip[3] >= 2 && ip[3] <= 254;
}

static void wifi_addr(unsigned char *a, int bsd, unsigned port, const unsigned char *ip)
{
    for (unsigned i = 0; i < 16; ++i) a[i] = 0;
    if (bsd) { a[0] = 16; a[1] = 2; } else { a[0] = 2; }
    a[2] = (unsigned char)(port >> 8); a[3] = (unsigned char)port;
    for (unsigned i = 0; ip && i < 4; ++i) a[4 + i] = ip[i];
}

#ifdef CARLINK_HOST_TEST
enum { SOL_SOCKET_WIFI = 1, SO_REUSEADDR_WIFI = 2 };          /* Linux */
#else
enum { SOL_SOCKET_WIFI = 0xffff, SO_REUSEADDR_WIFI = 4 };     /* QNX / BSD */
#endif

static void wifi_close_all(void)
{
    for (unsigned c = 1; c <= WIFI_CHANNELS; ++c)
        if (wifi_fd[c] >= 0) {
            close(wifi_fd[c]);
            wifi_fd[c] = -1;
        }
}

static wifi_socket_fn wifi_sock;
static wifi_addr_fn wifi_bind, wifi_connect_fn;
static wifi_setopt_fn wifi_setopt;
static int wifi_load(void)
{
    static const char *const libraries[] = {"libc.so.3", "libsocket.so.3", "libsocket.so.2", "libsocket.so.1", "libsocket.so",
                                            "libc.so.6"};
    for (unsigned i = 0; i < sizeof(libraries) / sizeof(libraries[0]) &&
                         !(wifi_sock && wifi_bind && wifi_connect_fn && wifi_setopt); ++i) {
        void *handle = dlopen(libraries[i], 0x102);
        if (!handle)
            continue;
        wifi_sock = (wifi_socket_fn)dlsym(handle, "socket");
        wifi_bind = (wifi_addr_fn)dlsym(handle, "bind");
        wifi_connect_fn = (wifi_addr_fn)dlsym(handle, "connect");
        wifi_setopt = (wifi_setopt_fn)dlsym(handle, "setsockopt");
    }
    return wifi_sock && wifi_bind && wifi_connect_fn && wifi_setopt ? 0 : failure("Wi-Fi: no socket functions found", 0);
}

/* One TCP connection attempt from a given local port; returns the socket or -1. *bsd keeps the address layout that works. */
static int wifi_try(const unsigned char *ip, unsigned port, unsigned local, int *bsd)
{
    unsigned char a[16];
    const int one = 1;
    const int s = wifi_sock(2, 1, 0); /* AF_INET, SOCK_STREAM */
    if (s < 0)
        return -1;
    wifi_setopt(s, SOL_SOCKET_WIFI, SO_REUSEADDR_WIFI, &one, sizeof(one));
    wifi_addr(a, *bsd, local, 0);
    if (wifi_bind(s, a, sizeof(a)) != 0) {
        *bsd = !*bsd;
        wifi_addr(a, *bsd, local, 0);
        if (wifi_bind(s, a, sizeof(a)) != 0) {
            close(s);
            return -1;
        }
    }
    wifi_addr(a, *bsd, port, ip);
    if (wifi_connect_fn(s, a, sizeof(a)) != 0) {
        close(s);
        return -1;
    }
    return s;
}

/* --wifi-wait IP[,IP...]: for up to CARLINK_WIFI_WAIT seconds (default 10), find the phone whose CarLife port 7240 is open
 * (it opens after "Start connection" is pressed), print its address and exit 0; exit 1 if none. Nothing is sent. */
static int wifi_wait(const char *list)
{
    unsigned char ips[8][4];
    unsigned count = 0;
    char one[16];
    for (const char *c = list;; ++c) {
        unsigned n = 0;
        while (*c && *c != ',' && n < sizeof(one) - 1)
            one[n++] = *c++;
        one[n] = 0;
        if (count >= 8 || !wifi_ip(one, ips[count]))
            return 2;
        ++count;
        if (*c != ',')
            break;
    }
    unsigned seconds = 10;
    const char *wait = getenv("CARLINK_WIFI_WAIT");
    if (wait && *wait) {
        seconds = 0;
        for (; *wait >= '0' && *wait <= '9' && seconds < 100000; ++wait)
            seconds = seconds * 10 + (unsigned)(*wait - '0');
    }
    if (wifi_load())
        return 4;
    alarm(seconds + 20); /* a connect to a silent address cannot hang the supervisor */
    int bsd = 1;
    for (unsigned round = 0; round <= seconds; ++round) {
        for (unsigned i = 0; i < count; ++i) {
            const int s = wifi_try(ips[i], 7240, WIFI_LOCAL_BASE + 60 + (round * 8 + i) % 400, &bsd);
            if (s >= 0) {
                close(s);
                char text[20];
                const int n = snprintf(text, sizeof(text), "%u.%u.%u.%u\n", ips[i][0], ips[i][1], ips[i][2], ips[i][3]);
                if (n > 0 && write(1, text, (size_t)n) != (ssize_t)n)
                    return 3;
                return 0;
            }
        }
        if (round < seconds)
            usleep(1000000);
    }
    return 1;
}

/* Commands (7240) and video (8240) are required; the others are used when the phone offers them. */
static int wifi_connect(const unsigned char *ip)
{
    if (wifi_load())
        return -1;
    int bsd = 1;
    for (unsigned i = 0; i < WIFI_CHANNELS; ++i) {
        char line[96];
        int s = wifi_try(ip, wifi_ports[i], WIFI_LOCAL_BASE + i, &bsd);
        /* The phone opens its ports only after "Start connection" is pressed: wait up to 30 s for the first one. */
        for (unsigned tries = 0; s < 0 && i == 0 && tries < 30; ++tries) {
            if (tries == 0)
                log_line("Wi-Fi: waiting for the phone to open port 7240 (press Start connection on the phone)");
            usleep(1000000);
            s = wifi_try(ip, wifi_ports[i], WIFI_LOCAL_BASE + 6 + tries, &bsd); /* a fresh local port each try */
        }
        if (s < 0) {
            snprintf(line, sizeof(line), "Wi-Fi: port %u not reachable (errno %d)%s", wifi_ports[i], *__get_errno_ptr(),
                     i < 2 ? "" : "; continuing without it");
            log_line(line);
            if (i < 2)
                break;
            continue;
        }
        wifi_fd[i + 1] = s;
        snprintf(line, sizeof(line), "Wi-Fi: connected to %u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], wifi_ports[i]);
        log_line(line);
    }
    if (wifi_fd[1] < 0 || wifi_fd[2] < 0) {
        wifi_close_all();
        return -1;
    }
    return 0;
}

/* Media audio on its own socket: a stream of [length 4][timestamp 4][type 4] + body records, fed to the same parser. */
static void wifi_audio(const unsigned char *data, unsigned size)
{
    static unsigned char head[12];
    static unsigned head_used, body_left;
    while (size) {
        if (!body_left) {
            unsigned count = 12 - head_used;
            if (count > size)
                count = size;
            memcpy(head + head_used, data, count);
            head_used += count;
            data += count;
            size -= count;
            if (head_used < 12)
                continue;
            head_used = 0;
            body_left = get_be32(head);
            audio_begin(12 + body_left);
            audio_collect(head, 12);
            continue;
        }
        unsigned count = body_left < size ? body_left : size;
        audio_collect(data, count);
        data += count;
        size -= count;
        body_left -= count;
    }
}

static int receive_wifi(struct capture *capture)
{
    static unsigned char buffer[USB_BUFFER_SIZE], cmd[8 + 0xffff], video[MAX_VIDEO_SIZE + 12];
    struct stream streams[] = {{cmd, sizeof(cmd), 8, 0, 8}, {video, sizeof(video), 12, 0, 12}};
    enum session_stage stage = WAIT_MATCH;
    const unsigned char version[] = {8, 1, 16, 0};
    if (send_cmd(-1, 0x00018001, version, sizeof(version)))
        return -1;
    log_line("HU_PROTOCOL_VERSION 1.0 sent over Wi-Fi");
    for (;;) {
        if (__atomic_load_n(&capture->writer_failed, __ATOMIC_ACQUIRE))
            return -1;
        struct wifi_pollfd wait[5];
        unsigned channel[5], count = 0;
        for (unsigned c = 1; c <= 5; ++c)
            if (wifi_fd[c] >= 0) {
                wait[count].fd = wifi_fd[c];
                wait[count].events = 1; /* POLLIN */
                wait[count].revents = 0;
                channel[count++] = c;
            }
        const int ready = poll(wait, count, 1000);
        if (ready < 0 && *__get_errno_ptr() != 4) /* EINTR */
            return failure("Wi-Fi: poll failed, errno", *__get_errno_ptr());
        for (unsigned i = 0; ready > 0 && i < count; ++i) {
            if (!wait[i].revents)
                continue;
            const ssize_t got = read(wait[i].fd, buffer, sizeof(buffer));
            if (got <= 0) {
                char line[80];
                snprintf(line, sizeof(line), "Wi-Fi: channel %u (port %u) closed by the phone", channel[i],
                         wifi_ports[channel[i] - 1]);
                log_line(line);
                return -1;
            }
            int rc = 0;
            if (channel[i] <= 2)
                rc = consume(-1, &stage, &streams[channel[i] - 1], buffer, (unsigned)got, capture);
            else if (channel[i] == 3)
                wifi_audio(buffer, (unsigned)got);
            /* 4 (TTS) and 5 (voice) are read and dropped so the phone never blocks on them */
            if (rc == INNER_RESYNC)
                return failure("Wi-Fi: invalid video record header (a TCP stream cannot resync)", 0);
            if (rc)
                return rc;
        }
    }
}

int carlink_usb_capture(int argc, char **argv)
{
    /* Optional bench input: ... --stream-visible /dev/oapN --input-file /dev/shmem/NAME */
    const char *input_path = 0;
    if (argc == 5 && strcmp(argv[3], "--input-file") == 0)
        input_path = argv[4];
    else if (argc != 3)
        return 2;
    if (argc == 3 && strcmp(argv[1], "--wifi-wait") == 0)
        return wifi_wait(argv[2]);
    int switching = strcmp(argv[1], "--switch") == 0;
    struct capture capture = {0};
    unsigned char phone_ip[4];
    wifi_mode = strcmp(argv[1], "--stream-wifi") == 0; /* ... --stream-wifi 172.16.224.N: the phone on the hotspot */
    if (wifi_mode && !wifi_ip(argv[2], phone_ip))
        return 2;
    capture.live = strcmp(argv[1], "--stream-mdev") == 0;
    if (strcmp(argv[1], "--stream-visible") == 0 || wifi_mode) capture.live = 2;
    {   /* CARLINK_SESSION_SECONDS=0: run until the phone, pipe or caller stops. */
        const char *session = getenv("CARLINK_SESSION_SECONDS");
        capture.continuous = capture.live == 2 && session && !strcmp(session, "0");
    }
    if (!wifi_mode && (switching ? !device_path(argv[2], "/dev/android", 12)
                                 : (!capture.live && strcmp(argv[1], "--capture")) ||
                                   !device_path(argv[2], "/dev/oap", 8)))
        return 2;
    if (input_path && (!capture.live || !input_path_ok(input_path)))
        return 2;

    int input_fd = -1;
    if (input_path) {
        input_fd = open(input_path, 0); /* O_RDONLY; the file must already exist */
        if (input_fd < 0) {
            failure("Input file open failed, errno", *__get_errno_ptr());
            return 14;
        }
    }
    if (capture.live == 2) {
        const char *reader = getenv("CARLINK_READ_THREAD");
        use_rx_thread = reader && !strcmp(reader, "1");
    }
    if (capture.live == 2) {
        const char *play = getenv("CARLINK_AUDIO_PLAY");
        if (play && !strcmp(play, "1")) {
            play_enabled = 1; /* the thread is started right before the receive loop */
        }
        const char *dump = getenv("CARLINK_AUDIO_DUMP");
        if (dump && *dump && input_path_ok(dump)) {
            audio_dump_fd = open(dump, 2); /* O_RDWR: the file must already exist */
            log_line(audio_dump_fd >= 0 ? "Audio dump enabled (raw channel 3)" : "Audio dump file could not be opened");
        }
    }
    alarm(switching ? 5 : capture.live == 2 ? 60 : 45);
    int fd = wifi_mode ? (wifi_connect(phone_ip) ? -1 : wifi_fd[1]) : open(argv[2], 2); /* O_RDWR */
    if (fd < 0) {
        failure("USB device open failed, errno", *__get_errno_ptr());
        if (input_fd >= 0)
            close(input_fd);
        return 10;
    }
    unsigned writer = 0;
    if (capture.live) {
        int rc = pthread_create(&writer, 0, live_writer, &capture);
        if (rc) {
            failure("Live pipe writer startup failed", rc);
            close(fd);
            if (input_fd >= 0)
                close(input_fd);
            return 13;
        }
    }
    struct input_args input_args = {&capture, fd, input_fd};
    if (input_fd >= 0)
        capture.input = &input_args; /* thread starts after the first video frame */
    if (play_enabled)
        play_start();
    int result = switching ? switch_phone(fd) : wifi_mode ? receive_wifi(&capture) : receive(fd, &capture);
    if (play_enabled || audio_dump_fd >= 0 || pcm_records) {
        play_shutdown();
        audio_pcm_log();
    }
    /* No key may reach the descriptor once it is closed. */
    lock_usb_writes();
    __atomic_store_n(&usb_closed, 1, __ATOMIC_RELEASE);
    unlock_usb_writes();
    if (wifi_mode)
        wifi_fd[1] = -1; /* closed just below */
    const int close_rc = close(fd);
    wifi_close_all();
    const int close_error = close_rc != 0 ? *__get_errno_ptr() : 0;
    if (capture.live) {
        __atomic_store_n(&capture.finished, 1, __ATOMIC_RELEASE);
        int rc = pthread_join(writer, 0);
        if (rc || capture.writer_failed) {
            failure("Live pipe writer stopped", rc);
            result = -1;
        }
        if (capture.input_started)
            pthread_join(capture.input_tid, 0); /* polls, so it never blocks the join */
        if (input_fd >= 0)
            close(input_fd);
    }
    if (close_rc != 0) {
        failure("USB device close failed, errno", close_error);
        return 11;
    }
    return result == (switching ? 0 : 1) ? 0 : 12;
}
