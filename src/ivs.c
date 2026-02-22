#include <string.h>
#include <imp/imp_ivs.h>
#include <imp/imp_ivs_move.h>
#include <meteor/ivs.h>
#include <meteor/log.h>

static IMPIVSInterface *move_intf = NULL;

int meteor_ivs_init(int grp, int chn, int width, int height, int sense)
{
	IMP_IVS_MoveParam param;
	int ret;

	memset(&param, 0, sizeof(param));
	param.skipFrameCnt = 5;
	param.frameInfo.width = width;
	param.frameInfo.height = height;
	param.roiRectCnt = 1;

	/* Full-frame single ROI */
	param.roiRect[0].p0.x = 0;
	param.roiRect[0].p0.y = 0;
	param.roiRect[0].p1.x = width - 1;
	param.roiRect[0].p1.y = height - 1;
	param.sense[0] = sense;

	move_intf = IMP_IVS_CreateMoveInterface(&param);
	if (!move_intf) {
		METEOR_LOG_ERR("IMP_IVS_CreateMoveInterface failed");
		return -1;
	}

	ret = IMP_IVS_CreateGroup(grp);
	if (ret) {
		METEOR_LOG_ERR("IMP_IVS_CreateGroup(%d) failed: %d", grp, ret);
		goto err_destroy_intf;
	}

	ret = IMP_IVS_CreateChn(chn, move_intf);
	if (ret) {
		METEOR_LOG_ERR("IMP_IVS_CreateChn(%d) failed: %d", chn, ret);
		goto err_destroy_grp;
	}

	ret = IMP_IVS_RegisterChn(grp, chn);
	if (ret) {
		METEOR_LOG_ERR("IMP_IVS_RegisterChn(%d, %d) failed: %d", grp, chn, ret);
		goto err_destroy_chn;
	}

	METEOR_LOG_INFO("IVS motion detection initialized: grp%d ch%d %dx%d sense=%d",
			grp, chn, width, height, sense);
	return 0;

err_destroy_chn:
	IMP_IVS_DestroyChn(chn);
err_destroy_grp:
	IMP_IVS_DestroyGroup(grp);
err_destroy_intf:
	IMP_IVS_DestroyMoveInterface(move_intf);
	move_intf = NULL;
	return ret ? ret : -1;
}

int meteor_ivs_start(int chn)
{
	int ret;

	ret = IMP_IVS_StartRecvPic(chn);
	if (ret) {
		METEOR_LOG_ERR("IMP_IVS_StartRecvPic(%d) failed: %d", chn, ret);
		return ret;
	}

	METEOR_LOG_INFO("IVS ch%d started receiving pictures", chn);
	return 0;
}

int meteor_ivs_poll(int chn, int timeout_ms)
{
	IMP_IVS_MoveOutput *result = NULL;
	int ret, i;

	ret = IMP_IVS_PollingResult(chn, timeout_ms);
	if (ret) {
		METEOR_LOG_ERR("IMP_IVS_PollingResult(%d) failed: %d", chn, ret);
		return ret;
	}

	ret = IMP_IVS_GetResult(chn, (void **)&result);
	if (ret) {
		METEOR_LOG_ERR("IMP_IVS_GetResult(%d) failed: %d", chn, ret);
		return ret;
	}

	for (i = 0; i < IMP_IVS_MOVE_MAX_ROI_CNT; i++) {
		if (result->retRoi[i])
			METEOR_LOG_INFO("motion detected in ROI %d", i);
	}

	ret = IMP_IVS_ReleaseResult(chn, (void *)result);
	if (ret) {
		METEOR_LOG_ERR("IMP_IVS_ReleaseResult(%d) failed: %d", chn, ret);
		return ret;
	}

	return 0;
}

int meteor_ivs_stop(int chn)
{
	int ret;

	ret = IMP_IVS_StopRecvPic(chn);
	if (ret) {
		METEOR_LOG_ERR("IMP_IVS_StopRecvPic(%d) failed: %d", chn, ret);
		return ret;
	}

	METEOR_LOG_INFO("IVS ch%d stopped", chn);
	return 0;
}

int meteor_ivs_exit(int grp, int chn)
{
	int ret;

	ret = IMP_IVS_UnRegisterChn(chn);
	if (ret)
		METEOR_LOG_WARN("IMP_IVS_UnRegisterChn(%d) failed: %d", chn, ret);

	ret = IMP_IVS_DestroyChn(chn);
	if (ret)
		METEOR_LOG_WARN("IMP_IVS_DestroyChn(%d) failed: %d", chn, ret);

	ret = IMP_IVS_DestroyGroup(grp);
	if (ret)
		METEOR_LOG_WARN("IMP_IVS_DestroyGroup(%d) failed: %d", grp, ret);

	if (move_intf) {
		IMP_IVS_DestroyMoveInterface(move_intf);
		move_intf = NULL;
	}

	METEOR_LOG_INFO("IVS grp%d ch%d destroyed", grp, chn);
	return 0;
}
