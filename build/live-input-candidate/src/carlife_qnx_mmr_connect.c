/* Bench probe for the running QNX MM-Renderer service. */
#include "carlife_qnx_short_replay.h"

void *dlopen(const char *, int);
void *dlsym(void *, const char *);
char *dlerror(void);
int dlclose(void *);
int printf(const char *, ...);
int fflush(void *);
int system(const char *);
char *getenv(const char *);
int strcmp(const char *, const char *);
int getpid(void);
int snprintf(char *, __SIZE_TYPE__, const char *, ...);
int *__get_errno_ptr(void);

enum { QNX_LOAD_NOW = 2, QNX_LOAD_NOW_GLOBAL = 0x102 };

typedef struct mmr_connection mmr_connection_t;
typedef struct mmr_context mmr_context_t;
typedef mmr_context_t *(*context_create_fn)(mmr_connection_t *, const char *,
                                             unsigned, unsigned);
typedef int (*context_destroy_fn)(mmr_context_t *);
typedef struct screen_context *screen_context_t;
typedef struct screen_window *screen_window_t;
typedef struct screen_event *screen_event_t;
int screen_create_context(screen_context_t *, int);
int screen_create_window_type(screen_window_t *, screen_context_t, int);
int screen_set_window_property_iv(screen_window_t, int, const int *);
int screen_get_window_property_iv(screen_window_t, int, int *);
int screen_create_window_group(screen_window_t, const char *);
int screen_destroy_window(screen_window_t);
int screen_destroy_context(screen_context_t);
int screen_create_event(screen_event_t *);
int screen_destroy_event(screen_event_t);
int screen_get_event(screen_context_t, screen_event_t, unsigned long long);
int screen_get_event_property_iv(screen_event_t, int, int *);
int screen_get_event_property_pv(screen_event_t, int, void **);
int screen_get_window_property_cv(screen_window_t, int, int, char *);
int screen_set_window_property_cv(screen_window_t, int, int, const char *);
int screen_get_context_property_iv(screen_context_t, int, int *);
int screen_get_context_property_pv(screen_context_t, int, void **);
int screen_set_window_property_pv(screen_window_t, int, void **);
int screen_get_window_property_pv(screen_window_t, int, void **);
int screen_create_window_buffers(screen_window_t, int);
int screen_fill(screen_context_t, void *, const int *);
int screen_post_window(screen_window_t, void *, int, const int *, int);

enum { PROPERTY_VISIBLE = 0x2b };
/* BMW ABI: visibility 43 confirmed in NBT-AAD setVisible(), not the
 * public SDK's value 51. This opt-in mode uses only a private surface. */
static int visible_file;
static int visible_clip;
static int visible_stream;
static unsigned visible_width = 1280, visible_height = 720;
int pthread_create(unsigned *, const void *, void *(*)(void *), void *);
int pthread_join(unsigned, void **);

struct output_watch {
    screen_context_t screen;
    screen_window_t anchor;
    const char *window_id;
};

static void *load_strm(void)
{
    const char *const paths[] = {
        "/opt/conn/lib/carplay/libstrm.so",
        "/opt/mm/lib/libstrm.so",
        "/fs/sda0/opt/conn/lib/carplay/libstrm.so.1",
        "/fs/sda0/opt/mm/lib/libstrm.so.1"
    };
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        void *library = dlopen(paths[i], QNX_LOAD_NOW_GLOBAL);
        if (library) {
            printf("loaded strm: %s\n", paths[i]);
            return library;
        }
        const char *error = dlerror();
        printf("load strm failed: %s: %s\n", paths[i], error ? error : "?");
    }
    return 0;
}

static int close_library(void *library, const char *name)
{
    const int rc = dlclose(library);
    printf("close %s rc=%d\n", name, rc);
    return rc;
}

static int record(const char *step, int rc)
{
    const int error = rc == -1 ? *__get_errno_ptr() : 0;
    printf("%s rc=%d errno=%d\n", step, rc, error);
    return rc;
}

