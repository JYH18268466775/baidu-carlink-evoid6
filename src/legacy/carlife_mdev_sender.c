/* Replay the verified MDEVv1 file produced by carlife_mdev_verify.py.
 * Define CARLINK_MDEV_DRY_RUN for a standalone parser with a mock receiver.
 * The QNX adapter is callable only by an integrator that already owns a
 * configured renderer context and has opened its mdev connection.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FORMAT = 2, FRAME = 3, CONFIG = 4, SYNC = 5, HEADER_SIZE = 24 };
#define MAX_BODY (64u * 1024u * 1024u + 8u)

typedef int (*MdevSend)(void *context, const unsigned char *header,
                       const unsigned char *body, uint32_t length, uint32_t kind);

static uint32_t read_le32(const unsigned char *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

int carlink_mdev_replay(FILE *input, MdevSend send, void *context)
{
    static const unsigned char magic[] = "MDEVv1\r\n";
    static const unsigned char reserved[16] = {0};
    static const uint32_t setup[] = {SYNC, FORMAT, CONFIG};
    unsigned char prefix[sizeof(magic) - 1];
    unsigned char header[HEADER_SIZE];
    unsigned char *body = NULL;
    uint32_t capacity = 0;
    size_t messages = 0;
    size_t got;
    int status = 0;

    if (fread(prefix, 1, sizeof(prefix), input) != sizeof(prefix) ||
        memcmp(prefix, magic, sizeof(prefix)) != 0)
        return EINVAL;

    while ((got = fread(header, 1, sizeof(header), input)) != 0) {
        uint32_t kind;
        uint32_t length;
        uint32_t expected = messages < 3 ? setup[messages] : FRAME;

        if (got != sizeof(header) || memcmp(header, reserved, sizeof(reserved)) != 0) {
            status = EINVAL;
            break;
        }
        kind = read_le32(header + 16);
        length = read_le32(header + 20);
        if (kind != expected || length == 0 || length > MAX_BODY ||
            (kind == FORMAT && length != 32) ||
            (kind == SYNC && length != 4) ||
            (kind == FRAME && length <= 8)) {
            status = EINVAL;
            break;
        }
        if (length > capacity) {
            unsigned char *larger = realloc(body, length);
            if (larger == NULL) {
                status = ENOMEM;
                break;
            }
            body = larger;
            capacity = length;
        }
        if (fread(body, 1, length, input) != length) {
            status = ferror(input) ? EIO : EINVAL;
            break;
        }
        status = send(context, header, body, length, kind);
        if (status != 0)
            break;
        ++messages;
    }
    free(body);
    if (status != 0)
        return status;
    if (ferror(input))
        return EIO;
    return messages > 3 ? 0 : EINVAL;
}

#ifdef __QNXNTO__
#include <sys/neutrino.h>
#include <sys/uio.h>
#include <time.h>
#ifdef CARLINK_USE_ARM_MDEV_TRANSPORT
#include "carlife_qnx_mdev_arm.h"
#endif

typedef struct {
    int coid;
    int started;
    uint64_t origin_us;
} QnxMdev;

static int monotonic_us(uint64_t *value)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        int status = errno;
        return status != 0 ? status : EIO;
    }
    *value = (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
    return 0;
}

static int qnx_send(void *context, const unsigned char *header,
                    const unsigned char *body, uint32_t length, uint32_t kind)
{
    QnxMdev *state = context;
    iov_t iov[3];
    int parts = 2;
    int status;
    uint64_t now_us;
    unsigned char timestamp[8];

    SETIOV(&iov[0], header, HEADER_SIZE);
    SETIOV(&iov[1], body, length);
    if (kind == FRAME) {
        uint64_t offset_us = read_le32(body) | ((uint64_t)read_le32(body + 4) << 32);
        uint64_t deadline;
        unsigned int byte;

        status = monotonic_us(&now_us);
        if (status != 0)
            return status;
        if (!state->started) {
            state->started = 1;
            state->origin_us = now_us;
        }
        if (offset_us > UINT64_MAX - state->origin_us)
            return EINVAL;
        deadline = state->origin_us + offset_us;
        while (now_us < deadline) {
            uint64_t remaining = deadline - now_us;
            struct timespec delay;

            /* Bound each sleep so the conversion fits the target's time_t. */
            if (remaining > 1000000u)
                remaining = 1000000u;
            delay.tv_sec = remaining / 1000000u;
            delay.tv_nsec = (remaining % 1000000u) * 1000u;
            if (nanosleep(&delay, NULL) != 0 && errno != EINTR)
                return errno;
            status = monotonic_us(&now_us);
            if (status != 0)
                return status;
        }
        for (byte = 0; byte < sizeof(timestamp); ++byte)
            timestamp[byte] = (unsigned char)(now_us >> (8 * byte));
        SETIOV(&iov[1], timestamp, sizeof(timestamp));
        SETIOV(&iov[2], body + sizeof(timestamp), length - sizeof(timestamp));
        parts = 3;
    }

#ifdef CARLINK_USE_ARM_MDEV_TRANSPORT
    return carlink_mdev_arm_sendv(state->coid, iov, parts);
#else
    /* nbt-aad 0x125708 uses 1 s for setup and 0x127bb8 uses 250 ms for frames. */
    uint64_t timeout_ns = parts == 2 ? 1000000000u : 250000000u;
    if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY,
                     NULL, &timeout_ns, NULL) == -1)
        return errno;
    /* The frame path at 0x127be0 clears errno and uses -status for a negative
     * reply, or errno for a positive reply. A positive reply is still a failure.
     */
    errno = 0;
    status = MsgSendvs_r(state->coid, iov, parts, NULL, 0);
    if (status <= 0)
        return -status;
    return errno != 0 ? errno : EIO;
#endif
}

int carlink_mdev_replay_qnx(FILE *input, int coid)
{
    QnxMdev state = {0, 0, 0};
    if (coid < 0)
        return EINVAL;
    state.coid = coid;
    return carlink_mdev_replay(input, qnx_send, &state);
}
#endif

#ifdef CARLINK_MDEV_DRY_RUN
static int mock_send(void *context, const unsigned char *header,
                     const unsigned char *body, uint32_t length, uint32_t kind)
{
    size_t *frames = context;
    (void)header;
    (void)body;
    (void)length;
    if (kind == FRAME)
        ++*frames;
    return 0;
}

int main(int argc, char **argv)
{
    FILE *input;
    size_t frames = 0;
    int status;

    if (argc != 2) {
        fprintf(stderr, "Usage: mdev_dry_run <verified-replay.bin>\n");
        return 2;
    }
    input = fopen(argv[1], "rb");
    if (input == NULL) {
        perror(argv[1]);
        return 1;
    }
    status = carlink_mdev_replay(input, mock_send, &frames);
    fclose(input);
    if (status != 0) {
        fprintf(stderr, "Replay stopped after %lu frames: %s\n",
                (unsigned long)frames, strerror(status));
        return 1;
    }
    printf("Mock receiver accepted %lu frames\n", (unsigned long)frames);
    return 0;
}
#endif
