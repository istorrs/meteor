#include <meteor/meteor_module.h>
#include <meteor/detector.h>
#include <meteor/framesource.h>
#include <meteor/log.h>
#include <meteor/meteor_config.h>
#include <imp/imp_common.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* -------------------------------------------------------------------------
 * Module state (singleton)
 * ------------------------------------------------------------------------- */

typedef struct {
	DetectorState *det;
	uint8_t       *detect_buf; /* downsampled Y plane: DETECT_WIDTH * DETECT_HEIGHT */
	int            fs_chn;
	volatile int   running;
	pthread_t      grab_thread;
} MeteorModule;

static MeteorModule s_mod;
static int          s_initialized;

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static uint64_t monotonic_ms(void)
{
	struct timespec ts;

	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/*
 * Nearest-neighbour downsample: src (src_w × src_h) → dst (dst_w × dst_h).
 * Only the Y plane (single byte per pixel) is processed.
 * Fast enough for MIPS32 at 25 fps.
 */
static void downsample_y(const uint8_t *src, int src_w, int src_h,
			 int src_stride,
			 uint8_t *dst, int dst_w, int dst_h)
{
	int x_step = src_w / dst_w;
	int y_step = src_h / dst_h;
	int dx, dy;

	for (dy = 0; dy < dst_h; dy++) {
		int             sy      = dy * y_step;
		const uint8_t  *src_row = src + sy * src_stride;
		uint8_t        *dst_row = dst + dy * dst_w;

		for (dx = 0; dx < dst_w; dx++)
			dst_row[dx] = src_row[dx * x_step];
	}
}

/* -------------------------------------------------------------------------
 * Frame-grabbing thread
 * ------------------------------------------------------------------------- */

static void *grab_thread_func(void *arg)
{
	MeteorModule *mod = (MeteorModule *)arg;
	IMPFrameInfo *frame;
	int           ret;

	METEOR_LOG_INFO("meteor_module: grab thread started");

	while (mod->running) {
		ret = meteor_framesource_get_frame(mod->fs_chn, &frame);
		if (ret != 0) {
			if (mod->running) {
				/* Brief pause to avoid spinning on transient errors */
				nanosleep(&(struct timespec){ .tv_nsec = 10000000 },
					  NULL);
			}
			continue;
		}

		downsample_y((const uint8_t *)frame->virAddr,
			     (int)frame->width, (int)frame->height,
			     (int)frame->width, /* assume stride = width */
			     mod->detect_buf,
			     DETECT_WIDTH, DETECT_HEIGHT);

		meteor_framesource_release_frame(mod->fs_chn, frame);

		detector_push_frame(mod->det,
				    mod->detect_buf, DETECT_WIDTH,
				    monotonic_ms());
	}

	METEOR_LOG_INFO("meteor_module: grab thread stopped");
	return NULL;
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

int meteor_module_init(int fs_chn, const meteor_config *cfg)
{
	PushConfig push;
	FFHeader   hdr;

	if (s_initialized) {
		METEOR_LOG_WARN("meteor_module: already initialised");
		return -1;
	}

	(void)memset(&s_mod, 0, sizeof(s_mod));

	s_mod.detect_buf = malloc((size_t)(DETECT_WIDTH * DETECT_HEIGHT));
	if (!s_mod.detect_buf) {
		METEOR_LOG_ERR("meteor_module: detect_buf alloc failed");
		return -1;
	}

	/* PushConfig */
	(void)memset(&push, 0, sizeof(push));
	(void)snprintf(push.server_ip, sizeof(push.server_ip),
		       "%s", cfg->server_ip);
	push.server_port = DETECTOR_SERVER_PORT;
	push.timeout_ms  = DETECTOR_HTTP_TIMEOUT_MS;

	/* FFHeader template */
	(void)memset(&hdr, 0, sizeof(hdr));
	(void)snprintf(hdr.station_id, sizeof(hdr.station_id),
		       "%s", cfg->station_id);
	hdr.width   = (uint16_t)DETECT_WIDTH;
	hdr.height  = (uint16_t)DETECT_HEIGHT;
	hdr.nframes = (uint16_t)FTP_BLOCK_FRAMES;
	hdr.fps     = FTP_FPS;
	hdr.camno   = 1u; /* default; set from station_id suffix if needed */

	s_mod.det = detector_create(&push, &hdr, DETECTOR_FF_TMP_DIR);
	if (!s_mod.det) {
		METEOR_LOG_ERR("meteor_module: detector_create failed");
		free(s_mod.detect_buf);
		return -1;
	}

	s_mod.fs_chn  = fs_chn;
	s_mod.running = 1;

	if (pthread_create(&s_mod.grab_thread, NULL,
			   grab_thread_func, &s_mod) != 0) {
		METEOR_LOG_ERR("meteor_module: grab thread create failed");
		detector_destroy(s_mod.det);
		free(s_mod.detect_buf);
		return -1;
	}

	s_initialized = 1;
	METEOR_LOG_INFO("meteor_module: initialised (server=%s station=%s)",
			cfg->server_ip, cfg->station_id);
	return 0;
}

void meteor_module_deinit(void)
{
	if (!s_initialized)
		return;

	s_mod.running = 0;
	(void)pthread_join(s_mod.grab_thread, NULL);

	detector_destroy(s_mod.det);
	free(s_mod.detect_buf);

	(void)memset(&s_mod, 0, sizeof(s_mod));
	s_initialized = 0;
	METEOR_LOG_INFO("meteor_module: stopped");
}
