/*
 * detector_main.c — entry point for the RMS FTP meteor detector.
 *
 * Initialises the IMP pipeline (System → ISP → FrameSource) then hands
 * control to the meteor_module grab thread, which runs the FTP/Hough
 * detection pipeline continuously and POSTs events and FF files to the
 * N100 receiver.
 *
 * No IVS, no JPEG capture, no event state machine.
 *
 * Usage: detector [-S server_ip] [-I station_id] [-o ff_tmp_dir] [-h]
 */
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
#include <meteor/config.h>
#include <meteor/meteor_module.h>

#define FS_CHN  0
#define WIDTH   1920
#define HEIGHT  1080
#define FPS     25

static volatile int running = 1;

static void signal_handler(int sig)
{
	(void)sig;
	running = 0;
}

int main(int argc, char **argv)
{
	meteor_config    cfg;
	struct sigaction sa;
	int              ret;

	if (meteor_config_parse(&cfg, argc, argv))
		return 1;

	(void)signal(SIGINT,  signal_handler);
	(void)signal(SIGTERM, signal_handler);

	sa.sa_handler = SIG_DFL;
	(void)sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_NOCLDWAIT;
	(void)sigaction(SIGCHLD, &sa, NULL);

	meteor_log_init();
	METEOR_LOG_INFO("detector starting");
	METEOR_LOG_INFO("server=%s station=%s",
			cfg.server_ip, cfg.station_id);

	/* 1. System */
	ret = meteor_system_init();
	if (ret)
		return 1;

	/* 2. ISP */
	ret = meteor_isp_init();
	if (ret)
		goto err_sys;

	/* 3. ISP tuning for night sky */
	ret = meteor_isp_tuning_init();
	if (ret)
		goto err_isp;

	/* 4. FrameSource */
	ret = meteor_framesource_init(FS_CHN, WIDTH, HEIGHT, FPS);
	if (ret)
		goto err_isp;

	/* 5. Enable streaming */
	ret = meteor_framesource_enable(FS_CHN);
	if (ret)
		goto err_fs;

	/* 6. Set frame buffer depth so grab thread can pull frames */
	ret = meteor_framesource_set_depth(FS_CHN, 1);
	if (ret)
		goto err_fsdis;

	/* 7. Start FTP detector grab thread */
	ret = meteor_module_init(FS_CHN, &cfg);
	if (ret)
		goto err_fsdis;

	METEOR_LOG_INFO("detector running — press Ctrl+C to stop");

	while (running)
		nanosleep(&(struct timespec){ .tv_sec = 1 }, NULL);

	METEOR_LOG_INFO("shutting down...");

	meteor_module_deinit();
err_fsdis:
	meteor_framesource_disable(FS_CHN);
err_fs:
	meteor_framesource_exit(FS_CHN);
err_isp:
	meteor_isp_exit();
err_sys:
	meteor_system_exit();

	METEOR_LOG_INFO("detector stopped");
	return ret ? 1 : 0;
}