static int verify_hidden(screen_window_t window, const char *step)
{
    int visible = -1;
    if (record(step, screen_get_window_property_iv(
                         window, PROPERTY_VISIBLE, &visible)) != 0)
        return -1;
    printf("visible=%d\n", visible);
    return visible == 0 ? 0 : -1;
}

/* Match BMW NBT-AAD createWindow(): the application/group window, not
 * just the renderer child, must have an ID and a posted video buffer. */
static int prepare_visible_anchor(screen_context_t screen, screen_window_t window)
{
    int count = 0;
    void *displays[16];
    if (record("query display count", screen_get_context_property_iv(screen, 59, &count)) ||
        count < 1 || count > 16 ||
        record("query displays", screen_get_context_property_pv(screen, 60, displays)) ||
        record("bind display", screen_set_window_property_pv(window, 11, displays)))
        return -1;
    const int video = 8, format = 8, size[2] = {1280, 480}, pos[2] = {0, 0};
    const int background = -1000, hidden = 0;
    if (record("set anchor video usage", screen_set_window_property_iv(window, 48, &video)) ||
        record("register anchor ID 1510", screen_set_window_property_cv(window, 20, 4, "1510")) ||
        record("set anchor source size", screen_set_window_property_iv(window, 40, size)) ||
        record("set anchor position", screen_set_window_property_iv(window, 35, pos)) ||
        record("set anchor format", screen_set_window_property_iv(window, 14, &format)) ||
        record("set anchor background", screen_set_window_property_iv(window, 54, &background)) ||
        record("create anchor buffer", screen_create_window_buffers(window, 1)))
        return -1;
    void *buffer = 0;
    const int fill[3] = {0x0c, 0x01000000, 0}, rect[4] = {0, 0, 0, 0};
    if (record("get anchor buffer", screen_get_window_property_pv(window, 37, &buffer)) ||
        !buffer || record("fill anchor buffer", screen_fill(screen, buffer, fill)) ||
        record("post anchor buffer", screen_post_window(window, buffer, 1, rect, 0)) ||
        record("keep anchor hidden", screen_set_window_property_iv(window, PROPERTY_VISIBLE, &hidden)))
        return -1;
    return verify_hidden(window, "verify prepared anchor hidden");
}

struct replay_parameters {
    void *(*dict_new)(void);
    void *(*dict_set)(void *, const char *, const char *);
    int (*context)(mmr_context_t *, void *);
    int (*output)(mmr_context_t *, unsigned, void *);
    int (*input)(mmr_context_t *, void *);
    int (*speed)(mmr_context_t *, int);
};

struct setting {
    const char *key;
    const char *value;
};

static int resolve_parameters(struct replay_parameters *p, void *client, void *strm)
{
    p->dict_new = (__typeof__(p->dict_new))dlsym(strm, "strm_dict_new");
    p->dict_set = (__typeof__(p->dict_set))dlsym(strm, "strm_dict_set");
    p->context = (__typeof__(p->context))dlsym(client, "mmr_context_parameters");
    p->output = (__typeof__(p->output))dlsym(client, "mmr_output_parameters");
    p->input = (__typeof__(p->input))dlsym(client, "mmr_input_parameters");
    p->speed = (__typeof__(p->speed))dlsym(client, "mmr_speed_set");
    return p->dict_new && p->dict_set && p->context && p->output && p->input &&
           p->speed;
}

static void *settings_dict(const struct replay_parameters *p,
                           const struct setting *settings, unsigned count)
{
    void *dict = p->dict_new();
    for (unsigned i = 0; dict && i < count; ++i)
        dict = p->dict_set(dict, settings[i].key, settings[i].value);
    if (!dict)
        printf("could not create renderer settings dictionary\n");
    return dict;
}

/* The mmr parameter functions consume their dictionaries. */
static int set_context_parameters(const struct replay_parameters *p,
                                  mmr_context_t *renderer)
{
    static const struct setting settings[] = {
        {"prefetch_mode", "-1"},
        {"VideoDecoderLowLatencyMode", "1"},
        {"ColorSpaceMode", "1"}
    };
    void *dict = settings_dict(p, settings, sizeof(settings) / sizeof(settings[0]));
    return dict ? record("mmr_context_parameters", p->context(renderer, dict)) : -1;
}

