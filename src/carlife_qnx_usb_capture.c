/* One-shot, plaintext CarLife USB receiver for the supervised QNX bench. */
#include "carlife_qnx_short_replay.h"

typedef __SIZE_TYPE__ size_t;
typedef __PTRDIFF_TYPE__ ssize_t;

int open(const char *, int, ...);
int close(int);
int devctl(int, int, void *, size_t, int *);
ssize_t read(int, void *, size_t);
ssize_t write(int, const void *, size_t);
unsigned alarm(unsigned);
int usleep(unsigned);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, size_t);
int snprintf(char *, size_t, const char *, ...);
void *memcpy(void *, const void *, size_t);
int memcmp(const void *, const void *, size_t);
int *__get_errno_ptr(void);
int pthread_create(unsigned *, const void *, void *(*)(void *), void *);
int pthread_join(unsigned, void **);

enum {
    AOA_GET_MODE = 0x40040501,
    AOA_GET_PROTOCOL = 0x40040504,
    USB_BUFFER_SIZE = 65536,
    MAX_VIDEO_SIZE = 2 * 1024 * 1024,
    VERSION_MATCH = 0x00010002,
    VIDEO_INIT_DONE = 0x00010008,
    MD_AUTH_RESULT = 0x0001004b,
    FEATURE_REQUEST = 0x00010051,
    RSA_KEY_REQUEST = 0x0001006a,
    VIDEO_DATA = 0x00020001,
    VIDEO_HEARTBEAT = 0x00020002
};
#define AOA_CONFIG_SWITCH 0xc6040503u

/* Layout and devctls from the matching NBT-AAD usb_aoap.hpp.
 * Accessory identity from Baidu's AOAHostSetup.java. */
static const struct {
    int protocol;
    char manufacturer[256], model[256], description[256];
    char version[256], url[256], serial[256];
} identity = {1, "Baidu", "CarLife", "Baidu CarLife", "1.0.0",
              "http://carlife.baidu.com/", "0720SerialNo."};
_Static_assert(sizeof(identity) == 1540, "aoap-switch payload size");

static void log_line(const char *message)
{
    char line[160];
    int count = snprintf(line, sizeof(line), "%s\n", message);
    if (count > 0 && (size_t)count < sizeof(line))
        (void)write(2, line, (size_t)count);
}

static int failure(const char *stage, int code)
{
    char line[160];
    snprintf(line, sizeof(line), "%s: %d", stage, code);
    log_line(line);
    return -1;
}

static int device_path(const char *path, const char *prefix, size_t length)
{
    if (strncmp(path, prefix, length) != 0)
        return 0;
    path += length;
    if (!*path)
        return 0;
    for (; *path; ++path)
        if (*path < '0' || *path > '9')
            return 0;
    return 1;
}

static int switch_phone(int fd)
{
    int mode = -1;
    int rc = devctl(fd, AOA_GET_MODE, &mode, sizeof(mode), 0);
    if (rc != 0 || mode != 0)
        return failure("AOA mode unavailable or another client already switched",
                       rc ? rc : mode);
    int protocol = 0;
    rc = devctl(fd, AOA_GET_PROTOCOL, &protocol, sizeof(protocol), 0);
    if (rc != 0)
        return failure("AOA_GET_PROTOCOL", rc);
    if (protocol != 1 && protocol != 2)
        return failure("Unsupported USB AOA version", protocol);

    unsigned char payload[sizeof(identity)];
    memcpy(payload, &identity, sizeof(payload));
    rc = devctl(fd, (int)AOA_CONFIG_SWITCH, payload, sizeof(payload), 0);
    if (rc != 0)
        return failure("AOA_CONFIG_SWITCH", rc);
    log_line("AOA switch requested with Baidu CarLife identity");
    return 0;
}

static unsigned get_be32(const unsigned char *p)
{
    return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 |
           (unsigned)p[2] << 8 | p[3];
}

static void put_be32(unsigned char *p, unsigned value)
{
    p[0] = (unsigned char)(value >> 24);
    p[1] = (unsigned char)(value >> 16);
    p[2] = (unsigned char)(value >> 8);
    p[3] = (unsigned char)value;
}

