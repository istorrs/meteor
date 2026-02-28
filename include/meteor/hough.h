/*
 * hough.h — Hough Transform line detection for sparse candidate point sets.
 *
 * Uses a fixed-point sin/cos lookup table (scaled by 1024) to avoid
 * per-vote floating-point operations on MIPS32 targets without an FPU.
 * The table is initialised on the first call to hough_create().
 */
#ifndef METEOR_HOUGH_H
#define METEOR_HOUGH_H

#include <stdint.h>
#include <meteor/meteor_config.h>

/*
 * Hough accumulator.
 *
 * rho index = rho + HOUGH_RHO_MAX  (rho ∈ [−HOUGH_RHO_MAX, +HOUGH_RHO_MAX])
 * theta index = theta in degrees   (theta ∈ [0, HOUGH_THETA_STEPS))
 *
 * Heap-allocated — do not embed on the stack (size ~ 648 KB).
 */
typedef struct {
	uint16_t accum[2 * HOUGH_RHO_MAX][HOUGH_THETA_STEPS];
} HoughAccum;

/* Meteor line candidate extracted from the accumulator. */
typedef struct {
	int rho;        /* signed rho in pixels */
	int theta;      /* angle in degrees (0-179) */
	int votes;      /* accumulator value at (rho, theta) */
	int length_px;  /* approximate streak length in pixels (= votes) */
} MeteorLine;

/* Allocate a zeroed HoughAccum and initialise the trig lookup table. */
HoughAccum *hough_create(void);

/* Free a HoughAccum. No-op if h is NULL. */
void hough_destroy(HoughAccum *h);

/* Zero the accumulator for a new detection round. */
void hough_reset(HoughAccum *h);

/*
 * Cast one vote for candidate point (x, y) across all theta values.
 * x must be in [0, DETECT_WIDTH)  and y in [0, DETECT_HEIGHT).
 */
void hough_vote(HoughAccum *h, int x, int y);

/*
 * Extract line candidates that exceed the vote threshold.
 * Applies 3×3 local-maximum suppression to avoid counting the same
 * peak multiple times.
 *
 * Returns the number of lines written to out[] (≤ max_lines).
 */
int hough_find_peaks(const HoughAccum *h, int threshold,
		     MeteorLine *out, int max_lines);

#endif /* METEOR_HOUGH_H */
