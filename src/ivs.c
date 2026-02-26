#include <string.h>
#include <imp/imp_ivs.h>
#include <imp/imp_ivs_move.h>
#include <meteor/ivs.h>
#include <meteor/log.h>

static IMPIVSInterface *move_intf = NULL;
static int configured_roi_count = 0;

int meteor_ivs_init(int grp, int chn, int width, int height,
		    int sense, int grid_cols, int grid_rows)
{
	IMP_IVS_MoveParam param;
	int col, row, idx, ret;
	int cell_w, cell_h;

	memset(&param, 0, sizeof(param));
	param.skipFrameCnt = 5;
	param.frameInfo.width = width;
	param.frameInfo.height = height;

	cell_w = width / grid_cols;
	cell_h = height / grid_rows;
	param.roiRectCnt = grid_cols * grid_rows;

	for (row = 0; row < grid_rows; row++) {
		for (col = 0; col < grid_cols; col++) {
			idx = row * grid_cols + col;
			param.roiRect[idx].p0.x = col * cell_w;
			param.roiRect[idx].p0.y = row * cell_h;
			param.roiRect[idx].p1.x = (col + 1) * cell_w - 1;
			param.roiRect[idx].p1.y = (row + 1) * cell_h - 1;
			param.sense[idx] = sense;
		}
	}

	/* Extend last column/row to cover any remainder pixels */
	for (row = 0; row < grid_rows; row++) {
		idx = row * grid_cols + (grid_cols - 1);
		param.roiRect[idx].p1.x = width - 1;
	}
	for (col = 0; col < grid_cols; col++) {
		idx = (grid_rows - 1) * grid_cols + col;
		param.roiRect[idx].p1.y = height - 1;
	}

	configured_roi_count = param.roiRectCnt;

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
		METEOR_LOG_ERR("IMP_IVS_RegisterChn(%d, %d) failed: %d",
			       grp, chn, ret);
		goto err_destroy_chn;
	}

	METEOR_LOG_INFO("IVS initialized: grp%d ch%d %dx%d grid=%dx%d (%d ROIs) sense=%d",
			grp, chn, width, height, grid_cols, grid_rows,
			param.roiRectCnt, sense);
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

int meteor_ivs_poll(int chn, int timeout_ms, meteor_ivs_result *result)
{
	IMP_IVS_MoveOutput *output = NULL;
	int ret, i;

	ret = IMP_IVS_PollingResult(chn, timeout_ms);
	if (ret) {
		/* Timeout is normal, not an error */
		return ret;
	}

	ret = IMP_IVS_GetResult(chn, (void **)&output);
	if (ret) {
		METEOR_LOG_ERR("IMP_IVS_GetResult(%d) failed: %d", chn, ret);
		return ret;
	}

	result->triggered = 0;
	result->roi_count = configured_roi_count;
	for (i = 0; i < configured_roi_count; i++) {
		result->roi[i] = output->retRoi[i] ? 1 : 0;
		if (result->roi[i])
			result->triggered++;
	}

	ret = IMP_IVS_ReleaseResult(chn, (void *)output);
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
