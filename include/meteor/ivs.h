#ifndef METEOR_IVS_H
#define METEOR_IVS_H

/*
 * Initialize IVS motion detection:
 *   - Create move interface with full-frame ROI
 *   - Create group, create channel, register channel
 *
 * sense: motion sensitivity (0-4, higher = more sensitive)
 */
int meteor_ivs_init(int grp, int chn, int width, int height, int sense);

/* Start receiving pictures on the IVS channel. */
int meteor_ivs_start(int chn);

/*
 * Poll for a motion detection result.
 * Returns 0 on success, logs motion events.
 */
int meteor_ivs_poll(int chn, int timeout_ms);

/* Stop receiving pictures. */
int meteor_ivs_stop(int chn);

/* Unregister, destroy channel and group. */
int meteor_ivs_exit(int grp, int chn);

#endif /* METEOR_IVS_H */
