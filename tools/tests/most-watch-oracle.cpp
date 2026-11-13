// Oracle for most-watch.sh: replays the same fixture through the upstream CcsDecoder (not shipped, not copied) and prints
// events in the same canonical form as most-watch-host.
// g++ -std=gnu++98 -I$UPSTREAM/milestone-7/src most-watch-oracle.cpp
#include "car/ccs_decoder.hpp"
#include <stdio.h>
#include <vector>
using namespace nbt::car;
int main(int argc, char **argv) {
    FILE *f = argc > 1 ? fopen(argv[1], "r") : NULL;
    if (!f) return 1;
    CcsDecoder dec;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        unsigned inst, fkt, op; int off = 0;
        if (line[0] == '#' || sscanf(line, "%x %x %x%n", &inst, &fkt, &op, &off) != 3) continue;
        std::vector<unsigned char> d; unsigned b; int k; const char *p = line + off;
        while (sscanf(p, " %2x%n", &b, &k) == 1) { d.push_back((unsigned char)b); p += k; }
        CcsEvent ev[16];
        int n = dec.feed(0xE5, (unsigned char)inst, (unsigned short)fkt, (unsigned char)op, d.empty() ? NULL : &d[0], d.size(), ev, 16);
        for (int i = 0; i < n; ++i) {
            if (ev[i].kind == 2) printf("Knob: %s delta=%d\n", ev[i].what, ev[i].delta);
            else printf("Knob: %s %s\n", ev[i].what, ev[i].down ? "down" : "up");
        }
    }
    return 0;
}
