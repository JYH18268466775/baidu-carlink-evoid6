/* UDP control port for the CarLife session supervisor (sh). Stands in for the
 * control socket of the upstream NBT-AAD daemon: UDP 127.0.0.1:PORT.
 *   "ping"  -> reply "pong" (the iDrive app uses this to see that a daemon runs)
 *   others  -> printed to stdout as one line each (show, hide, quit, gear N, pdc N)
 * The head unit's libc has no socket functions (they live in libsocket), so they
 * are loaded at run time. Exits when the supervisor that started it is gone, so
 * a killed supervisor never leaves the port taken. */
void *dlopen(const char *, int);
void *dlsym(void *, const char *);
int printf(const char *, ...);
int fflush(void *);
int strcmp(const char *, const char *);
void *memset(void *, int, __SIZE_TYPE__);
int getppid(void);
int poll(void *, unsigned, int);
int close(int);
int write(int, const void *, __SIZE_TYPE__);
int *__get_errno_ptr(void);

enum { QNX_LOAD_NOW = 2, QNX_LOAD_NOW_GLOBAL = 0x102, AF_INET_ = 2, SOCK_DGRAM_ = 2,
       POLLIN_ = 1, MESSAGE_MAX = 31, MIN_PORT = 1024, MAX_PORT = 65535 };

typedef int (*socket_fn)(int, int, int);
typedef int (*bind_fn)(int, const void *, unsigned);
typedef __PTRDIFF_TYPE__ (*recvfrom_fn)(int, void *, __SIZE_TYPE__, int, void *, unsigned *);
typedef __PTRDIFF_TYPE__ (*sendto_fn)(int, const void *, __SIZE_TYPE__, int, const void *, unsigned);

struct pollfd_ { int fd; short events, revents; };

static int load_socket_api(socket_fn *sock, bind_fn *bind_, recvfrom_fn *recv_, sendto_fn *send_)
{
    static const char *const libraries[] = {"libc.so.3", "libsocket.so.3", "libsocket.so.2",
                                            "libsocket.so.1", "libsocket.so"};
    for (unsigned i = 0; i < sizeof(libraries) / sizeof(libraries[0]); ++i) {
        void *handle = dlopen(libraries[i], QNX_LOAD_NOW_GLOBAL);
        if (!handle)
            continue;
        *sock = (socket_fn)dlsym(handle, "socket");
        *bind_ = (bind_fn)dlsym(handle, "bind");
        *recv_ = (recvfrom_fn)dlsym(handle, "recvfrom");
        *send_ = (sendto_fn)dlsym(handle, "sendto");
        if (*sock && *bind_ && *recv_ && *send_) {
            printf("ctl: socket functions from %s\n", libraries[i]);
            fflush(0);
            return 0;
        }
    }
    return -1;
}

/* QNX (BSD) puts a length byte first; Linux does not. Whichever the system
 * accepts is used, so a wrong guess costs nothing. */
static int bind_loopback(int fd, bind_fn bind_, unsigned port, const char **layout)
{
    unsigned char a[16];
    memset(a, 0, sizeof(a));
    a[0] = 16; a[1] = AF_INET_;                         /* BSD: len, family */
    a[2] = (unsigned char)(port >> 8); a[3] = (unsigned char)port;
    a[4] = 127; a[7] = 1;
    if (bind_(fd, a, sizeof(a)) == 0) { *layout = "bsd"; return 0; }
    memset(a, 0, sizeof(a));
    a[0] = AF_INET_; a[1] = 0;                          /* Linux: 16-bit family */
    a[2] = (unsigned char)(port >> 8); a[3] = (unsigned char)port;
    a[4] = 127; a[7] = 1;
    if (bind_(fd, a, sizeof(a)) == 0) { *layout = "linux"; return 0; }
    return -1;
}

static int message_ok(const unsigned char *text, unsigned size)
{
    if (!size || size > MESSAGE_MAX)
        return 0;
    for (unsigned i = 0; i < size; ++i) {
        const unsigned char c = text[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_'))
            return 0;
    }
    return 1;
}

static int write_all(const unsigned char *data, unsigned size)
{
    while (size) {
        const int count = write(1, data, size);
        if (count <= 0)
            return -1;
        data += count;
        size -= (unsigned)count;
    }
    return 0;
}

int carlink_ctl(int argc, char **argv)
{
    unsigned port = 0;
    if (argc == 3 && strcmp(argv[1], "--serve") == 0) {
        for (const char *c = argv[2]; *c; ++c) {
            if (*c < '0' || *c > '9' || port > MAX_PORT) { port = 0; break; }
            port = port * 10 + (unsigned)(*c - '0');
        }
    }
    if (port < MIN_PORT || port > MAX_PORT) {
        printf("Usage: --serve PORT(%d-%d)\n", MIN_PORT, MAX_PORT);
        return 2;
    }
    socket_fn sock; bind_fn bind_; recvfrom_fn recv_; sendto_fn send_;
    if (load_socket_api(&sock, &bind_, &recv_, &send_)) {
        printf("ctl: no socket functions found\n");
        return 4;
    }
    const int fd = sock(AF_INET_, SOCK_DGRAM_, 0);
    if (fd < 0) {
        printf("ctl: socket failed errno=%d\n", *__get_errno_ptr());
        return 5;
    }
    const char *layout = "";
    if (bind_loopback(fd, bind_, port, &layout)) {
        printf("ctl: bind 127.0.0.1:%u failed errno=%d (port in use?)\n", port, *__get_errno_ptr());
        close(fd);
        return 3;
    }
    printf("ctl: bound UDP 127.0.0.1:%u (%s address layout)\n", port, layout);
    fflush(0);
    const int parent = getppid();
    for (;;) {
        struct pollfd_ wait = {fd, POLLIN_, 0};
        const int ready = poll(&wait, 1, 1000);
        if (ready < 0 && *__get_errno_ptr() != 4)       /* 4 = EINTR */
            break;
        if (ready <= 0) {
            if (getppid() != parent)
                break;                                  /* the supervisor is gone */
            continue;
        }
        unsigned char text[MESSAGE_MAX + 2];
        unsigned char from[32];
        unsigned from_size = sizeof(from);
        const __PTRDIFF_TYPE__ got = recv_(fd, text, sizeof(text) - 1, 0, from, &from_size);
        if (got <= 0)
            continue;
        unsigned size = (unsigned)got;
        while (size && (text[size - 1] == '\n' || text[size - 1] == '\r' || text[size - 1] == 0))
            --size;
        if (size == 4 && text[0] == 'p' && text[1] == 'i' && text[2] == 'n' && text[3] == 'g') {
            send_(fd, "pong", 4, 0, from, from_size);
            continue;
        }
        if (!message_ok(text, size))
            continue;
        text[size++] = '\n';
        if (write_all(text, size))
            break;                                      /* nobody is reading */
    }
    close(fd);
    return 0;
}
