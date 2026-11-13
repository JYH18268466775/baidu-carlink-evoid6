/* Minimal player for the captured CarLink 1280x720 / 30 fps replay.
 * The host build exercises startup and cleanup without touching any device.
 * The QNX build requires matching Screen/MM-Renderer headers and libraries.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int (*MdevSend)(void *, const unsigned char *, const unsigned char *,
                       uint32_t, uint32_t);
int carlink_mdev_replay(FILE *, MdevSend, void *);

enum Stage {
    WINDOW, RENDERER, CONTEXT, OUTPUT, INPUT, PLAY, MDEV, STAGE_COUNT
};

static const char *const stage_names[] = {
    "Screen window", "renderer connection", "renderer context",
    "video output", "video input", "playback start", "mdev connection"
};

typedef struct {
    int (*start)(void *, enum Stage);
    void (*stop)(void *, enum Stage);
    int (*replay)(void *, FILE *);
} PlayerOps;

static int verify_format(void *unused, const unsigned char *header,
                         const unsigned char *body, uint32_t length, uint32_t kind)
{
    static const unsigned char expected[32] = {
        2, 0, 0, 0, 0x34, 0x36, 0x32, 0x48, 0, 5, 0, 0, 0xd0, 2, 0, 0,
        16, 0, 0, 0, 30, 0, 0, 0
    };
    (void)unused;
    (void)header;
    if (kind == 2 && (length != sizeof(expected) || memcmp(body, expected, length)))
        return EINVAL;
    return 0;
}

static int run_player(FILE *input, const PlayerOps *ops, void *state)
{
    int stage;
    int status = carlink_mdev_replay(input, verify_format, NULL);

    if (status != 0 || fseek(input, 0, SEEK_SET) != 0) {
        fprintf(stderr, "Invalid or non-seekable 1280x720 replay\n");
        return status != 0 ? status : EIO;
    }
    for (stage = 0; stage < STAGE_COUNT; ++stage) {
        status = ops->start(state, (enum Stage)stage);
        if (status != 0) {
            fprintf(stderr, "%s failed: %s\n", stage_names[stage], strerror(status));
            break;
        }
    }
    if (stage == STAGE_COUNT) {
        status = ops->replay(state, input);
        if (status != 0)
            fprintf(stderr, "Replay failed: %s\n", strerror(status));
        --stage;
    }
    /* Include the failed stage in case startup acquired only some of its resources. */
    for (; stage >= 0; --stage)
        ops->stop(state, (enum Stage)stage);
    return status;
}

#ifdef __QNXNTO__
#include <dlfcn.h>
#include <mm/renderer.h>
#include <screen/screen.h>
#include <sys/dispatch.h>
#include <unistd.h>
#ifdef CARLINK_USE_ARM_MDEV_TRANSPORT
#include "carlife_qnx_mdev_arm.h"
#endif

int carlink_mdev_replay_qnx(FILE *, int);

typedef struct {
    const char *library_path;
    void *library;
    __typeof__(&mmr_connect) connect;
    __typeof__(&mmr_disconnect) disconnect;
    __typeof__(&mmr_context_create) create;
    __typeof__(&mmr_context_destroy) destroy;
    __typeof__(&mmr_output_attach) output_attach;
    __typeof__(&mmr_input_attach) input_attach;
    __typeof__(&mmr_play) play;
    __typeof__(&mmr_stop) stop;
    mmr_connection_t *connection;
    mmr_context_t *context;
    screen_context_t screen;
    screen_window_t window;
    char group[40];
    char output_url[256];
    int playing;
    int coid;
} QnxPlayer;

static int create_window(QnxPlayer *state)
{
    screen_buffer_t buffer;
    void *pixels;
    int stride;
    int size[] = {1280, 720};
    int rect[] = {0, 0, 1280, 720};
    int usage = SCREEN_USAGE_WRITE;
    int format = SCREEN_FORMAT_RGBA8888;
    int visible = 1;

    if (screen_create_context(&state->screen, SCREEN_APPLICATION_CONTEXT) != 0 ||
        screen_create_window(&state->window, state->screen) != 0 ||
        screen_create_window_group(state->window, state->group) != 0 ||
        screen_set_window_property_iv(state->window, SCREEN_PROPERTY_USAGE, &usage) != 0 ||
        screen_set_window_property_iv(state->window, SCREEN_PROPERTY_FORMAT, &format) != 0 ||
        screen_set_window_property_iv(state->window, SCREEN_PROPERTY_SIZE, size) != 0 ||
        screen_set_window_property_iv(state->window, SCREEN_PROPERTY_BUFFER_SIZE, size) != 0 ||
        screen_create_window_buffers(state->window, 1) != 0 ||
        screen_get_window_property_pv(state->window, SCREEN_PROPERTY_RENDER_BUFFERS,
                                      (void **)&buffer) != 0 ||
        screen_get_buffer_property_pv(buffer, SCREEN_PROPERTY_POINTER, &pixels) != 0 ||
        screen_get_buffer_property_iv(buffer, SCREEN_PROPERTY_STRIDE, &stride) != 0)
        return EIO;
    if (pixels == NULL || stride < 1280 * 4 || (size_t)stride > SIZE_MAX / 720u)
        return EIO;
    memset(pixels, 0, (size_t)stride * 720u);
    if (screen_post_window(state->window, buffer, 1, rect, 0) != 0 ||
        screen_set_window_property_iv(state->window, SCREEN_PROPERTY_VISIBLE, &visible) != 0)
        return EIO;
    return 0;
}