static int send_cmd(int fd, unsigned type, const unsigned char *body, unsigned size)
{
    unsigned char outer[8], inner[64] = {0};
    if (size > sizeof(inner) - 8)
        return -1;
    put_be32(outer, 1);
    put_be32(outer + 4, 8 + size);
    inner[1] = (unsigned char)size;
    put_be32(inner + 4, type);
    if (size)
        memcpy(inner + 8, body, size);
    /* The phone reads the outer header and body in separate bulk transfers. */
    if (write(fd, outer, sizeof(outer)) != (ssize_t)sizeof(outer) ||
        write(fd, inner, 8 + size) != (ssize_t)(8 + size))
        return failure("USB command write failed, errno", *__get_errno_ptr());
    return 0;
}

static int read_varint(const unsigned char **data, unsigned *size, unsigned *value)
{
    *value = 0;
    for (unsigned shift = 0; *size && shift <= 28; shift += 7) {
        unsigned byte = *(*data)++;
        --*size;
        if (shift == 28 && byte > 15)
            return -1;
        *value |= (byte & 127) << shift;
        if (byte < 128)
            return 0;
    }
    return -1;
}

/* Read the varint fields used by MATCH_STATUS and VIDEO_INIT_DONE. */
static int read_fields(const unsigned char *data, unsigned size,
                       unsigned *values, unsigned count)
{
    unsigned seen = 0;
    while (size) {
        unsigned key, value;
        if (read_varint(&data, &size, &key) || !(key >> 3))
            return -1;
        unsigned field = key >> 3;
        if (!(key & 7)) {
            if (read_varint(&data, &size, &value))
                return -1;
            if (field <= count) {
                values[field - 1] = value;
                seen |= 1u << (field - 1);
            }
            continue;
        }
        switch (key & 7) {
        case 1: value = 8; break;
        case 2: if (read_varint(&data, &size, &value)) return -1; break;
        case 5: value = 4; break;
        default: return -1;
        }
        if (value > size)
            return -1;
        data += value;
        size -= value;
    }
    return seen == ((1u << count) - 1) ? 0 : -1;
}

enum session_stage { WAIT_MATCH, WAIT_VIDEO_INIT, WAIT_IDR };
enum version_retry { RETRY_UNUSED, RETRY_PENDING, RETRY_SENT };

struct capture {
    unsigned live, frames;
    unsigned width, height, fps;
    unsigned waiting_for_idr;
    enum version_retry version_retry;
    unsigned published, finished, writer_failed;
};

static int request_version(int fd)
{
    const unsigned char version[] = {8, 1, 16, 0};
    return send_cmd(fd, 0x00018001, version, sizeof(version));
}

static int handle_cmd(int fd, enum session_stage *stage, unsigned type,
                      const unsigned char *body, unsigned size, struct capture *capture)
{
    static const unsigned char stats[] = {
        0x0a, 0, 0x12, 3, '1', '.', '0', 0x18, 1, 0x22, 0,
        0x28, 0, 0x30, 0, 0x38, 0
    };
    static const unsigned char hu_info[] = {
        0x0a, 3, 'Q', 'N', 'X', 0x22, 3, 'B', 'M', 'W',
        0x72, 7, 'N', 'B', 'T', ' ', 'E', 'V', 'O'
    };
    static const unsigned char video_init[] = {8, 0x80, 10, 16, 0xd0, 5, 24, 30};
    static const unsigned char module_control[] = {8, 5, 16, 2};
    static const unsigned char no_features[] = {8, 0};
    char line[96];
    snprintf(line, sizeof(line), "CarLife CMD RX 0x%08x (%u bytes)", type, size);
    log_line(line);

    if (*stage == WAIT_MATCH && capture->version_retry == RETRY_PENDING &&
        (type == 0x00010004 || type == 0x00010026)) {
        capture->version_retry = RETRY_SENT;
        log_line("Version response may have been lost; retrying version request once");
        return request_version(fd);
    }

    switch (type) {
    case VERSION_MATCH: {
        unsigned status = 0;
        if (*stage != WAIT_MATCH)
            return 0;
        if (read_fields(body, size, &status, 1) || status != 1)
            return failure("CarLife 1.0 version rejected or invalid", (int)status);
        log_line("CarLife 1.0 accepted; requesting 1280x720 @ 30 fps");
        if (send_cmd(fd, 0x00018027, stats, sizeof(stats)) ||
            send_cmd(fd, 0x00018003, hu_info, sizeof(hu_info)) ||
            send_cmd(fd, 0x00018007, video_init, sizeof(video_init)) ||
            send_cmd(fd, 0x00018028, module_control, sizeof(module_control)))
            return -1;
        *stage = WAIT_VIDEO_INIT;
        return 0;
    }
    case VIDEO_INIT_DONE: {
        unsigned video[3];
        if (*stage != WAIT_VIDEO_INIT)
            return 0;
        if (read_fields(body, size, video, 3) || !video[0] || !video[1] ||
            video[0] > 8192 || video[1] > 8192 || video[2] > 240)
            return failure("Invalid VIDEO_ENCODER_INIT_DONE", 0);
        snprintf(line, sizeof(line), "Phone encoder: %ux%u @ %u fps",
                 video[0], video[1], video[2]);
        log_line(line);
        capture->width = video[0];
        capture->height = video[1];
        capture->fps = video[2] ? video[2] : 30;
        if (capture->live && capture->fps != 30)
            return failure("Live bench requires negotiated 30 fps", (int)capture->fps);
        if (send_cmd(fd, 0x00018009, 0, 0) || send_cmd(fd, 0x0001801d, 0, 0))
            return -1;
        *stage = WAIT_IDR;
        log_line("VIDEO_ENCODER_START sent; waiting for SPS, PPS and IDR");
        return 0;
    }
    case FEATURE_REQUEST:
        return send_cmd(fd, 0x00018052, no_features, sizeof(no_features));
    case MD_AUTH_RESULT: {
        unsigned accepted = 0;
        if (read_fields(body, size, &accepted, 1) || accepted != 1)
            return failure("Phone rejected HU authentication", (int)accepted);
        return 0;
    }
    case RSA_KEY_REQUEST:
        return failure("Phone requires CarLife encryption; plaintext capture stopped", 0);
    default:
        return 0;
    }
}

