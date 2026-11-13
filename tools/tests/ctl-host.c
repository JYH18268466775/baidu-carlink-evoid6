#include <dlfcn.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

int carlink_ctl(int argc, char **argv);

int *t_errno_ptr(void) { return &errno; }

/* MOCK_LIBS: comma list of library names whose dlopen succeeds ("none" = none).
 * MOCK_QNX=1: bind accepts only the BSD layout (length byte first), otherwise only the Linux layout. */
static int lib_ok(const char *name)
{
    const char *list = getenv("MOCK_LIBS");
    if (!list) return strcmp(name, "libsocket.so.3") == 0;
    char copy[256]; snprintf(copy, sizeof copy, "%s", list);
    for (char *t = strtok(copy, ","); t; t = strtok(0, ","))
        if (!strcmp(t, name)) return 1;
    return 0;
}
void *t_dlopen(const char *name, int flags) { (void)flags; return lib_ok(name) ? (void *)name : 0; }

static int m_socket(int a, int b, int c) { return socket(a, b, c); }
static int m_bind(int fd, const void *addr, unsigned len)
{
    const unsigned char *a = addr;
    struct sockaddr_in in; memset(&in, 0, sizeof in);
    if (len != 16) { errno = EINVAL; return -1; }
    if (getenv("MOCK_QNX")) {
        if (a[0] != 16 || a[1] != 2) { errno = EAFNOSUPPORT; return -1; }
    } else if (a[0] != 2 || a[1] != 0) { errno = EAFNOSUPPORT; return -1; }
    in.sin_family = AF_INET; memcpy(&in.sin_port, a + 2, 2); memcpy(&in.sin_addr, a + 4, 4);
    return bind(fd, (struct sockaddr *)&in, sizeof in);
}
static long m_recvfrom(int fd, void *b, size_t n, int f, void *from, unsigned *fl) { return recvfrom(fd, b, n, f, from, fl); }
static long m_sendto(int fd, const void *b, size_t n, int f, const void *to, unsigned tl) { return sendto(fd, b, n, f, to, tl); }

void *t_dlsym(void *h, const char *name)
{
    (void)h;
    if (!strcmp(name, "socket")) return (void *)m_socket;
    if (!strcmp(name, "bind")) return (void *)m_bind;
    if (!strcmp(name, "recvfrom")) return (void *)m_recvfrom;
    if (!strcmp(name, "sendto")) return (void *)m_sendto;
    return 0;
}

int main(int argc, char **argv) { return carlink_ctl(argc, argv); }
