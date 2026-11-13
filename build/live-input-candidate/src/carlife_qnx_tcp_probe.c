/* Outbound TCP connect probe for the wireless CarLife check. Usage: --scan A.B.C.D  (the phone's address on the head unit's
 * hotspot, 172.16.224.2-254) or, on a PC test, any 127.x address.
 * For each port in a fixed list (the CarLife wireless ports 7240 8240 9240 9241 9242 9340, their local counterparts, and a few
 * others a phone might serve) it opens a TCP connection from a local port >= 49512 (the head unit's firewall passes those),
 * waits up to 2 s, says open / refused / no answer, and for an open port waits 1.5 s and dumps the first bytes the phone
 * sends on its own. It writes nothing to the connection and closes it at once. The head unit's libc has no socket functions
 * (they live in libsocket), so they are loaded at run time like in the other probes. */
#ifdef CARLINK_PROBE_HOST
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <dlfcn.h>
#else
void *dlopen(const char *, int);
void *dlsym(void *, const char *);
int printf(const char *, ...);
int fflush(void *);
int strcmp(const char *, const char *);
int poll(void *, unsigned, int);
int close(int);
unsigned alarm(unsigned);
#endif

#ifdef CARLINK_PROBE_HOST
enum { SOL_SOCKET_ = SOL_SOCKET, SO_ERROR_ = SO_ERROR, SO_REUSEADDR_ = SO_REUSEADDR, FIONBIO_ = FIONBIO };
#else
/* BSD / QNX io-pkt values */
enum { SOL_SOCKET_ = 0xffff, SO_ERROR_ = 0x1007, SO_REUSEADDR_ = 4, FIONBIO_ = 0x8004667e };
#endif
#ifdef CARLINK_PROBE_HOST
enum { POLLOUT_ = 4 };
#else
enum { POLLOUT_ = 2 };   /* QNX: POLLOUT = POLLWRNORM = 0x0002 (4 is POLLRDBAND there; the first car run never woke up) */
#endif
enum { QNX_LOAD_NOW_GLOBAL = 0x102, AF_INET_ = 2, SOCK_STREAM_ = 1, POLLIN_ = 1, LOCAL_BASE = 50300,
       DUMP_BYTES = 48, WAIT_CONNECT_MS = 2000, WAIT_BANNER_MS = 1500 };

typedef int (*socket_fn)(int, int, int);
typedef int (*bind_fn)(int, const void *, unsigned);
typedef int (*connect_fn)(int, const void *, unsigned);
typedef int (*setsockopt_fn)(int, int, int, const void *, unsigned);
typedef int (*getsockopt_fn)(int, int, int, void *, unsigned *);
typedef __PTRDIFF_TYPE__ (*recv_fn)(int, void *, __SIZE_TYPE__, int);
typedef int (*ioctl_fn)(int, unsigned long, ...);
struct pollfd_ { int fd; short events, revents; };

struct api { socket_fn sock; bind_fn bind_; connect_fn connect_; setsockopt_fn setopt; getsockopt_fn getopt; recv_fn recv_; ioctl_fn ioctl_; };

static int load_api(struct api *api)
{
#ifdef CARLINK_PROBE_HOST
    api->sock = socket; api->bind_ = (bind_fn)bind; api->connect_ = (connect_fn)connect;
    api->setopt = (setsockopt_fn)setsockopt; api->getopt = (getsockopt_fn)getsockopt;
    api->recv_ = (recv_fn)recv; api->ioctl_ = (ioctl_fn)ioctl;
    return 0;
#else
    static const char *const libraries[] = {"libc.so.3", "libsocket.so.3", "libsocket.so.2", "libsocket.so.1", "libsocket.so"};
    for (unsigned i = 0; i < sizeof(libraries) / sizeof(libraries[0]); ++i) {
        void *handle = dlopen(libraries[i], QNX_LOAD_NOW_GLOBAL);
        if (!handle)
            continue;
        api->sock = (socket_fn)dlsym(handle, "socket");
        api->bind_ = (bind_fn)dlsym(handle, "bind");
        api->connect_ = (connect_fn)dlsym(handle, "connect");
        api->setopt = (setsockopt_fn)dlsym(handle, "setsockopt");
        api->getopt = (getsockopt_fn)dlsym(handle, "getsockopt");
        api->recv_ = (recv_fn)dlsym(handle, "recv");
        void *libc = dlopen("libc.so.3", QNX_LOAD_NOW_GLOBAL);
        api->ioctl_ = (ioctl_fn)dlsym(libc ? libc : handle, "ioctl");
        if (api->sock && api->bind_ && api->connect_ && api->setopt && api->getopt && api->recv_ && api->ioctl_) {
            printf("probe: socket functions from %s\n", libraries[i]);
            return 0;
        }
    }
    return -1;
#endif
}

