#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <imp/imp_system.h>
#include <meteor/log.h>
#include <meteor/system.h>
#include <meteor/isp.h>
#include <meteor/framesource.h>
#include <meteor/ivs.h>

#define FS_CHN     0
#define IVS_GRP    0
#define IVS_CHN    0
#define WIDTH      1920
#define HEIGHT     1080
#define FPS        25
#define SENSE      3
#define POLL_MS    1000

static volatile int running = 1;

static void signal_handler(int sig)
{
	(void)sig;
	running = 0;
}

int main(void)
{
	IMPCell fs_cell  = { DEV_ID_FS,  FS_CHN,  0 };
	IMPCell ivs_cell = { DEV_ID_IVS, IVS_GRP, 0 };
	int ret;

	(void)signal(SIGINT,  signal_handler);
	(void)signal(SIGTERM, signal_handler);

	meteor_log_init();
	METEOR_LOG_INFO("meteor starting");

	/* 1. System */
	ret = meteor_system_init();
	if (ret)
		return 1;

	/* 2. ISP */
	ret = meteor_isp_init();
	if (ret)
		goto err_sys;

	/* 3. FrameSource */
	ret = meteor_framesource_init(FS_CHN, WIDTH, HEIGHT, FPS);
	if (ret)
		goto err_isp;

	/* 4. IVS */
	ret = meteor_ivs_init(IVS_GRP, IVS_CHN, WIDTH, HEIGHT, SENSE);
	if (ret)
		goto err_fs;

	/* 5. Enable FrameSource (must be before bind in some SDK versions,
	 *    but T31 1.1.6 expects bind during init, before enable) */

	/* 6. Bind FrameSource -> IVS */
	ret = meteor_system_bind(&fs_cell, &ivs_cell);
	if (ret)
		goto err_ivs;

	/* 7. Enable streaming */
	ret = meteor_framesource_enable(FS_CHN);
	if (ret)
		goto err_unbind;

	/* 8. Start IVS */
	ret = meteor_ivs_start(IVS_CHN);
	if (ret)
		goto err_fsdis;

	METEOR_LOG_INFO("pipeline running — press Ctrl+C to stop");

	/* 9. Main loop */
	while (running) {
		ret = meteor_ivs_poll(IVS_CHN, POLL_MS);
		if (ret && running)
			nanosleep(&(struct timespec){.tv_nsec = 100000000}, NULL);
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
