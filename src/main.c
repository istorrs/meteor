#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <imp/imp_system.h>
#include <meteor/log.h>
#include <meteor/system.h>
#include <meteor/isp.h>
#include <meteor/isp_tuning.h>
#include <meteor/framesource.h>
#include <meteor/ivs.h>
#include <meteor/config.h>
#include <meteor/event.h>
#include <meteor/capture.h>

#define FS_CHN     0
#define IVS_GRP    0
#define IVS_CHN    0
#define WIDTH      1920
#define HEIGHT     1080
#define FPS        25
#define POLL_MS    1000

static volatile int running = 1;

static void signal_handler(int sig)
{
	(void)sig;
	running = 0;
}

static int ensure_output_dir(const char *path)
{
	struct stat st;

	if (stat(path, &st) == 0)
		return 0;

	if (mkdir(path, 0755)) {
		METEOR_LOG_ERR("cannot create output directory: %s", path);
		return -1;
	}

	return 0;
}

int main(int argc, char **argv)
{
	IMPCell fs_cell  = { DEV_ID_FS,  FS_CHN,  0 };
	IMPCell ivs_cell = { DEV_ID_IVS, IVS_GRP, 0 };
	meteor_config cfg;
	meteor_event_ctx evt;
	meteor_ivs_result ivs_result;
	struct sigaction sa;
	int ret;

	if (meteor_config_parse(&cfg, argc, argv))
		return 1;

	(void)signal(SIGINT,  signal_handler);
	(void)signal(SIGTERM, signal_handler);

	/* Reap child processes automatically (zombie-free) */
	sa.sa_handler = SIG_DFL;
	(void)sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_NOCLDWAIT;
	(void)sigaction(SIGCHLD, &sa, NULL);

	meteor_log_init();
	METEOR_LOG_INFO("meteor starting");
	METEOR_LOG_INFO("config: sense=%d grid=%dx%d cooldown=%ds capture=%dms out=%s",
			cfg.sensitivity, cfg.grid_cols, cfg.grid_rows,
			cfg.cooldown_secs, cfg.capture_interval_ms,
			cfg.output_dir);
	METEOR_LOG_INFO("storage: max_frames=%d retention=%dd",
			cfg.max_event_frames, cfg.retention_days);

	if (ensure_output_dir(cfg.output_dir))
		return 1;

	meteor_event_cleanup_old(&cfg);
	meteor_event_init(&evt, &cfg);

	/* 1. System */
	ret = meteor_system_init();
	if (ret)
		return 1;

	/* 2. ISP */
	ret = meteor_isp_init();
	if (ret)
		goto err_sys;

	/* 3. ISP tuning for meteor detection */
	ret = meteor_isp_tuning_init();
	if (ret)
		goto err_isp;

	/* 4. FrameSource */
	ret = meteor_framesource_init(FS_CHN, WIDTH, HEIGHT, FPS);
	if (ret)
		goto err_isp;

	/* 5. IVS */
	ret = meteor_ivs_init(IVS_GRP, IVS_CHN, WIDTH, HEIGHT,
			      cfg.sensitivity, cfg.grid_cols, cfg.grid_rows);
	if (ret)
		goto err_fs;

	/* 6. Bind FrameSource -> IVS */
	ret = meteor_system_bind(&fs_cell, &ivs_cell);
	if (ret)
		goto err_ivs;

	/* 7. Enable streaming */
	ret = meteor_framesource_enable(FS_CHN);
	if (ret)
		goto err_unbind;

	/* 8. Enable frame capture (SetFrameDepth after EnableChn) */
	ret = meteor_capture_enable(FS_CHN);
	if (ret)
		goto err_fsdis;

	/* 9. Start IVS */
	ret = meteor_ivs_start(IVS_CHN);
	if (ret)
		goto err_fsdis;

	METEOR_LOG_INFO("pipeline running — press Ctrl+C to stop");

	/* 10. Main loop: poll → event_update → conditional capture */
	while (running) {
		ret = meteor_ivs_poll(IVS_CHN, POLL_MS, &ivs_result);
		if (ret) {
			if (running)
				nanosleep(&(struct timespec){.tv_nsec = 100000000}, NULL);
			continue;
		}

		(void)meteor_event_update(&evt, &ivs_result);

		if (meteor_event_should_capture(&evt)) {
			if (meteor_capture_frame(FS_CHN, evt.event_dir,
						 evt.frame_count, WIDTH,
						 HEIGHT) == 0)
				meteor_event_frame_captured(&evt);
		}
	}

	METEOR_LOG_INFO("shutting down...");

	/* Teardown in reverse order */
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

	METEOR_LOG_INFO("meteor stopped");
	return ret ? 1 : 0;
}
