#ifndef METEOR_IVS_H
#define METEOR_IVS_H

#include <imp/imp_ivs_move.h>

#define METEOR_IVS_MAX_ROI IMP_IVS_MOVE_MAX_ROI_CNT

/* Result from a single poll cycle. */
typedef struct {
	int triggered;                    /* count of ROIs with motion */
	int roi[METEOR_IVS_MAX_ROI];     /* 0/1 per ROI */
	int roi_count;                    /* total configured ROIs */
} meteor_ivs_result;

/*
 * Initialize IVS motion detection with a grid of ROIs.
 *
 * grid_cols x grid_rows cells are laid out across the frame.
 * sense: motion sensitivity (0-4, higher = more sensitive).
 */
int meteor_ivs_init(int grp, int chn, int width, int height,
		    int sense, int grid_cols, int grid_rows);

/* Start receiving pictures on the IVS channel. */
int meteor_ivs_start(int chn);

/*
 * Poll for a motion detection result.
 * Returns 0 on success, populates result.
 * Returns non-zero on timeout or error.
 */
int meteor_ivs_poll(int chn, int timeout_ms, meteor_ivs_result *result);

/* Stop receiving pictures. */
int meteor_ivs_stop(int chn);

/* Unregister, destroy channel and group. */
int meteor_ivs_exit(int grp, int chn);

#endif /* METEOR_IVS_H */