static unsigned parse_ip(const char *text, unsigned char *ip)
{
    unsigned part = 0, digits = 0, index = 0;
    for (;; ++text) {
        if (*text >= '0' && *text <= '9') {
            part = part * 10 + (unsigned)(*text - '0');
            if (++digits > 3 || part > 255)
                return 0;
        } else if (*text == '.' || !*text) {
            if (!digits || index > 3)
                return 0;
            ip[index++] = (unsigned char)part;
            part = digits = 0;
            if (!*text)
                break;
        } else
            return 0;
    }
    return index == 4;
}

/* Only the head unit's own hotspot network (and loopback for the PC test) may be probed. */
static int target_ok(const unsigned char *ip)
{
#ifdef CARLINK_PROBE_HOST
    if (ip[0] == 127)
        return 1;
#endif
    return ip[0] == 172 && ip[1] == 16 && ip[2] == 224 && ip[3] >= 2 && ip[3] <= 254;
}

static void make_addr(unsigned char *a, int bsd, unsigned port, const unsigned char *ip)
{
    for (int i = 0; i < 16; ++i) a[i] = 0;
    if (bsd) { a[0] = 16; a[1] = AF_INET_; } else { a[0] = AF_INET_; a[1] = 0; }
    a[2] = (unsigned char)(port >> 8); a[3] = (unsigned char)port;
    for (int i = 0; i < 4; ++i) a[4 + i] = ip ? ip[i] : 0;
}

static const unsigned short ports[] = {7240, 8240, 9240, 9241, 9242, 9340, 7200, 8200, 9200, 9201, 9202, 9300, 7999, 7998,
                                       5277, 5000, 5001, 8000, 8080, 7000, 6000, 4000, 23, 22, 80};

/* 1 = open, 0 = refused, -1 = no answer, -2 = could not even try */
static int try_port(const struct api *api, const unsigned char *ip, unsigned port, unsigned local, int *layout_bsd, int *got_fd)
{
    const int fd = api->sock(AF_INET_, SOCK_STREAM_, 0);
    unsigned char a[16];
    int one = 1;
    *got_fd = -1;
    if (fd < 0)
        return -2;
    api->setopt(fd, SOL_SOCKET_, SO_REUSEADDR_, &one, sizeof(one));
    make_addr(a, *layout_bsd, local, 0);
    if (api->bind_(fd, a, sizeof(a)) != 0) {
        *layout_bsd = !*layout_bsd;
        make_addr(a, *layout_bsd, local, 0);
        if (api->bind_(fd, a, sizeof(a)) != 0) {
            close(fd);
            return -2;
        }
    }
    api->ioctl_(fd, (unsigned long)FIONBIO_, &one);
    make_addr(a, *layout_bsd, port, ip);
    const int rc = api->connect_(fd, a, sizeof(a));
    if (rc != 0) {
        struct pollfd_ wait = {fd, POLLOUT_, 0};
        if (poll(&wait, 1, WAIT_CONNECT_MS) <= 0) {
            close(fd);
            return -1;
        }
        int error = 0;
        unsigned size = sizeof(error);
        if (api->getopt(fd, SOL_SOCKET_, SO_ERROR_, &error, &size) != 0 || error) {
            close(fd);
            return 0;
        }
    }
    *got_fd = fd;
    return 1;
}

int carlink_tcp_probe(int argc, char **argv)
{
    unsigned char ip[4];
    if (argc != 3 || strcmp(argv[1], "--scan") != 0 || !parse_ip(argv[2], ip) || !target_ok(ip)) {
        printf("Usage: --scan 172.16.224.N   (N = 2..254, the phone's address on the head unit's hotspot)\n");
        return 2;
    }
    struct api api;
    if (load_api(&api)) {
        printf("probe: no socket functions found\n");
        return 4;
    }
#ifndef CARLINK_PROBE_HOST
    alarm(120);
#endif
    printf("probe: connecting to %u.%u.%u.%u from local ports from %d; nothing is sent\n", ip[0], ip[1], ip[2], ip[3], LOCAL_BASE);
    fflush(0);
    int bsd = 1;
    unsigned open_count = 0;
    for (unsigned i = 0; i < sizeof(ports) / sizeof(ports[0]); ++i) {
        int fd;
        const int state = try_port(&api, ip, ports[i], LOCAL_BASE + i, &bsd, &fd);
        if (state == 1) {
            ++open_count;
            printf("probe: port %u OPEN", ports[i]);
            struct pollfd_ wait = {fd, POLLIN_, 0};
            if (poll(&wait, 1, WAIT_BANNER_MS) > 0) {
                unsigned char data[DUMP_BYTES + 1];
                const __PTRDIFF_TYPE__ got = api.recv_(fd, data, DUMP_BYTES, 0);
                if (got > 0) {
                    printf(", sent %d bytes first:", (int)got);
                    for (__PTRDIFF_TYPE__ k = 0; k < got; ++k)
                        printf(" %02x", data[k]);
                } else
                    printf(", closed by the phone at once");
            } else
                printf(", silent (waits for us to speak)");
            printf("\n");
            close(fd);
        } else
            printf("probe: port %u %s\n", ports[i], state == 0 ? "refused" : state == -1 ? "no answer" : "could not try");
        fflush(0);
    }
    printf("probe: done, %u port(s) open\n", open_count);
    return open_count ? 0 : 1;
}
