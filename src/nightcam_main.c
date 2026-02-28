/*
 * nightcam_main.c — combined RMS FTP meteor detector + full-resolution
 * timelapse stacker with IVS motion annotation.
 *
 * A single IMP pipeline is shared between three subsystems:
 *   • RMS FTP meteor detector — downsampled 640×480 Y plane, 256-frame blocks
 *   • Timelapse stacker       — full 1920×1080 NV12, configurable interval
 *   • IVS motion monitor      — background poll thread, metadata only
 *
 * The main grab loop feeds both the detector and stacker from every frame;
 * IVS is bound to the FrameSource and runs its own SDK-side pipeline.
 *
 * Usage: nightcam [-S server_ip] [-I station_id] [-t stack_secs]
 *                 [-q quality] [-s sensitivity] [-h]
 *
 *   -S IP     N100 receiver IP     (default: DETECTOR_DEFAULT_SERVER_IP)
 *   -I ID     RMS station ID       (default: DETECTOR_DEFAULT_STATION_ID)
 *   -t SECS   Stack interval secs  (default: 30)
 *   -q N      JPEG quality 1-100   (default: 85)
 *   -s N      IVS sensitivity 0-4  (default: 2)
 */
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <imp/imp_system.h>

#include <meteor/log.h>
#include <meteor/system.h>
#include <meteor/isp.h>
#include <meteor/isp_tuning.h>
#include <meteor/framesource.h>
#include <meteor/ivs.h>
#include <meteor/ivs_monitor.h>
#include <meteor/detector.h>
#include <meteor/stacker.h>
#include <meteor/meteor_config.h>
#include <meteor/ff_writer.h>
#include <meteor/event_push.h>

#define FS_CHN   0
#define IVS_GRP  0
#define IVS_CHN  0
#define WIDTH    1920
#define HEIGHT   1080
#define FPS      25

/* IVS grid: 8 columns × 6 rows = 48 ROIs covering the full frame. */
#define IVS_GRID_COLS 8
#define IVS_GRID_ROWS 6

/* Default stack interval in seconds. */
#define DEFAULT_STACK_SECS   30
#define DEFAULT_JPEG_QUALITY 85
#define DEFAULT_IVS_SENSE     2
#define DEFAULT_DARK_PATH    "/mnt/mmcblk0p1/astrostack/dark.raw"

/* -------------------------------------------------------------------------
 * Signal handling
 * ------------------------------------------------------------------------- */

static volatile int running = 1;

static void signal_handler(int sig)
{
	(void)sig;
	running = 0;
}

/* -------------------------------------------------------------------------
 * Y-plane downsample (nearest-neighbour, single byte per pixel)
 * ------------------------------------------------------------------------- */

static void downsample_y(const uint8_t *src, int src_w, int src_h,
			 int src_stride,
			 uint8_t *dst, int dst_w, int dst_h)
{
	int x_step = src_w / dst_w;
	int y_step = src_h / dst_h;
	int dx, dy;

	for (dy = 0; dy < dst_h; dy++) {
		int             sy      = dy * y_step;
		const uint8_t  *src_row = src + (size_t)sy * (size_t)src_stride;
		uint8_t        *dst_row = dst + (size_t)dy * (size_t)dst_w;

		for (dx = 0; dx < dst_w; dx++)
			dst_row[dx] = src_row[dx * x_step];
	}
}

/* -------------------------------------------------------------------------
 * Monotonic wall-clock helper (ms)
 * ------------------------------------------------------------------------- */

static uint64_t monotonic_ms(void)
{
	struct timespec ts;

	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u
	     + (uint64_t)((uint64_t)ts.tv_nsec / 1000000u);
}

/* -------------------------------------------------------------------------
 * Usage
 * ------------------------------------------------------------------------- */