static int set_output_parameters(const struct replay_parameters *p,
                                 mmr_context_t *renderer, unsigned id)
{
    static const struct setting settings[] = {
        {"mmf.SCREEN_WRITER_DISABLE_CADENCE", "1"},
        {"mmf.SCREEN_WRITER_DISABLE_MEDIACLOCK", "1"},
        {"mmf.SCREEN_WRITER_POST_BUFFER_NOSYNC", "1"}
    };
    void *dict = settings_dict(p, settings, sizeof(settings) / sizeof(settings[0]));
    return dict ? record("mmr_output_parameters", p->output(renderer, id, dict)) : -1;
}

static int set_input_parameters(const struct replay_parameters *p,
                                mmr_context_t *renderer)
{
    static const struct setting settings[] = {
        {"VideoDecoderLowLatencyMode", "1"},
        {"ColorSpaceMode", "1"}
    };
    void *dict = settings_dict(p, settings, sizeof(settings) / sizeof(settings[0]));
    return dict ? record("mmr_input_parameters", p->input(renderer, dict)) : -1;
}

#ifdef CARLINK_ENABLE_SHORT_REPLAY
enum { EVENT_NONE = 0, EVENT_POST = 9, PROPERTY_ID_STRING = 20,
       PROPERTY_EVENT_TYPE = 47, PROPERTY_EVENT_WINDOW = 52 };

/* Only this private group's renderer window is evidence of an output post. */
static int renderer_post(const struct output_watch *watch, screen_event_t event,
                         unsigned long long timeout)
{
    for (unsigned i = 0; i < 32; ++i) {
        int type = EVENT_NONE;
        if (screen_get_event(watch->screen, event, timeout) != 0 ||
            screen_get_event_property_iv(event, PROPERTY_EVENT_TYPE, &type) != 0)
            return -1;
        if (type == EVENT_NONE) {
            if (!timeout)
                return 0;
            continue;
        }
        if (type != EVENT_POST)
            continue;

        void *handle = 0;
        if (screen_get_event_property_pv(event, PROPERTY_EVENT_WINDOW, &handle) != 0 ||
            !handle)
            return -1;
        if (handle == watch->anchor)
            continue;
        screen_window_t window = handle;
        char id[64];
        id[sizeof(id) - 1] = 0;
        const int id_rc = screen_get_window_property_cv(
            window, PROPERTY_ID_STRING, sizeof(id) - 1, id);
        const int matched = id_rc == 0 && strcmp(id, watch->window_id) == 0;
        int visible = -1;
        const int visibility_rc = matched
            ? screen_get_window_property_iv(window, PROPERTY_VISIBLE, &visible) : 0;
        int show_rc = 0;
        if (matched && visible_file && visibility_rc == 0 && visible == 0) {
            const int shown = 1, hidden = 0;
            const int z = 2000;
            show_rc = record("raise private anchor", screen_set_window_property_iv(
                watch->anchor, 54, &z));
            if (!show_rc) show_rc = record("show private anchor", screen_set_window_property_iv(
                watch->anchor, PROPERTY_VISIBLE, &shown));
            if (!show_rc) show_rc = record("show private video", screen_set_window_property_iv(
                window, PROPERTY_VISIBLE, &shown));
            int actual = 0;
            if (!show_rc) show_rc = record("flush private visibility", screen_get_window_property_iv(
                window, PROPERTY_VISIBLE, &actual));
            printf("private video visibility before LayerManager=%d\n", actual);
            /* LayerManager has not selected this surface yet. Its readback
             * here is diagnostic, not proof that the later switch failed. */
            if (!show_rc) {
                printf("calling visible LayerManager stage\n");
                fflush(0);
                char command[192];
                snprintf(command, sizeof(command),
                    "CARLINK_VIDEO_WIDTH=%u CARLINK_VIDEO_HEIGHT=%u sh /dev/shmem/carlife_visible_first_frame.sh %s",
                    visible_width, visible_height, visible_stream ? "--show-live" : "--show");
                show_rc = system(command);
                printf("visible LayerManager stage raw status=%d\n", show_rc);
            }
            const int hide_video = record("hide private video", screen_set_window_property_iv(
                window, PROPERTY_VISIBLE, &hidden));
            const int hide_anchor = record("hide private anchor", screen_set_window_property_iv(
                watch->anchor, PROPERTY_VISIBLE, &hidden));
            if (hide_video || hide_anchor || verify_hidden(watch->anchor, "verify anchor restored"))
                show_rc = -1;
        }
        const int release_rc = record("release renderer window handle",
                                      screen_destroy_window(window));
        if (id_rc != 0 || visibility_rc != 0 || release_rc != 0 || show_rc != 0)
            return -1;
        if (matched) {
            printf("renderer window post id=%s visible=%d\n", id, visible);
            return visible == 0 ? 1 : -1;
        }
    }
    return timeout ? 0 : -1;
}

