/*
 * astrostack — astrophotography frame stacker for Ingenic T31X / T20X
 *
 * Captures N long-exposure frames from the ISP, accumulates them into
 * 32-bit buffers, and writes the averaged result as a JPEG image.
 * Stacking reduces sensor noise by a factor of sqrt(N) while preserving
 * star and nebula signal.
 *
 * Usage:
 *   astrostack [options]
 *     -n N         Number of sub-exposures to stack (default: 30)
 *                  In timelapse mode, auto-computed from -t and actual FPS
 *     -e SECS      Sub-exposure time in seconds (default: 2)
 *     -o FILE      Output filename (default: stack.jpg)
 *     -d DIR       Output directory (default: /mnt/mmcblk0p1/astrostack)
 *     -t SECS      Timelapse: stack SECS seconds of frames, save, repeat
 *     -q QUALITY   JPEG quality 1-100 (default: 90)
 *     -g           Output grayscale JPEG instead of color
 *     -c           Enable outlier rejection (min/max clipping, needs N>=4)
 *     -m THRESH    Composite bright transients (meteors) onto stacked image
 *                  Pixels where max-avg > THRESH are replaced with max value
 *                  (default threshold if omitted: 40, valid range: 1-255)
 *     -D           Capture dark frame (cover the lens!) and save to output dir
 */

#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <jpeglib.h>

#include <imp/imp_framesource.h>
#include <imp/imp_isp.h>
#include <imp/imp_system.h>
#include <meteor/jpeg.h>

/* --- logging ------------------------------------------------------------ */

#define TAG "ASTROSTACK"

