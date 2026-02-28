/*
 * detector.h — FTP → threshold → Hough → validate → push pipeline.
 *
 * Double-buffers two FTP blocks so that frame accumulation continues in the
 * caller's thread while the previous block is processed asynchronously in a
 * dedicated POSIX thread.
 */
#ifndef METEOR_DETECTOR_H
#define METEOR_DETECTOR_H

#include <stdint.h>
#include <pthread.h>
#include <meteor/ftp.h>
#include <meteor/hough.h>
#include <meteor/ff_writer.h>
#include <meteor/event_push.h>

#define DETECTOR_MAX_LINES    16   /* max Hough peaks examined per block */
#define DETECTOR_MAX_CANDS  4096   /* max threshold candidate pixels */

/* Internal state — callers should treat this as opaque. */
typedef struct {
	FTPBlock        *blocks[2];    /* double buffer */
	int              active;       /* index of the block being filled (0/1) */

	HoughAccum      *hough;

	uint8_t         *maxpixel_buf;
	uint8_t         *maxframe_buf;
	uint8_t         *avgpixel_buf;
	uint8_t         *stdpixel_buf;

	int              cand_x[DETECTOR_MAX_CANDS];
	int              cand_y[DETECTOR_MAX_CANDS];

	PushConfig       push_cfg;
	FFHeader         ff_hdr_tpl;   /* template; timestamp filled per block */
	char             ff_tmp_dir[128]; /* flawfinder: ignore */

	int              frame_count;  /* frames in the current block (0-255) */

	pthread_t        proc_thread;
	pthread_mutex_t  mutex;
	pthread_cond_t   cond;
	int              pending;      /* index of block ready for processing, or -1 */
	int              running;
} DetectorState;

/*
 * Allocate and initialise a DetectorState.
 *   push_cfg     : network parameters for the N100 receiver
 *   hdr_template : station metadata template (timestamp fields will be overwritten)
 *   ff_tmp_dir   : temporary directory for staging FF files before upload
 * Returns NULL on allocation or thread creation failure.
 */
DetectorState *detector_create(const PushConfig *push_cfg,
			       const FFHeader   *hdr_template,
			       const char       *ff_tmp_dir);

/* Stop the processing thread and free all resources. */
void detector_destroy(DetectorState *det);

/*
 * Feed one downsampled Y-plane frame (DETECT_WIDTH × DETECT_HEIGHT) to the
 * detector.  Call this from the frame-grabbing thread once per camera frame.
 *   y_plane      : pointer to luma data at detection resolution
 *   stride       : row stride in bytes (normally DETECT_WIDTH)
 *   timestamp_ms : current wall-clock time in milliseconds
 */
void detector_push_frame(DetectorState *det, const uint8_t *y_plane,
			 int stride, uint64_t timestamp_ms);

#endif /* METEOR_DETECTOR_H */
