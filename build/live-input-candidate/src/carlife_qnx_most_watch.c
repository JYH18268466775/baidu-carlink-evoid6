/* Read-only observer of the iDrive controller on the MOST bus.
 *
 * Registers a MOST client with MOSTSubSystem.DSYSMOSTServer through the head unit's DSI library, subscribes, and prints
 * what the controller (FBlock 0xE5) and steering wheel (FBlock 0x15) send, plus the CarLife action each event would
 * map to. It sends nothing to the phone and no MOST message of its own; with --emit it only appends the CarLife action
 * lines to the capture program's input file. The only requests it ever issues are the client
 * registration (0xF) and the device-control registration (0x1), the same two the upstream NBT-AAD observer issues;
 * dsi_request() refuses every other id (the server's setCommunicationCtrl, 0xC, can turn MOST off).
 *
 * Message layout and the DSI recipe come from the public NBT-AAD sources (PolyForm Noncommercial 1.0.0,
 * https://github.com/Sq00ky/Supra-iDrive6-Android-Auto); the code here is a fresh freestanding implementation. */
#include <stdint.h>

void *dlopen(const char *, int);
void *dlsym(void *, const char *);
char *dlerror(void);
int printf(const char *, ...);
int fflush(void *);
int strcmp(const char *, const char *);
int usleep(unsigned);
unsigned alarm(unsigned);
void *memset(void *, int, __SIZE_TYPE__);
void *memcpy(void *, const void *, __SIZE_TYPE__);
int pthread_create(unsigned *, const void *, void *(*)(void *), void *);
int open(const char *, int, ...);
int close(int);
int read(int, void *, unsigned);
int write(int, const void *, unsigned);
int *__get_errno_ptr(void);
char *getenv(const char *);
int clock_gettime(int, void *);
void _exit(int);
int system(const char *);

#ifndef CARLINK_SHMEM_DIR
#define CARLINK_SHMEM_DIR "/dev/shmem/"
#endif

enum { LOAD_NOW_GLOBAL = 0x102 };
enum { MAX_SECONDS = 120, DEFAULT_SECONDS = 30 };

/* ---- MOST message parsing (little endian; header 32 bytes) -------------------------------------------------------- */
struct most_view {
    uint32_t tx_type, var_tag, var_val, hdr_tag;
    uint16_t dst, src, fkt;
    uint8_t fblock, inst, op;
    const uint8_t *data;
    uint32_t len;
};

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int parse_view(const uint8_t *p, uint32_t len, struct most_view *m)
{
    if (!p || len < 32)
        return -1;
    m->tx_type = rd32(p);
    m->var_tag = rd32(p + 4);
    m->var_val = rd32(p + 8);
    m->dst = rd16(p + 12);
    m->src = rd16(p + 14);
    m->fblock = p[16];
    m->inst = p[17];
    m->fkt = rd16(p + 18);
    m->op = p[20];
    m->hdr_tag = rd32(p + 24);
    const uint32_t n = rd32(p + 28);
    if (n > len - 32)
        return -1;
    m->data = p + 32;
    m->len = n;
    return 0;
}

/* Only plain messages are decoded: segment (txType 1, varVal 19..21) and transmit-status (varVal 4..9) frames are not. */
static int is_plain_message(const struct most_view *m)
{
    if (m->var_tag != 2)
        return 0;
    if (m->tx_type == 1 && m->var_val >= 19 && m->var_val <= 21)
        return 0;
    return !(m->var_val >= 4 && m->var_val <= 9);
}

/* ---- controller decoding ------------------------------------------------------------------------------------------ */
enum { SLOTS = 7 };
static const char *const slot_names[SLOTS] = {"MENU", "BACK", "OPTIONS", "COM", "MEDIA", "NAV", "MAP"};

struct knob_event {
    int rotary; /* 1: delta is a rotary step count */
    int delta;
    int down;
    const char *what;
};

