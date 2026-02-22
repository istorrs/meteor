#ifndef METEOR_FRAMESOURCE_H
#define METEOR_FRAMESOURCE_H

/* Create and configure a FrameSource channel (NV12, physical). */
int meteor_framesource_init(int chn, int width, int height, int fps);

/* Enable (start streaming) a FrameSource channel. */
int meteor_framesource_enable(int chn);

/* Disable (stop streaming) a FrameSource channel. */
int meteor_framesource_disable(int chn);

/* Destroy a FrameSource channel. */
int meteor_framesource_exit(int chn);

#endif /* METEOR_FRAMESOURCE_H */
