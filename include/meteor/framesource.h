#ifndef METEOR_FRAMESOURCE_H
#define METEOR_FRAMESOURCE_H

#include <imp/imp_common.h>

/* Create and configure a FrameSource channel (NV12, physical). */
int meteor_framesource_init(int chn, int width, int height, int fps);

/* Enable (start streaming) a FrameSource channel. */
int meteor_framesource_enable(int chn);

/* Disable (stop streaming) a FrameSource channel. */
int meteor_framesource_disable(int chn);

/* Destroy a FrameSource channel. */
int meteor_framesource_exit(int chn);

/* Set frame buffer depth (must be called after EnableChn). */
int meteor_framesource_set_depth(int chn, int depth);

/* Get a frame (caller must release with meteor_framesource_release_frame). */
int meteor_framesource_get_frame(int chn, IMPFrameInfo **frame);

/* Release a previously acquired frame. */
int meteor_framesource_release_frame(int chn, IMPFrameInfo *frame);

#endif /* METEOR_FRAMESOURCE_H */
