/* Read-only packet listener for the wireless CarLife check: carlife_qnx_sniff IFNAME SECONDS (e.g. uap0 40).
 * Opens /dev/bpf on the interface (the head unit has no tcpdump), and prints one line per IPv4/ARP/IPv6 packet seen
 * (up to LINE_LIMIT lines), then a per-flow summary. It never sends anything and changes no setting except its own bpf
 * descriptor. The head unit's io-pkt is NetBSD-based; struct ifreq sizes differ between versions, so BIOCSETIF is tried
 * with each plausible size. */
#ifdef CARLINK_SNIFF_HOST
#include <stdio.h>
#include <string.h>
#else
int printf(const char *, ...);
int fflush(void *);
int strcmp(const char *, const char *);
int open(const char *, int, ...);
int close(int);
int ioctl(int, unsigned long, ...);
int poll(void *, unsigned, int);
__PTRDIFF_TYPE__ read(int, void *, __SIZE_TYPE__);
long time(long *);
unsigned alarm(unsigned);
int *__get_errno_ptr(void);
#endif

enum { LINE_LIMIT = 600, FLOW_MAX = 96, BUFFER_SIZE = 32768 };

struct flow { unsigned char proto, src[4], dst[4]; unsigned short sport, dport; unsigned count, flags; };
static struct flow flows[FLOW_MAX];
static unsigned flow_count, lines, packets, dropped_flows;

static unsigned be16(const unsigned char *p) { return (unsigned)p[0] << 8 | p[1]; }
static unsigned le32(const unsigned char *p) { return p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24; }

static void note_flow(unsigned proto, const unsigned char *src, const unsigned char *dst, unsigned sport, unsigned dport,
                      unsigned flags)
{
    for (unsigned i = 0; i < flow_count; ++i) {
        struct flow *f = &flows[i];
        if (f->proto == proto && f->sport == sport && f->dport == dport && !((f->src[0] ^ src[0]) | (f->src[1] ^ src[1]) |
            (f->src[2] ^ src[2]) | (f->src[3] ^ src[3]) | (f->dst[0] ^ dst[0]) | (f->dst[1] ^ dst[1]) |
            (f->dst[2] ^ dst[2]) | (f->dst[3] ^ dst[3]))) {
            ++f->count;
            f->flags |= flags;
            return;
        }
    }
    if (flow_count >= FLOW_MAX) {
        ++dropped_flows;
        return;
    }
    struct flow *f = &flows[flow_count++];
    f->proto = (unsigned char)proto;
    for (int i = 0; i < 4; ++i) { f->src[i] = src[i]; f->dst[i] = dst[i]; }
    f->sport = (unsigned short)sport; f->dport = (unsigned short)dport; f->count = 1; f->flags = flags;
}

static void tcp_flags(unsigned flags, char *out)
{
    static const char names[] = "FSRPAU";
    unsigned n = 0;
    for (unsigned i = 0; i < 6; ++i)
        if (flags & (1u << i))
            out[n++] = names[i];
    if (!n)
        out[n++] = '.';
    out[n] = 0;
}

