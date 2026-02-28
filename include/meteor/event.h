#ifndef METEOR_EVENT_H
#define METEOR_EVENT_H

#include <time.h>
#include <meteor/config.h>
#include <meteor/ivs.h>

typedef enum {
	METEOR_EVENT_IDLE,
	METEOR_EVENT_ACTIVE,
	METEOR_EVENT_COOLDOWN
} meteor_event_state;

typedef struct {
	meteor_event_state state;
	char event_dir[256];        /* flawfinder: ignore — bounded by snprintf */
	int frame_count;
	int total_triggers;         /* cumulative ROI trigger count */
	struct timespec start_time;
	struct timespec last_motion;
	struct timespec last_capture;
	const meteor_config *cfg;
} meteor_event_ctx;

/* Initialize the event context (starts in IDLE). */
void meteor_event_init(meteor_event_ctx *ctx, const meteor_config *cfg);

/*
 * Feed an IVS result into the state machine.
 * Returns the new state.
 */
meteor_event_state meteor_event_update(meteor_event_ctx *ctx,
				       const meteor_ivs_result *result);

/* Returns non-zero if enough time has elapsed since the last capture. */
int meteor_event_should_capture(const meteor_event_ctx *ctx);

/* Record that a frame was just captured. */
void meteor_event_frame_captured(meteor_event_ctx *ctx);

/*
 * Delete event directories under cfg->output_dir that are older than
 * cfg->retention_days days.  No-op if retention_days == 0.
 * Call once at startup before the main loop.
 */
void meteor_event_cleanup_old(const meteor_config *cfg);

#endif /* METEOR_EVENT_H */
