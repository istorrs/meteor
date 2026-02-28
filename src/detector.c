/* gmtime_r requires _POSIX_C_SOURCE >= 200112L */
#define _POSIX_C_SOURCE 200809L
#include <meteor/detector.h>
#include <meteor/meteor_config.h>
#include <meteor/log.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* -------------------------------------------------------------------------
 * Candidate thresholding
 * ------------------------------------------------------------------------- */

static int collect_candidates(const uint8_t *maxpx, const uint8_t *avgpx,
			       const uint8_t *stdpx,
			       int *cx, int *cy, int max_cands)
{
	int count = 0;
	int i;

	for (i = 0; i < DETECT_WIDTH * DETECT_HEIGHT && count < max_cands; i++) {
		int diff = (int)maxpx[i] - (int)avgpx[i];

		if (diff > 0 && (uint8_t)diff > (uint8_t)(METEOR_FTP_K * stdpx[i])) {
			cx[count] = i % DETECT_WIDTH;
			cy[count] = i / DETECT_WIDTH;
			count++;
		}
	}
	return count;
}

/* -------------------------------------------------------------------------
 * Line endpoint estimation (approximate, from image-boundary intersections)
 * ------------------------------------------------------------------------- */

static void line_endpoints(int rho, int theta_deg,
			    int *x1, int *y1, int *x2, int *y2)
{
	/*
	 * Parametric line: x*cos(θ) + y*sin(θ) = rho.
	 * Intersect with image borders and pick two valid points.
	 */
	double theta = (double)theta_deg * M_PI / 180.0;
	double c     = cos(theta);
	double s     = sin(theta);
	int    pts_x[4], pts_y[4];
	int    n = 0;
	double v;
	int    W = DETECT_WIDTH, H = DETECT_HEIGHT;

	/* left edge x=0 */
	if (s > 1e-6 || s < -1e-6) {
		v = (double)rho / s;
		if (v >= 0.0 && v < (double)H) {
			pts_x[n] = 0;
			pts_y[n] = (int)v;
			n++;
		}
	}
	/* right edge x=W-1 */
	if ((s > 1e-6 || s < -1e-6) && n < 4) {
		v = ((double)rho - (double)(W - 1) * c) / s;
		if (v >= 0.0 && v < (double)H) {
			pts_x[n] = W - 1;
			pts_y[n] = (int)v;
			n++;
		}
	}
	/* top edge y=0 */
	if ((c > 1e-6 || c < -1e-6) && n < 4) {
		v = (double)rho / c;
		if (v >= 0.0 && v < (double)W) {
			pts_x[n] = (int)v;
			pts_y[n] = 0;
			n++;
		}
	}
	/* bottom edge y=H-1 */
	if ((c > 1e-6 || c < -1e-6) && n < 4) {
		v = ((double)rho - (double)(H - 1) * s) / c;
		if (v >= 0.0 && v < (double)W) {
			pts_x[n] = (int)v;
			pts_y[n] = H - 1;
			n++;
		}
	}

	if (n >= 2) {
		*x1 = pts_x[0]; *y1 = pts_y[0];
		*x2 = pts_x[1]; *y2 = pts_y[1];
	} else {
		*x1 = *y1 = *x2 = *y2 = 0;
	}
}

/* -------------------------------------------------------------------------
 * Processing thread — runs on the inactive (just-completed) FTP block
 * ------------------------------------------------------------------------- */

