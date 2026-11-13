/* Host harness: runs the real carlife_qnx_most_watch.c against a fake DSI library.
 * usage: most-watch-host [FIXTURE]   FIXTURE lines: "inst fkt op payload-hex" (E5 messages, as in the upstream car logs).
 * Prints the decoded "Knob:" lines (without the CarLife action) on stdout; the handshake checks fail with exit 1. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

static int h_open(const char *p, int f, ...) { return open(p, f); }
static int h_read(int fd, void *b, unsigned n) { return (int)read(fd, b, n); }
static int h_write(int fd, const void *b, unsigned n) { return (int)write(fd, b, n); }
static int h_close(int fd) { return close(fd); }
static int mock_errno_value;
int *mock_errno_ptr(void) { return &mock_errno_value; }
#define __get_errno_ptr mock_errno_ptr
#define open h_open
#define read h_read
#define write h_write
#define close h_close
#define dlopen mock_dlopen
#define dlsym mock_dlsym
#define dlerror mock_dlerror
#define printf mock_printf
#define fflush mock_fflush
#define usleep mock_usleep
#define alarm mock_alarm
#define pthread_create mock_pthread_create
#define _exit mock_exit
static unsigned system_calls; static char system_cmd[400];
static int mock_system(const char *c) { ++system_calls; snprintf(system_cmd, sizeof(system_cmd), "%s", c); return 0; }
#define system mock_system
static int exit_calls, exit_code = -1;
static void mock_exit(int c) { ++exit_calls; exit_code = c; }
#include "../../build/live-input-candidate/src/carlife_qnx_most_watch.c"
#undef printf

static char log_buf[1 << 20];
static unsigned log_len;
int mock_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_len += (unsigned)vsnprintf(log_buf + log_len, sizeof(log_buf) - log_len, fmt, ap);
    va_end(ap);
    return 0;
}
int mock_fflush(void *f) { (void)f; return 0; }
char *mock_dlerror(void) { return "mock"; }
static unsigned sleeps, alarm_seconds;
int mock_usleep(unsigned u)
{
    (void)u; ++sleeps;
    const char *after = getenv("MOCK_STOP_AFTER"), *stop = getenv("CARLINK_WATCH_STOP");
    if (after && stop && sleeps == (unsigned)atoi(after)) { FILE *f = fopen(stop, "w"); if (f) fclose(f); }
    return 0;
}
unsigned mock_alarm(unsigned s) { alarm_seconds = s; return 0; }

/* ---- fake DSI library ---- */
static void *fake_vtable[64];
static struct { uint32_t id; uint8_t payload[256]; unsigned size; } requests[8];
static unsigned request_count, stream_ctors, notif_count;
static uint32_t notifs[32];
static unsigned cur_size;
static uint8_t cur_payload[256];
static uint32_t cur_id;
static void *seen_chan, *seen_tcp, *seen_p1, *seen_p2;
static int seen_fd;
static const char *seen_iface, *seen_role;
static int seen_major, seen_minor;
static const char *fixture_path;

static void f_conn_ctor(void *c) { (void)c; }
static int f_conn_add(void *c, void *cl) { (void)c; (void)cl; return 1; }
static void f_cli_ctor(void *cl, const char *i, const char *r, int a, int b)
{
    (void)cl; seen_iface = i; seen_role = r; seen_major = a; seen_minor = b;
}
static void f_set_notif(void *cl, unsigned a) { (void)cl; if (notif_count < 32) notifs[notif_count++] = a; }
static void f_stream_ctor(void *s, void *chan, void *tcp, int fd, unsigned a, unsigned b, unsigned id, const void *p1, const void *p2, int z)
{
    (void)s; (void)a; (void)b; (void)z;
    ++stream_ctors; seen_chan = chan; seen_tcp = tcp; seen_fd = fd; seen_p1 = (void *)p1; seen_p2 = (void *)p2;
    cur_id = id; cur_size = 0;
}
static void f_stream_write(void *s, const void *p, unsigned n) { (void)s; memcpy(cur_payload, p, n); cur_size = n; }
static int f_stream_send(void *s, int x)
{
    (void)s; (void)x;
    if (request_count < 8) {
        requests[request_count].id = cur_id;
        requests[request_count].size = cur_size;
        memcpy(requests[request_count].payload, cur_payload, cur_size);
        ++request_count;
    }
    return 0;
}
static void f_stream_dtor(void *s) { (void)s; }

