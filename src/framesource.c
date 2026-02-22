#include <string.h>
#include <imp/imp_framesource.h>
#include <meteor/framesource.h>
#include <meteor/log.h>

#define DEFAULT_NRVBS 3

int meteor_framesource_init(int chn, int width, int height, int fps)
{
	IMPFSChnAttr attr;
	int ret;

	memset(&attr, 0, sizeof(attr));
	attr.picWidth = width;
	attr.picHeight = height;
	attr.pixFmt = PIX_FMT_NV12;
	attr.outFrmRateNum = fps;
	attr.outFrmRateDen = 1;
	attr.nrVBs = DEFAULT_NRVBS;
	attr.type = FS_PHY_CHANNEL;

	attr.crop.enable = 0;
	attr.scaler.enable = 0;
#ifndef PLATFORM_T20
	attr.fcrop.enable = 0;
#endif

	ret = IMP_FrameSource_CreateChn(chn, &attr);
	if (ret) {
		METEOR_LOG_ERR("IMP_FrameSource_CreateChn(%d) failed: %d", chn, ret);
		return ret;
	}

	ret = IMP_FrameSource_SetChnAttr(chn, &attr);
	if (ret) {
		METEOR_LOG_ERR("IMP_FrameSource_SetChnAttr(%d) failed: %d", chn, ret);
		IMP_FrameSource_DestroyChn(chn);
		return ret;
	}

	METEOR_LOG_INFO("framesource ch%d created: %dx%d @%dfps NV12, %d VBs",
			chn, width, height, fps, DEFAULT_NRVBS);
	return 0;
}

int meteor_framesource_enable(int chn)
{
	int ret;

	ret = IMP_FrameSource_EnableChn(chn);
	if (ret) {
		METEOR_LOG_ERR("IMP_FrameSource_EnableChn(%d) failed: %d", chn, ret);
		return ret;
	}

	METEOR_LOG_INFO("framesource ch%d enabled", chn);
	return 0;
}

int meteor_framesource_disable(int chn)
{
	int ret;

	ret = IMP_FrameSource_DisableChn(chn);
	if (ret) {
		METEOR_LOG_ERR("IMP_FrameSource_DisableChn(%d) failed: %d", chn, ret);
		return ret;
	}

	METEOR_LOG_INFO("framesource ch%d disabled", chn);
	return 0;
}

int meteor_framesource_exit(int chn)
{
	int ret;

	ret = IMP_FrameSource_DestroyChn(chn);
	if (ret) {
		METEOR_LOG_ERR("IMP_FrameSource_DestroyChn(%d) failed: %d", chn, ret);
		return ret;
	}

	METEOR_LOG_INFO("framesource ch%d destroyed", chn);
	return 0;
}
