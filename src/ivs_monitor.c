#define _POSIX_C_SOURCE 200809L
#include <meteor/ivs_monitor.h>
#include <meteor/ivs.h>
#include <meteor/log.h>
#include <pthread.h>
#include <string.h>

#define IVS_POLL_MS 500

typedef struct {
	int             chn;
	volatile int    running;
	pthread_t       thread;
	pthread_mutex_t mutex;
	IVSMotionStats  stats;
} IVSMonitor;

static IVSMonitor s_mon;
static int        s_started;

static void *monitor_thread(void *arg)
{
	IVSMonitor       *mon = (IVSMonitor *)arg;
	meteor_ivs_result result;

	while (mon->running) {
		int ret = meteor_ivs_poll(mon->chn, IVS_POLL_MS, &result);

		if (ret != 0)
			continue;

		(void)pthread_mutex_lock(&mon->mutex);
		mon->stats.polls++;
		mon->stats.last_rois = result.triggered;
		if (result.triggered > 0) {
			mon->stats.active_polls++;
			mon->stats.total_rois += result.triggered;
		}
		(void)pthread_mutex_unlock(&mon->mutex);
	}

	return NULL;
}

int ivs_monitor_start(int chn)
{
	(void)memset(&s_mon, 0, sizeof(s_mon));
	s_started = 0;
	s_mon.chn = chn;

	if (pthread_mutex_init(&s_mon.mutex, NULL) != 0)
		return -1;

	s_mon.running = 1;

	if (pthread_create(&s_mon.thread, NULL, monitor_thread, &s_mon) != 0) {
		s_mon.running = 0;
		(void)pthread_mutex_destroy(&s_mon.mutex);
		return -1;
	}

	s_started = 1;
	METEOR_LOG_INFO("ivs_monitor: started (chn=%d)", chn);
	return 0;
}

void ivs_monitor_stop(void)
{
	s_mon.running = 0;
	(void)pthread_join(s_mon.thread, NULL);
	(void)pthread_mutex_destroy(&s_mon.mutex);
	METEOR_LOG_INFO("ivs_monitor: stopped");
}

void ivs_monitor_get_stats(IVSMotionStats *out)
{
	if (!s_started) {
		(void)memset(out, 0, sizeof(*out));
		return;
	}
	(void)pthread_mutex_lock(&s_mon.mutex);
	*out = s_mon.stats;
	(void)pthread_mutex_unlock(&s_mon.mutex);
}

void ivs_monitor_reset_stats(void)
{
	if (!s_started)
		return;
	(void)pthread_mutex_lock(&s_mon.mutex);
	(void)memset(&s_mon.stats, 0, sizeof(s_mon.stats));
	(void)pthread_mutex_unlock(&s_mon.mutex);
}