struct decoder {
    uint8_t inst, ccs_inst;
    int pos, ccs_pos; /* -1: no baseline yet */
    int tilt, ccs_tilt; /* 0 none, else 1 up 2 right 3 down 4 left */
    int push, ccs_push;
    int slot[SLOTS], ccs_slot[SLOTS];
};

static void decoder_init(struct decoder *d)
{
    memset(d, 0, sizeof(*d));
    d->inst = 0x03;
    d->ccs_inst = 0x01;
    d->pos = d->ccs_pos = -1;
}

static const char *tilt_name(int t)
{
    return t == 1 ? "TILT_UP" : t == 2 ? "TILT_RIGHT" : t == 3 ? "TILT_DOWN" : "TILT_LEFT";
}

static int add_event(struct knob_event *out, int max, int *k, int rotary, int delta, int down, const char *what)
{
    if (*k >= max)
        return 0;
    out[*k].rotary = rotary;
    out[*k].delta = delta;
    out[*k].down = down;
    out[*k].what = what;
    ++*k;
    return 1;
}

/* 12-step clock of the 35up controller: 1..2 and 12 up, 3..5 right, 6..8 down, 9..11 left. */
static int clock_tilt(unsigned dir)
{
    if (dir == 0 || dir > 12)
        return 0;
    if (dir == 12 || dir <= 2)
        return 1;
    return dir <= 5 ? 2 : dir <= 8 ? 3 : 4;
}

static int step_delta(int now, int before) { return (int)(int16_t)(uint16_t)(now - before); }

static void set_tilt(struct knob_event *out, int max, int *k, int *held, int now)
{
    if (now == *held)
        return;
    if (*held)
        add_event(out, max, k, 0, 0, 0, tilt_name(*held));
    if (now)
        add_event(out, max, k, 0, 0, 1, tilt_name(now));
    *held = now;
}

static int decode_ccs(struct decoder *d, uint8_t inst, uint16_t fkt, const uint8_t *p, uint32_t n,
                      struct knob_event *out, int max)
{
    int k = 0;
    if (fkt == 0xC10) {
        const uint32_t need = inst == 1 ? 7 : 8;
        if (n < need)
            return 0;
        const int pos = (p[3] << 8) | p[4];
        if (p[2] == 0xFF || p[need - 2] != 0x1E) {
            d->ccs_pos = -1;
            return 0;
        }
        if (p[need - 1] != 0 || d->ccs_pos < 0) {
            d->ccs_pos = pos;
            return 0;
        }
        const int delta = step_delta(pos, d->ccs_pos);
        d->ccs_pos = pos;
        if (delta)
            add_event(out, max, &k, 1, delta, 0, "ROTARY");
        return k;
    }
    if (n < 7 || p[2] == 0xFF)
        return 0;
    const uint8_t status = p[3], dir = p[4], cluster = p[5], sensor = p[6];
    if (status > 2)
        return 0;
    const int down = status != 0;
    if (cluster == 0x1E) {
        if (sensor == 1 && down != d->ccs_push) {
            d->ccs_push = down;
            add_event(out, max, &k, 0, 0, down, "PUSH");
        }
    } else if (cluster == 0x1D) {
        const int now = (down && dir) ? (dir == 1 ? 1 : dir == 2 ? 2 : dir == 4 ? 3 : dir == 8 ? 4 : 0) : 0;
        set_tilt(out, max, &k, &d->ccs_tilt, now);
    } else if (cluster == 0x00) {
        for (int b = 0; b < SLOTS; ++b)
            if (sensor == (1u << b) && down != d->ccs_slot[b]) {
                d->ccs_slot[b] = down;
                add_event(out, max, &k, 0, 0, down, slot_names[b]);
            }
    }
    return k;
}