static void process_block(DetectorState *det, int bidx, uint64_t ts_ms)
{
	FTPBlock   *blk = det->blocks[bidx];
	MeteorLine  lines[DETECTOR_MAX_LINES];
	int         ncands, nlines, li;
	char        ff_path[256], ff_name[128]; /* flawfinder: ignore */
	FFHeader    hdr;
	struct tm   tm_val;
	time_t      t;

	/* Finalise the FTP block */
	ftp_block_finalize(blk,
			   det->maxpixel_buf, det->maxframe_buf,
			   det->avgpixel_buf, det->stdpixel_buf);

	/* Collect candidate pixels */
	ncands = collect_candidates(det->maxpixel_buf, det->avgpixel_buf,
				    det->stdpixel_buf,
				    det->cand_x, det->cand_y,
				    DETECTOR_MAX_CANDS);

	METEOR_LOG_DBG("detector: block %u — %d candidates",
		       (unsigned)blk->block_index, ncands);

	if (ncands < METEOR_MIN_CANDIDATES)
		goto reset;

	/*
	 * If the candidate buffer saturated, the frame is dominated by sensor
	 * noise or a scene-wide brightness event (cloud, dew, gain surge).
	 * No real meteor streak produces this many candidates — skip Hough.
	 */
	if (ncands >= DETECTOR_MAX_CANDS) {
		METEOR_LOG_DBG("detector: block saturated — skipping (raise METEOR_FTP_K)");
		goto reset;
	}

	/* Hough vote */
	hough_reset(det->hough);
	for (li = 0; li < ncands; li++)
		hough_vote(det->hough, det->cand_x[li], det->cand_y[li]);

	/* Extract peaks */
	nlines = hough_find_peaks(det->hough, HOUGH_PEAK_THRESHOLD,
				  lines, DETECTOR_MAX_LINES);

	METEOR_LOG_DBG("detector: %d Hough peaks", nlines);

	/* Validate each candidate line */
	for (li = 0; li < nlines; li++) {
		int x1, y1, x2, y2;
		int dx, dy, len_px;
		char json[512]; /* flawfinder: ignore */

		if (lines[li].votes < METEOR_MIN_VOTES)
			continue;

		line_endpoints(lines[li].rho, lines[li].theta,
			       &x1, &y1, &x2, &y2);

		dx     = x2 - x1;
		dy     = y2 - y1;
		len_px = (int)sqrt((double)(dx * dx + dy * dy));

		if (len_px < METEOR_MIN_LENGTH_PX)
			continue;

		METEOR_LOG_INFO("detector: meteor candidate "
				"rho=%d theta=%d votes=%d len=%dpx",
				lines[li].rho, lines[li].theta,
				lines[li].votes, len_px);

		/* Build FF header from template + current timestamp */
		hdr = det->ff_hdr_tpl;
		t   = (time_t)(ts_ms / 1000u);
		(void)gmtime_r(&t, &tm_val);
		hdr.year        = (uint16_t)(tm_val.tm_year + 1900);
		hdr.month       = (uint8_t)(tm_val.tm_mon + 1);
		hdr.day         = (uint8_t)tm_val.tm_mday;
		hdr.hour        = (uint8_t)tm_val.tm_hour;
		hdr.minute      = (uint8_t)tm_val.tm_min;
		hdr.second      = (uint8_t)tm_val.tm_sec;
		hdr.millisecond = (uint16_t)(ts_ms % 1000u);

		ff_make_filename(ff_name, sizeof(ff_name), &hdr);
		(void)snprintf(ff_path, sizeof(ff_path), "%s/%s",
			       det->ff_tmp_dir, ff_name);

		if (ff_write(ff_path, &hdr,
			     det->maxpixel_buf, det->maxframe_buf,
			     det->avgpixel_buf, det->stdpixel_buf) < 0) {
			METEOR_LOG_WARN("detector: ff_write failed: %s", ff_path);
			continue;
		}

		/* JSON event notification */
		(void)snprintf(json, sizeof(json),
			"{\"camera_id\":\"%s\","
			"\"type\":\"meteor\","
			"\"timestamp_ms\":%llu,"
			"\"block_start_ms\":%llu,"
			"\"candidate\":{"
			"\"rho\":%d,\"theta\":%d,"
			"\"x1\":%d,\"y1\":%d,"
			"\"x2\":%d,\"y2\":%d,"
			"\"length_px\":%d,"
			"\"votes\":%d}}",
			hdr.station_id,
			(unsigned long long)ts_ms,
			(unsigned long long)blk->timestamp_ms,
			lines[li].rho, lines[li].theta,
			x1, y1, x2, y2, len_px, lines[li].votes);

		if (event_push_json(&det->push_cfg, json) < 0)
			METEOR_LOG_WARN("detector: event_push_json failed");

		if (event_push_ff(&det->push_cfg, ff_path, ff_name) < 0)
			METEOR_LOG_WARN("detector: event_push_ff failed");

		(void)unlink(ff_path);
		break; /* one detection per block is sufficient */
	}

reset:
	ftp_block_reset(blk, 0);
}

