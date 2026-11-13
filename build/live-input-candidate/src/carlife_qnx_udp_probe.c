/* Read-only UDP listener for the wireless CarLife check: binds UDP 0.0.0.0:PORT and logs every datagram it receives
 * (sender address, length, first bytes). It never sends anything. Usage: --listen PORT SECONDS (1..600).
 * The head unit's libc has no socket functions (they live in libsocket), so they are loaded at run time like in ctl. */
#ifdef CARLINK_PROBE_HOST
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <dlfcn.h>
#else
void *dlopen(const char *, int);
void *dlsym(void *, const char *);
int printf(const char *, ...);
int fflush(void *);
int strcmp(const char *, const char *);
void *memset(void *, int, __SIZE_TYPE__);
int poll(void *, unsigned, int);
int close(int);
unsigned alarm(unsigned);
int *__get_errno_ptr(void);
#endif

enum { QNX_LOAD_NOW_GLOBAL = 0x102, AF_INET_ = 2, SOCK_DGRAM_ = 2, POLLIN_ = 1, DUMP_BYTES = 48, MAX_SECONDS = 600,
       MAX_LOGGED = 200 };

typedef int (*socket_fn)(int, int, int);
typedef int (*bind_fn)(int, const void *, unsigned);
typedef __PTRDIFF_TYPE__ (*recvfrom_fn)(int, void *, __SIZE_TYPE__, int, void *, unsigned *);
struct pollfd_ { int fd; short events, revents; };

static int load_socket_api(socket_fn *sock, bind_fn *bind_, recvfrom_fn *recv_)
{
#ifdef CARLINK_PROBE_HOST
    *sock = socket; *bind_ = (bind_fn)bind; *recv_ = (recvfrom_fn)recvfrom;
    return 0;
#else
    static const char *const libraries[] = {"libc.so.3", "libsocket.so.3", "libsocket.so.2", "libsocket.so.1", "libsocket.so"};
    for (unsigned i = 0; i < sizeof(libraries) / sizeof(libraries[0]); ++i) {
        void *handle = dlopen(libraries[i], QNX_LOAD_NOW_GLOBAL);
        if (!handle)
            continue;
        *sock = (socket_fn)dlsym(handle, "socket");
        *bind_ = (bind_fn)dlsym(handle, "bind");
        *recv_ = (recvfrom_fn)dlsym(handle, "recvfrom");
        if (*sock && *bind_ && *recv_) {
            printf("probe: socket functions from %s\n", libraries[i]);
            return 0;
        }
    }
    return -1;
#endif
}

/* QNX (BSD) puts a length byte first, Linux does not; the system accepts one of them. */
static int bind_any(int fd, bind_fn bind_, unsigned port, const char **layout)
{
    unsigned char a[16];
    for (int i = 0; i < 16; ++i) a[i] = 0;
#ifndef CARLINK_PROBE_HOST
    a[0] = 16; a[1] = AF_INET_;
    a[2] = (unsigned char)(port >> 8); a[3] = (unsigned char)port;
    if (bind_(fd, a, sizeof(a)) == 0) { *layout = "bsd"; return 0; }
    for (int i = 0; i < 16; ++i) a[i] = 0;
#endif
    a[0] = AF_INET_; a[1] = 0;
    a[2] = (unsigned char)(port >> 8); a[3] = (unsigned char)port;
    if (bind_(fd, a, sizeof(a)) == 0) { *layout = "linux"; return 0; }
    return -1;
}

static unsigned parse_number(const char *text, unsigned limit)
{
    unsigned value = 0;
    if (!*text)
        return 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9' || value > limit)
            return 0;
        value = value * 10 + (unsigned)(*text - '0');
    }
    return value <= limit ? value : 0;
}

static void show(const unsigned char *from, const unsigned char *data, unsigned size, unsigned number)
{
    /* BSD layout: [len][family][port 2][addr 4]; Linux layout: [family 2][port 2][addr 4]. */
    const int bsd = from[1] == AF_INET_ && from[0] != AF_INET_;
    const unsigned port = (unsigned)from[2] << 8 | from[3];
    printf("probe: #%u from %u.%u.%u.%u:%u, %u bytes:", number, from[4], from[5], from[6], from[7], port, size);
    for (unsigned i = 0; i < size && i < DUMP_BYTES; ++i)
        printf(" %02x", data[i]);
    printf("%s\n", bsd ? "" : "");
    fflush(0);
}

int carlink_udp_probe(int argc, char **argv)
{
    unsigned port = 0, seconds = 0;
    if (argc == 4 && strcmp(argv[1], "--listen") == 0) {
        port = parse_number(argv[2], 65535);
        seconds = parse_number(argv[3], MAX_SECONDS);
    }
    if (port < 1024 || !seconds) {
        printf("Usage: --listen PORT(1024-65535) SECONDS(1-%d)\n", MAX_SECONDS);
        return 2;
    }
    socket_fn sock; bind_fn bind_; recvfrom_fn recv_;
    if (load_socket_api(&sock, &bind_, &recv_)) {
        printf("probe: no socket functions found\n");
        return 4;
    }
    const int fd = sock(AF_INET_, SOCK_DGRAM_, 0);
    if (fd < 0) {
        printf("probe: socket failed\n");
        return 5;
    }
    const char *layout = "";
    if (bind_any(fd, bind_, port, &layout)) {
        printf("probe: bind UDP %u failed (port in use?)\n", port);
        close(fd);
        return 3;
    }
    printf("probe: listening on UDP 0.0.0.0:%u for %u s (%s address layout); nothing is sent\n", port, seconds, layout);
    fflush(0);
#ifndef CARLINK_PROBE_HOST
    alarm(seconds + 10); /* the loop below counts quiet 200 ms waits; this is the hard limit */
#endif
    unsigned left = seconds * 5, number = 0, logged = 0;
    while (left) {
        struct pollfd_ wait = {fd, POLLIN_, 0};
        const int ready = poll(&wait, 1, 200);
        if (ready <= 0) {
            --left;
            continue;
        }
        unsigned char data[1500], from[32];
        unsigned from_size = sizeof(from);
        const __PTRDIFF_TYPE__ got = recv_(fd, data, sizeof(data), 0, from, &from_size);
        if (got <= 0)
            continue;
        ++number;
        if (logged < MAX_LOGGED) {
            show(from, data, (unsigned)got, number);
            ++logged;
        }
    }
    printf("probe: done, %u datagram(s) received\n", number);
    close(fd);
    return number ? 0 : 1;
}