static int decode_message(struct decoder *d, const struct most_view *m, struct knob_event *out, int max)
{
    int k = 0;
    if (m->fblock != 0xE5 || m->op != 0xC || !m->data)
        return 0;
    if ((m->fkt == 0xC10 || m->fkt == 0xC20) && m->inst == d->ccs_inst)
        return decode_ccs(d, m->inst, m->fkt, m->data, m->len, out, max);
    if (m->inst != d->inst)
        return 0;
    const uint8_t *p = m->data;
    if (m->fkt == 0xC46 && m->len >= 5) {
        const int pos = (p[1] << 8) | p[2];
        if (d->pos >= 0 && pos != d->pos) {
            const int delta = step_delta(pos, d->pos);
            if (delta)
                add_event(out, max, &k, 1, delta, 0, "ROTARY");
        }
        d->pos = pos;
        set_tilt(out, max, &k, &d->tilt, clock_tilt(p[4]));
        const int push = p[3] != 0;
        if (push != d->push) {
            d->push = push;
            add_event(out, max, &k, 0, 0, push, "PUSH");
        }
    } else if ((m->fkt == 0xC47 || m->fkt == 0xC4F) && m->len >= 1) {
        for (uint32_t i = 0; i < m->len && i < SLOTS; ++i) {
            const int pressed = p[i] != 0;
            if (pressed != d->slot[i]) {
                d->slot[i] = pressed;
                add_event(out, max, &k, 0, 0, pressed, slot_names[i]);
            }
        }
    }
    return k;
}

/* The line a bridge would append to the CarLife input file for this event; 0 when it sends nothing. */
static const char *carlife_action(const struct knob_event *e, char *buf, unsigned size)
{
    if (e->rotary) {
        int n = e->delta;
        if (n > 32) n = 32;
        if (n < -32) n = -32;
        unsigned i = 0;
        if (size < 5)
            return 0;
        buf[i++] = n < 0 ? '-' : '+';
        if (n < 0) n = -n;
        if (n >= 10)
            buf[i++] = (char)('0' + n / 10);
        buf[i++] = (char)('0' + n % 10);
        buf[i] = 0;
        return buf;
    }
    if (!e->down)
        return 0;
    if (!strcmp(e->what, "PUSH")) return "OK";
    if (!strcmp(e->what, "BACK")) return "BACK";
    if (!strcmp(e->what, "TILT_UP")) return "UP";
    if (!strcmp(e->what, "TILT_DOWN")) return "DOWN";
    if (!strcmp(e->what, "TILT_LEFT")) return "LEFT";
    if (!strcmp(e->what, "TILT_RIGHT")) return "RIGHT";
    return 0;
}

/* ---- DSI plumbing (head unit libsys_dsi_colibry, C++ ABI called from C) -------------------------------------------- */
typedef void (*conn_ctor_fn)(void *);
typedef int (*conn_add_fn)(void *, void *);
typedef void (*conn_start_fn)(void *);
typedef void (*cli_ctor_fn)(void *, const char *, const char *, int, int);
typedef void (*set_notif_fn)(void *, unsigned);
typedef void (*stream_ctor_fn)(void *, void *, void *, int, unsigned, unsigned, unsigned, const void *, const void *, int);
typedef void (*stream_write_fn)(void *, const void *, unsigned);
typedef int (*stream_send_fn)(void *, int);
typedef void (*stream_dtor_fn)(void *);

static struct {
    conn_ctor_fn conn_ctor;
    conn_add_fn conn_add;
    conn_start_fn conn_start;
    cli_ctor_fn cli_ctor;
    set_notif_fn set_notif;
    stream_ctor_fn stream_ctor;
    stream_write_fn stream_write;
    stream_send_fn stream_send;
    stream_dtor_fn stream_dtor;
    void *vtable;
} dsi;

enum {
    REQ_REGISTER_DEVCTRLS = 0x1,
    REQ_REGISTER_CLIENT = 0xF,
    INFO_CLIENT_RESPONSE = 0x80000000u,
    INFO_MESSAGE = 0x80000001u,
    ATTR_COMM_STATE = 0xC0000000u
};
static const uint32_t subscriptions[] = {
    0xC0000002u, 0xC0000003u, 0xC0000001u, 0x80000001u, 0x80000002u,
    0x80000003u, 0x80000004u, 0x80000005u, 0x80000006u, 0x80000007u
};
static const uint16_t devctrl_keys[] = {0xE510, 0xE503, 0xE505, 0xE512, 0xE514, 0xE501, 0xE502, 0x1502};

