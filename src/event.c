#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <meteor/event.h>
#include <meteor/log.h>

static void get_time(struct timespec *ts)
{
	(void)clock_gettime(CLOCK_MONOTONIC, ts);
}

static int64_t elapsed_ms(const struct timespec *from, const struct timespec *to)
{
	int64_t sec = to->tv_sec - from->tv_sec;
	int64_t nsec = to->tv_nsec - from->tv_nsec;

	return sec * 1000 + nsec / 1000000;
}

static int make_event_dir(meteor_event_ctx *ctx)
{
	time_t now;
	struct tm tm;
	int ret;

	now = time(NULL);
	(void)localtime_r(&now, &tm);

	(void)snprintf(ctx->event_dir, sizeof(ctx->event_dir),
		       "%s/%04d%02d%02d_%02d%02d%02d",
		       ctx->cfg->output_dir,
		       tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		       tm.tm_hour, tm.tm_min, tm.tm_sec);

	ret = mkdir(ctx->event_dir, 0755);
	if (ret && errno != EEXIST) {
		METEOR_LOG_ERR("mkdir(%s) failed: %s",
			       ctx->event_dir, strerror(errno));
		return -1;
	}

	return 0;
}

static void write_metadata(const meteor_event_ctx *ctx)
{
	char path[320]; /* flawfinder: ignore — bounded by snprintf */
	FILE *fp;
	struct timespec now;
	int64_t dur;

	(void)snprintf(path, sizeof(path), "%s/metadata.txt", ctx->event_dir);

	fp = fopen(path, "w"); /* flawfinder: ignore */
	if (!fp) {
		METEOR_LOG_ERR("cannot write metadata: %s", path);
		return;
	}

	get_time(&now);
	dur = elapsed_ms(&ctx->start_time, &now);

	(void)fprintf(fp, "duration_ms: %lld\n", (long long)dur);
	(void)fprintf(fp, "frames: %d\n", ctx->frame_count);
	(void)fprintf(fp, "total_triggers: %d\n", ctx->total_triggers);

	(void)fclose(fp);
	METEOR_LOG_INFO("event ended: %s (%lldms, %d frames, %d triggers)",
			ctx->event_dir, (long long)dur, ctx->frame_count,
			ctx->total_triggers);
}

void meteor_event_init(meteor_event_ctx *ctx, const meteor_config *cfg)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->state = METEOR_EVENT_IDLE;
	ctx->cfg = cfg;
}

meteor_event_state meteor_event_update(meteor_event_ctx *ctx,
				       const meteor_ivs_result *result)
{
	struct timespec now;

	get_time(&now);

	if (result->triggered > 0) {
		ctx->total_triggers += result->triggered;
		ctx->last_motion = now;

		switch (ctx->state) {
		case METEOR_EVENT_IDLE:
			if (make_event_dir(ctx))
				return ctx->state;
			ctx->start_time = now;
			ctx->frame_count = 0;
			ctx->total_triggers = result->triggered;
			ctx->state = METEOR_EVENT_ACTIVE;
			METEOR_LOG_INFO("event started: %s (%d ROIs triggered)",
					ctx->event_dir, result->triggered);
			break;
		case METEOR_EVENT_COOLDOWN:
			ctx->state = METEOR_EVENT_ACTIVE;
			METEOR_LOG_DBG("event reactivated (%d ROIs)",
				       result->triggered);
			break;
		case METEOR_EVENT_ACTIVE:
			break;
		}
	} else {
		switch (ctx->state) {
		case METEOR_EVENT_ACTIVE:
			ctx->state = METEOR_EVENT_COOLDOWN;
			break;
		case METEOR_EVENT_COOLDOWN:
			if (elapsed_ms(&ctx->last_motion, &now) >=
			    (int64_t)ctx->cfg->cooldown_secs * 1000) {
				write_metadata(ctx);
				ctx->state = METEOR_EVENT_IDLE;
				ctx->event_dir[0] = '\0';
			}
			break;
		case METEOR_EVENT_IDLE:
			break;
		}
	}

	return ctx->state;
}

int meteor_event_should_capture(const meteor_event_ctx *ctx)
{
	struct timespec now;

	if (ctx->state == METEOR_EVENT_IDLE)
		return 0;

	get_time(&now);
	return elapsed_ms(&ctx->last_capture, &now) >=
	       (int64_t)ctx->cfg->capture_interval_ms;
}

void meteor_event_frame_captured(meteor_event_ctx *ctx)
{
	ctx->frame_count++;
	get_time(&ctx->last_capture);
}
