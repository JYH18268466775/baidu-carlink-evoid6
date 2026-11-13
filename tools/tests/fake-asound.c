/* Stand-in for the head unit's libasound shim: checks that snd_pcm_plugin_params is filled exactly like the real library
 * reads it (each field at the offset read off the car's libasound, everything else zero) and records what is played.
 * FAKE_MODE = open | params | prepare | write makes that call fail; FAKE_OUT is the file the played PCM is appended to. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned u32(const unsigned char *p, unsigned o) { return p[o] | p[o + 1] << 8 | p[o + 2] << 16 | (unsigned)p[o + 3] << 24; }
static int mode_is(const char *m) { const char *e = getenv("FAKE_MODE"); return e && !strcmp(e, m); }
int snd_pcm_open_preferred(void **h, int *card, int *dev, int mode)
{
    if (mode_is("open")) return -5;
    if (mode != 1) return -22;
    *h = (void *)0x1234; *card = 0; *dev = 0;
    return 0;
}
int snd_pcm_plugin_params(void *h, void *params)
{
    const unsigned char *p = params;
    static const unsigned used[] = {0x00, 0x04, 0x18, 0x1c, 0x20, 0x24, 0x1d8, 0x1dc, 0x1e8, 0x1ec, 0x1f0};
    if (h != (void *)0x1234 || mode_is("params")) return -22;
    if (u32(p, 0x00) != 0 || u32(p, 0x04) != 1 || (u32(p, 0x18) & 1) != 1 || u32(p, 0x1c) != 4 || u32(p, 0x20) != 44100 ||
        u32(p, 0x24) != 2 || u32(p, 0x1d8) != 2 || u32(p, 0x1dc) != 1 || u32(p, 0x1e8) != 4096 || u32(p, 0x1ec) != 1 ||
        u32(p, 0x1f0) != 5)
        return -22;
    for (unsigned i = 0; i < 1024; ++i) {   /* everything outside the known fields must be zero */
        int known = 0;
        for (unsigned k = 0; k < sizeof(used) / sizeof(used[0]); ++k)
            if (i >= used[k] && i < used[k] + 4) known = 1;
        if (!known && p[i]) return -22;
    }
    return 0;
}
int snd_pcm_plugin_prepare(void *h, int channel) { (void)h; return mode_is("prepare") && channel == 0 ? -5 : channel == 0 ? 0 : -22; }
int snd_pcm_plugin_write(void *h, const void *buf, unsigned n)
{
    if (h != (void *)0x1234 || n != 4096 || mode_is("write")) return -1;
    FILE *f = fopen(getenv("FAKE_OUT"), "ab");
    if (!f) return -1;
    fwrite(buf, 1, n, f); fclose(f);
    return (int)n;
}
int snd_pcm_plugin_flush(void *h, int channel) { (void)h; (void)channel; return 0; }
int snd_pcm_close(void *h)
{
    (void)h;
    FILE *f = fopen("/tmp/fake_asound_closed", "wb"); if (f) fclose(f);
    return 0;
}
