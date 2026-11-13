#ifndef CARLIFE_QNX_SHORT_REPLAY_H
#define CARLIFE_QNX_SHORT_REPLAY_H

enum { CARLINK_LIVE_FRAMES = 20 };

struct carlink_replay {
    const unsigned char *start, *end;
    unsigned width, height, fps, frames;
    int live;
};

int carlink_load_replay(struct carlink_replay *, const char *path);
int carlink_load_live_replay(struct carlink_replay *);
int carlink_play_replay(const struct carlink_replay *,
                        int (*after_frame)(unsigned frames, void *), void *);

#endif