struct live_posts {
    const struct output_watch *watch;
    screen_event_t event;
    unsigned count;
};

static int observe_live_post(unsigned frames, void *arg)
{
    struct live_posts *posts = arg;
    const int post = renderer_post(posts->watch, posts->event, 0);
    if (post < 0)
        return 73;
    if (post) {
        ++posts->count;
        printf("live renderer POST observed after mdev frame=%u\n", frames);
    }
    return 0;
}

struct clip_feed { const struct carlink_replay *replay; int status; };
static void *feed_clip(void *arg)
{
    struct clip_feed *feed = arg;
    feed->status = carlink_play_replay(feed->replay, 0, 0);
    return 0;
}

static int play_replay(void *client, mmr_context_t *renderer,
                       const struct replay_parameters *p,
                       const struct output_watch *watch,
                       const struct carlink_replay *replay)
{
    typedef int (*playback_fn)(mmr_context_t *);
    playback_fn play = (playback_fn)dlsym(client, "mmr_play");
    playback_fn stop = (playback_fn)dlsym(client, "mmr_stop");
    printf("resolve play=%d stop=%d\n", !!play, !!stop);
    if (!play || !stop)
        return 62;
    if (record("speed before play", p->speed(renderer, 1000)) != 0)
        return 72;
    screen_event_t event = 0;
    if (record("create output event", screen_create_event(&event)) != 0)
        return 73;
    printf("starting renderer\n");
    fflush(0);
    int status = 63;
    if (record("mmr_play", play(renderer)) == 0) {
        const int speed_rc = record("speed after play", p->speed(renderer, 1000));
        status = speed_rc == 0 ? 0 : 72;
        int pre_post = 0;
        struct live_posts posts = {watch, event, 0};
        if (status == 0) {
            pre_post = renderer_post(watch, event, 0);
            printf("renderer post before feed=%d\n", pre_post);
            if (visible_clip && pre_post == 0) {
                struct clip_feed feed = {replay, 65};
                unsigned thread = 0;
                status = pthread_create(&thread, 0, feed_clip, &feed);
                if (status == 0) {
                    const int post = renderer_post(watch, event, 20000000ULL);
                    printf("visible clip first POST=%d\n", post);
                    const int joined = pthread_join(thread, 0);
                    status = joined ? 73 : feed.status;
                    if (status == 0 && post != 1) status = 78;
                } else status = 73;
            } else {
                status = pre_post < 0 || (visible_clip && pre_post != 0) ? 73 : carlink_play_replay(
                    replay, replay->live ? observe_live_post : 0, &posts);
            }
        }
        if (status == 0 && !replay->live && !visible_clip) {
            const int post = renderer_post(watch, event, 20000000ULL);
            printf("renderer post after feed=%d\n", post);
            status = post < 0 ? 73 : (pre_post == 0 && post == 1 ? 0 : 78);
        }
        if (replay->live && !visible_clip) {
            const unsigned during = posts.count;
            while (status == 0 && posts.count < replay->frames) {
                const int next = renderer_post(watch, event,
                    posts.count == 0 ? 20000000ULL : 0);
                if (next <= 0) {
                    if (next < 0)
                        status = 73;
                    break;
                }
                ++posts.count;
            }
            printf("live renderer posts observed=%u (during feed=%u, after feed=%u)\n",
                   posts.count, during, posts.count - during);
            /* A normal child window emits POST on its first frame only.
             * This verifies first output, not a count of decoded frames. */
            printf("live evidence: mdev acceptance and first output only; continuous display unverified\n");
            if (status == 0 && (pre_post != 0 || posts.count == 0))
                status = 78;
        }
        if (record("mmr_stop", stop(renderer)) != 0 && status == 0)
            status = 69;
    }
    if (record("destroy output event", screen_destroy_event(event)) != 0 &&
        status == 0)
        status = 73;
    return status;
}
#endif