static int qnx_start(void *opaque, enum Stage stage)
{
    QnxPlayer *state = opaque;
    static const char input_url[] =
        "mdevpush:/dev/name/local/mdev?fourcc=H264&queue_capacity=60"
        "&video.frame_rate=30&video.depth=16&video.width=1280&video.height=720";

    switch (stage) {
    case WINDOW:
        return create_window(state);
    case RENDERER:
        state->library = dlopen(state->library_path, RTLD_NOW | RTLD_LOCAL);
        if (state->library == NULL) {
            fprintf(stderr, "%s\n", dlerror());
            return ENOENT;
        }
#define LOAD(member, symbol) do { \
    state->member = (__typeof__(state->member))dlsym(state->library, #symbol); \
    if (state->member == NULL) { \
        fprintf(stderr, "Missing MM-Renderer symbol: %s\n", #symbol); \
        return ENOSYS; \
    } \
} while (0)
        LOAD(connect, mmr_connect);
        LOAD(disconnect, mmr_disconnect);
        LOAD(create, mmr_context_create);
        LOAD(destroy, mmr_context_destroy);
        LOAD(output_attach, mmr_output_attach);
        LOAD(input_attach, mmr_input_attach);
        LOAD(play, mmr_play);
        LOAD(stop, mmr_stop);
#undef LOAD
        state->connection = state->connect(NULL);
        return state->connection != NULL ? 0 : EIO;
    case CONTEXT:
        state->context = state->create(state->connection, state->group, 0, 0700);
        return state->context != NULL ? 0 : EIO;
    case OUTPUT:
        return state->output_attach(state->context, state->output_url, "video") >= 0 ? 0 : EIO;
    case INPUT:
        return state->input_attach(state->context, input_url, "track") == 0 ? 0 : EIO;
    case PLAY:
        if (state->play(state->context) != 0)
            return EIO;
        state->playing = 1;
        return 0;
    case MDEV:
        /* nbt-aad 0x1293dc passes this full path and flags 0. */
#ifdef CARLINK_USE_ARM_MDEV_TRANSPORT
        state->coid = carlink_mdev_arm_open();
#else
        state->coid = name_open("/dev/name/local/mdev", 0);
#endif
        if (state->coid >= 0)
            return 0;
        return errno != 0 ? errno : EIO;
    default:
        return EINVAL;
    }
}

static void qnx_stop(void *opaque, enum Stage stage)
{
    QnxPlayer *state = opaque;

    switch (stage) {
    case MDEV:
        if (state->coid >= 0) {
#ifdef CARLINK_USE_ARM_MDEV_TRANSPORT
            carlink_mdev_arm_close(state->coid);
#else
            name_close(state->coid);
#endif
        }
        break;
    case PLAY:
        if (state->playing)
            state->stop(state->context);
        break;
    case CONTEXT:
        if (state->context != NULL)
            state->destroy(state->context);
        break;
    case RENDERER:
        if (state->connection != NULL)
            state->disconnect(state->connection);
        if (state->library != NULL)
            dlclose(state->library);
        break;
    case WINDOW:
        if (state->window != NULL)
            screen_destroy_window(state->window);
        if (state->screen != NULL)
            screen_destroy_context(state->screen);
        break;
    default:
        break;
    }
}

static int qnx_replay(void *opaque, FILE *input)
{
    QnxPlayer *state = opaque;
    return carlink_mdev_replay_qnx(input, state->coid);
}

int main(int argc, char **argv)
{
    const PlayerOps ops = {qnx_start, qnx_stop, qnx_replay};
    QnxPlayer state = {0};
    FILE *input;
    int status;

    if (argc != 4 || strcmp(argv[1], "--bench") != 0) {
        fprintf(stderr, "Usage: carlife_qnx_player --bench <replay.bin> <mmrndclient.so>\n");
        return 2;
    }
    state.library_path = argv[3];
    state.coid = -1;
    snprintf(state.group, sizeof(state.group), "carlinkbench%ld", (long)getpid());
    snprintf(state.output_url, sizeof(state.output_url),
             "screen:/any?winid=%s-video&nodstviewport=0&dstx=0&dsty=0&dstw=1280&dsth=720&wingrp=%s",
             state.group, state.group);
    input = fopen(argv[2], "rb");
    if (input == NULL) {
        perror(argv[2]);
        return 1;
    }
    status = run_player(input, &ops, &state);
    fclose(input);
    return status != 0;
}
#endif