static unsigned long long conn_mem[(0x8c + 64) / 8 + 1];
static unsigned long long cli_mem[(0x14c + 64) / 8 + 1];
static void *client_vtable[32];
static volatile int attached, registered;
static uint32_t client_id, comm_state;

static struct decoder decoder;
static struct {
    unsigned messages, controller, steering, skipped, bad, events, raw_logged;
} stats;
enum { RAW_LOG_LIMIT = 40 };

/* Optional output: each CarLife action is written as one line to the capture program's input file
 * (CARLINK_INPUT_FILE). The file must already exist and be empty: this descriptor writes from offset 0 and the reader
 * follows it, so nothing else may append to the file while the watch runs. */
static int emit_fd = -1;
static unsigned emit_lines, emit_failed, emit_hidden;
/* CARLINK_EMIT_HIDE=/dev/shmem/NAME: while that file exists nothing is emitted (iDrive belongs to the car: reverse
 * gear, park assist, a popup). CARLINK_WATCH_STOP=/dev/shmem/NAME: the file's appearance ends a --watch 0 run. */
static const char *hide_path, *stop_path;

static int emit_path_ok(const char *path)
{
    static const char prefix[] = CARLINK_SHMEM_DIR;
    unsigned n = 0;
    while (prefix[n]) {
        if (path[n] != prefix[n])
            return 0;
        ++n;
    }
    const char *name = path + n;
    unsigned length = 0;
    for (; name[length]; ++length) {
        const char c = name[length];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return 0;
    }
    return length > 0 && length <= 48 && name[0] != '.';
}

static int path_present(const char *path)
{
    if (!path)
        return 0;
    const int fd = open(path, 0);
    if (fd < 0)
        return 0;
    close(fd);
    return 1;
}

static int emit_open(const char *path)
{
    if (!emit_path_ok(path)) {
        printf("Emit file must be %sNAME with NAME of [A-Za-z0-9._-], at most 48 characters\n", CARLINK_SHMEM_DIR);
        return 2;
    }
    const int fd = open(path, 2); /* O_RDWR: the file has to exist */
    if (fd < 0) {
        printf("Emit file open failed, errno=%d\n", *__get_errno_ptr());
        return 6;
    }
    char probe;
    if (read(fd, &probe, 1) != 0) {
        printf("Emit file is not empty or unreadable; refusing so no stale lines are replayed\n");
        close(fd);
        return 7;
    }
    emit_fd = fd;
    printf("Emitting CarLife actions to %s\n", path);
    return 0;
}

static void emit_action(const char *action)
{
    char line[16];
    unsigned n = 0;
    if (emit_fd < 0 || !action)
        return;
    if (path_present(hide_path)) {
        ++emit_hidden;
        return;
    }
    for (; action[n] && n < sizeof(line) - 2; ++n)
        line[n] = action[n];
    line[n++] = '\n';
    if (write(emit_fd, line, n) == (int)n)
        ++emit_lines;
    else
        ++emit_failed;
}

/* Fast hide: the iDrive application tells the supervisor about a MENU or MEDIA press only after its own screen change, which
 * showed as a delay of about a second before the CarLife picture went away. This watcher sees the key at once, so it hides
 * the picture itself: one background shell that hides surface 1510 (LayerManagerControl) and then creates the hide file,
 * exactly what the supervisor does a moment later when the application's message arrives (it then finds nothing left to
 * do but the iDrive layer commands). Only with CARLINK_WATCH_SHOWN (the file that exists while the picture is on the
 * panel) and CARLINK_FAST_HIDE_LMC (the LayerManagerControl path) set, only while the picture is shown and the hide file
 * is absent, and only for MENU and MEDIA. The command text is built from validated names only. */