static void deliver(uint32_t id, const uint8_t *payload, uint32_t len)
{
    char stream[0x80];
    memset(stream, 0, sizeof(stream));
    *(uint32_t *)(stream + 0x08) = len;
    *(const uint8_t **)(stream + 0x10) = payload;
    *(uint32_t *)(stream + 0x48) = id;
    ((void (*)(void *, void *))client_vtable[7])(cli_mem, stream);
}

static unsigned make_msg(uint8_t *out, uint32_t tx, uint32_t tag, uint32_t val, uint8_t fb, uint8_t inst, uint16_t fkt,
                         uint8_t op, const uint8_t *data, uint32_t n)
{
    memset(out, 0, 32);
    out[0] = (uint8_t)tx; out[4] = (uint8_t)tag; out[8] = (uint8_t)val;
    out[12] = 0x00; out[13] = 0x0F; out[14] = 0x00; out[15] = 0x01;
    out[16] = fb; out[17] = inst; out[18] = (uint8_t)(fkt & 0xff); out[19] = (uint8_t)(fkt >> 8); out[20] = op;
    out[28] = (uint8_t)n; out[29] = (uint8_t)(n >> 8);
    if (n) memcpy(out + 32, data, n);
    return 32 + n;
}

static void f_conn_start(void *c)
{
    (void)c;
    ((void (*)(void *))client_vtable[3])(cli_mem);
    uint8_t resp[8] = {7, 0, 0, 0, 0x00, 0x0F, 0, 0};
    deliver(0x80000000u, resp, 6);
    uint8_t comm[4] = {2, 0, 0, 0};
    deliver(0xC0000000u, comm, 4);
    uint8_t m[512], data[64];
    unsigned n;
    /* noise that must be ignored */
    n = make_msg(m, 0, 2, 0, 0x20, 0, 0xC08, 0xC, (const uint8_t *)"\1\2", 2); deliver(0x80000001u, m, n);
    n = make_msg(m, 1, 2, 19, 0xE5, 3, 0xC46, 0xC, (const uint8_t *)"\1\2\3\4\5\6\7\xff", 8); deliver(0x80000001u, m, n);
    n = make_msg(m, 0, 2, 5, 0xE5, 3, 0xC46, 0xC, (const uint8_t *)"\1\2\3\4\5\6\7\xff", 8); deliver(0x80000001u, m, n);
    n = make_msg(m, 0, 1, 0, 0xE5, 3, 0xC46, 0xC, (const uint8_t *)"\1\2\3\4\5\6\7\xff", 8); deliver(0x80000001u, m, n);
    deliver(0x80000001u, m, 10); /* truncated */
    FILE *f = fixture_path ? fopen(fixture_path, "r") : NULL;
    char line[512];
    while (f && fgets(line, sizeof(line), f)) {
        unsigned inst, fkt, op; int off = 0;
        if (line[0] == '#' || sscanf(line, "%x %x %x%n", &inst, &fkt, &op, &off) != 3) continue;
        unsigned len = 0, b; int k; const char *p = line + off;
        while (sscanf(p, " %2x%n", &b, &k) == 1 && len < sizeof(data)) { data[len++] = (uint8_t)b; p += k; }
        n = make_msg(m, 0, 2, 0, 0xE5, (uint8_t)inst, (uint16_t)fkt, (uint8_t)op, data, len);
        deliver(0x80000001u, m, n);
    }
    if (f) fclose(f);
}

