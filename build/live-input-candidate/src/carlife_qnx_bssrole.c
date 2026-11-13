/* Reads (and, when asked, sets) the BSS role of the Marvell 88W8787 Wi-Fi interface: AP (1) or station/client (0).
 * Usage: --get IFNAME | --set IFNAME 0|1      (IFNAME: uap0; the factory stack also knows wcl0 and wfd0)
 * It sends exactly the message the factory networking service (libnetworkingservice.so, get_set_bss_role) sends to
 * io-pkt: a devctl SIOCSDRVSPEC (_IOW('i', 123, struct ifdrv)) whose ifdrv carries the driver's private command
 * WOAL_UAP_SETONEINT_GETONEINT (0x8be5) for uap0, followed by an iwreq whose u.data.flags selects
 * WOAL_UAP_SET_GET_BSS_ROLE (1) and whose u.data.length is 1 to set, 0 to get; the role itself travels as a 4-byte
 * integer in the last message part. --get changes nothing. */
#ifdef CARLINK_BSSROLE_HOST
#include <stdio.h>
#include <string.h>
#else
int printf(const char *, ...);
int strcmp(const char *, const char *);
void *memset(void *, int, __SIZE_TYPE__);
void *dlopen(const char *, int);
void *dlsym(void *, const char *);
int close(int);
int *__get_errno_ptr(void);
#endif

struct iov_ { void *base; unsigned len; };
int MsgSendv(int coid, const struct iov_ *siov, int sparts, const struct iov_ *riov, int rparts);

enum {
    IO_DEVCTL = 0x106,
    DCMD_SIOCSDRVSPEC = 0x801c697b,            /* _IOW('i', 123, struct ifdrv) */
    DCMD_SIOCGDRVSPEC = 0xc01c697b,            /* _IOWR('i', 123, struct ifdrv): no privilege check in BSD stacks */
    PRIV_UAP_SETONEINT_GETONEINT = 0x8be5,     /* SIOCIWFIRSTPRIV + 5, the uAP interface */
    PRIV_UAP_SET_GET_BSS_ROLE = 1,
    PRIV_STA_SET_GET_BSS_ROLE = 0x15,
    STA_SETONEINT_GETONEINT = 0x801c699e,      /* as stored by the factory code for wcl0/wfd0 */
};

struct devctl_hdr { unsigned short type, combine_len; int dcmd; int nbytes; int zero; };      /* 16 bytes */
struct ifdrv_ { char name[16]; unsigned long cmd; unsigned len; void *data; };              /* 28 bytes */
struct iwreq_ { char name[16]; void *pointer; unsigned short length, flags; char pad[8]; }; /* 32 bytes */

#ifndef CARLINK_BSSROLE_HOST
_Static_assert(sizeof(struct devctl_hdr) == 16, "devctl header");
_Static_assert(sizeof(struct ifdrv_) == 28, "struct ifdrv on 32-bit QNX");
_Static_assert(sizeof(struct iwreq_) == 32, "struct iwreq on 32-bit QNX");
_Static_assert(__builtin_offsetof(struct iwreq_, length) == 20 && __builtin_offsetof(struct iwreq_, flags) == 22, "iwreq u.data");
_Static_assert(__builtin_offsetof(struct ifdrv_, cmd) == 16 && __builtin_offsetof(struct ifdrv_, data) == 24, "ifdrv fields");
#endif

typedef int (*socket_fn)(int, int, int);