static int test_input(void *client, mmr_context_t *renderer,
                      const struct replay_parameters *p,
                      const struct output_watch *watch,
                      const struct carlink_replay *replay)
{
    typedef int (*input_attach_fn)(mmr_context_t *, const char *, const char *);
    typedef int (*input_detach_fn)(mmr_context_t *);
    input_attach_fn attach = (input_attach_fn)dlsym(client, "mmr_input_attach");
    input_detach_fn detach = (input_detach_fn)dlsym(client, "mmr_input_detach");
    printf("resolve input attach=%d detach=%d\n", !!attach, !!detach);
    if (!attach || !detach)
        return 58;

    char input_url[160];
    const int length = snprintf(input_url, sizeof(input_url),
        "mdevpush:/dev/name/local/mdev?fourcc=H264&queue_capacity=60"
        "&video.frame_rate=%u&video.depth=16&video.width=%u&video.height=%u",
        replay ? replay->fps : 30u, replay ? replay->width : 1280u,
        replay ? replay->height : 720u);
    if (length < 0 || (__SIZE_TYPE__)length >= sizeof(input_url))
        return 59;
    if (p && (set_input_parameters(p, renderer) != 0 ||
              record("speed before input", p->speed(renderer, 1000)) != 0))
        return 71;
    printf("attaching H.264 input\n");
    fflush(0);
    if (record("mmr_input_attach", attach(renderer, input_url, "track")) != 0)
        return 59;

    int status = 0;
#ifdef CARLINK_ENABLE_SHORT_REPLAY
    if (p) {
        printf("reapplying context and input parameters after input attach\n");
        status = set_context_parameters(p, renderer) != 0 ||
                 set_input_parameters(p, renderer) != 0
            ? 71 : play_replay(client, renderer, p, watch, replay);
    }
#else
    (void)watch;
#endif
    printf("detaching input\n");
    fflush(0);
    const int detach_rc = record("mmr_input_detach", detach(renderer));
    return status != 0 ? status : (detach_rc == 0 ? 0 : 60);
}