static const char *shown_path, *fasthide_lmc;
static unsigned fasthide_runs;

static int lmc_path_ok(const char *path)
{
    unsigned n = 0;
    if (!path || path[0] != '/')
        return 0;
    for (; path[n]; ++n) {
        const char c = path[n];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' || c == '.' || c == '_' || c == '-'))
            return 0;
    }
    return n > 1 && n < 100;
}

static unsigned append_text(char *out, unsigned n, unsigned size, const char *text)
{
    for (; *text && n + 1 < size; ++text)
        out[n++] = *text;
    out[n] = 0;
    return n;
}

/* Wall clock (UTC) to the millisecond for the knob log, so a key can be lined up with the receiver's "Latency:" lines. */
static unsigned watch_ms(void)
{
    long ts[2] = {0, 0};
    if (clock_gettime(0, ts) != 0) /* CLOCK_REALTIME; timespec is two longs on the head unit */
        return 0;
    return ((unsigned)ts[0] % 86400u) * 1000u + (unsigned)ts[1] / 1000000u;
}
#define MS_FMT "%02u:%02u:%02u.%03u"
#define MS_ARGS(m) (m) / 3600000u, (m) / 60000u % 60u, (m) / 1000u % 60u, (m) % 1000u

static void fast_hide(const struct knob_event *e)
{
    if (!fasthide_lmc || !shown_path || !hide_path || e->rotary || !e->down)
        return;
    if (strcmp(e->what, "MENU") && strcmp(e->what, "MEDIA"))
        return;
    if (!path_present(shown_path) || path_present(hide_path))
        return;
    char cmd[360];
    unsigned n = append_text(cmd, 0, sizeof(cmd), "( ");
    n = append_text(cmd, n, sizeof(cmd), fasthide_lmc);
    n = append_text(cmd, n, sizeof(cmd), " set surface 1510 visibility 0; : > ");
    n = append_text(cmd, n, sizeof(cmd), hide_path);
    n = append_text(cmd, n, sizeof(cmd), " ) >/dev/null 2>&1 &");
    ++fasthide_runs;
    const unsigned now = watch_ms();
    printf("Fast hide: %s key, hiding the CarLife picture at once (" MS_FMT " UTC)\n", e->what, MS_ARGS(now));
    fflush(0);
    system(cmd);
}

static int dsi_request(uint32_t id, const uint8_t *payload, unsigned size)
{
    if (id != REQ_REGISTER_CLIENT && id != REQ_REGISTER_DEVCTRLS) {
        printf("Refused DSI request 0x%x: only registration is allowed\n", (unsigned)id);
        return -1;
    }
    char *const cli = (char *)cli_mem;
    unsigned long long storage[(0xBC + 64) / 8 + 1];
    memset(storage, 0, sizeof(storage));
    void *stream = storage;
    dsi.stream_ctor(stream, cli + 0x130, cli + 0x138, *(int *)(cli + 0x140), 0x100u, 7u, id, cli + 0x110, cli + 0x118, 0);
    if (size)
        dsi.stream_write(stream, payload, size);
    const int rc = dsi.stream_send(stream, -1);
    dsi.stream_dtor(stream);
    printf("DSI request 0x%x, %u bytes: rc=%d\n", (unsigned)id, size, rc);
    return rc;
}

static int dsi_notify(uint32_t attr)
{
    int allowed = attr == ATTR_COMM_STATE;
    for (unsigned i = 0; i < sizeof(subscriptions) / sizeof(subscriptions[0]); ++i)
        allowed = allowed || attr == subscriptions[i];
    if (!allowed) {
        printf("Refused DSI notification 0x%x\n", (unsigned)attr);
        return -1;
    }
    dsi.set_notif(cli_mem, attr);
    return 0;
}

static unsigned put_u32(uint8_t *v, unsigned n, uint32_t x)
{
    while (n % 4)
        v[n++] = 0;
    for (int i = 0; i < 4; ++i)
        v[n++] = (uint8_t)(x >> (8 * i));
    return n;
}