static unsigned nal_prefix(const unsigned char *p, unsigned size)
{
    if (size < 4 || p[0] || p[1])
        return 0;
    if (p[2] == 1)
        return 3;
    return size >= 5 && !p[2] && p[3] == 1 ? 4 : 0;
}

static int save_bytes(const unsigned char *data, unsigned size)
{
    while (size) {
        ssize_t count = write(1, data, size);
        if (count <= 0)
            return failure("Capture file write failed, errno", *__get_errno_ptr());
        data += count;
        size -= (unsigned)count;
    }
    return 0;
}

/* A bounded, append-only spool for this 20-frame test. Keep USB reads
 * independent of pipe backpressure while the renderer starts or sends. */
static unsigned char live_spool[4 * 1024 * 1024];

static void *live_writer(void *arg)
{
    struct capture *capture = arg;
    unsigned sent = 0;
    for (;;) {
        unsigned finished = __atomic_load_n(&capture->finished, __ATOMIC_ACQUIRE);
        unsigned available = __atomic_load_n(&capture->published, __ATOMIC_ACQUIRE);
        if (sent < available) {
            if (save_bytes(live_spool + sent, available - sent)) {
                __atomic_store_n(&capture->writer_failed, 1, __ATOMIC_RELEASE);
                return 0;
            }
            sent = available;
        } else if (finished) {
            return 0;
        } else {
            usleep(1000);
        }
    }
}

/* ARMv7 QNX and mdev use little endian words. Publish only complete records. */
static int live_record(struct capture *capture, unsigned kind,
                       const void *first, unsigned first_size,
                       const void *second, unsigned second_size)
{
    const unsigned header[6] = {0, 0, 0, 0, kind, first_size + second_size};
    unsigned offset = __atomic_load_n(&capture->published, __ATOMIC_RELAXED);
    const unsigned prefix_size = kind == 5 ? 8 : 0;
    const unsigned size = prefix_size + sizeof(header) + first_size + second_size;
    if (size > sizeof(live_spool) - offset)
        return failure("Live video spool full after accepted USB frames", (int)capture->frames);
    if (prefix_size) {
        memcpy(live_spool + offset, "MDEVv1\r\n", prefix_size);
        offset += prefix_size;
    }
    memcpy(live_spool + offset, header, sizeof(header));
    offset += sizeof(header);
    memcpy(live_spool + offset, first, first_size);
    offset += first_size;
    if (second_size) {
        memcpy(live_spool + offset, second, second_size);
        offset += second_size;
    }
    __atomic_store_n(&capture->published, offset, __ATOMIC_RELEASE);
    return 0;
}