/* Builds the message; split out so the PC test can check every byte. Returns the number of send parts. */
static int use_gdrvspec;
int carlink_bssrole_message(const char *ifname, int set, int *role, struct devctl_hdr *hdr, struct ifdrv_ *ifd,
                            struct iwreq_ *iwr, struct iov_ *siov, struct iov_ *riov)
{
    const int uap = strcmp(ifname, "uap0") == 0;
    memset(hdr, 0, sizeof(*hdr));
    memset(ifd, 0, sizeof(*ifd));
    memset(iwr, 0, sizeof(*iwr));
    for (unsigned i = 0; i < 15 && ifname[i]; ++i)
        ifd->name[i] = ifname[i];
    ifd->cmd = uap ? PRIV_UAP_SETONEINT_GETONEINT : STA_SETONEINT_GETONEINT;
    ifd->len = 32 + 4; /* the driver checks u.data.length * 4 + 32 against it before answering */
    iwr->pointer = role;
    iwr->length = set ? 1 : 0;
    iwr->flags = uap ? PRIV_UAP_SET_GET_BSS_ROLE : PRIV_STA_SET_GET_BSS_ROLE;
    hdr->type = IO_DEVCTL;
    hdr->combine_len = 16;
    hdr->dcmd = (int)(use_gdrvspec ? DCMD_SIOCGDRVSPEC : DCMD_SIOCSDRVSPEC);
    hdr->nbytes = 0x4c;
    siov[0].base = hdr; siov[0].len = 16;
    siov[1].base = ifd; siov[1].len = 28;
    siov[2].base = iwr; siov[2].len = 32;
    siov[3].base = role; siov[3].len = 4;
    riov[0] = siov[0]; riov[1] = siov[1]; riov[2] = siov[2]; riov[3] = siov[3];
    return set ? 4 : 3;
}

int carlink_bssrole(int argc, char **argv)
{
    int set = 0, want = -1;
    const char *ifname = 0;
    if (argc == 3 && strcmp(argv[1], "--get") == 0)
        ifname = argv[2];
    else if (argc == 3 && strcmp(argv[1], "--get-g") == 0) { /* same query through SIOCGDRVSPEC */
        ifname = argv[2];
        use_gdrvspec = 1;
    }
    else if (argc == 4 && strcmp(argv[1], "--set") == 0 && (argv[3][0] == '0' || argv[3][0] == '1') && !argv[3][1]) {
        ifname = argv[2];
        set = 1;
        want = argv[3][0] - '0';
    }
    if (!ifname || (strcmp(ifname, "uap0") && strcmp(ifname, "wcl0") && strcmp(ifname, "wfd0"))) {
        printf("Usage: --get uap0 | --get-g uap0 | --set uap0 0|1   (0 = client/station, 1 = access point)\n");
        return 2;
    }
#ifdef CARLINK_BSSROLE_HOST
    socket_fn sock = 0;
#else
    socket_fn sock = 0;
    static const char *const libs[] = {"libc.so.3", "libsocket.so.3", "libsocket.so.2"};
    for (unsigned i = 0; i < 3 && !sock; ++i) {
        void *h = dlopen(libs[i], 0x102);
        if (h)
            sock = (socket_fn)dlsym(h, "socket");
    }
#endif
    if (!sock) {
        printf("bssrole: no socket function\n");
        return 4;
    }
    const int fd = sock(2, 2, 0); /* AF_INET, SOCK_DGRAM: the factory code uses the same */
    if (fd < 0) {
        printf("bssrole: socket failed\n");
        return 5;
    }
    struct devctl_hdr hdr;
    struct ifdrv_ ifd;
    struct iwreq_ iwr;
    struct iov_ siov[4], riov[4];
    int role = set ? want : -1;
    const int parts = carlink_bssrole_message(ifname, set, &role, &hdr, &ifd, &iwr, siov, riov);
    const int rc = MsgSendv(fd, siov, parts, riov, 4);
    close(fd);
    if (rc == -1) {
#ifdef CARLINK_BSSROLE_HOST
        printf("bssrole: %s %s failed\n", set ? "set" : "get", ifname);
#else
        printf("bssrole: %s %s failed, errno %d\n", set ? "set" : "get", ifname, *__get_errno_ptr());
#endif
        return 6;
    }
    if (set)
        printf("bssrole: %s set to %d (%s)\n", ifname, want, want ? "access point" : "client");
    else
        printf("bssrole: %s role %d (%s)\n", ifname, role, role == 1 ? "access point" : role == 0 ? "client" : "unknown");
    return 0;
}
