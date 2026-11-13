/* QNX ARM bench probe. Startup and Screen values are from local ID6 ELFs. */
__asm__(
    ".syntax unified\n"
    ".arm\n"
    ".text\n"
    ".global _start\n"
    ".type _start, %function\n"
    "_start:\n"
    "mov r7, r0\n"
    "ldr r4, [sp]\n"
    "add r5, sp, #4\n"
    "add r6, r5, r4, lsl #2\n"
    "add r6, r6, #4\n"
    "mov r3, r6\n"
    "1: ldr r12, [r3], #4\n"
    "cmp r12, #0\n"
    "bne 1b\n"
    "bic sp, sp, #7\n"
    "sub sp, sp, #8\n"
    "str r7, [sp]\n"
    "mov r0, r4\n"
    "mov r1, r5\n"
    "mov r2, r6\n"
    "bl _init_libc\n"
    "mov r0, r4\n"
    "mov r1, r5\n"
    "bl carlink_screen_lifecycle\n"
    "bl exit\n"
    "udf #0\n"
    ".size _start, . - _start\n"
    ".pushsection .note.qnx.ident, \"a\", %note\n"
    ".balign 4\n"
    ".long 4, 8, 3\n"
    ".asciz \"QNX\"\n"
    ".long 0, 0x1000\n"
    ".popsection\n");

typedef struct screen_context *screen_context_t;
typedef struct screen_window *screen_window_t;
typedef struct screen_buffer *screen_buffer_t;

int screen_create_context(screen_context_t *, int);
int screen_create_window_type(screen_window_t *, screen_context_t, int);
int screen_set_window_property_iv(screen_window_t, int, const int *);
int screen_get_window_property_iv(screen_window_t, int, int *);
int screen_get_window_property_pv(screen_window_t, int, void **);
int screen_create_window_buffers(screen_window_t, int);
int screen_fill(screen_context_t, screen_buffer_t, const int *);
int screen_post_window(screen_window_t, screen_buffer_t, int, const int *, int);
int screen_destroy_window(screen_window_t);
int screen_destroy_context(screen_context_t);
int *__get_errno_ptr(void);
int printf(const char *, ...);
int strcmp(const char *, const char *);

enum {
    APPLICATION_CONTEXT = 0,
    APPLICATION_WINDOW = 0,
    PROPERTY_VISIBLE = 0x2b,
    PROPERTY_USAGE = 0x30,
    PROPERTY_FORMAT = 0x0e,
    PROPERTY_SIZE = 0x28,
    PROPERTY_RENDER_BUFFERS = 0x25,
    PROBE_USAGE = 8,
    PROBE_FORMAT = 8,
    SIDE = 64
};

static int record(const char *step, int rc)
{
    int error = rc == -1 ? *__get_errno_ptr() : 0;
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

static int post_hidden_frame(screen_context_t context, screen_window_t window)
{
    const int usage = PROBE_USAGE;
    const int format = PROBE_FORMAT;
    const int size[] = {SIDE, SIDE};
    const int rect[] = {0, 0, SIDE, SIDE};
    const int fill[] = {12, 0x01000000, 0};
    void *buffer = 0;

    if (record("set usage", screen_set_window_property_iv(
                                window, PROPERTY_USAGE, &usage)) != 0 ||
        record("set format", screen_set_window_property_iv(
                                 window, PROPERTY_FORMAT, &format)) != 0 ||
        record("set size", screen_set_window_property_iv(
                               window, PROPERTY_SIZE, size)) != 0)
        return 24;
    if (record("create buffer", screen_create_window_buffers(window, 1)) != 0 ||
        record("get buffer", screen_get_window_property_pv(
                                 window, PROPERTY_RENDER_BUFFERS, &buffer)) != 0)
        return 25;
    if (!buffer) {
        printf("missing render buffer\n");
        return 25;
    }
    if (record("fill buffer", screen_fill(context, buffer, fill)) != 0 ||
        verify_hidden(window, "read visibility before post") != 0)
        return 26;
    if (record("post frame", screen_post_window(window, buffer, 1, rect, 0)) != 0)
        return 27;
    return verify_hidden(window, "read visibility after post") == 0 ? 0 : 28;
}

int carlink_screen_lifecycle(int argc, char **argv)
{
    screen_context_t context = 0;
    screen_window_t window = 0;
    const int hidden = 0;
    int status = 20;

    if (argc != 2) {
        printf("Expected --lifecycle or --hidden-post\n");
        return 2;
    }
    const int hidden_post = strcmp(argv[1], "--hidden-post") == 0;
    if (!hidden_post && strcmp(argv[1], "--lifecycle") != 0) {
        printf("Expected --lifecycle or --hidden-post\n");
        return 2;
    }
    if (record("create context", screen_create_context(&context, APPLICATION_CONTEXT)) != 0)
        goto cleanup;
    status = 21;
    if (record("create window", screen_create_window_type(
                                    &window, context, APPLICATION_WINDOW)) != 0)
        goto cleanup;
    status = 22;
    if (record("set hidden", screen_set_window_property_iv(
                                 window, PROPERTY_VISIBLE, &hidden)) != 0)
        goto cleanup;
    status = 23;
    if (verify_hidden(window, "read visibility after create") != 0)
        goto cleanup;
    status = hidden_post ? post_hidden_frame(context, window) : 0;

cleanup:
    if (window && record("destroy window", screen_destroy_window(window)) != 0 && status == 0)
        status = 29;
    if (context && record("destroy context", screen_destroy_context(context)) != 0 && status == 0)
        status = 30;
    printf("exit=%d\n", status);
    return status;
}
