#ifndef METEOR_CAPTURE_H
#define METEOR_CAPTURE_H

/* Enable frame capture on a channel (SetFrameDepth after EnableChn). */
int meteor_capture_enable(int chn);

/*
 * Capture the current Y plane (grayscale) to dir/frame_NNN.jpg.
 * Returns 0 on success, -1 on error.
 */
int meteor_capture_frame(int chn, const char *dir, int frame_num,
			 int width, int height);

#endif /* METEOR_CAPTURE_H */