static int capture_video(struct capture *capture, const unsigned char *data, unsigned size)
{
    static unsigned char sps[4096], pps[4096];
    static unsigned sps_size, pps_size;
    unsigned picture = 0;
    unsigned idr_offset = size;
    for (unsigned offset = 0; offset < size;) {
        unsigned prefix = nal_prefix(data + offset, size - offset);
        if (!prefix)
            return failure("Video payload has no Annex B start code", 0);
        unsigned type = data[offset + prefix] & 31;
        unsigned end = offset + prefix + 1;
        while (end < size && !nal_prefix(data + end, size - end))
            ++end;
        if (type == 7 || type == 8) {
            unsigned count = end - offset;
            if (count > sizeof(sps))
                return failure("H.264 parameter set exceeds capture limit", (int)count);
            unsigned char *stored = type == 7 ? sps : pps;
            unsigned *stored_size = type == 7 ? &sps_size : &pps_size;
            if (capture->frames) {
                const unsigned old_prefix = nal_prefix(stored, *stored_size);
                if (count - prefix != *stored_size - old_prefix ||
                    memcmp(stored + old_prefix, data + offset + prefix, count - prefix) != 0)
                    return failure("H.264 parameter set changed during live capture", (int)type);
            }
            memcpy(stored, data + offset, count);
            *stored_size = count;
        }
        picture |= capture->frames && type >= 1 && type <= 5;
        if (type == 5 && idr_offset == size)
            idr_offset = offset;
        if (!capture->frames && type == 5 && sps_size && pps_size) {
            if (!capture->live) {
                if (save_bytes(sps, sps_size) || save_bytes(pps, pps_size) ||
                    save_bytes(data + offset, size - offset))
                    return -1;
                log_line("Captured Annex B SPS + PPS + first IDR");
                return 1;
            }
            const unsigned sync = 1;
            const unsigned format[8] = {2, 0x48323634, capture->width, capture->height,
                                        16, capture->fps, 0, 0};
            if (live_record(capture, 5, &sync, sizeof(sync), 0, 0) ||
                live_record(capture, 2, format, sizeof(format), 0, 0) ||
                live_record(capture, 4, sps, sps_size, pps, pps_size))
                return -1;
            log_line("Captured Annex B SPS + PPS + first IDR");
            data += offset;
            size -= offset;
            idr_offset = 0;
            picture = 1;
            break;
        }
        offset = end;
    }
    if (capture->waiting_for_idr) {
        if (idr_offset == size || !sps_size || !pps_size)
            return 0;
        data += idr_offset;
        size -= idr_offset;
        capture->waiting_for_idr = 0;
        log_line("Recovered video at an IDR after USB transfer loss");
    }
    if (capture->live && picture) {
        const unsigned timestamp[2] = {capture->frames * 1000000u / 30u, 0};
        if (live_record(capture, 3, timestamp, sizeof(timestamp), data, size))
            return -1;
        if (++capture->frames == (capture->live == 2 ? 300u : CARLINK_LIVE_FRAMES)) {
            log_line(capture->live == 2 ? "Queued 300 live USB frames" : "Queued 20 live USB frames");
            return 1;
        }
    }
    return 0;
}

struct stream {
    unsigned char *data;
    unsigned capacity, header_size, used, wanted;
};

enum { BAD_VIDEO_HEADER = -2 };

static int is_phone_command(unsigned type)
{
    return type > 0x00010000 && type <= 0x000100ff;
}

/* Outer payloads can carry a full inner record or slices of one record.
 * Keep independent command and video state so channels may interleave. */
static int consume(int fd, enum session_stage *stage, struct stream *stream,
                   const unsigned char *data, unsigned size, struct capture *capture)
{
    while (size) {
        unsigned count = stream->wanted - stream->used;
        if (count > size)
            count = size;
        memcpy(stream->data + stream->used, data, count);
        stream->used += count;
        data += count;
        size -= count;
        if (stream->used != stream->wanted)
            continue;

        int command = stream->header_size == 8;
        if (stream->wanted == stream->header_size) {
            const unsigned type = get_be32(stream->data + stream->header_size - 4);
            unsigned body_size = command
                ? (unsigned)stream->data[0] << 8 | stream->data[1]
                : get_be32(stream->data);
            if (body_size > stream->capacity - stream->header_size ||
                (command ? !is_phone_command(type) :
                    (type != VIDEO_DATA && type != VIDEO_HEARTBEAT))) {
                char line[120];
                snprintf(line, sizeof(line), "Invalid CarLife inner %s header: length=%08x "
                         "type=%08x after %u video frames", command ? "CMD" : "VIDEO",
                         body_size, type, capture->frames);
                log_line(line);
                return command ? -1 : BAD_VIDEO_HEADER;
            }
            stream->wanted += body_size;
            if (body_size)
                continue;
        }
        unsigned type = get_be32(stream->data + stream->header_size - 4);
        unsigned body_size = stream->wanted - stream->header_size;
        const unsigned char *body = stream->data + stream->header_size;
        int result = 0;
        if (command)
            result = handle_cmd(fd, stage, type, body, body_size, capture);
        else if (type == VIDEO_DATA && body_size && *stage == WAIT_IDR)
            result = capture_video(capture, body, body_size);
        stream->used = 0;
        stream->wanted = stream->header_size;
        if (result)
            return result;
    }
    return 0;
}

