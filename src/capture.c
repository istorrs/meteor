#include <stdint.h>
#include <stdio.h>
#include <imp/imp_common.h>
#include <meteor/capture.h>
#include <meteor/framesource.h>
#include <meteor/jpeg.h>
#include <meteor/log.h>

int meteor_capture_enable(int chn)
{
	return meteor_framesource_set_depth(chn, 1);
}

int meteor_capture_frame(int chn, const char *dir, int frame_num,
			 int width, int height)
{
	IMPFrameInfo *frame = NULL;
	char path[320]; /* flawfinder: ignore — bounded by snprintf */
	uint8_t *data;
	int ret;

	ret = meteor_framesource_get_frame(chn, &frame);
	if (ret)
		return -1;

	data = (uint8_t *)(unsigned long)frame->virAddr;

	(void)snprintf(path, sizeof(path), "%s/frame_%03d.jpg", dir, frame_num);
	ret = meteor_jpeg_write_gray(path, data, width, height, 90);

	if (meteor_framesource_release_frame(chn, frame))
		METEOR_LOG_WARN("failed to release frame after capture");

	return ret;
}