static void usage(const char *prog)
{
	(void)fprintf(stderr,
		"Usage: %s [options]\n"
		"  -S IP    N100 server IP (default: %s)\n"
		"  -I ID    RMS station ID (default: %s)\n"
		"  -t SECS  Stack interval in seconds (default: %d)\n"
		"  -q N     JPEG quality 1-100 (default: %d)\n"
		"  -s N     IVS sensitivity 0-4 (default: %d)\n"
		"  -d PATH  Dark frame file from astrostack -D "
				"(default: %s)\n"
		"  -h       Show this help\n",
		prog,
		DETECTOR_DEFAULT_SERVER_IP,
		DETECTOR_DEFAULT_STATION_ID,
		DEFAULT_STACK_SECS,
		DEFAULT_JPEG_QUALITY,
		DEFAULT_IVS_SENSE,
		DEFAULT_DARK_PATH);
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
	char      server_ip[64];  /* flawfinder: ignore */
	char      station_id[20]; /* flawfinder: ignore */
	char      dark_path[256]; /* flawfinder: ignore */
	int       stack_secs   = DEFAULT_STACK_SECS;
	int       jpeg_quality = DEFAULT_JPEG_QUALITY;
	int       ivs_sense    = DEFAULT_IVS_SENSE;
	int       opt, ret;
	int       ivs_monitor_ok = 0;

	PushConfig   push;
	FFHeader     ff_hdr;
	DetectorState *det = NULL;
	StackerState  *stk = NULL;

	IMPCell fs_cell  = { DEV_ID_FS,  FS_CHN,  0 };
	IMPCell ivs_cell = { DEV_ID_IVS, IVS_GRP, 0 };

	uint8_t *detect_buf = NULL; /* downsampled Y plane */

	(void)snprintf(server_ip,  sizeof(server_ip),
		       "%s", DETECTOR_DEFAULT_SERVER_IP);
	(void)snprintf(station_id, sizeof(station_id),
		       "%s", DETECTOR_DEFAULT_STATION_ID);
	(void)snprintf(dark_path,  sizeof(dark_path),
		       "%s", DEFAULT_DARK_PATH);

	while ((opt = getopt(argc, argv, "S:I:t:q:s:d:h")) != -1) { /* flawfinder: ignore */
		switch (opt) {
		case 'S':
			(void)snprintf(server_ip, sizeof(server_ip),
				       "%s", optarg);
			break;
		case 'I':
			(void)snprintf(station_id, sizeof(station_id),
				       "%s", optarg);
			break;
		case 't':
			stack_secs = (int)strtol(optarg, NULL, 10);
			if (stack_secs < 1) {
				(void)fprintf(stderr,
					"stack interval must be >= 1\n");
				return 1;
			}
			break;
		case 'q':
			jpeg_quality = (int)strtol(optarg, NULL, 10);
			if (jpeg_quality < 1 || jpeg_quality > 100) {
				(void)fprintf(stderr,
					"quality must be 1-100\n");
				return 1;
			}
			break;
		case 's':
			ivs_sense = (int)strtol(optarg, NULL, 10);
			if (ivs_sense < 0 || ivs_sense > 4) {
				(void)fprintf(stderr,
					"sensitivity must be 0-4\n");
				return 1;
			}
			break;
		case 'd':
			(void)snprintf(dark_path, sizeof(dark_path),
				       "%s", optarg);
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	(void)signal(SIGINT,  signal_handler);
	(void)signal(SIGTERM, signal_handler);

	meteor_log_init();
	METEOR_LOG_INFO("nightcam starting: server=%s station=%s "
			"stack=%ds q=%d ivs_sense=%d",
			server_ip, station_id,
			stack_secs, jpeg_quality, ivs_sense);

	/* Downsampled Y buffer for the FTP detector */
	detect_buf = malloc((size_t)(DETECT_WIDTH * DETECT_HEIGHT));
	if (!detect_buf) {
		METEOR_LOG_ERR("nightcam: detect_buf alloc failed");
		return 1;
	}

	/* PushConfig */
	(void)memset(&push, 0, sizeof(push));
	(void)snprintf(push.server_ip, sizeof(push.server_ip),
		       "%s", server_ip);
	push.server_port = DETECTOR_SERVER_PORT;
	push.timeout_ms  = DETECTOR_HTTP_TIMEOUT_MS;

	/* FF header template */
	(void)memset(&ff_hdr, 0, sizeof(ff_hdr));
	(void)snprintf(ff_hdr.station_id, sizeof(ff_hdr.station_id),
		       "%s", station_id);
	ff_hdr.width   = (uint16_t)DETECT_WIDTH;
	ff_hdr.height  = (uint16_t)DETECT_HEIGHT;
	ff_hdr.nframes = (uint16_t)FTP_BLOCK_FRAMES;
	ff_hdr.fps     = FTP_FPS;
	ff_hdr.camno   = 1u;

	/* Create subsystems before IMP pipeline so failures are cheap */
	det = detector_create(&push, &ff_hdr, DETECTOR_FF_TMP_DIR);
	if (!det) {
		METEOR_LOG_ERR("nightcam: detector_create failed");
		ret = -1;
		goto err_alloc;
	}

	stk = stacker_create(&push, station_id,
			     stack_secs * FPS, jpeg_quality,
			     dark_path);
	if (!stk) {
		METEOR_LOG_ERR("nightcam: stacker_create failed");
		ret = -1;
		goto err_alloc;
	}

	/* 1. System */
	ret = meteor_system_init();
	if (ret)
		goto err_alloc;

	/* 2. ISP */
	ret = meteor_isp_init();
	if (ret)
		goto err_sys;

	/* 3. ISP tuning (night mode, denoising off, 25 fps) */
	ret = meteor_isp_tuning_init();
	if (ret)
		goto err_isp;

	/* 4. FrameSource (1920×1080, 25 fps, NV12) */
	ret = meteor_framesource_init(FS_CHN, WIDTH, HEIGHT, FPS);
	if (ret)
		goto err_isp;

	/* 5. IVS motion detector */
	ret = meteor_ivs_init(IVS_GRP, IVS_CHN, WIDTH, HEIGHT,
			      ivs_sense, IVS_GRID_COLS, IVS_GRID_ROWS);
	if (ret)
		goto err_fs;

	/* 6. Bind FrameSource → IVS */
	ret = meteor_system_bind(&fs_cell, &ivs_cell);
	if (ret)
		goto err_ivs;

	/* 7. Enable streaming */
	ret = meteor_framesource_enable(FS_CHN);
	if (ret)
		goto err_unbind;

	/* 8. Set frame depth for direct GetFrame calls */
	ret = meteor_framesource_set_depth(FS_CHN, 2);
	if (ret)
		goto err_fsdis;

	/* 9. Start IVS */
	ret = meteor_ivs_start(IVS_CHN);
	if (ret)
		goto err_fsdis;

	/* 10. Start IVS motion monitor background poll thread.
	 * IVS SDK is already running (steps 5–9); monitor just needs chn. */
	if (ivs_monitor_start(IVS_CHN) == 0) {
		ivs_monitor_ok = 1;
	} else {
		METEOR_LOG_WARN("nightcam: ivs_monitor_start failed "
				"(continuing without IVS stats)");
	}

	METEOR_LOG_INFO("nightcam running — press Ctrl+C to stop");

	/* 11. Main grab loop */
	while (running) {
		IMPFrameInfo *frame = NULL;
		const uint8_t *data;
		uint64_t ts;

		ret = meteor_framesource_get_frame(FS_CHN, &frame);
		if (ret != 0) {
			if (running)
				nanosleep(&(struct timespec){
					.tv_nsec = 10000000 }, NULL);
			continue;
		}

		data = (const uint8_t *)(unsigned long)frame->virAddr;
		ts   = monotonic_ms();

		/* Feed FTP detector (downsampled Y plane) */
		downsample_y(data,
			     WIDTH, HEIGHT, WIDTH,
			     detect_buf,
			     DETECT_WIDTH, DETECT_HEIGHT);
		detector_push_frame(det, detect_buf, DETECT_WIDTH, ts);

		/* Feed stacker (full-resolution NV12) */
		stacker_on_frame(stk, data, ts);

		meteor_framesource_release_frame(FS_CHN, frame);
	}

	ret = 0; /* clean shutdown; reset any transient GetFrame error code */
	METEOR_LOG_INFO("nightcam shutting down...");

	/* Teardown in reverse order */
	if (ivs_monitor_ok)
		ivs_monitor_stop();
	meteor_ivs_stop(IVS_CHN);
err_fsdis:
	meteor_framesource_disable(FS_CHN);
err_unbind:
	meteor_system_unbind(&fs_cell, &ivs_cell);
err_ivs:
	meteor_ivs_exit(IVS_GRP, IVS_CHN);
err_fs:
	meteor_framesource_exit(FS_CHN);
err_isp:
	meteor_isp_exit();
err_sys:
	meteor_system_exit();
err_alloc:
	stacker_destroy(stk);
	detector_destroy(det);
	free(detect_buf);

	METEOR_LOG_INFO("nightcam stopped");
	return ret ? 1 : 0;
}
