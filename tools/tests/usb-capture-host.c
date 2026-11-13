/* Windows host transport for the QNX capture regression tests. */
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static const char *scenario;
static int mock_errno;
static unsigned pending_length;
static int opened, closed, alarm_set;
static HANDLE pipe_writer, usb_closed;
static void *(*writer_entry)(void *);
static void *writer_arg;

static DWORD WINAPI run_writer(LPVOID unused)
{
    (void)unused;
    writer_entry(writer_arg);
    return 0;
}

int test_pthread_create(unsigned *id, const void *attributes,
                        void *(*start)(void *), void *arg)
{
    if (attributes)
        return 99;
    usb_closed = CreateEventW(NULL, TRUE, FALSE, NULL);
    writer_entry = start;
    writer_arg = arg;
    pipe_writer = usb_closed ? CreateThread(NULL, 0, run_writer, NULL, 0, NULL) : NULL;
    if (!pipe_writer) {
        if (usb_closed)
            CloseHandle(usb_closed);
        return 99;
    }
    *id = 1;
    return 0;
}

int test_pthread_join(unsigned id, void **result)
{
    if (id != 1 || result || WaitForSingleObject(pipe_writer, 2000) != WAIT_OBJECT_0)
        return 99;
    CloseHandle(pipe_writer);
    CloseHandle(usb_closed);
    return 0;
}

static unsigned be32(const unsigned char *p)
{
    return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3];
}

static DWORD WINAPI expire(LPVOID unused)
{
    (void)unused;
    Sleep(150);
    ExitProcess(124); /* Simulate the independent watchdog waking a blocked read. */
}

unsigned test_alarm(unsigned seconds)
{
    alarm_set = seconds == 45 ||
        (seconds == 60 && !strcmp(scenario, "live-visible-usb-transfers"));
    if (!strcmp(scenario, "silent")) {
        HANDLE timer = CreateThread(NULL, 0, expire, NULL, 0, NULL);
        if (!timer)
            ExitProcess(98);
        CloseHandle(timer);
    }
    return 0;
}

int test_open(const char *path, int flags, ...)
{
    if (strcmp(path, "/dev/oap02") || flags != 2 || !alarm_set)
        return -1;
    opened = 1;
    return 3;
}

int test_close(int fd)
{
    closed = fd == 3 && !pending_length;
    if (closed && usb_closed)
        SetEvent(usb_closed);
    return closed ? 0 : -1;
}

int test_devctl(int fd, int request, void *data, size_t size, int *info)
{
    (void)fd; (void)request; (void)data; (void)size; (void)info;
    return 99;
}

int test_usleep(unsigned usec)
{
    if (usec == 1000 && !strncmp(scenario, "live", 4))
        Sleep(1);
    return 0;
}

ptrdiff_t test_read(int fd, void *data, size_t size)
{
    static unsigned reads;
    if (fd != 3 || size != 65536 || pending_length)
        ExitProcess(97);
    if (!strcmp(scenario, "silent"))
        Sleep(5000);
    ++reads;
    if (!strcmp(scenario, "delayed") && reads <= 4) {
        Sleep(25);
        return 0;
    }
    if (!strcmp(scenario, "usb-transfers") || !strcmp(scenario, "live-usb-transfers") ||
        !strcmp(scenario, "live-visible-usb-transfers")) {
        unsigned char length[4];
        if (fread(length, 1, sizeof(length), stdin) == sizeof(length)) {
            const unsigned count = be32(length);
            if (count > size || fread(data, 1, count, stdin) != count)
                ExitProcess(92);
            return count;
        }
        mock_errno = 19;
        return -1;
    }
    const size_t count = fread(data, 1, 1 + reads % 139, stdin);
    if (count)
        return (ptrdiff_t)count;
    mock_errno = 19;
    return -1;
}

ptrdiff_t test_write(int fd, const void *data, size_t size)
{
    /* Refuse to consume pipe bytes until the capture thread has closed USB.
     * The old synchronous pipe writer could not finish in this scenario. */
    if (fd == 1 && usb_closed && WaitForSingleObject(usb_closed, 1500) != WAIT_OBJECT_0)
        ExitProcess(91);
    if (fd == 1 || fd == 2)
        return (ptrdiff_t)fwrite(data, 1, size, fd == 1 ? stdout : stderr);
    if (fd != 3)
        ExitProcess(96);
    const unsigned char *bytes = data;
    if (!pending_length) {
        if (size != 8 || be32(bytes) != 1 || be32(bytes + 4) < 8)
            ExitProcess(95);
        pending_length = be32(bytes + 4);
    } else {
        if (size != pending_length || size != 8u + bytes[1])
            ExitProcess(94);
        fprintf(stderr, "Mock phone saw CMD 0x%08x\n", be32(bytes + 4));
        pending_length = 0;
    }
    return (ptrdiff_t)size;
}

int *__get_errno_ptr(void)
{
    return &mock_errno;
}

int carlink_usb_capture(int argc, char **argv);

int main(int argc, char **argv)
{
    if (argc != 2)
        return 99;
    scenario = argv[1];
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    char *mode = "--capture";
    if (!strcmp(scenario, "live-visible-usb-transfers"))
        mode = "--stream-visible";
    else if (!strncmp(scenario, "live", 4))
        mode = "--stream-mdev";
    char *args[] = {"usb-capture-host", mode,
                    "/dev/oap02"};
    int status = carlink_usb_capture(3, args);
    return opened && closed ? status : 93;
}