/* One captured frame (Ethernet). */
static void frame(const unsigned char *p, unsigned size, unsigned ms)
{
    ++packets;
    if (size < 14)
        return;
    const unsigned type = be16(p + 12);
    const int show = lines < LINE_LIMIT;
    if (type == 0x0806 && size >= 42) {
        if (show) {
            ++lines;
            printf("+%u.%03u ARP %s %u.%u.%u.%u -> %u.%u.%u.%u\n", ms / 1000, ms % 1000, be16(p + 20) == 1 ? "request" : "reply",
                   p[28], p[29], p[30], p[31], p[38], p[39], p[40], p[41]);
        }
        return;
    }
    if (type == 0x86dd && size >= 54) {
        if (show) {
            ++lines;
            printf("+%u.%03u IPv6 next-header %u, %u bytes\n", ms / 1000, ms % 1000, p[20], size);
        }
        return;
    }
    if (type != 0x0800 || size < 34) {
        if (show) {
            ++lines;
            printf("+%u.%03u ethertype 0x%04x, %u bytes\n", ms / 1000, ms % 1000, type, size);
        }
        return;
    }
    const unsigned char *ip = p + 14;
    const unsigned ihl = (ip[0] & 15u) * 4;
    const unsigned proto = ip[9];
    const unsigned char *src = ip + 12, *dst = ip + 16;
    if (ihl < 20 || 14 + ihl > size)
        return;
    const unsigned char *l4 = ip + ihl;
    const unsigned l4size = size - 14 - ihl;
    unsigned sport = 0, dport = 0, flags = 0;
    char extra[48];
    extra[0] = 0;
    const char *name = "IP";
    if (proto == 6 && l4size >= 20) {
        name = "TCP";
        sport = be16(l4); dport = be16(l4 + 2); flags = l4[13] & 0x3f;
        char f[8];
        tcp_flags(flags, f);
        const unsigned payload = l4size - (unsigned)(l4[12] >> 4) * 4;
        int n = 0;
        for (const char *c = "["; *c; ++c) extra[n++] = *c;
        for (const char *c = f; *c; ++c) extra[n++] = *c;
        extra[n++] = ']';
        extra[n] = 0;
        if (show)
            ++lines, printf("+%u.%03u TCP %u.%u.%u.%u:%u > %u.%u.%u.%u:%u %s data %u\n", ms / 1000, ms % 1000, src[0], src[1],
                            src[2], src[3], sport, dst[0], dst[1], dst[2], dst[3], dport, extra, payload);
    } else if (proto == 17 && l4size >= 8) {
        name = "UDP";
        sport = be16(l4); dport = be16(l4 + 2);
        if (show) {
            ++lines;
            printf("+%u.%03u UDP %u.%u.%u.%u:%u > %u.%u.%u.%u:%u len %u", ms / 1000, ms % 1000, src[0], src[1], src[2], src[3],
                   sport, dst[0], dst[1], dst[2], dst[3], dport, be16(l4 + 4));
            /* first bytes of small payloads (discovery packets are usually short text or protobuf) */
            const unsigned payload = l4size - 8 < 24 ? l4size - 8 : 24;
            if (payload) {
                printf(" :");
                for (unsigned i = 0; i < payload; ++i)
                    printf(" %02x", l4[8 + i]);
            }
            printf("\n");
        }
    } else if (proto == 1 && l4size >= 4) {
        name = "ICMP";
        sport = l4[0];
        if (show)
            ++lines, printf("+%u.%03u ICMP %u.%u.%u.%u > %u.%u.%u.%u type %u code %u\n", ms / 1000, ms % 1000, src[0], src[1],
                            src[2], src[3], dst[0], dst[1], dst[2], dst[3], l4[0], l4[1]);
    } else if (show) {
        ++lines;
        printf("+%u.%03u %s proto %u %u.%u.%u.%u > %u.%u.%u.%u\n", ms / 1000, ms % 1000, name, proto, src[0], src[1], src[2],
               src[3], dst[0], dst[1], dst[2], dst[3]);
    }
    /* ephemeral (client-side) ports are folded to 0 so one connection pattern is one flow */
    if (sport > 49151 && dport <= 49151)
        sport = 0;
    else if (dport > 49151 && sport <= 49151)
        dport = 0;
    note_flow(proto, src, dst, sport, dport, flags);
}

/* A buffer of bpf records: NetBSD struct bpf_hdr = timeval (two longs = 8 bytes on 32-bit ARM, 16 on a 64-bit host),
 * caplen, datalen, hdrlen (u16); each record is padded to a long. tv_size is 8 or 16. */
static unsigned start_sec, start_set;
static void bpf_buffer(const unsigned char *buf, unsigned size, unsigned tv_size)
{
    const unsigned align = tv_size / 2;
    unsigned at = 0;
    while (at + tv_size + 10 <= size) {
        const unsigned char *h = buf + at;
        const unsigned sec = le32(h), usec = le32(h + tv_size / 2);
        const unsigned caplen = le32(h + tv_size), hdrlen = h[tv_size + 8] | (unsigned)h[tv_size + 9] << 8;
        if (hdrlen < tv_size + 10 || hdrlen > 64 || at + hdrlen + caplen > size)
            break;
        if (!start_set) { start_sec = sec; start_set = 1; }
        frame(h + hdrlen, caplen, (sec - start_sec) * 1000 + usec / 1000);
        at += (hdrlen + caplen + align - 1) & ~(align - 1);
    }
}