static unsigned build_register_client(uint8_t *v, const char *name)
{
    unsigned length = 0;
    while (name[length])
        ++length;
    unsigned n = put_u32(v, 0, length + 1);
    memcpy(v + n, name, length + 1);
    n += length + 1;
    while (n % 4)
        v[n++] = 0;
    return n;
}

static unsigned build_register_devctrls(uint8_t *v, uint32_t client)
{
    unsigned n = put_u32(v, 0, client);
    n = put_u32(v, n, (uint32_t)(sizeof(devctrl_keys) / sizeof(devctrl_keys[0])));
    for (unsigned i = 0; i < sizeof(devctrl_keys) / sizeof(devctrl_keys[0]); ++i) {
        v[n++] = (uint8_t)(devctrl_keys[i] & 0xff);
        v[n++] = (uint8_t)(devctrl_keys[i] >> 8);
    }
    while (n % 4)
        v[n++] = 0;
    return n;
}

static void log_raw(const struct most_view *m)
{
    printf("MOST %04X->%04X %02X/%02X fkt=%03X op=%X len=%u:", m->src, m->dst, m->fblock, m->inst, m->fkt, m->op, m->len);
    for (uint32_t i = 0; i < m->len && i < 16; ++i)
        printf(" %02x", m->data[i]);
    printf("\n");
}

static void on_message(const uint8_t *p, uint32_t len)
{
    struct most_view m;
    ++stats.messages;
    if (parse_view(p, len, &m)) {
        ++stats.bad;
        return;
    }
    if (!is_plain_message(&m)) {
        ++stats.skipped;
        return;
    }
    if (m.fblock != 0xE5 && m.fblock != 0x15)
        return;
    if (m.fblock == 0xE5) ++stats.controller; else ++stats.steering;
    if (stats.raw_logged < RAW_LOG_LIMIT) {
        ++stats.raw_logged;
        log_raw(&m);
    }
    struct knob_event events[16];
    const int count = decode_message(&decoder, &m, events, 16);
    for (int i = 0; i < count; ++i) {
        char buf[8];
        const char *action = carlife_action(&events[i], buf, sizeof(buf));
        ++stats.events;
        fast_hide(&events[i]);
        emit_action(action);
        const unsigned now = watch_ms();
        if (events[i].rotary)
            printf("Knob: %s delta=%d -> CarLife %s at " MS_FMT "\n", events[i].what, events[i].delta,
                   action ? action : "(none)", MS_ARGS(now));
        else
            printf("Knob: %s %s -> CarLife %s at " MS_FMT "\n", events[i].what, events[i].down ? "down" : "up",
                   action ? action : "(none)", MS_ARGS(now));
    }
}

static void on_attached(void *self)
{
    (void)self;
    uint8_t payload[64];
    attached = 1;
    printf("DSI attached to MOSTSubSystem.DSYSMOSTServer; registering client\n");
    fflush(0);
    dsi_request(REQ_REGISTER_CLIENT, payload, build_register_client(payload, "CarLinkWatch"));
    for (unsigned i = 0; i < sizeof(subscriptions) / sizeof(subscriptions[0]); ++i)
        dsi_notify(subscriptions[i]);
}

static void on_detached(void *self)
{
    (void)self;
    attached = 0;
    printf("DSI detached from the server\n");
}

static void on_data(void *self, void *istream)
{
    (void)self;
    if (!istream)
        return;
    const char *s = (const char *)istream;
    const uint32_t id = *(const uint32_t *)(s + 0x48);
    const uint8_t *payload = *(const uint8_t *const *)(s + 0x10);
    const uint32_t len = *(const uint32_t *)(s + 0x08);
    if (id == INFO_MESSAGE) {
        on_message(payload, len);
    } else if (id == INFO_CLIENT_RESPONSE && len >= 6 && !registered) {
        client_id = rd32(payload);
        printf("Registered: clientId=%u deviceId=0x%04X%s\n", (unsigned)client_id, rd16(payload + 4),
               client_id == 0xffffffffu ? " (REFUSED)" : "");
        if (client_id == 0xffffffffu)
            return;
        registered = 1;
        dsi_notify(ATTR_COMM_STATE);
        uint8_t request[64];
        dsi_request(REQ_REGISTER_DEVCTRLS, request, build_register_devctrls(request, client_id));
    } else if (id == ATTR_COMM_STATE && len >= 4) {
        comm_state = rd32(payload);
        printf("Comm state %u\n", (unsigned)comm_state);
    }
}

