/* Validate and send a bounded H.264 replay on a private renderer. */
#include "carlife_qnx_short_replay.h"

extern const unsigned char carlink_gop_start[], carlink_gop_end[];
int carlink_mdev_arm_open(void);
int carlink_mdev_arm_close(int);
int carlink_mdev_arm_sendv(int, const void *, int);
int usleep(unsigned);
int printf(const char *, ...);
int *__get_errno_ptr(void);
int open(const char *, int, ...);
int close(int);
char *getenv(const char *);
__PTRDIFF_TYPE__ read(int, void *, __SIZE_TYPE__);
int memcmp(const void *, const void *, __SIZE_TYPE__);

struct segment {
    const void *data;
    unsigned size;
};

static unsigned char buffer[2 * 1024 * 1024 + 2 * 4096 + 256];
static const unsigned char reserved[16] = {0};

static int display_completed(void)
{
    const char *path = getenv("CARLINK_DISPLAY_DONE");
    if (!path || !*path) return 0;
    const int fd = open(path, 0);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}

static unsigned read_le32(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) |
           ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static int starts_with_nal(const unsigned char *p, unsigned size, unsigned kind)
{
    if (size < 4 || p[0] || p[1])
        return 0;
    if (p[2] == 1)
        return (p[3] & 31) == kind;
    return size >= 5 && !p[2] && p[3] == 1 && (p[4] & 31) == kind;
}

static int validate(struct carlink_replay *replay, unsigned records)
{
    const unsigned char *p = replay->start;
    const unsigned char *end = replay->end;
    static const unsigned setup[] = {5, 2, 4};

    if (end - p < 8 || memcmp(p, "MDEVv1\r\n", 8) != 0)
        return 65;
    p += 8;
    for (unsigned i = 0; i < records; ++i) {
        if (end - p < 24 || memcmp(p, reserved, sizeof(reserved)))
            return 65;
        const unsigned kind = read_le32(p + 16);
        const unsigned length = read_le32(p + 20);
        if (kind != (i < 3 ? setup[i] : 3) || length > (unsigned)(end - p - 24))
            return 65;
        p += 24;
        if (kind == 5 && (length != 4 || read_le32(p) != 1))
            return 65;
        if (kind == 2) {
            if (length != 32 || read_le32(p) != 2 || read_le32(p + 4) != 0x48323634 ||
                read_le32(p + 16) != 16 || read_le32(p + 24) || read_le32(p + 28))
                return 65;
            replay->width = read_le32(p + 8);
            replay->height = read_le32(p + 12);
            replay->fps = read_le32(p + 20);
            if (!replay->width || replay->width > 65535 || !replay->height ||
                replay->height > 65535 || !replay->fps || replay->fps > 1000)
                return 65;
        }
        if ((kind == 4 && !starts_with_nal(p, length, 7)) ||
            (kind == 3 && (length <= 8 ||
                (i == 3 && (memcmp(p, reserved, 8) || !starts_with_nal(p + 8, length - 8, 5))))))
            return 65;
        p += length;
    }
    return p == end ? 0 : 65;
}

int carlink_load_replay(struct carlink_replay *replay, const char *path)
{
    replay->live = 0;
    if (!path) {
        replay->start = carlink_gop_start;
        replay->end = carlink_gop_end;
        replay->frames = 20;
        return validate(replay, 3 + replay->frames);
    }

    /* Match the converter's capture limit, with room for mdev framing. */
    const int fd = open(path, 0);
    if (fd < 0) {
        printf("replay file open failed errno=%d\n", *__get_errno_ptr());
        return 65;
    }
    unsigned used = 0;
    __PTRDIFF_TYPE__ count = 0;
    while (used < sizeof(buffer)) {
        count = read(fd, buffer + used, sizeof(buffer) - used);
        if (count <= 0)
            break;
        used += (unsigned)count;
    }
    const int close_rc = close(fd);
    if (count < 0 || used == sizeof(buffer) || close_rc != 0)
        return 65;
    replay->start = buffer;
    replay->end = buffer + used;
    /* Count bounded records before full validation; at most six seconds. */
    unsigned records = 0, offset = 8;
    while (offset < used) {
        if (used - offset < 24) return 65;
        const unsigned length = read_le32(buffer + offset + 20);
        if (length > used - offset - 24) return 65;
        offset += 24 + length;
        if (++records > 183) return 65;
    }
    if (records < 4) return 65;
    replay->frames = records - 3;
    return validate(replay, 3 + replay->frames);
}

