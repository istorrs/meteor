#include <stdio.h>
#include <time.h>
#include <meteor/log.h>

void meteor_log_init(void)
{
	(void)setvbuf(stdout, NULL, _IOLBF, 0);
	(void)setvbuf(stderr, NULL, _IOLBF, 0);
	METEOR_LOG_INFO("logging initialized");
}
