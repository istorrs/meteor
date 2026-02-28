/*
 * ivs_monitor.h — background IVS motion polling thread.
 *
 * Runs a dedicated thread that continuously polls the IVS channel and
 * accumulates per-block motion statistics.  Any other thread can snapshot
 * the current stats and reset the counters between blocks.
 *
 * The caller is responsible for the full IVS SDK lifecycle:
 *   meteor_ivs_init() → meteor_system_bind() → meteor_framesource_enable()
 *   → meteor_ivs_start() → ivs_monitor_start() → ... →
 *   ivs_monitor_stop() → meteor_ivs_stop() → meteor_ivs_exit()
 *
 * Used by nightcam to annotate stacker JPEGs with motion metadata
 * (e.g. wind-blown foliage indicators).
 */
#ifndef METEOR_IVS_MONITOR_H
#define METEOR_IVS_MONITOR_H

/* Motion statistics accumulated since the last reset. */
typedef struct {
	int polls;          /* number of IVS polls completed */
	int active_polls;   /* polls where at least one ROI triggered */
	int total_rois;     /* cumulative ROI trigger count across all polls */
	int last_rois;      /* ROI count from the most recent poll */
} IVSMotionStats;

/*
 * Start the background poll thread on an already-running IVS channel.
 * Does NOT call meteor_ivs_init() or meteor_ivs_start() — the caller
 * must have already initialised and started the IVS channel.
 * Returns 0 on success, -1 on failure.
 */
int ivs_monitor_start(int chn);

/*
 * Stop the background poll thread.
 * Does NOT call meteor_ivs_stop() or meteor_ivs_exit() — the caller
 * is responsible for tearing down the IVS channel afterwards.
 */
void ivs_monitor_stop(void);

/*
 * Snapshot the current accumulated stats into *out.
 * Thread-safe; can be called from any thread.
 */
void ivs_monitor_get_stats(IVSMotionStats *out);

/*
 * Reset the accumulated counters to zero.
 * Call at the start of each detection block or stacker interval.
 */
void ivs_monitor_reset_stats(void);

#endif /* METEOR_IVS_MONITOR_H */
