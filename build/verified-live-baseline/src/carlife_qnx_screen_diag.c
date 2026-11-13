/* Standalone QNX Screen diagnostic for a bench head unit. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <screen/screen.h>

enum { SIDE = 64, PIXEL_BYTES = 4 };

static int record(const char *step, int rc)
{
    const int error = rc == -1 ? errno : 0;
    fprintf(stderr, "%s rc=%d errno=%d\n", step, rc, error);
    return rc;
}

static int verify_hidden(screen_window_t window, const char *step)
{
    int visible = -1;
    if (record(step, screen_get_window_property_iv(
                         window, SCREEN_PROPERTY_VISIBLE, &visible)) != 0)
        return -1;
    fprintf(stderr, "%s visible=%d\n", step, visible);
    return visible == 0 ? 0 : -1;
}

int main(int argc, char **argv)
{
    screen_context_t context = NULL;
    screen_window_t window = NULL;
    int status = 0;
    const int hidden = 0;
    const int usage = SCREEN_USAGE_WRITE;
    const int format = SCREEN_FORMAT_RGBA8888;
    const int size[] = { SIDE, SIDE };
    const int rect[] = { 0, 0, SIDE, SIDE };
    void *render_buffer = NULL;
    void *pixels = NULL;
    int stride = 0;
    char id[48];

    if (argc != 2 ||
        (strcmp(argv[1], "--lifecycle") != 0 &&
         strcmp(argv[1], "--hidden-post") != 0)) {
        fprintf(stderr, "Usage: %s --lifecycle | --hidden-post\n", argv[0]);
        return 2;
    }
    const int length = snprintf(id, sizeof(id), "carlink-screen-%ld", (long)getpid());
    if (length < 0 || (size_t)length >= sizeof(id))
        return 19;
    fprintf(stderr, "mode=%s window=%s\n", argv[1], id);

    status = 20;
    if (record("create context", screen_create_context(
                                    &context, SCREEN_APPLICATION_CONTEXT)) != 0)
        goto cleanup;
    status = 21;
    if (record("create window", screen_create_window(&window, context)) != 0)
        goto cleanup;
    status = 22;
    if (record("set hidden", screen_set_window_property_iv(
                                 window, SCREEN_PROPERTY_VISIBLE, &hidden)) != 0 ||
        verify_hidden(window, "read visibility after create") != 0)
        goto cleanup;
    if (strcmp(argv[1], "--lifecycle") == 0) {
        status = 0;
        goto cleanup;
    }

    status = 23;
    if (record("create group", screen_create_window_group(window, id)) != 0 ||
        record("set ID", screen_set_window_property_cv(
                             window, SCREEN_PROPERTY_ID_STRING, length, id)) != 0)
        goto cleanup;
    status = 24;
    if (record("set usage", screen_set_window_property_iv(
                                window, SCREEN_PROPERTY_USAGE, &usage)) != 0 ||
        record("set format", screen_set_window_property_iv(
                                 window, SCREEN_PROPERTY_FORMAT, &format)) != 0 ||
        record("set window size", screen_set_window_property_iv(
                                      window, SCREEN_PROPERTY_SIZE, size)) != 0 ||
        record("set buffer size", screen_set_window_property_iv(
                                      window, SCREEN_PROPERTY_BUFFER_SIZE, size)) != 0)
        goto cleanup;
    status = 25;
    if (record("create buffer", screen_create_window_buffers(window, 1)) != 0 ||
        record("get buffer", screen_get_window_property_pv(
                                 window, SCREEN_PROPERTY_RENDER_BUFFERS,
                                 &render_buffer)) != 0)
        goto cleanup;
    if (render_buffer == NULL) {
        fprintf(stderr, "missing render buffer\n");
        goto cleanup;
    }
    screen_buffer_t buffer = render_buffer;
    if (record("get pixels", screen_get_buffer_property_pv(
                                 buffer, SCREEN_PROPERTY_POINTER, &pixels)) != 0 ||
        record("get stride", screen_get_buffer_property_iv(
                                 buffer, SCREEN_PROPERTY_STRIDE, &stride)) != 0)
        goto cleanup;
    if (pixels == NULL || stride < SIDE * PIXEL_BYTES) {
        fprintf(stderr, "invalid pixel buffer or stride=%d\n", stride);
        goto cleanup;
    }
    for (int row = 0; row < SIDE; ++row)
        memset((unsigned char *)pixels + (size_t)row * stride, 0,
               SIDE * PIXEL_BYTES);
    status = 26;
    if (verify_hidden(window, "read visibility before post") != 0)
        goto cleanup;
    status = 27;
    if (record("post transparent frame", screen_post_window(
                                             window, buffer, 1, rect, 0)) != 0)
        goto cleanup;
    status = 28;
    if (verify_hidden(window, "read visibility after post") != 0)
        goto cleanup;
    status = 0;

cleanup:
    if (window != NULL &&
        record("destroy window", screen_destroy_window(window)) != 0 &&
        status == 0)
        status = 29;
    if (context != NULL &&
        record("destroy context", screen_destroy_context(context)) != 0 &&
        status == 0)
        status = 30;
    fprintf(stderr, "exit=%d\n", status);
    return status;
}
