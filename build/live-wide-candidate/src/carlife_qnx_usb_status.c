/* Query the native aoap-switch driver without switching the phone's USB mode. */
int open(const char *, int, ...);
int close(int);
int devctl(int, int, void *, __SIZE_TYPE__, int *);
int strncmp(const char *, const char *, __SIZE_TYPE__);
int printf(const char *, ...);
int *__get_errno_ptr(void);

enum {
    AOA_GET_MODE = 0x40040501,
    AOA_GET_PROTOCOL = 0x40040504
};

int carlink_usb_status(int argc, char **argv)
{
    static const char prefix[] = "/dev/android";
    if (argc != 2 || strncmp(argv[1], prefix, sizeof(prefix) - 1) != 0) {
        printf("Expected a /dev/android<digits> path\n");
        return 2;
    }
    const char *suffix = argv[1] + sizeof(prefix) - 1;
    if (!*suffix)
        return 2;
    for (; *suffix; ++suffix)
        if (*suffix < '0' || *suffix > '9')
            return 2;

    const int fd = open(argv[1], 0);
    if (fd < 0) {
        const int error = *__get_errno_ptr();
        printf("open failed errno=%d\n", error);
        return 10;
    }
    int mode = -1;
    int protocol = -1;
    const int mode_rc = devctl(fd, AOA_GET_MODE, &mode, sizeof(mode), 0);
    const int protocol_rc = devctl(fd, AOA_GET_PROTOCOL, &protocol, sizeof(protocol), 0);
    const int close_rc = close(fd);
    printf("AOA mode=%d mode_rc=%d protocol=%d protocol_rc=%d close_rc=%d\n",
           mode, mode_rc, protocol, protocol_rc, close_rc);
    return mode_rc || protocol_rc || close_rc ? 11 : 0;
}