void *mock_dlopen(const char *p, int f) { (void)f; return strstr(p, "libsys_dsi_colibry") ? (void *)fake_vtable : NULL; }
void *mock_dlsym(void *l, const char *n)
{
    (void)l;
    if (!strcmp(n, "_ZN14CDSIConnectionC1Ev")) return (void *)f_conn_ctor;
    if (!strcmp(n, "_ZN14CDSIConnection3addER10CDSIClient")) return (void *)f_conn_add;
    if (!strcmp(n, "_ZN14CDSIConnection5startEv")) return (void *)f_conn_start;
    if (!strcmp(n, "_ZN10CDSIClientC1EPKcS1_ii")) return (void *)f_cli_ctor;
    if (!strcmp(n, "_ZN10CDSIClient15setNotificationEj")) return (void *)f_set_notif;
    if (!strncmp(n, "_ZN11CDSIOStreamC1", 18)) return (void *)f_stream_ctor;
    if (!strcmp(n, "_ZN11CDSIOStream5writeEPKvj")) return (void *)f_stream_write;
    if (!strcmp(n, "_ZN11CDSIOStream4sendEi")) return (void *)f_stream_send;
    if (!strcmp(n, "_ZN11CDSIOStreamD1Ev")) return (void *)f_stream_dtor;
    if (!strcmp(n, "_ZTV10CDSIClient")) return (void *)fake_vtable;
    return NULL;
}
int mock_pthread_create(unsigned *t, const void *a, void *(*fn)(void *), void *arg) { (void)a; *t = 1; fn(arg); return 0; }

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main(int argc, char **argv)
{
    fixture_path = argc > 1 ? argv[1] : NULL;
    *(int *)((char *)cli_mem + 0x140) = 99;
    if (argc > 2) { /* emit scenario: most-watch-host FIXTURE EMIT_PATH; the result code goes to stderr */
        char *eargs[] = {"p", "--watch", getenv("WATCH_MODE") ? "0" : "5", "--emit", argv[2]};
        const int rc = carlink_most_watch(5, eargs);
        fprintf(stderr, "RC=%d\nSLEEPS=%u\nALARM=%u\n", rc, sleeps, alarm_seconds);
        for (char *l = strtok(log_buf, "\n"); l; l = strtok(NULL, "\n"))
            if (!strncmp(l, "Emit", 4) || !strncmp(l, "Registered", 10)) fprintf(stderr, "%s\n", l);
        return 0;
    }
    char *args[] = {"p", "--watch", "5"};
    CHECK(carlink_most_watch(3, args) == 0);
    CHECK(exit_calls == 1 && exit_code == 0); /* leaves with _exit, not exit(): the DSI library's exit handlers crash */
    CHECK(emit_fd < 0 && emit_lines == 0);
    /* identity of the service and the fixed memory layout */
    CHECK(!strcmp(seen_iface, "DSYSMOSTServer") && !strcmp(seen_role, "MOSTSubSystem") && seen_major == 4 && seen_minor == 5);
    CHECK(seen_chan == (char *)cli_mem + 0x130 && seen_tcp == (char *)cli_mem + 0x138 && seen_fd == 99);
    CHECK(seen_p1 == (char *)cli_mem + 0x110 && seen_p2 == (char *)cli_mem + 0x118);
    CHECK(*(void ***)cli_mem == client_vtable);
    /* the only requests: registerClient then registerDevCtrls */
    CHECK(request_count == 2 && stream_ctors == 2);
    static const uint8_t expect0[] = {13,0,0,0,'C','a','r','L','i','n','k','W','a','t','c','h',0,0,0,0};
    CHECK(requests[0].id == 0xF && requests[0].size == sizeof(expect0) && !memcmp(requests[0].payload, expect0, sizeof(expect0)));
    static const uint8_t expect1[] = {7,0,0,0, 8,0,0,0, 0x10,0xE5, 0x03,0xE5, 0x05,0xE5, 0x12,0xE5, 0x14,0xE5, 0x01,0xE5, 0x02,0xE5, 0x02,0x15};
    CHECK(requests[1].id == 0x1 && requests[1].size == sizeof(expect1) && !memcmp(requests[1].payload, expect1, sizeof(expect1)));
    /* subscriptions in the vendor order, then the comm state after the registration answer */
    static const uint32_t expect_notifs[] = {0xC0000002u, 0xC0000003u, 0xC0000001u, 0x80000001u, 0x80000002u,
                                             0x80000003u, 0x80000004u, 0x80000005u, 0x80000006u, 0x80000007u, 0xC0000000u};
    CHECK(notif_count == 11 && !memcmp(notifs, expect_notifs, sizeof(expect_notifs)));
    /* nothing else can be sent */
    const unsigned ctors = stream_ctors, nn = notif_count;
    CHECK(dsi_request(0xC, NULL, 0) == -1 && dsi_request(0x2, NULL, 0) == -1 && dsi_request(0x0, NULL, 0) == -1);
    CHECK(dsi_notify(0x1234) == -1 && dsi_notify(0xC0000004u) == -1);
    CHECK(stream_ctors == ctors && notif_count == nn);
    /* run summary, watchdog, duration */
    CHECK(alarm_seconds == 20 && sleeps == 50);
    CHECK(strstr(log_buf, "Registered: clientId=7 deviceId=0x0F00"));
    CHECK(strstr(log_buf, "Comm state 2"));
    CHECK(stats.bad == 1 && stats.skipped == 3);
    /* argument handling */
    char *bad1[] = {"p", "--watch", "0"}, *bad2[] = {"p", "--watch", "121"}, *bad3[] = {"p", "--watch", "5x"}, *bad4[] = {"p", "--send"};
    CHECK(carlink_most_watch(3, bad1) == 2 && carlink_most_watch(3, bad2) == 2 && carlink_most_watch(3, bad3) == 2 && carlink_most_watch(2, bad4) == 2);
    /* fast hide: only MENU and MEDIA going down, only while the picture is shown and the hide file is absent */
    {
        char shown[64], hide[64];
        snprintf(shown, sizeof(shown), "/tmp/most-watch-shown-%d", (int)getpid());
        snprintf(hide, sizeof(hide), "/tmp/most-watch-hide-%d", (int)getpid());
        unlink(shown); unlink(hide);
        FILE *f = fopen(shown, "w"); fclose(f);
        shown_path = shown; hide_path = hide; fasthide_lmc = "/opt/sys/bin/LayerManagerControl";
        struct knob_event menu_down = {0, 0, 1, "MENU"}, menu_up = {0, 0, 0, "MENU"}, media_down = {0, 0, 1, "MEDIA"},
                          back_down = {0, 0, 1, "BACK"}, push_down = {0, 0, 1, "PUSH"}, rotary = {1, 3, 0, "ROTARY"};
        fast_hide(&menu_up); fast_hide(&back_down); fast_hide(&push_down); fast_hide(&rotary);
        CHECK(system_calls == 0 && fasthide_runs == 0);
        fast_hide(&menu_down);
        char want[400];
        snprintf(want, sizeof(want), "( /opt/sys/bin/LayerManagerControl set surface 1510 visibility 0; : > %s ) >/dev/null 2>&1 &", hide);
        CHECK(system_calls == 1 && fasthide_runs == 1 && !strcmp(system_cmd, want));
        f = fopen(hide, "w"); fclose(f); /* the hide file exists: nothing more to do */
        fast_hide(&menu_down); fast_hide(&media_down);
        CHECK(system_calls == 1);
        unlink(hide); unlink(shown); /* the picture is not shown: nothing to hide */
        fast_hide(&menu_down);
        CHECK(system_calls == 1);
        f = fopen(shown, "w"); fclose(f);
        fast_hide(&media_down);
        CHECK(system_calls == 2 && fasthide_runs == 2);
        fasthide_lmc = NULL; /* without the LayerManagerControl path it is off */
        fast_hide(&menu_down);
        CHECK(system_calls == 2);
        unlink(shown); unlink(hide);
        CHECK(lmc_path_ok("/opt/sys/bin/LayerManagerControl") && lmc_path_ok("/tmp/x/fake-lmc_1.sh"));
        CHECK(!lmc_path_ok(NULL) && !lmc_path_ok("") && !lmc_path_ok("lmc") && !lmc_path_ok("/opt/a b") && !lmc_path_ok("/opt/x;reboot")
              && !lmc_path_ok("/opt/$(id)") && !lmc_path_ok("/opt/x'y"));
        shown_path = hide_path = fasthide_lmc = NULL;
    }
    /* canonical event lines for comparison with the upstream decoder */
    for (char *l = strtok(log_buf, "\n"); l; l = strtok(NULL, "\n")) {
        if (strncmp(l, "Knob: ", 6)) continue;
        char *arrow = strstr(l, " -> CarLife");
        if (arrow) *arrow = 0;
        puts(l);
    }
    return 0;
}