static void *connection_main(void *arg)
{
    (void)arg;
    dsi.conn_start(conn_mem);
    return 0;
}

static void *need(void *library, const char *name)
{
    void *symbol = dlsym(library, name);
    printf("  dlsym %s %s\n", name, symbol ? "ok" : "MISSING");
    return symbol;
}

static int dsi_open(void)
{
    static const char *const paths[] = {
        "/opt/sys/lib/libsys_dsi_colibry.cpp-ne.so.8",
        "/fs/sda0/opt/sys/lib/libsys_dsi_colibry.cpp-ne.so.8",
        "libsys_dsi_colibry.cpp-ne.so.8"
    };
    void *library = 0;
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]) && !library; ++i) {
        library = dlopen(paths[i], LOAD_NOW_GLOBAL);
        printf("dlopen %s %s\n", paths[i], library ? "ok" : "failed");
    }
    if (!library)
        return 40;
    dsi.conn_ctor = (conn_ctor_fn)need(library, "_ZN14CDSIConnectionC1Ev");
    dsi.conn_add = (conn_add_fn)need(library, "_ZN14CDSIConnection3addER10CDSIClient");
    dsi.conn_start = (conn_start_fn)need(library, "_ZN14CDSIConnection5startEv");
    dsi.cli_ctor = (cli_ctor_fn)need(library, "_ZN10CDSIClientC1EPKcS1_ii");
    dsi.set_notif = (set_notif_fn)need(library, "_ZN10CDSIClient15setNotificationEj");
    dsi.stream_ctor = (stream_ctor_fn)need(library, "_ZN11CDSIOStreamC1ER11CDSIChannelP13CDSITCPSocketiN3DSI11RequestTypeENS4_7CommandEjRK8SPartyIDS9_i");
    dsi.stream_write = (stream_write_fn)need(library, "_ZN11CDSIOStream5writeEPKvj");
    dsi.stream_send = (stream_send_fn)need(library, "_ZN11CDSIOStream4sendEi");
    dsi.stream_dtor = (stream_dtor_fn)need(library, "_ZN11CDSIOStreamD1Ev");
    dsi.vtable = need(library, "_ZTV10CDSIClient");
    if (!dsi.conn_ctor || !dsi.conn_add || !dsi.conn_start || !dsi.cli_ctor || !dsi.set_notif || !dsi.stream_ctor ||
        !dsi.stream_write || !dsi.stream_send || !dsi.stream_dtor || !dsi.vtable) {
        printf("required DSI symbols missing\n");
        return 41;
    }
    dsi.conn_ctor(conn_mem);
    dsi.cli_ctor(cli_mem, "DSYSMOSTServer", "MOSTSubSystem", 4, 5);
    memcpy(client_vtable, (const char *)dsi.vtable + 8, sizeof(client_vtable));
    client_vtable[3] = (void *)on_attached;
    client_vtable[4] = (void *)on_detached;
    client_vtable[7] = (void *)on_data;
    *(void ***)cli_mem = client_vtable;
    printf("CDSIConnection::add -> %d\n", dsi.conn_add(conn_mem, cli_mem));
    return 0;
}