static void summary(void)
{
    printf("sniff: %u packets, %u lines shown%s\n", packets, lines, lines >= LINE_LIMIT ? " (limit reached)" : "");
    for (unsigned i = 0; i < flow_count; ++i) {
        const struct flow *f = &flows[i];
        char fl[8];
        tcp_flags(f->flags, fl);
        printf("flow: %s %u.%u.%u.%u:%u > %u.%u.%u.%u:%u  x%u%s%s\n", f->proto == 6 ? "TCP" : f->proto == 17 ? "UDP" :
               f->proto == 1 ? "ICMP" : "other", f->src[0], f->src[1], f->src[2], f->src[3], f->sport, f->dst[0], f->dst[1],
               f->dst[2], f->dst[3], f->dport, f->count, f->proto == 6 ? " flags " : "", f->proto == 6 ? fl : "");
    }
    if (dropped_flows)
        printf("flow: %u more packets in flows beyond the table\n", dropped_flows);
}

#ifndef CARLINK_SNIFF_HOST
struct pollfd_ { int fd; short events, revents; };

int carlink_sniff(int argc, char **argv)
{
    unsigned seconds = 0;
    const char *name = argc == 3 ? argv[1] : "";
    unsigned name_len = 0;
    for (; name[name_len]; ++name_len)
        if (name_len >= 15 || !((name[name_len] >= 'a' && name[name_len] <= 'z') || (name[name_len] >= '0' && name[name_len] <= '9'))) {
            name_len = 99;
            break;
        }
    for (const char *c = argc == 3 ? argv[2] : ""; *c; ++c)
        seconds = *c >= '0' && *c <= '9' && seconds < 1000 ? seconds * 10 + (unsigned)(*c - '0') : 9999;
    if (!name_len || name_len > 15 || seconds < 1 || seconds > 600) {
        printf("Usage: IFNAME SECONDS   (e.g. uap0 40; 1..600 s)\n");
        return 2;
    }
    alarm(seconds + 5);
    int fd = -1;
    char path[16] = "/dev/bpf";
    fd = open(path, 0);
    for (int n = 0; fd < 0 && n < 8; ++n) {
        path[8] = (char)('0' + n); path[9] = 0;
        fd = open(path, 0);
    }
    if (fd < 0) {
        printf("sniff: cannot open /dev/bpf errno=%d\n", *__get_errno_ptr());
        return 3;
    }
    unsigned blen = BUFFER_SIZE;
    ioctl(fd, 0xc0044266u, &blen);                     /* BIOCSBLEN (may be refused; read back below) */
    static const unsigned sizes[] = {32, 40, 144, 48, 64};
    unsigned char ifr[160];
    int bound = 0;
    for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]) && !bound; ++s) {
        for (unsigned i = 0; i < sizeof(ifr); ++i) ifr[i] = 0;
        for (unsigned i = 0; i < name_len; ++i) ifr[i] = (unsigned char)name[i];
        const unsigned long request = 0x80000000u | (sizes[s] << 16) | 0x426cu; /* BIOCSETIF */
        if (ioctl(fd, request, ifr) == 0) {
            bound = 1;
            printf("sniff: listening on %s (/dev/bpf, ifreq %u bytes)\n", name, sizes[s]);
        }
    }
    if (!bound) {
        printf("sniff: BIOCSETIF %s failed errno=%d\n", name, *__get_errno_ptr());
        close(fd);
        return 4;
    }
    unsigned one = 1;
    ioctl(fd, 0x80044270u, &one);                      /* BIOCIMMEDIATE */
    blen = 0;
    if (ioctl(fd, 0x40044266u, &blen) != 0 || blen < 4096 || blen > BUFFER_SIZE)
        blen = BUFFER_SIZE;                            /* BIOCGBLEN */
    unsigned dlt = 0;
    if (ioctl(fd, 0x4004426au, &dlt) == 0)             /* BIOCGDLT: 1 = Ethernet */
        printf("sniff: link type %u, buffer %u bytes, %u s\n", dlt, blen, seconds);
    fflush(0);
    static unsigned char buffer[BUFFER_SIZE];
    const long end = time(0) + (long)seconds;
    while (time(0) < end) {
        struct pollfd_ wait = {fd, 1, 0};
        const int ready = poll(&wait, 1, 500);
        if (ready < 0 && *__get_errno_ptr() != 4)
            break;
        if (ready <= 0)
            continue;
        const __PTRDIFF_TYPE__ got = read(fd, buffer, blen);
        if (got <= 0)
            continue;
        bpf_buffer(buffer, (unsigned)got, 8);
        fflush(0);
    }
    close(fd);
    summary();
    return 0;
}
#endif
