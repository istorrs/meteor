/* gmtime_r requires _POSIX_C_SOURCE >= 200112L */
#define _POSIX_C_SOURCE 200809L
#include <meteor/stacker.h>
#include <meteor/log.h>
#include <jpeglib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * NV12 → JPEG writer
 * ------------------------------------------------------------------------- */

static inline uint8_t clamp8(int v)
{
	if (v < 0)   return 0;
	if (v > 255) return 255;
	return (uint8_t)v;
}

/*
 * Encode averaged NV12 planes (y, uv) as a JPEG file.
 * Converts NV12 → RGB one scanline at a time to minimise stack usage.
 * Returns 0 on success, -1 on error.
 */
static int write_jpeg_nv12(const char *path, const uint8_t *y,
			   const uint8_t *uv, int quality)
{
	FILE *fp;
	struct jpeg_compress_struct cinfo;
	struct jpeg_error_mgr       jerr;
	uint8_t  row[STACKER_WIDTH * 3]; /* flawfinder: ignore */
	JSAMPROW row_ptr[1];
	int x, r;

	fp = fopen(path, "wb"); /* flawfinder: ignore */
	if (!fp)
		return -1;

	cinfo.err = jpeg_std_error(&jerr);
	jpeg_create_compress(&cinfo);
	jpeg_stdio_dest(&cinfo, fp);

	cinfo.image_width      = (JDIMENSION)STACKER_WIDTH;
	cinfo.image_height     = (JDIMENSION)STACKER_HEIGHT;
	cinfo.input_components = 3;
	cinfo.in_color_space   = JCS_RGB;

	jpeg_set_defaults(&cinfo);
	jpeg_set_quality(&cinfo, quality, TRUE);
	jpeg_start_compress(&cinfo, TRUE);

	row_ptr[0] = row;

	for (r = 0; r < STACKER_HEIGHT; r++) {
		for (x = 0; x < STACKER_WIDTH; x++) {
			int yi  = r * STACKER_WIDTH + x;
			int uvi = (r / 2) * STACKER_WIDTH + (x & ~1);
			int yv  = (int)y[yi];
			int u   = (int)uv[uvi]     - 128;
			int v   = (int)uv[uvi + 1] - 128;

			row[x * 3]     = clamp8(yv + ((v * 1436) >> 10));
			row[x * 3 + 1] = clamp8(yv - ((u * 352 + v * 731) >> 10));
			row[x * 3 + 2] = clamp8(yv + ((u * 1815) >> 10));
		}
		(void)jpeg_write_scanlines(&cinfo, row_ptr, 1);
	}

	jpeg_finish_compress(&cinfo);
	jpeg_destroy_compress(&cinfo);
	(void)fclose(fp);
	return 0;
}

/* -------------------------------------------------------------------------
 * Encode thread
 * ------------------------------------------------------------------------- */