static int is_unwrapped_command(const unsigned char *data, unsigned size)
{
    if (size < 8 || data[2] || data[3])
        return 0;
    const unsigned length = (unsigned)data[0] << 8 | data[1];
    const unsigned type = get_be32(data + 4);
    return length == size - 8 && is_phone_command(type);
}

static int valid_envelope(unsigned channel, unsigned length)
{
    return channel >= 1 && channel <= 6 && length && length <= MAX_VIDEO_SIZE + 12;
}

static int is_complete_media_record(const unsigned char *data, unsigned size)
{
    if (size < 12 || get_be32(data) != size - 12)
        return 0;
    const unsigned type = get_be32(data + 8);
    return type >= 0x00030001 && type <= 0x00030006;
}

static int receive(int fd, struct capture *capture)
{
    /* Commands carry a 16-bit body length and may span several USB reads. */
    static unsigned char usb[USB_BUFFER_SIZE], cmd[8 + 0xffff], video[MAX_VIDEO_SIZE + 12];
    struct stream streams[] = {{cmd, sizeof(cmd), 8, 0, 8},
                               {video, sizeof(video), 12, 0, 12}};
    enum session_stage stage = WAIT_MATCH;
    unsigned char header[8];
    unsigned header_used = 0, channel = 0, remaining = 0;
    unsigned transfer = 0, previous_count = 0, envelope_length = 0;
    unsigned header_transfer = 0, header_offset = 0;
    int discard_payload = 0;

    usleep(1000000);
    if (request_version(fd))
        return -1;
    log_line("HU_PROTOCOL_VERSION 1.0 sent");
    for (;;) {
        if (__atomic_load_n(&capture->writer_failed, __ATOMIC_ACQUIRE))
            return -1;
        ssize_t count = read(fd, usb, sizeof(usb));
        if (count < 0)
            return failure("USB read failed, errno", *__get_errno_ptr());
        if (!count) {
            /* devc-serusb-aoap reports a USB zero-length packet as read() == 0. */
            usleep(1000);
            continue;
        }
        ++transfer;
        /* A complete media record can arrive without its outer header while
         * the preceding standalone audio header is still waiting for data. */
        if (capture->live && channel == 3 && remaining && remaining == envelope_length &&
            previous_count == 8 && (unsigned)count > remaining &&
            is_complete_media_record(usb, (unsigned)count)) {
            log_line("Skipped complete CarLife media record with mismatched AOA length");
            remaining = 0;
            previous_count = (unsigned)count;
            continue;
        }
        /* devc-serusb-aoap returns individual bulk transfers. A standalone
         * AOA header at the next read boundary is not the previous envelope's
         * missing body. Discard that incomplete record before accepting it. */
        if (remaining && count == 8 && valid_envelope(get_be32(usb), get_be32(usb + 4))) {
            char line[120];
            snprintf(line, sizeof(line), "Missing AOA payload at RX #%u: channel=%u "
                     "remaining=%u; resuming at next header", transfer, channel, remaining);
            log_line(line);
            if (channel <= 2) {
                struct stream *interrupted = &streams[channel - 1];
                interrupted->used = 0;
                interrupted->wanted = interrupted->header_size;
            }
            if (channel == 2)
                capture->waiting_for_idr = 1;
            if (channel == 1 && stage == WAIT_MATCH && capture->version_retry == RETRY_UNUSED)
                capture->version_retry = RETRY_PENDING;
            remaining = 0;
        }
        /* The bench phone delivered MD_AUTHEN_RESULT as a whole USB read
         * without its AOA header. Only accept an exact command at a read
         * boundary, with no unfinished outer envelope or command. */
        if (!remaining && !header_used && !streams[0].used &&
            is_unwrapped_command(usb, (unsigned)count)) {
            log_line("Complete CarLife CMD received without AOA header");
            const int rc = consume(fd, &stage, &streams[0], usb, (unsigned)count, capture);
            if (rc)
                return rc;
            previous_count = (unsigned)count;
            continue;
        }
        for (unsigned offset = 0; offset < (unsigned)count;) {
            if (!remaining) {
                if (!header_used) {
                    header_transfer = transfer;
                    header_offset = offset;
                }
                header[header_used++] = usb[offset++];
                if (header_used != sizeof(header))
                    continue;
                const unsigned next_channel = get_be32(header);
                const unsigned next_length = get_be32(header + 4);
                header_used = 0;
                if (!valid_envelope(next_channel, next_length)) {
                    char line[144];
                    snprintf(line, sizeof(line),
                             "Invalid AOA header %08x %08x at RX #%u+%u; "
                             "finished in RX #%u (%u bytes)",
                             next_channel, next_length, header_transfer, header_offset,
                             transfer, (unsigned)count);
                    log_line(line);
                    snprintf(line, sizeof(line),
                             "Previous RX size=%u; prior AOA channel=%u length=%u; "
                             "inner pending cmd=%u video=%u",
                             previous_count, channel, envelope_length,
                             streams[0].used, streams[1].used);
                    log_line(line);
                    if (count >= 12) {
                        const unsigned body_length = get_be32(usb);
                        const unsigned type = get_be32(usb + 8);
                        snprintf(line, sizeof(line), "RX start: word0=%08x word2=%08x "
                                 "matches complete media record=%u", body_length, type,
                                 (unsigned)is_complete_media_record(usb, (unsigned)count));
                        log_line(line);
                    }
                    return -1;
                }
                channel = next_channel;
                remaining = envelope_length = next_length;
                discard_payload = 0;
                continue;
            }
            unsigned chunk = (unsigned)count - offset;
            if (chunk > remaining)
                chunk = remaining;
            if (channel <= 2 && !discard_payload) {
                int rc = consume(fd, &stage, &streams[channel - 1], usb + offset, chunk, capture);
                if (rc < 0) {
                    char line[124];
                    snprintf(line, sizeof(line), "RX #%u offset=%u size=%u AOA channel=%u "
                             "length=%u left=%u", transfer, offset, (unsigned)count,
                             channel, envelope_length, remaining);
                    log_line(line);
                }
                if (rc == BAD_VIDEO_HEADER && capture->live && capture->waiting_for_idr) {
                    /* During recovery an envelope can begin mid-picture.
                     * Trust only the outer length and wait for a fresh IDR. */
                    streams[1].used = 0;
                    streams[1].wanted = streams[1].header_size;
                    discard_payload = 1;
                    log_line("Discarding damaged video payload while waiting for IDR");
                } else if (rc) {
                    return rc;
                }
            }
            offset += chunk;
            remaining -= chunk;
        }
        previous_count = (unsigned)count;
    }
}