static int test_output(void *client, void *strm, mmr_context_t *renderer,
                       const char *group, int input_mode,
                       const struct carlink_replay *replay)
{
    typedef int (*attach_fn)(mmr_context_t *, const char *, const char *);
    typedef int (*detach_fn)(mmr_context_t *, int);
    attach_fn attach = (attach_fn)dlsym(client, "mmr_output_attach");
    detach_fn detach = (detach_fn)dlsym(client, "mmr_output_detach");
    printf("resolve output attach=%d detach=%d\n", !!attach, !!detach);
    if (!attach || !detach)
        return 50;

    struct replay_parameters parameters;
    const struct replay_parameters *p = 0;
    if (replay) {
        if (!resolve_parameters(&parameters, client, strm)) {
            printf("required replay parameter API unavailable\n");
            return 70;
        }
        p = &parameters;
        if (set_context_parameters(p, renderer) != 0)
            return 71;
    }

    char window_id[64];
    /* BMW's LayerManager binding follows the numeric window ID. As in
     * NBT-AAD, the renderer output must use the prepared anchor's ID;
     * a separate child ID leaves surface 1510 showing the anchor fill. */
    const int id_length = visible_file
        ? snprintf(window_id, sizeof(window_id), "1510")
        : snprintf(window_id, sizeof(window_id), "%s-video", group);
    if (id_length < 0 || (__SIZE_TYPE__)id_length >= sizeof(window_id))
        return 51;
    char url[192];
    const int length = snprintf(url, sizeof(url),
        "screen:/any?winid=%s&nodstviewport=0&dstx=0&dsty=0"
        "&dstw=%d&dsth=%d&wingrp=%s", window_id,
        visible_file ? 1280 : 64, visible_file ? 480 : 64, group);
    if (length < 0 || (__SIZE_TYPE__)length >= sizeof(url))
        return 51;

    screen_context_t screen = 0;
    screen_window_t window = 0;
    const int hidden = 0;
    int status = 52;
    if (record("create screen context", screen_create_context(&screen, 0)) != 0 ||
        record("create window", screen_create_window_type(&window, screen, 0)) != 0 ||
        record("set hidden", screen_set_window_property_iv(
                                 window, PROPERTY_VISIBLE, &hidden)) != 0 ||
        verify_hidden(window, "read visibility before attach") != 0 ||
        record("create window group", screen_create_window_group(window, group)) != 0)
        goto cleanup;

    if (visible_file && prepare_visible_anchor(screen, window) != 0)
        goto cleanup;

    status = 53;
    printf("attaching output %s\n", url);
    fflush(0);
    const int output = record("mmr_output_attach", attach(renderer, url, "video"));
    if (output < 0)
        goto cleanup;
    status = verify_hidden(window, "read visibility after attach") == 0 ? 0 : 54;
    if (status == 0 && p && set_output_parameters(p, renderer, (unsigned)output) != 0)
        status = 71;
    if (status == 0 && input_mode) {
        const struct output_watch watch = {screen, window, window_id};
        status = test_input(client, renderer, p, &watch, replay);
        if (verify_hidden(window, "read visibility after input") != 0 &&
            status == 0)
            status = 61;
    }
    printf("detaching output %d\n", output);
    fflush(0);
    if (record("mmr_output_detach", detach(renderer, output)) != 0 && status == 0)
        status = 55;

cleanup:
    if (window && record("destroy window", screen_destroy_window(window)) != 0 &&
        status == 0)
        status = 56;
    if (screen && record("destroy screen context", screen_destroy_context(screen)) != 0 &&
        status == 0)
        status = 57;
    return status;
}

static int test_context(void *client, void *strm, mmr_connection_t *connection,
                        int test_screen_output, int input_mode,
                        const struct carlink_replay *replay)
{
    context_create_fn create = (context_create_fn)dlsym(client, "mmr_context_create");
    context_destroy_fn destroy = (context_destroy_fn)dlsym(client, "mmr_context_destroy");
    printf("resolve context create=%d destroy=%d\n", !!create, !!destroy);
    if (!create || !destroy)
        return 46;

    char name[40];
    const int length = snprintf(name, sizeof(name), "carlifebench%d", getpid());
    if (length < 0 || (__SIZE_TYPE__)length >= sizeof(name))
        return 47;
    printf("creating context %s\n", name);
    fflush(0);
    mmr_context_t *context = create(connection, name, 0, 0700);
    if (!context) {
        const int error = *__get_errno_ptr();
        printf("mmr_context_create failed errno=%d\n", error);
        return 48;
    }
    const int status = test_screen_output
        ? test_output(client, strm, context, name, input_mode, replay) : 0;
    printf("context created; destroying %s\n", name);
    fflush(0);
    const int rc = destroy(context);
    const int error = rc == -1 ? *__get_errno_ptr() : 0;
    printf("mmr_context_destroy rc=%d errno=%d\n", rc, error);
    return status != 0 ? status : (rc == 0 ? 0 : 49);
}