static int read_exact(unsigned char *destination, unsigned size)
{
    while (size) {
        const __PTRDIFF_TYPE__ count = read(0, destination, size);
        if (count <= 0)
            return 65;
        destination += count;
        size -= (unsigned)count;
    }
    return 0;
}

int carlink_load_live_replay(struct carlink_replay *replay)
{
    static unsigned char setup[8 + 3 * 24 + 4 + 32 + 2 * 4096];
    unsigned used = 8;
    if (read_exact(setup, used) || memcmp(setup, "MDEVv1\r\n", used))
        return 65;
    for (unsigned i = 0; i < 3; ++i) {
        if (sizeof(setup) - used < 24 || read_exact(setup + used, 24))
            return 65;
        const unsigned length = read_le32(setup + used + 20);
        used += 24;
        if (length > sizeof(setup) - used || read_exact(setup + used, length))
            return 65;
        used += length;
    }
    replay->start = setup;
    replay->end = setup + used;
    replay->frames = CARLINK_LIVE_FRAMES;
    replay->live = 1;
    return validate(replay, 3);
}

int carlink_play_replay(const struct carlink_replay *replay,
                        int (*after_frame)(unsigned, void *), void *arg)
{
    const unsigned char *p = replay->start + 8;
    int status = 0;
    unsigned frames = 0;
    const int coid = carlink_mdev_arm_open();
    if (coid < 0) {
        const int error = *__get_errno_ptr();
        printf("mdev open failed errno=%d\n", error);
        return 64;
    }

    for (unsigned i = 0; i < 3 + replay->frames; ++i) {
        if (replay->live && i >= 3) {
            if (frames && display_completed()) break;
            p = buffer;
            if (read_exact(buffer, 24)) {
                status = frames && display_completed() ? 0 : 65;
                break;
            }
            const unsigned length = read_le32(p + 20);
            if (memcmp(p, reserved, sizeof(reserved)) || read_le32(p + 16) != 3 ||
                length <= 8 || length > sizeof(buffer) - 24) {
                status = 65;
                break;
            }
            if (read_exact(buffer + 24, length)) {
                status = frames && display_completed() ? 0 : 65;
                break;
            }
            if (
                (i == 3 && (memcmp(p + 24, reserved, 8) ||
                    !starts_with_nal(p + 32, length - 8, 5)))) {
                status = 65;
                break;
            }
        }
        const unsigned kind = read_le32(p + 16);
        const unsigned length = read_le32(p + 20);
        struct segment iov[3];
        iov[0].data = p;
        iov[0].size = 24;
        iov[1].data = p + 24;
        iov[1].size = kind == 3 ? 8 : length;
        iov[2].data = p + 32;
        iov[2].size = kind == 3 ? length - 8 : 0;
        /* Both multi-frame bench modes use 30 fps. Avoid draining buffered
         * USB frames in a burst before Screen can display them. */
        if (kind == 3 && frames && usleep(33333) != 0) {
            const int error = *__get_errno_ptr();
            printf("frame pacing failed errno=%d\n", error);
            status = 66;
            break;
        }
        const int rc = carlink_mdev_arm_sendv(coid, iov, kind == 3 ? 3 : 2);
        if (rc != 0) {
            printf("mdev message kind=%u frame=%u error=%d\n", kind, frames, rc);
            status = 67;
            break;
        }
        if (kind == 3) {
            ++frames;
            if (after_frame) {
                status = after_frame(frames, arg);
                if (status != 0)
                    break;
            }
        }
        p += 24 + length;
    }
    if (status == 65 && replay->live)
        printf("Live mdev stream ended early or had an invalid frame\n");
    const int close_rc = carlink_mdev_arm_close(coid);
    printf("mdev accepted frames=%u close rc=%d\n", frames, close_rc);
    if (status == 0 && close_rc != 0)
        status = 68;
    return status;
}