static void *proc_thread_func(void *arg)
{
	DetectorState *det = (DetectorState *)arg;

	(void)pthread_mutex_lock(&det->mutex);
	while (det->running) {
		while (det->running && det->pending < 0)
			(void)pthread_cond_wait(&det->cond, &det->mutex);

		if (!det->running)
			break;

		{
			int      bidx = det->pending;
			uint64_t ts   = det->blocks[bidx]->timestamp_ms;

			det->pending = -1;
			(void)pthread_mutex_unlock(&det->mutex);

			process_block(det, bidx, ts);

			(void)pthread_mutex_lock(&det->mutex);
		}
	}
	(void)pthread_mutex_unlock(&det->mutex);
	return NULL;
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

DetectorState *detector_create(const PushConfig *push_cfg,
			       const FFHeader   *hdr_template,
			       const char       *ff_tmp_dir)
{
	DetectorState *det;
	size_t         plane = (size_t)(DETECT_WIDTH * DETECT_HEIGHT);
	int            i;

	det = calloc(1, sizeof(*det));
	if (!det)
		return NULL;

	for (i = 0; i < 2; i++) {
		det->blocks[i] = ftp_block_create(DETECT_WIDTH, DETECT_HEIGHT);
		if (!det->blocks[i])
			goto err;
	}

	det->hough = hough_create();
	if (!det->hough)
		goto err;

	det->maxpixel_buf = malloc(plane);
	det->maxframe_buf = malloc(plane);
	det->avgpixel_buf = malloc(plane);
	det->stdpixel_buf = malloc(plane);
	if (!det->maxpixel_buf || !det->maxframe_buf ||
	    !det->avgpixel_buf || !det->stdpixel_buf)
		goto err;

	det->push_cfg   = *push_cfg;
	det->ff_hdr_tpl = *hdr_template;
	(void)snprintf(det->ff_tmp_dir, sizeof(det->ff_tmp_dir),
		       "%s", ff_tmp_dir);

	det->active      = 0;
	det->frame_count = 0;
	det->pending     = -1;
	det->running     = 1;

	if (pthread_mutex_init(&det->mutex, NULL) != 0)
		goto err;
	if (pthread_cond_init(&det->cond, NULL) != 0) {
		(void)pthread_mutex_destroy(&det->mutex);
		goto err;
	}
	if (pthread_create(&det->proc_thread, NULL, proc_thread_func, det) != 0) {
		(void)pthread_cond_destroy(&det->cond);
		(void)pthread_mutex_destroy(&det->mutex);
		goto err;
	}

	/* Ensure FF staging directory exists */
	(void)mkdir(det->ff_tmp_dir, 0755);

	return det;

err:
	for (i = 0; i < 2; i++)
		ftp_block_destroy(det->blocks[i]);
	hough_destroy(det->hough);
	free(det->maxpixel_buf);
	free(det->maxframe_buf);
	free(det->avgpixel_buf);
	free(det->stdpixel_buf);
	free(det);
	return NULL;
}

void detector_destroy(DetectorState *det)
{
	int i;

	if (!det)
		return;

	(void)pthread_mutex_lock(&det->mutex);
	det->running = 0;
	(void)pthread_cond_signal(&det->cond);
	(void)pthread_mutex_unlock(&det->mutex);

	(void)pthread_join(det->proc_thread, NULL);
	(void)pthread_cond_destroy(&det->cond);
	(void)pthread_mutex_destroy(&det->mutex);

	for (i = 0; i < 2; i++)
		ftp_block_destroy(det->blocks[i]);
	hough_destroy(det->hough);
	free(det->maxpixel_buf);
	free(det->maxframe_buf);
	free(det->avgpixel_buf);
	free(det->stdpixel_buf);
	free(det);
}

void detector_push_frame(DetectorState *det, const uint8_t *y_plane,
			 int stride, uint64_t timestamp_ms)
{
	int     a = det->active;
	uint8_t fidx = (uint8_t)(det->frame_count & 0xFF);

	if (det->frame_count == 0)
		ftp_block_reset(det->blocks[a], timestamp_ms);

	ftp_block_update(det->blocks[a], y_plane, stride, fidx);
	det->frame_count++;

	if (det->frame_count >= FTP_BLOCK_FRAMES) {
		/* Store block timestamp before handing off */
		det->blocks[a]->timestamp_ms = timestamp_ms;

		(void)pthread_mutex_lock(&det->mutex);
		if (det->pending < 0) {
			det->pending = a;
			det->active  = 1 - a;
			det->frame_count = 0;
			(void)pthread_cond_signal(&det->cond);
		} else {
			/* Previous block still being processed — skip this block */
			METEOR_LOG_WARN("detector: processing thread busy, dropping block");
			det->frame_count = 0;
		}
		(void)pthread_mutex_unlock(&det->mutex);
	}
}
