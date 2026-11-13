/* Destroys one factory CarPlay video context (cpvideoctx<digits>) the way the
 * upstream NBT-AAD video manager does: mmr_context_open, mmr_stop,
 * mmr_context_destroy. Nothing else is touched. The factory CarPlay cannot
 * recreate its context until the head unit restarts. */
void *dlopen(const char *, int);
void *dlsym(void *, const char *);
char *dlerror(void);
int dlclose(void *);
int printf(const char *, ...);
int fflush(void *);
int strncmp(const char *, const char *, __SIZE_TYPE__);
int strcmp(const char *, const char *);
int usleep(unsigned);
int *__get_errno_ptr(void);

enum { QNX_LOAD_NOW = 2, QNX_LOAD_NOW_GLOBAL = 0x102 };

typedef struct mmr_connection mmr_connection_t;
typedef struct mmr_context mmr_context_t;
typedef mmr_connection_t *(*connect_fn)(const char *);
typedef void (*disconnect_fn)(mmr_connection_t *);
typedef mmr_context_t *(*context_open_fn)(mmr_connection_t *, const char *);
typedef int (*context_step_fn)(mmr_context_t *);

static int purge_name_ok(const char *name)
{
    static const char prefix[] = "cpvideoctx";
    if (strncmp(name, prefix, sizeof(prefix) - 1) != 0)
        return 0;
    const char *digits = name + sizeof(prefix) - 1;
    if (!*digits)
        return 0;
    for (unsigned count = 0; *digits; ++digits, ++count)
        if (*digits < '0' || *digits > '9' || count >= 12)
            return 0;
    return 1;
}

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
    }
    return 0;
}

int carlink_cp_purge(int argc, char **argv)
{
    if (argc != 3 || strcmp(argv[1], "--destroy") != 0 || !purge_name_ok(argv[2])) {
        printf("Usage: --destroy cpvideoctx<digits>\n");
        return 2;
    }
    const char *name = argv[2];
    int status = 40;
    void *strm = load_strm();
    if (!strm) {
        printf("load strm failed\n");
        return status;
    }
    status = 41;
    void *client = dlopen("/fs/sda0/opt/conn/lib/carplay/libmmrndclient.so.1", QNX_LOAD_NOW);
    if (!client) {
        const char *error = dlerror();
        printf("load mmr client failed: %s\n", error ? error : "?");
        goto cleanup;
    }
    status = 42;
    connect_fn connect = (connect_fn)dlsym(client, "mmr_connect");
    disconnect_fn disconnect = (disconnect_fn)dlsym(client, "mmr_disconnect");
    context_open_fn open_context = (context_open_fn)dlsym(client, "mmr_context_open");
    context_step_fn stop = (context_step_fn)dlsym(client, "mmr_stop");
    context_step_fn destroy = (context_step_fn)dlsym(client, "mmr_context_destroy");
    printf("resolve connect=%d disconnect=%d open=%d stop=%d destroy=%d\n",
           !!connect, !!disconnect, !!open_context, !!stop, !!destroy);
    if (!connect || !disconnect || !open_context || !stop || !destroy)
        goto cleanup;

    status = 43;
    mmr_connection_t *connection = connect(0);
    if (!connection) {
        printf("mmr_connect failed errno=%d\n", *__get_errno_ptr());
        goto cleanup;
    }
    printf("mmr_connect ok\n");
    status = 44;
    mmr_context_t *context = open_context(connection, name);
    if (!context) {
        printf("mmr_context_open %s failed errno=%d\n", name, *__get_errno_ptr());
    } else {
        printf("mmr_context_open %s ok\n", name);
        const int stop_rc = stop(context);
        printf("mmr_stop rc=%d errno=%d\n", stop_rc, stop_rc ? *__get_errno_ptr() : 0);
        const int destroy_rc = destroy(context); /* also closes the handle */
        printf("mmr_context_destroy rc=%d errno=%d\n", destroy_rc, destroy_rc ? *__get_errno_ptr() : 0);
        status = destroy_rc ? 45 : 0;
    }
    usleep(50000); /* let the renderer finish unregistering */
    disconnect(connection);
    printf("mmr_disconnect returned\n");

cleanup:
    if (client)
        dlclose(client);
    dlclose(strm);
    printf("exit=%d\n", status);
    return status;
}
