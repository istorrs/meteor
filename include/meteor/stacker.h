/*
 * stacker.h — full-resolution NV12 frame accumulator with background
 * JPEG encode and HTTP push to the N100 receiver.
 *
 * Designed for use in nightcam where a single grab loop feeds both the
 * RMS FTP meteor detector and this stacker at 25 FPS.
 *
 * Every frames_per_stack frames the accumulated average is computed,
 * encoded as a JPEG by a background thread, and POSTed to /stack on
 * the N100 receiver.  IVS motion statistics from ivs_monitor are
 * snapshotted at stack completion and sent as a companion JSON /event.
 */
#ifndef METEOR_STACKER_H
#define METEOR_STACKER_H

#include <stdint.h>
#include <pthread.h>
#include <meteor/event_push.h>
#include <meteor/ivs_monitor.h>

/* Full sensor resolution fed to the stacker. */
#define STACKER_WIDTH   1920
#define STACKER_HEIGHT  1080

typedef struct {
	/* Configuration */
	PushConfig push_cfg;
	int        jpeg_quality;
	int        frames_per_stack;
	char       station_id[20]; /* flawfinder: ignore */

	/*
	 * Accumulators — only written by the grab-thread caller of
	 * stacker_on_frame(); never touched by the encode thread.
	 */
	uint32_t  *y_acc;   /* STACKER_WIDTH * STACKER_HEIGHT  uint32 */
	uint32_t  *uv_acc;  /* STACKER_WIDTH * (STACKER_HEIGHT/2) uint32 */
	int        frame_count;
	int        stack_index;

	/*
	 * Averaged output buffers.
	 * Owned by grab thread when enc_pending == 0.
	 * Owned by encode thread when enc_pending == 1.
	 * The transition is protected by mutex + cond.
	 */
	uint8_t   *y_avg;   /* STACKER_WIDTH * STACKER_HEIGHT  uint8 */
	uint8_t   *uv_avg;  /* STACKER_WIDTH * (STACKER_HEIGHT/2) uint8 */

	/*
	 * Optional dark frame — loaded once at create time, read-only
	 * thereafter.  NULL if no dark file was found or provided.
	 */
	uint8_t   *y_dark;  /* STACKER_WIDTH * STACKER_HEIGHT  uint8 */
	uint8_t   *uv_dark; /* STACKER_WIDTH * (STACKER_HEIGHT/2) uint8 */

	/* Encode thread state */
	pthread_t        enc_thread;
	pthread_mutex_t  mutex;
	pthread_cond_t   cond;
	int              enc_pending;  /* 1 = y_avg/uv_avg ready to encode */
	uint64_t         enc_ts_ms;    /* wall-clock ms of the completed stack */
	IVSMotionStats   enc_ivs;      /* IVS snapshot taken at stack completion */
	int              running;
} StackerState;

/*
 * Allocate and start the stacker.
 *   push_cfg        : N100 receiver connection parameters
 *   station_id      : identifier embedded in output JPEG filenames
 *   frames_per_stack: number of 25 fps frames averaged per output JPEG
 *   jpeg_quality    : libjpeg quality 1–100
 *   dark_path       : path to a dark frame raw file written by astrostack -D,
 *                     or NULL to skip dark subtraction.  The file must contain
 *                     a Y plane (STACKER_WIDTH*STACKER_HEIGHT bytes) followed
 *                     by a UV plane (STACKER_WIDTH*(STACKER_HEIGHT/2) bytes).
 *                     If the file does not exist the stacker starts without it.
 * Returns NULL on failure.
 */
StackerState *stacker_create(const PushConfig *push_cfg,
			     const char *station_id,
			     int frames_per_stack, int jpeg_quality,
			     const char *dark_path);

/* Stop the encode thread and free all resources. */
void stacker_destroy(StackerState *st);

/*
 * Feed one full-resolution NV12 frame.
 *   nv12_data    : raw NV12 bytes (Y plane then interleaved UV half-plane)
 *   timestamp_ms : wall-clock milliseconds for JPEG filename generation
 *
 * When frames_per_stack frames have accumulated the average is computed,
 * the IVS motion counters are snapshotted and reset, and the encode
 * thread is signalled.  Returns immediately without blocking.
 */
void stacker_on_frame(StackerState *st, const uint8_t *nv12_data,
		      uint64_t timestamp_ms);

#endif /* METEOR_STACKER_H */