static void *enc_thread_func(void *arg)
{
	StackerState *st = (StackerState *)arg;

	(void)pthread_mutex_lock(&st->mutex);

	while (st->running) {
		while (st->running && !st->enc_pending)
			(void)pthread_cond_wait(&st->cond, &st->mutex);

		if (!st->running)
			break;

		{
			uint64_t       ts  = st->enc_ts_ms;
			int            idx = st->stack_index;
			IVSMotionStats ivs = st->enc_ivs;

			st->enc_pending = 0;
			(void)pthread_mutex_unlock(&st->mutex);

			{
				char      filename[80];  /* flawfinder: ignore */
				char      tmp_path[64];  /* flawfinder: ignore */
				char      json[512];     /* flawfinder: ignore */
				struct tm tm_val;
				time_t    t = (time_t)(ts / 1000u);

				(void)gmtime_r(&t, &tm_val);

				(void)snprintf(filename, sizeof(filename),
					"STACK_%s_%04d%02d%02d_%02d%02d%02d_%03d.jpg",
					st->station_id,
					tm_val.tm_year + 1900,
					tm_val.tm_mon  + 1,
					tm_val.tm_mday,
					tm_val.tm_hour,
					tm_val.tm_min,
					tm_val.tm_sec,
					(int)(ts % 1000u));

				(void)snprintf(tmp_path, sizeof(tmp_path),
					"/tmp/nightcam-%d.jpg", idx);

				if (write_jpeg_nv12(tmp_path, st->y_avg,
						    st->uv_avg,
						    st->jpeg_quality) < 0) {
					METEOR_LOG_WARN("stacker: JPEG encode "
							"failed for %s",
							filename);
				} else {
					if (event_push_file(&st->push_cfg,
							    "/stack",
							    "image/jpeg",
							    tmp_path,
							    filename) < 0)
						METEOR_LOG_WARN("stacker: "
								"push /stack "
								"failed");
					else
						METEOR_LOG_INFO("stacker: "
								"pushed %s",
								filename);

					(void)unlink(tmp_path);
				}

				/* Companion JSON event with IVS motion stats */
				(void)snprintf(json, sizeof(json),
					"{\"camera_id\":\"%s\","
					"\"type\":\"stack\","
					"\"timestamp_ms\":%llu,"
					"\"filename\":\"%s\","
					"\"ivs_polls\":%d,"
					"\"ivs_active_polls\":%d,"
					"\"ivs_total_rois\":%d,"
					"\"ivs_last_rois\":%d}",
					st->station_id,
					(unsigned long long)ts,
					filename,
					ivs.polls,
					ivs.active_polls,
					ivs.total_rois,
					ivs.last_rois);

				if (event_push_json(&st->push_cfg, json) < 0)
					METEOR_LOG_WARN("stacker: "
							"push /event failed");
			}

			(void)pthread_mutex_lock(&st->mutex);
		}
	}

	(void)pthread_mutex_unlock(&st->mutex);
	return NULL;
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

/*
 * Load a dark frame written by astrostack -D.
 * Format: Y plane (W*H bytes) then UV plane (W*(H/2) bytes).
 * Populates *y_out and *uv_out on success (caller must free).
 * Returns 0 on success, -1 if file absent or wrong size (non-fatal).
 */
static int load_dark(const char *path,
		     uint8_t **y_out, uint8_t **uv_out)
{
	size_t   y_sz   = (size_t)(STACKER_WIDTH * STACKER_HEIGHT);
	size_t   uv_sz  = (size_t)(STACKER_WIDTH * (STACKER_HEIGHT / 2));
	size_t   expect = y_sz + uv_sz;
	struct stat st_buf;
	FILE    *fp;

	*y_out  = NULL;
	*uv_out = NULL;

	if (!path || stat(path, &st_buf) != 0)
		return -1;

	if ((size_t)st_buf.st_size != expect) {
		METEOR_LOG_WARN("stacker: dark frame %s is %ld bytes "
				"(expected %zu) — skipping",
				path, (long)st_buf.st_size, expect);
		return -1;
	}

	fp = fopen(path, "rb"); /* flawfinder: ignore */
	if (!fp)
		return -1;

	*y_out = malloc(y_sz);
	if (!*y_out) {
		(void)fclose(fp);
		return -1;
	}

	if (fread(*y_out, 1, y_sz, fp) != y_sz) {
		free(*y_out);
		*y_out = NULL;
		(void)fclose(fp);
		return -1;
	}

	*uv_out = malloc(uv_sz);
	if (!*uv_out) {
		free(*y_out);
		*y_out = NULL;
		(void)fclose(fp);
		return -1;
	}

	if (fread(*uv_out, 1, uv_sz, fp) != uv_sz) {
		free(*y_out);
		free(*uv_out);
		*y_out  = NULL;
		*uv_out = NULL;
		(void)fclose(fp);
		return -1;
	}

	(void)fclose(fp);
	return 0;
}

StackerState *stacker_create(const PushConfig *push_cfg,
			     const char *station_id,
			     int frames_per_stack, int jpeg_quality,
			     const char *dark_path)
{
	StackerState *st;
	size_t        y_sz  = (size_t)(STACKER_WIDTH * STACKER_HEIGHT);
	size_t        uv_sz = (size_t)(STACKER_WIDTH * (STACKER_HEIGHT / 2));

	st = calloc(1, sizeof(*st));
	if (!st)
		return NULL;

	st->push_cfg         = *push_cfg;
	(void)snprintf(st->station_id, sizeof(st->station_id),
		       "%s", station_id);
	st->frames_per_stack = frames_per_stack;
	st->jpeg_quality     = jpeg_quality;

	st->y_acc  = calloc(y_sz,  sizeof(uint32_t));
	st->uv_acc = calloc(uv_sz, sizeof(uint32_t));
	st->y_avg  = malloc(y_sz);
	st->uv_avg = malloc(uv_sz);

	if (!st->y_acc || !st->uv_acc || !st->y_avg || !st->uv_avg)
		goto err;

	/* Dark frame — optional, non-fatal if absent */
	if (load_dark(dark_path, &st->y_dark, &st->uv_dark) == 0)
		METEOR_LOG_INFO("stacker: dark frame loaded from %s", dark_path);
	else
		METEOR_LOG_INFO("stacker: no dark frame (run astrostack -D "
				"to create one)");

	st->running = 1;

	if (pthread_mutex_init(&st->mutex, NULL) != 0)
		goto err;

	if (pthread_cond_init(&st->cond, NULL) != 0) {
		(void)pthread_mutex_destroy(&st->mutex);
		goto err;
	}

	if (pthread_create(&st->enc_thread, NULL, enc_thread_func, st) != 0) {
		(void)pthread_cond_destroy(&st->cond);
		(void)pthread_mutex_destroy(&st->mutex);
		goto err;
	}

	METEOR_LOG_INFO("stacker: created (%d frames/stack, q%d)",
			frames_per_stack, jpeg_quality);
	return st;

err:
	free(st->y_acc);
	free(st->uv_acc);
	free(st->y_avg);
	free(st->uv_avg);
	free(st->y_dark);
	free(st->uv_dark);
	free(st);
	return NULL;
}

void stacker_destroy(StackerState *st)
{
	if (!st)
		return;

	(void)pthread_mutex_lock(&st->mutex);
	st->running = 0;
	(void)pthread_cond_signal(&st->cond);
	(void)pthread_mutex_unlock(&st->mutex);

	(void)pthread_join(st->enc_thread, NULL);
	(void)pthread_cond_destroy(&st->cond);
	(void)pthread_mutex_destroy(&st->mutex);

	free(st->y_acc);
	free(st->uv_acc);
	free(st->y_avg);
	free(st->uv_avg);
	free(st->y_dark);
	free(st->uv_dark);
	free(st);
}

void stacker_on_frame(StackerState *st, const uint8_t *nv12_data,
		      uint64_t timestamp_ms)
{
	int            y_sz  = STACKER_WIDTH * STACKER_HEIGHT;
	int            uv_sz = STACKER_WIDTH * (STACKER_HEIGHT / 2);
	const uint8_t *uv    = nv12_data + y_sz;
	uint32_t       n;
	int            i;

	/* Accumulate Y and UV planes */
	for (i = 0; i < y_sz; i++)
		st->y_acc[i] += nv12_data[i];
	for (i = 0; i < uv_sz; i++)
		st->uv_acc[i] += uv[i];

	st->frame_count++;
	if (st->frame_count < st->frames_per_stack)
		return;

	/* Stack complete — compute average */
	n = (uint32_t)st->frame_count;
	for (i = 0; i < y_sz; i++)
		st->y_avg[i] = (uint8_t)(st->y_acc[i] / n);
	for (i = 0; i < uv_sz; i++)
		st->uv_avg[i] = (uint8_t)(st->uv_acc[i] / n);

	/* Reset accumulators for next stack */
	(void)memset(st->y_acc,  0, (size_t)y_sz  * sizeof(uint32_t));
	(void)memset(st->uv_acc, 0, (size_t)uv_sz * sizeof(uint32_t));
	st->frame_count = 0;

	/* Subtract dark frame if loaded */
	if (st->y_dark) {
		for (i = 0; i < y_sz; i++) {
			int v = (int)st->y_avg[i] - (int)st->y_dark[i];
			st->y_avg[i] = (v > 0) ? (uint8_t)v : 0;
		}
		if (st->uv_dark) {
			for (i = 0; i < uv_sz; i++) {
				int v = (int)st->uv_avg[i]
				      - (int)st->uv_dark[i] + 128;
				st->uv_avg[i] = clamp8(v);
			}
		}
	}

	/* Snapshot IVS motion stats and reset counters */
	{
		IVSMotionStats ivs;

		ivs_monitor_get_stats(&ivs);
		ivs_monitor_reset_stats();

		/* Hand the averaged buffers to the encode thread */
		(void)pthread_mutex_lock(&st->mutex);

		if (!st->enc_pending) {
			st->enc_ts_ms   = timestamp_ms;
			st->enc_ivs     = ivs;
			st->enc_pending = 1;
			st->stack_index++;
			(void)pthread_cond_signal(&st->cond);
		} else {
			METEOR_LOG_WARN("stacker: encode busy, "
					"dropping stack %d",
					st->stack_index);
		}

		(void)pthread_mutex_unlock(&st->mutex);
	}
}