int carlink_usb_capture(int argc, char **argv)
{
    if (argc != 3)
        return 2;
    int switching = strcmp(argv[1], "--switch") == 0;
    struct capture capture = {0};
    capture.live = strcmp(argv[1], "--stream-mdev") == 0;
    if (strcmp(argv[1], "--stream-visible") == 0) capture.live = 2;
    if (switching ? !device_path(argv[2], "/dev/android", 12)
                  : (!capture.live && strcmp(argv[1], "--capture")) ||
                    !device_path(argv[2], "/dev/oap", 8))
        return 2;

    alarm(switching ? 5 : 45);
    int fd = open(argv[2], 2); /* O_RDWR */
    if (fd < 0) {
        failure("USB device open failed, errno", *__get_errno_ptr());
        return 10;
    }
    unsigned writer = 0;
    if (capture.live) {
        int rc = pthread_create(&writer, 0, live_writer, &capture);
        if (rc) {
            failure("Live pipe writer startup failed", rc);
            close(fd);
            return 13;
        }
    }
    int result = switching ? switch_phone(fd) : receive(fd, &capture);
    const int close_rc = close(fd);
    const int close_error = close_rc != 0 ? *__get_errno_ptr() : 0;
    if (capture.live) {
        __atomic_store_n(&capture.finished, 1, __ATOMIC_RELEASE);
        int rc = pthread_join(writer, 0);
        if (rc || capture.writer_failed) {
            failure("Live pipe writer stopped", rc);
            result = -1;
        }
    }
    if (close_rc != 0) {
        failure("USB device close failed, errno", close_error);
        return 11;
    }
    return result == (switching ? 0 : 1) ? 0 : 12;
}
