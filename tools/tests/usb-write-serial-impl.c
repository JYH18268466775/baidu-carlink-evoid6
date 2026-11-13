/* Host-test wrapper: compiles the real QNX receiver source with its libc I/O
 * renamed to the harness mocks (see usb-write-serial.sh), then exposes the
 * static helpers. No system headers here: the receiver declares its own
 * prototypes, exactly as it does for the QNX build. */
#include "carlife_qnx_usb_capture.c"

int hook_send_cmd(int fd, unsigned type, const unsigned char *body, unsigned size)
{
    return send_cmd(fd, type, body, size);
}

int hook_send_hardkey(int fd, unsigned code)
{
    return send_hardkey(fd, (unsigned char)code);
}

/* Input thread under test: the harness runs hook_input_entry on a real thread. */
static struct capture hook_capture;
static struct input_args hook_args;

void hook_input_reset(int usb_fd, int file_fd)
{
    hook_capture = (struct capture){0};
    hook_capture.live = 2;
    hook_args.capture = &hook_capture;
    hook_args.usb_fd = usb_fd;
    hook_args.file_fd = file_fd;
    __atomic_store_n(&usb_closed, 0, __ATOMIC_RELEASE);
}

void *hook_input_entry(void *unused)
{
    (void)unused;
    return input_thread(&hook_args);
}

void hook_set_ready(unsigned value)
{
    __atomic_store_n(&hook_capture.input_ready, value, __ATOMIC_RELEASE);
}

void hook_set_finished(void)
{
    __atomic_store_n(&hook_capture.finished, 1, __ATOMIC_RELEASE);
}

void hook_set_closed(void)
{
    lock_usb_writes();
    __atomic_store_n(&usb_closed, 1, __ATOMIC_RELEASE);
    unlock_usb_writes();
}

int hook_path_ok(const char *path)
{
    return input_path_ok(path);
}

/* Drives the real capture_video(): returns bit0 = input_ready after parameter
 * sets only, bit1 = input_ready after the first IDR, frames << 2. */
int hook_gate_probe(void)
{
    static const unsigned char params[] = {
        0, 0, 0, 1, 0x67, 0x42, 0x00, 0x1f, 0, 0, 0, 1, 0x68, 0xce, 0x3c, 0x80
    };
    static const unsigned char idr[] = {0, 0, 0, 1, 0x65, 0x88, 0x84, 0x21};
    struct capture capture = {0};
    capture.live = 2;
    capture.width = 1280;
    capture.height = 720;
    capture.fps = 30;
    int result = 0;
    if (capture_video(&capture, params, sizeof(params)) < 0)
        return -1;
    result |= (int)capture.input_ready;
    if (capture_video(&capture, idr, sizeof(idr)) < 0)
        return -1;
    result |= (int)capture.input_ready << 1;
    result |= (int)capture.frames << 2;
    return result;
}

int hook_main(int argc, char **argv)
{
    return carlink_usb_capture(argc, argv);
}