int carlink_most_watch(int argc, char **argv)
{
    unsigned seconds = DEFAULT_SECONDS;
    const char *emit = 0;
    const int ok = argc == 1 || (argc >= 3 && !strcmp(argv[1], "--watch") && (argc == 3 || (argc == 5 && !strcmp(argv[3], "--emit"))));
    if (!ok) {
        printf("Usage: [--watch SECONDS(1-%d) [--emit %sNAME]]\n", MAX_SECONDS, CARLINK_SHMEM_DIR);
        return 2;
    }
    if (argc >= 3) {
        seconds = 0;
        for (const char *c = argv[2]; *c; ++c) {
            if (*c < '0' || *c > '9' || seconds > MAX_SECONDS) {
                seconds = 0;
                break;
            }
            seconds = seconds * 10 + (unsigned)(*c - '0');
        }
        if (seconds < 1 || seconds > MAX_SECONDS) {
            if (seconds || argc != 5 || strcmp(argv[2], "0") != 0) {
                printf("Usage: [--watch SECONDS(1-%d) [--emit %sNAME]]\n", MAX_SECONDS, CARLINK_SHMEM_DIR);
                return 2;
            }
            /* --watch 0 --emit FILE: run until CARLINK_WATCH_STOP appears (checked below). */
        }
    }
    {
        const char *hide = getenv("CARLINK_EMIT_HIDE"), *stop = getenv("CARLINK_WATCH_STOP");
        if ((hide && *hide && !emit_path_ok(hide)) || (stop && *stop && !emit_path_ok(stop))) {
            printf("CARLINK_EMIT_HIDE / CARLINK_WATCH_STOP must be %sNAME\n", CARLINK_SHMEM_DIR);
            return 2;
        }
        hide_path = hide && *hide ? hide : 0;
        {
            const char *shown = getenv("CARLINK_WATCH_SHOWN"), *lmc = getenv("CARLINK_FAST_HIDE_LMC");
            if (hide_path && shown && *shown && emit_path_ok(shown) && lmc_path_ok(lmc)) {
                shown_path = shown;
                fasthide_lmc = lmc;
            }
        }
        stop_path = stop && *stop ? stop : 0;
        if (argc >= 3 && !seconds && !stop_path) {
            printf("--watch 0 needs CARLINK_WATCH_STOP\n");
            return 2;
        }
    }
    if (argc == 5) {
        emit = argv[4];
        const int rc = emit_open(emit);
        if (rc)
            return rc;
    }
    decoder_init(&decoder);
    if (seconds)
        printf("Read-only MOST watch for %u s. Nothing is sent to the phone. Turn, push and tilt the iDrive controller and press the hard keys.\n", seconds);
    fflush(0);
    if (seconds)
        alarm(seconds + 15); /* watchdog: the connection thread can block forever */
    else
        printf("Read-only MOST watch until the stop file appears.\n"), fflush(0);
    int status = dsi_open();
    if (status)
        return status;
    unsigned thread = 0;
    if (pthread_create(&thread, 0, connection_main, 0)) {
        printf("connection thread failed\n");
        return 42;
    }
    if (seconds) {
        for (unsigned tick = 0; tick < seconds * 10; ++tick)
            usleep(100000);
    } else {
        while (!path_present(stop_path))
            usleep(100000);
    }
    printf("Summary: attached=%d registered=%d comm=%u messages=%u controller=%u steering=%u skipped=%u bad=%u events=%u\n",
           attached, registered, (unsigned)comm_state, stats.messages, stats.controller, stats.steering, stats.skipped,
           stats.bad, stats.events);
    if (emit_fd >= 0) {
        printf("Emitted %u action lines, %u write failures, %u held back while iDrive was hidden, %u fast hides\n",
               emit_lines, emit_failed, emit_hidden, fasthide_runs);
        close(emit_fd);
        emit_fd = -1;
    }
    const int result = registered ? 0 : 43;
    fflush(0);
    /* Leave without exit(): the entry code would run the exit handlers of the head unit's DSI libraries while their
     * connection thread is still inside conn_start(), and that crashed in libstdc++ (SIGSEGV in __exchange_and_add) every
     * time the watcher was stopped in the car. _exit() just ends the process, as the stop file asked. */
    _exit(result);
    return result; /* only the host test's stand-in for _exit() comes back */
}