int carlink_mmr_connect_probe(int argc, char **argv)
{
    typedef mmr_connection_t *(*connect_fn)(const char *);
    typedef void (*disconnect_fn)(mmr_connection_t *);
    int status = 40;
    const int context_only = argc == 2 && strcmp(argv[1], "--context-only") == 0;
    const int output_only = argc == 2 && strcmp(argv[1], "--output-only") == 0;
    const struct carlink_replay *replay = 0;
#ifdef CARLINK_ENABLE_SHORT_REPLAY
    struct carlink_replay loaded;
    const int replay_once = argc == 2 && strcmp(argv[1], "--replay-once") == 0;
    const int visible_live = argc == 2 && strcmp(argv[1], "--visible-stream") == 0;
    visible_stream = visible_live;
    visible_clip = visible_live || (argc == 3 && strcmp(argv[1], "--visible-clip") == 0);
    visible_file = visible_clip || (argc == 3 && strcmp(argv[1], "--visible-file") == 0);
    if (visible_file && system("sh /dev/shmem/carlife_visible_first_frame.sh --check") != 0)
        return 75;
    const int replay_file = argc == 3 && (strcmp(argv[1], "--replay-file") == 0 || visible_file);
    const int replay_live = visible_live || (argc == 2 && strcmp(argv[1], "--replay-stream") == 0);
    if (replay_once || replay_file || replay_live) {
        const int rc = replay_live ? carlink_load_live_replay(&loaded)
            : carlink_load_replay(&loaded, replay_file ? argv[2] : 0);
        if (rc != 0) {
            printf("invalid or unreadable mdev replay\n");
            return rc;
        }
        if (replay_live &&
            system("sh /dev/shmem/carlife_qnx_replay_guard.sh --check </dev/null") != 0) {
            printf("mdev availability changed before live replay\n");
            return 75;
        }
        replay = &loaded;
        visible_width = loaded.width;
        visible_height = loaded.height;
        /* CARLINK_SESSION_SECONDS=0: no frame cap; the stream ends at EOF or
         * when the display script reports completion. */
        const char *session = getenv("CARLINK_SESSION_SECONDS");
        const int continuous = visible_live && session && !strcmp(session, "0");
        if (visible_live) loaded.frames = continuous ? 0x7fffffffu : CARLINK_VISIBLE_FRAMES;
        if (visible_file && !visible_clip && loaded.frames != 1) {
            printf("Visible test requires exactly one saved frame\n");
            return 2;
        }
        if (visible_clip && (loaded.fps != 30 || loaded.frames < 2 ||
            loaded.frames > (continuous ? 0x7fffffffu : visible_live ? CARLINK_VISIBLE_FRAMES : 180u)))
            return 2;
    }
#endif
    const int input_only = replay ||
        (argc == 2 && strcmp(argv[1], "--input-only") == 0);
    if (!context_only && !output_only && !input_only &&
        (argc != 2 || strcmp(argv[1], "--connect-only") != 0)) {
        printf("Expected --connect-only, --context-only, --output-only, or --input-only\n");
#ifdef CARLINK_ENABLE_SHORT_REPLAY
        printf("Replay options: --replay-once, --replay-file PATH or --replay-stream\n");
#endif
        return 2;
    }

    printf("loading strm and mmr client\n");
    fflush(0);
    void *strm = load_strm();
    if (!strm)
        return status;
    status = 41;
    void *client = dlopen(
        "/fs/sda0/opt/conn/lib/carplay/libmmrndclient.so.1", QNX_LOAD_NOW);
    if (!client) {
        const char *error = dlerror();
        printf("load mmr client failed: %s\n", error ? error : "?");
        goto cleanup;
    }

    status = 42;
    connect_fn connect = (connect_fn)dlsym(client, "mmr_connect");
    disconnect_fn disconnect = (disconnect_fn)dlsym(client, "mmr_disconnect");
    printf("resolve mmr_connect=%d mmr_disconnect=%d\n", !!connect, !!disconnect);
    if (!connect || !disconnect)
        goto cleanup;

    status = 43;
    printf("calling mmr_connect(NULL)\n");
    fflush(0);
    mmr_connection_t *connection = connect(0);
    if (!connection) {
        const int error = *__get_errno_ptr();
        printf("mmr_connect failed errno=%d\n", error);
        goto cleanup;
    }
    printf("mmr_connect ok\n");
    status = context_only || output_only || input_only
        ? test_context(client, strm, connection, output_only || input_only, input_only, replay)
        : 0;
    printf("calling mmr_disconnect\n");
    fflush(0);
    disconnect(connection);
    printf("mmr_disconnect returned\n");

cleanup:
    if (client && close_library(client, "mmr client") != 0 && status == 0)
        status = 44;
    if (close_library(strm, "strm") != 0 && status == 0)
        status = 45;
    printf("exit=%d\n", status);
    return status;
}
