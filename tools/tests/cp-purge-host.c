/* Host harness for build/live-input-candidate/src/carlife_qnx_cp_purge.c (real source, mocked QNX calls). */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int carlink_cp_purge(int, char **);
static char calls[512];
static int fail_step; /* 1 open, 2 destroy */
static void note(const char *s) { strcat(calls, s); strcat(calls, ","); }
static int fake_errno;
int *mock_errno(void) { return &fake_errno; }
int mock_usleep(unsigned u) { (void)u; note("usleep"); return 0; }
char *mock_dlerror(void) { return "err"; }
int mock_dlclose(void *l) { (void)l; note("dlclose"); return 0; }
static void *conn_fn(const char *a) { (void)a; note("connect"); return (void *)1; }
static void disc_fn(void *c) { (void)c; note("disconnect"); }
static void *open_fn(void *c, const char *n) { (void)c; note("open:"); note(n); return fail_step == 1 ? 0 : (void *)2; }
static int stop_fn(void *c) { (void)c; note("stop"); return 0; }
static int destroy_fn(void *c) { (void)c; note("destroy"); return fail_step == 2 ? -1 : 0; }
void *mock_dlopen(const char *p, int f) { (void)f; note("dlopen"); return (void *)p; }
void *mock_dlsym(void *l, const char *n) {
    (void)l;
    if (!strcmp(n, "mmr_connect")) return (void *)conn_fn;
    if (!strcmp(n, "mmr_disconnect")) return (void *)disc_fn;
    if (!strcmp(n, "mmr_context_open")) return (void *)open_fn;
    if (!strcmp(n, "mmr_stop")) return (void *)stop_fn;
    if (!strcmp(n, "mmr_context_destroy")) return (void *)destroy_fn;
    return 0;
}
static int run(int step, const char *a1, const char *a2) {
    calls[0] = 0; fail_step = step;
    char *argv[] = {"p", (char *)a1, (char *)a2};
    return carlink_cp_purge(a1 ? (a2 ? 3 : 2) : 1, argv);
}
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s | calls=%s\n", __LINE__, #c, calls); return 1; } } while (0)
int main(void) {
    int rc = run(0, "--destroy", "cpvideoctx16838");
    CHECK(rc == 0);
    CHECK(strstr(calls, "connect,open:,cpvideoctx16838,stop,destroy,usleep,disconnect,"));
    const char *bad[] = {"aadvideoctx1", "cpvideoctx", "cpvideoctx12a", "cpvideoctx1234567890123", "cpvideoctx1/../x", "", "*", "cpvideoctx1 cpvideoctx2"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        rc = run(0, "--destroy", bad[i]);
        CHECK(rc == 2); CHECK(calls[0] == 0);
    }
    rc = run(0, "--other", "cpvideoctx1"); CHECK(rc == 2 && calls[0] == 0);
    rc = run(0, "--destroy", 0); CHECK(rc == 2 && calls[0] == 0);
    rc = run(1, "--destroy", "cpvideoctx1");
    CHECK(rc == 44); CHECK(!strstr(calls, "stop")); CHECK(!strstr(calls, "destroy"));
    rc = run(2, "--destroy", "cpvideoctx1"); CHECK(rc == 45);
    CHECK(strstr(calls, "destroy,usleep,disconnect"));
    printf("PASS: only cpvideoctx<digits> is destroyed, in open/stop/destroy order; failures reported\n");
    return 0;
}