#define LOG_INFO(fmt, ...) \
	(void)fprintf(stdout, "[INFO ] " TAG ": " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) \
	(void)fprintf(stderr, "[WARN ] " TAG ": " fmt "\n", ##__VA_ARGS__)
#define LOG_ERR(fmt, ...) \
	(void)fprintf(stderr, "[ERROR] " TAG ": " fmt "\n", ##__VA_ARGS__)

/* --- platform ----------------------------------------------------------- */

#ifdef PLATFORM_T20
#define DEFAULT_SENSOR  "jxf22"
#define SENSOR_I2C_ADDR 0x40
#else
#define DEFAULT_SENSOR  "gc2053"
#define SENSOR_I2C_ADDR 0x37
#endif
#define SENSOR_I2C_BUS  0
#define SENSOR_MODULE_PATH "/etc/modules.d/sensor"

/* --- defaults ----------------------------------------------------------- */

#define WIDTH           1920
#define HEIGHT          1080
#define DEFAULT_FRAMES  30
#define DEFAULT_EXPOSURE 2
#define DEFAULT_OUTPUT     "stack.jpg"
#define DEFAULT_OUTPUT_DIR "/mnt/mmcblk0p1/astrostack"
#define DEFAULT_QUALITY    90
#define FS_CHN          0
#define NRVBS           3
#define DEFAULT_METEOR_THRESH 40
#define MIN_MOTION_PIXELS     50
#define DARK_FILENAME         "dark.raw"

/* --- signal handling ---------------------------------------------------- */

static volatile int running = 1;

static void signal_handler(int sig)
{
	(void)sig;
	running = 0;
}

/* --- output directory --------------------------------------------------- */

static int ensure_output_dir(const char *path)
{
	struct stat st;

	if (stat(path, &st) == 0)
		return 0;

	if (mkdir(path, 0755)) {
		LOG_ERR("cannot create output directory: %s", path);
		return -1;
	}

	return 0;
}

/* --- NV12 to RGB conversion --------------------------------------------- */

static inline uint8_t clamp8(int v)
{
	if (v < 0) return 0;
	if (v > 255) return 255;
	return (uint8_t)v;
}

/*
 * Write a JPEG image from averaged Y and UV buffers.
 * Y is full resolution (w*h), UV is interleaved NV12 half-res (w*(h/2)).
 * Converts NV12→RGB one scanline at a time to minimize memory usage.
 */
static int write_jpeg(const char *path, const uint8_t *y, const uint8_t *uv,
		      int w, int h, int quality)
{
	FILE *fp;
	struct jpeg_compress_struct cinfo;
	struct jpeg_error_mgr jerr;
	uint8_t row[WIDTH * 3];
	JSAMPROW row_ptr[1];
	int x, r;

	fp = fopen(path, "wb"); /* flawfinder: ignore */
	if (!fp) {
		LOG_ERR("cannot open %s for writing", path);
		return -1;
	}

	cinfo.err = jpeg_std_error(&jerr);
	jpeg_create_compress(&cinfo);
	jpeg_stdio_dest(&cinfo, fp);

	cinfo.image_width = (JDIMENSION)w;
	cinfo.image_height = (JDIMENSION)h;
	cinfo.input_components = 3;
	cinfo.in_color_space = JCS_RGB;

	jpeg_set_defaults(&cinfo);
	jpeg_set_quality(&cinfo, quality, TRUE);
	jpeg_start_compress(&cinfo, TRUE);

	row_ptr[0] = row;

	for (r = 0; r < h; r++) {
		for (x = 0; x < w; x++) {
			int yi = r * w + x;
			/* NV12: UV plane is half-res, interleaved U,V */
			int uvi = (r / 2) * w + (x & ~1);
			int yv = y[yi];
			int u = uv[uvi] - 128;
			int v = uv[uvi + 1] - 128;

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

/* Grayscale JPEG writing delegated to shared meteor_jpeg_write_gray() */

/* --- IMP SDK setup/teardown --------------------------------------------- */

static IMPSensorInfo sensor_info;
static char detected_sensor[20]; /* flawfinder: ignore */

/*
 * Read /etc/modules.d/sensor and parse "sensor_<name>_<soc>" to extract
 * the sensor name.  Falls back to the compile-time default on failure.
 */
static void detect_sensor_name(char *buf, size_t len)
{
	FILE *fp;
	char line[128]; /* flawfinder: ignore */
	const char *first_us;
	const char *last_us;
	size_t name_len;

	(void)snprintf(buf, len, "%s", DEFAULT_SENSOR);

	fp = fopen(SENSOR_MODULE_PATH, "r"); /* flawfinder: ignore */
	if (!fp)
		return;

	if (!fgets(line, (int)sizeof(line), fp)) {
		(void)fclose(fp);
		return;
	}
	(void)fclose(fp);

	/* Strip trailing whitespace / newline */
	line[strcspn(line, " \t\n\r")] = '\0';

	/* Parse "sensor_<name>_<soc>" */
	first_us = strchr(line, '_');
	if (!first_us)
		return;
	last_us = strrchr(line, '_');
	if (!last_us || last_us == first_us)
		return;

	name_len = (size_t)(last_us - first_us - 1);
	if (name_len == 0 || name_len >= len)
		return;

	(void)snprintf(buf, len, "%.*s", (int)name_len, first_us + 1);
}

static int isp_init(void)
{
	int ret;

	detect_sensor_name(detected_sensor, sizeof(detected_sensor));

	ret = IMP_ISP_Open();
	if (ret) {
		LOG_ERR("IMP_ISP_Open failed: %d", ret);
		return ret;
	}

	memset(&sensor_info, 0, sizeof(sensor_info));
	(void)snprintf(sensor_info.name, sizeof(sensor_info.name),
		       "%s", detected_sensor);
	sensor_info.cbus_type = TX_SENSOR_CONTROL_INTERFACE_I2C;
	(void)snprintf(sensor_info.i2c.type, sizeof(sensor_info.i2c.type),
		       "%s", detected_sensor);
	sensor_info.i2c.addr = SENSOR_I2C_ADDR;
	sensor_info.i2c.i2c_adapter_id = SENSOR_I2C_BUS;

	ret = IMP_ISP_AddSensor(&sensor_info);
	if (ret) {
		LOG_ERR("IMP_ISP_AddSensor failed: %d", ret);
		goto err_close;
	}

	ret = IMP_ISP_EnableSensor();
	if (ret) {
		LOG_ERR("IMP_ISP_EnableSensor failed: %d", ret);
		goto err_del;
	}

	ret = IMP_ISP_EnableTuning();
	if (ret) {
		LOG_ERR("IMP_ISP_EnableTuning failed: %d", ret);
		goto err_disable;
	}

	LOG_INFO("ISP initialized (sensor: %s)", detected_sensor);
	return 0;

err_disable:
	IMP_ISP_DisableSensor();
err_del:
	IMP_ISP_DelSensor(&sensor_info);
err_close:
	IMP_ISP_Close();
	return ret;
}

static void isp_exit(void)
{
	IMP_ISP_DisableTuning();
	IMP_ISP_DisableSensor();
	IMP_ISP_DelSensor(&sensor_info);
	IMP_ISP_Close();
}

static int isp_configure_for_stacking(int exposure_secs)
{
	int ret;

	/* Night mode — always */
	ret = IMP_ISP_Tuning_SetISPRunningMode(IMPISP_RUNNING_MODE_NIGHT);
	if (ret)
		LOG_WARN("SetISPRunningMode(NIGHT) failed: %d", ret);

	/* Disable temporal denoising — we are the denoiser */
	ret = IMP_ISP_Tuning_SetTemperStrength(0);
	if (ret)
		LOG_WARN("SetTemperStrength(0) failed: %d", ret);

	/* Disable spatial denoising — preserve all star signal */
	ret = IMP_ISP_Tuning_SetSinterStrength(0);
	if (ret)
		LOG_WARN("SetSinterStrength(0) failed: %d", ret);

	/* Disable DRC — want linear sensor response for proper averaging */
#ifdef PLATFORM_T31
	ret = IMP_ISP_Tuning_SetDRC_Strength(0);
	if (ret)
		LOG_WARN("SetDRC_Strength(0) failed: %d", ret);
#endif

	/* Slow down the sensor for longer exposures per frame.
	 * Try the requested FPS first, then progressively faster
	 * fallbacks including 1, 2, 3 FPS which the gc2053 may accept.
	 * Non-fatal — we just get shorter sub-exposures per frame. */
	{
		static const uint32_t fps_table[][2] = {
			{1, 1},   /* 1 FPS   — 1000ms/frame */
			{2, 1},   /* 2 FPS   —  500ms/frame */
			{3, 1},   /* 3 FPS   —  333ms/frame */
			{5, 1},   /* 5 FPS   —  200ms/frame */
			{10, 1},  /* 10 FPS  —  100ms/frame */
			{15, 1},  /* 15 FPS  —   67ms/frame */
		};
		int i, ok = 0;

		ret = IMP_ISP_Tuning_SetSensorFPS(1,
						   (uint32_t)exposure_secs);
		if (ret == 0) {
			LOG_INFO("sensor FPS set to 1/%d (%ds/frame)",
				 exposure_secs, exposure_secs);
			ok = 1;
		} else {
			LOG_WARN("SetSensorFPS(1/%d) failed, trying "
				 "fallbacks...", exposure_secs);
			for (i = 0; i < 6; i++) {
				ret = IMP_ISP_Tuning_SetSensorFPS(
					fps_table[i][0], fps_table[i][1]);
				if (ret == 0) {
					LOG_INFO("sensor FPS set to %u/%u "
						 "(~%ums/frame)",
						 fps_table[i][0],
						 fps_table[i][1],
						 1000 * fps_table[i][1]
						 / fps_table[i][0]);
					ok = 1;
					break;
				}
			}
		}

		if (!ok)
			LOG_WARN("could not lower sensor FPS, using default");
	}

	/* Log integration time limits and warn if the sensor cannot
	 * deliver the requested exposure time. */
	{
		IMPISPExpr expr;
		uint32_t fps_n = 0;
		uint32_t fps_d = 1;

		if (IMP_ISP_Tuning_GetExpr(&expr) == 0)
			LOG_INFO("integration time: cur=%u min=%u max=%u "
				 "(%uus/line)",
				 expr.g_attr.integration_time,
				 expr.g_attr.integration_time_min,
				 expr.g_attr.integration_time_max,
				 expr.g_attr.one_line_expr_in_us);

		if (IMP_ISP_Tuning_GetSensorFPS(&fps_n, &fps_d) == 0 &&
		    fps_n > 0) {
			unsigned int frame_ms = 1000 * fps_d / fps_n;
			unsigned int req_ms = (unsigned int)exposure_secs
					      * 1000;

			if (frame_ms < req_ms)
				LOG_WARN("-e %d requested %ums/frame but "
					 "sensor minimum is %u/%u FPS "
					 "(%ums/frame); using %ums",
					 exposure_secs, req_ms,
					 fps_n, fps_d, frame_ms, frame_ms);
		}
	}

	LOG_INFO("ISP configured: night mode, no denoise, %ds target",
		 exposure_secs);
	return 0;
}

static int framesource_init(void)
{
	IMPFSChnAttr attr;
	int ret;

	memset(&attr, 0, sizeof(attr));
	attr.picWidth = WIDTH;
	attr.picHeight = HEIGHT;
	attr.pixFmt = PIX_FMT_NV12;
	attr.outFrmRateNum = 1;
	attr.outFrmRateDen = 1;
	attr.nrVBs = NRVBS;
	attr.type = FS_PHY_CHANNEL;
	attr.crop.enable = 0;
	attr.scaler.enable = 0;
#ifndef PLATFORM_T20
	attr.fcrop.enable = 0;
#endif

	ret = IMP_FrameSource_CreateChn(FS_CHN, &attr);
	if (ret) {
		LOG_ERR("CreateChn failed: %d", ret);
		return ret;
	}

	ret = IMP_FrameSource_SetChnAttr(FS_CHN, &attr);
	if (ret) {
		LOG_ERR("SetChnAttr failed: %d", ret);
		IMP_FrameSource_DestroyChn(FS_CHN);
		return ret;
	}

	ret = IMP_FrameSource_EnableChn(FS_CHN);
	if (ret) {
		LOG_ERR("EnableChn failed: %d", ret);
		IMP_FrameSource_DestroyChn(FS_CHN);
		return ret;
	}

	/* Allow GetFrame to work (must be after EnableChn on some SDKs) */
	ret = IMP_FrameSource_SetFrameDepth(FS_CHN, 1);
	if (ret) {
		LOG_ERR("SetFrameDepth failed: %d", ret);
		IMP_FrameSource_DisableChn(FS_CHN);
		IMP_FrameSource_DestroyChn(FS_CHN);
		return ret;
	}

	LOG_INFO("framesource ready: %dx%d NV12", WIDTH, HEIGHT);
	return 0;
}

static void framesource_exit(void)
{
	IMP_FrameSource_DisableChn(FS_CHN);
	IMP_FrameSource_DestroyChn(FS_CHN);
}

/* --- dark frame capture / load ------------------------------------------ */

static int capture_dark(int num_frames, int grayscale,
			const char *output_dir)
{
	int y_size = WIDTH * HEIGHT;
	int uv_size = WIDTH * (HEIGHT / 2);
	uint32_t *y_acc = NULL;
	uint32_t *uv_acc = NULL;
	uint8_t *y_avg = NULL;
	uint8_t *uv_avg = NULL;
	char dark_path[PATH_MAX]; /* flawfinder: ignore */
	FILE *fp;
	int i, ret;

	(void)snprintf(dark_path, sizeof(dark_path), "%s/%s",
		       output_dir, DARK_FILENAME);

	y_acc = calloc((size_t)y_size, sizeof(uint32_t));
	y_avg = malloc((size_t)y_size);
	if (!grayscale) {
		uv_acc = calloc((size_t)uv_size, sizeof(uint32_t));
		uv_avg = malloc((size_t)uv_size);
	}

	if (!y_acc || !y_avg || (!grayscale && (!uv_acc || !uv_avg))) {
		LOG_ERR("failed to allocate dark frame buffers");
		ret = -1;
		goto cleanup;
	}

	LOG_INFO("capturing %d dark frames...", num_frames);

	for (i = 0; i < num_frames && running; i++) {
		IMPFrameInfo *frame = NULL;
		uint8_t *data;
		int j;

		ret = IMP_FrameSource_GetFrame(FS_CHN, &frame);
		if (ret) {
			LOG_ERR("GetFrame failed on dark frame %d: %d",
				i + 1, ret);
			goto cleanup;
		}

		data = (uint8_t *)(unsigned long)frame->virAddr;

		for (j = 0; j < y_size; j++)
			y_acc[j] += data[j];

		if (!grayscale) {
			for (j = 0; j < uv_size; j++)
				uv_acc[j] += data[y_size + j];
		}

		ret = IMP_FrameSource_ReleaseFrame(FS_CHN, frame);
		if (ret)
			LOG_WARN("ReleaseFrame failed: %d", ret);

		LOG_INFO("  dark frame %d/%d", i + 1, num_frames);
	}

	if (!running) {
		LOG_INFO("interrupted after %d dark frames", i);
		num_frames = i;
	}

	if (num_frames == 0) {
		LOG_ERR("no dark frames captured");
		ret = -1;
		goto cleanup;
	}

	for (i = 0; i < y_size; i++)
		y_avg[i] = (uint8_t)(y_acc[i] / (uint32_t)num_frames);

	if (!grayscale) {
		for (i = 0; i < uv_size; i++)
			uv_avg[i] = (uint8_t)(uv_acc[i]
					      / (uint32_t)num_frames);
	}

	fp = fopen(dark_path, "wb"); /* flawfinder: ignore */
	if (!fp) {
		LOG_ERR("cannot open %s for writing", dark_path);
		ret = -1;
		goto cleanup;
	}

	if (fwrite(y_avg, 1, (size_t)y_size, fp) != (size_t)y_size) {
		LOG_ERR("failed to write Y plane to %s", dark_path);
		(void)fclose(fp);
		ret = -1;
		goto cleanup;
	}

	if (!grayscale) {
		if (fwrite(uv_avg, 1, (size_t)uv_size, fp)
		    != (size_t)uv_size) {
			LOG_ERR("failed to write UV plane to %s", dark_path);
			(void)fclose(fp);
			ret = -1;
			goto cleanup;
		}
	}

	(void)fclose(fp);
	LOG_INFO("dark frame saved to %s (%d frames averaged)", dark_path,
		 num_frames);
	ret = 0;

cleanup:
	free(y_acc);
	free(uv_acc);
	free(y_avg);
	free(uv_avg);
	return ret;
}

static int load_dark(const char *dark_path, int grayscale,
		     uint8_t **y_dark_out, uint8_t **uv_dark_out)
{
	int y_size = WIDTH * HEIGHT;
	int uv_size = WIDTH * (HEIGHT / 2);
	size_t color_size = (size_t)y_size + (size_t)uv_size;
	size_t gray_size = (size_t)y_size;
	struct stat st;
	FILE *fp;
	int has_uv = 0;

	*y_dark_out = NULL;
	*uv_dark_out = NULL;

	if (stat(dark_path, &st) != 0)
		return -1;

	if ((size_t)st.st_size == color_size)
		has_uv = 1;
	else if ((size_t)st.st_size == gray_size)
		has_uv = 0;
	else {
		LOG_WARN("dark frame %s has unexpected size %ld "
			 "(expected %zu or %zu)", dark_path,
			 (long)st.st_size, color_size, gray_size);
		return -1;
	}

	fp = fopen(dark_path, "rb"); /* flawfinder: ignore */
	if (!fp)
		return -1;

	*y_dark_out = malloc((size_t)y_size);
	if (!*y_dark_out) {
		(void)fclose(fp);
		return -1;
	}

	if (fread(*y_dark_out, 1, (size_t)y_size, fp) != (size_t)y_size) {
		free(*y_dark_out);
		*y_dark_out = NULL;
		(void)fclose(fp);
		return -1;
	}

	if (!grayscale && has_uv) {
		*uv_dark_out = malloc((size_t)uv_size);
		if (!*uv_dark_out) {
			free(*y_dark_out);
			*y_dark_out = NULL;
			(void)fclose(fp);
			return -1;
		}

		if (fread(*uv_dark_out, 1, (size_t)uv_size, fp)
		    != (size_t)uv_size) {
			free(*y_dark_out);
			free(*uv_dark_out);
			*y_dark_out = NULL;
			*uv_dark_out = NULL;
			(void)fclose(fp);
			return -1;
		}
	}

	(void)fclose(fp);
	return 0;
}

/* --- stacking ----------------------------------------------------------- */

static int stack_frames(int num_frames, int grayscale, int clip,
			int meteor_thresh, const uint8_t *y_dark,
			const uint8_t *uv_dark, const char *output,
			int quality)
{
	int y_size = WIDTH * HEIGHT;
	int uv_size = WIDTH * (HEIGHT / 2);
	uint32_t *y_acc = NULL;
	uint32_t *uv_acc = NULL;
	uint8_t *y_avg = NULL;
	uint8_t *uv_avg = NULL;
	/* Min/max tracking for outlier rejection and meteor compositing */
	uint8_t *y_min = NULL;
	uint8_t *y_max = NULL;
	uint8_t *uv_min = NULL;
	uint8_t *uv_max = NULL;
	int need_max = clip || meteor_thresh;
	int i, ret;

	/* Allocate accumulators */
	y_acc = calloc((size_t)y_size, sizeof(uint32_t));
	if (!grayscale)
		uv_acc = calloc((size_t)uv_size, sizeof(uint32_t));

	y_avg = malloc((size_t)y_size);
	if (!grayscale)
		uv_avg = malloc((size_t)uv_size);

	if (!y_acc || !y_avg || (!grayscale && (!uv_acc || !uv_avg))) {
		LOG_ERR("failed to allocate buffers");
		ret = -1;
		goto cleanup;
	}

	/* Allocate max arrays for outlier rejection and/or meteor compositing */
	if (need_max) {
		y_max = calloc((size_t)y_size, 1);
		if (!grayscale)
			uv_max = calloc((size_t)uv_size, 1);

		if (!y_max || (!grayscale && !uv_max)) {
			LOG_ERR("failed to allocate max buffers");
			ret = -1;
			goto cleanup;
		}
	}

	/* Allocate min arrays for outlier rejection only */
	if (clip) {
		y_min = malloc((size_t)y_size);
		if (!grayscale)
			uv_min = malloc((size_t)uv_size);

		if (!y_min || (!grayscale && !uv_min)) {
			LOG_ERR("failed to allocate clipping buffers");
			ret = -1;
			goto cleanup;
		}

		memset(y_min, 0xFF, (size_t)y_size);
		if (!grayscale)
			memset(uv_min, 0xFF, (size_t)uv_size);
	}

	LOG_INFO("stacking %d frames%s%s...", num_frames,
		 clip ? " (outlier rejection)" : "",
		 meteor_thresh ? " (meteor compositing)" : "");

	for (i = 0; i < num_frames && running; i++) {
		IMPFrameInfo *frame = NULL;
		uint8_t *data;
		int j;

		ret = IMP_FrameSource_GetFrame(FS_CHN, &frame);
		if (ret) {
			LOG_ERR("GetFrame failed on frame %d: %d", i + 1, ret);
			goto cleanup;
		}

		data = (uint8_t *)(unsigned long)frame->virAddr;

		/* Per-frame motion detection for meteor logging */
		if (meteor_thresh && i > 0) {
			int bright = 0;

			for (j = 0; j < y_size; j++) {
				/* data[j]*i > y_acc[j] + thresh*i
				 * avoids division for running average */
				if ((uint32_t)data[j] * (uint32_t)i >
				    y_acc[j] + (uint32_t)meteor_thresh
				    * (uint32_t)i)
					bright++;
			}

			if (bright > MIN_MOTION_PIXELS)
				LOG_INFO("meteor: frame %d has %d bright "
					 "pixels", i + 1, bright);
		}

		/* Accumulate Y plane */
		for (j = 0; j < y_size; j++) {
			uint8_t v = data[j];
			y_acc[j] += v;
			if (clip) {
				if (v < y_min[j]) y_min[j] = v;
				if (v > y_max[j]) y_max[j] = v;
			} else if (need_max) {
				if (v > y_max[j]) y_max[j] = v;
			}
		}

		/* Accumulate UV plane */
		if (!grayscale) {
			for (j = 0; j < uv_size; j++) {
				uint8_t v = data[y_size + j];
				uv_acc[j] += v;
				if (clip) {
					if (v < uv_min[j]) uv_min[j] = v;
					if (v > uv_max[j]) uv_max[j] = v;
				} else if (need_max) {
					if (v > uv_max[j]) uv_max[j] = v;
				}
			}
		}

		ret = IMP_FrameSource_ReleaseFrame(FS_CHN, frame);
		if (ret)
			LOG_WARN("ReleaseFrame failed: %d", ret);

		{
			IMPISPEVAttr ev_attr;
			uint32_t fps_n = 0;
			uint32_t fps_d = 1;

			(void)IMP_ISP_Tuning_GetSensorFPS(&fps_n, &fps_d);
			if (IMP_ISP_Tuning_GetEVAttr(&ev_attr) == 0)
				LOG_INFO("  frame %d/%d  fps=%u/%u"
					 "  exp=%uus  again=%u  dgain=%u",
					 i + 1, num_frames,
					 fps_n, fps_d,
					 ev_attr.expr_us,
					 ev_attr.again,
					 ev_attr.dgain);
			else
				LOG_INFO("  captured frame %d/%d",
					 i + 1, num_frames);
		}
	}

	if (!running) {
		LOG_INFO("interrupted after %d frames", i);
		num_frames = i;
	}

	if (num_frames == 0) {
		LOG_ERR("no frames captured");
		ret = -1;
		goto cleanup;
	}

	/* Fall back to simple average if too few frames for clipping */
	if (clip && num_frames < 4) {
		LOG_WARN("only %d frames — disabling outlier rejection",
			 num_frames);
		clip = 0;
	}

	/* Average — with or without min/max rejection */
	if (clip) {
		uint32_t divisor = (uint32_t)(num_frames - 2);

		LOG_INFO("averaging %d frames (rejecting min/max per pixel)...",
			 num_frames);
		for (i = 0; i < y_size; i++)
			y_avg[i] = (uint8_t)((y_acc[i] - y_min[i] - y_max[i])
					     / divisor);
		if (!grayscale) {
			for (i = 0; i < uv_size; i++)
				uv_avg[i] = (uint8_t)((uv_acc[i] - uv_min[i]
						       - uv_max[i]) / divisor);
		}
	} else {
		LOG_INFO("averaging %d frames...", num_frames);
		for (i = 0; i < y_size; i++)
			y_avg[i] = (uint8_t)(y_acc[i] / (uint32_t)num_frames);
		if (!grayscale) {
			for (i = 0; i < uv_size; i++)
				uv_avg[i] = (uint8_t)(uv_acc[i]
						      / (uint32_t)num_frames);
		}
	}

	/* Composite bright transients (meteor pixels) onto the average */
	if (meteor_thresh) {
		int composited = 0;

		if (grayscale) {
			for (i = 0; i < y_size; i++) {
				if (y_max[i] - y_avg[i] > meteor_thresh) {
					y_avg[i] = y_max[i];
					composited++;
				}
			}
		} else {
			int r, x;

			for (r = 0; r < HEIGHT; r++) {
				for (x = 0; x < WIDTH; x++) {
					int yi = r * WIDTH + x;

					if (y_max[yi] - y_avg[yi]
					    > meteor_thresh) {
						int uvi = (r / 2) * WIDTH
							  + (x & ~1);

						y_avg[yi] = y_max[yi];
						uv_avg[uvi] = uv_max[uvi];
						uv_avg[uvi + 1] =
							uv_max[uvi + 1];
						composited++;
					}
				}
			}
		}

		LOG_INFO("composited %d pixels (%.2f%%)", composited,
			 100.0 * composited / y_size);
	}

	/* Subtract dark frame */
	if (y_dark) {
		LOG_INFO("subtracting dark frame");
		for (i = 0; i < y_size; i++) {
			int v = y_avg[i] - y_dark[i];
			y_avg[i] = (uint8_t)(v > 0 ? v : 0);
		}
		if (!grayscale && uv_dark) {
			for (i = 0; i < uv_size; i++) {
				int v = uv_avg[i] - uv_dark[i] + 128;
				uv_avg[i] = clamp8(v);
			}
		}
	}

	/* Write output */
	if (grayscale)
		ret = meteor_jpeg_write_gray(output, y_avg, WIDTH, HEIGHT,
					     quality);
	else
		ret = write_jpeg(output, y_avg, uv_avg, WIDTH, HEIGHT,
				 quality);

	if (ret == 0)
		LOG_INFO("wrote %s (%dx%d, %d frames stacked%s%s)",
			 output, WIDTH, HEIGHT, num_frames,
			 clip ? ", clipped" : "",
			 meteor_thresh ? ", meteor" : "");

cleanup:
	free(y_acc);
	free(uv_acc);
	free(y_avg);
	free(uv_avg);
	free(y_min);
	free(y_max);
	free(uv_min);
	free(uv_max);
	return ret;
}

/* --- main --------------------------------------------------------------- */

static void usage(const char *prog)
{
	(void)fprintf(stderr,
		"Usage: %s [options]\n"
		"  -n N      Sub-exposures to stack (default: %d, auto in timelapse)\n"
		"  -e SECS   Sub-exposure time in seconds (default: %d)\n"
		"  -o FILE   Output filename (default: %s)\n"
		"  -d DIR    Output directory (default: %s)\n"
		"  -t SECS   Timelapse: stack SECS seconds of frames, save, repeat\n"
		"  -q N      JPEG quality 1-100 (default: %d)\n"
		"  -g        Output grayscale JPEG instead of color\n"
		"  -c        Enable outlier rejection (min/max clipping)\n"
		"  -m THRESH Composite bright transients (meteors) onto stack\n"
		"  -D        Capture dark frame (cover the lens!) and save to output dir\n"
		"  -h        Show this help\n",
		prog, DEFAULT_FRAMES, DEFAULT_EXPOSURE, DEFAULT_OUTPUT,
		DEFAULT_OUTPUT_DIR, DEFAULT_QUALITY);
}

int main(int argc, char **argv)
{
	int num_frames = DEFAULT_FRAMES;
	int exposure = DEFAULT_EXPOSURE;
	const char *output = DEFAULT_OUTPUT;
	const char *output_dir = DEFAULT_OUTPUT_DIR;
	int quality = DEFAULT_QUALITY;
	int grayscale = 0;
	int clip = 0;
	int meteor_thresh = 0;
	int timelapse = 0;
	int dark_mode = 0;
	int opt, ret;
	char path_buf[PATH_MAX]; /* flawfinder: ignore */

	while ((opt = getopt(argc, argv, "n:e:o:d:t:q:m:gcDh")) != -1) { /* flawfinder: ignore */
		switch (opt) {
		case 'n':
			num_frames = (int)strtol(optarg, NULL, 10);
			if (num_frames < 1) {
				(void)fprintf(stderr, "invalid frame count\n");
				return 1;
			}
			break;
		case 'e':
			exposure = (int)strtol(optarg, NULL, 10);
			if (exposure < 1 || exposure > 30) {
				(void)fprintf(stderr,
					      "exposure must be 1-30 seconds\n");
				return 1;
			}
			break;
		case 'o':
			output = optarg;
			break;
		case 'd':
			output_dir = optarg;
			break;
		case 't':
			timelapse = (int)strtol(optarg, NULL, 10);
			if (timelapse < 1) {
				(void)fprintf(stderr,
					      "timelapse interval must be >= 1 second\n");
				return 1;
			}
			break;
		case 'q':
			quality = (int)strtol(optarg, NULL, 10);
			if (quality < 1 || quality > 100) {
				(void)fprintf(stderr,
					      "quality must be 1-100\n");
				return 1;
			}
			break;
		case 'g':
			grayscale = 1;
			break;
		case 'm':
			meteor_thresh = (int)strtol(optarg, NULL, 10);
			if (meteor_thresh < 1 || meteor_thresh > 255) {
				(void)fprintf(stderr,
					      "meteor threshold must be 1-255\n");
				return 1;
			}
			break;
		case 'c':
			clip = 1;
			break;
		case 'D':
			dark_mode = 1;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	(void)signal(SIGINT, signal_handler);
	(void)signal(SIGTERM, signal_handler);

	(void)setvbuf(stdout, NULL, _IOLBF, 0);
	(void)setvbuf(stderr, NULL, _IOLBF, 0);

	if (ensure_output_dir(output_dir)) {
		return 1;
	}

	/* 1. System */
	ret = IMP_System_Init();
	if (ret) {
		LOG_ERR("IMP_System_Init failed: %d", ret);
		return 1;
	}

	/* 2. ISP */
	ret = isp_init();
	if (ret)
		goto err_sys;

	/* 3. ISP tuning for stacking */
	ret = isp_configure_for_stacking(exposure);
	if (ret)
		goto err_isp;

	/* 4. FrameSource */
	ret = framesource_init();
	if (ret)
		goto err_isp;

	/* 5. Dark capture mode — capture and save, then exit */
	if (dark_mode) {
		ret = capture_dark(num_frames, grayscale, output_dir);
		goto err_fs;
	}

	/* 6. In timelapse mode, compute frame count from actual FPS
	 * so each stack fills the full interval with captures. */
	if (timelapse) {
		uint32_t fps_n = 0;
		uint32_t fps_d = 1;

		if (IMP_ISP_Tuning_GetSensorFPS(&fps_n, &fps_d) == 0 &&
		    fps_n > 0) {
			/* fps = fps_n / fps_d, frames = fps * interval */
			num_frames = (int)(fps_n * (uint32_t)timelapse
					   / fps_d);
			if (num_frames < 4)
				num_frames = 4;
			LOG_INFO("timelapse: sensor=%u/%u FPS, stacking %d "
				 "frames per %ds interval",
				 fps_n, fps_d, num_frames, timelapse);
		} else {
			LOG_WARN("cannot read sensor FPS, using -n %d",
				 num_frames);
		}
	}

	if (clip && num_frames < 4) {
		LOG_ERR("outlier rejection requires at least 4 frames");
		ret = -1;
		goto err_fs;
	}

	/* 7. Load dark frame if available */
	{
		uint8_t *y_dark = NULL;
		uint8_t *uv_dark = NULL;
		char dark_path[PATH_MAX]; /* flawfinder: ignore */

		(void)snprintf(dark_path, sizeof(dark_path), "%s/%s",
			       output_dir, DARK_FILENAME);

		if (load_dark(dark_path, grayscale, &y_dark, &uv_dark) == 0)
			LOG_INFO("loaded dark frame from %s", dark_path);

		if (timelapse)
			LOG_INFO("astrostack starting: %d frames per %ds, "
				 "timelapse -> %s/",
				 num_frames, timelapse, output_dir);
		else
			LOG_INFO("astrostack starting: %d x %ds subs -> %s/%s",
				 num_frames, exposure, output_dir, output);

		/* 8. Stack (single-shot or timelapse loop) */
		if (timelapse) {
			int frame_idx = 0;

			while (running) {
				(void)snprintf(path_buf, sizeof(path_buf),
					       "%s/timelapse-%04d.jpg",
					       output_dir, frame_idx);

				ret = stack_frames(num_frames, grayscale, clip,
						   meteor_thresh, y_dark,
						   uv_dark, path_buf,
						   quality);
				if (ret || !running)
					break;

				frame_idx++;
			}
		} else {
			(void)snprintf(path_buf, sizeof(path_buf), "%s/%s",
				       output_dir, output);
			ret = stack_frames(num_frames, grayscale, clip,
					   meteor_thresh, y_dark, uv_dark,
					   path_buf, quality);
		}

		free(y_dark);
		free(uv_dark);
	}

	/* Teardown */
err_fs:
	framesource_exit();
err_isp:
	isp_exit();
err_sys:
	IMP_System_Exit();

	LOG_INFO("astrostack %s", ret ? "failed" : "done");
	return ret ? 1 : 0;
}
