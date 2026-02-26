#include <imp/imp_isp.h>
#include <meteor/isp_tuning.h>
#include <meteor/log.h>

/*
 * Default tuning values for meteor (astronomical) detection.
 * These prioritize detecting brief, faint streaks against a dark sky.
 */
#define DEFAULT_TEMPER_STRENGTH    0    /* off — preserve single-frame events */
#define DEFAULT_SINTER_STRENGTH   48   /* low — reduce noise, keep faint streaks */
#define DEFAULT_DRC_STRENGTH      192  /* high — pull detail from dark background */

int meteor_isp_tuning_init(void)
{
	int ret;

	/* Lock ISP to night mode — this camera always faces the night sky */
	ret = IMP_ISP_Tuning_SetISPRunningMode(IMPISP_RUNNING_MODE_NIGHT);
	if (ret) {
		METEOR_LOG_ERR("SetISPRunningMode(NIGHT) failed: %d", ret);
		return ret;
	}
	METEOR_LOG_INFO("ISP running mode locked to NIGHT");

	/* Disable temporal denoising — it averages across frames and will
	 * suppress the single-frame flash of a meteor */
	ret = meteor_isp_set_temper_strength(DEFAULT_TEMPER_STRENGTH);
	if (ret)
		METEOR_LOG_WARN("failed to set temporal denoise: %d", ret);

	/* Low spatial denoising — enough to tame sensor noise without
	 * smoothing away faint meteor streaks */
	ret = meteor_isp_set_sinter_strength(DEFAULT_SINTER_STRENGTH);
	if (ret)
		METEOR_LOG_WARN("failed to set spatial denoise: %d", ret);

	/* DRC — expand shadow detail to reveal faint streaks in dark frames */
	ret = meteor_isp_set_drc_strength(DEFAULT_DRC_STRENGTH);
	if (ret)
		METEOR_LOG_WARN("failed to set DRC strength: %d", ret);

	METEOR_LOG_INFO("ISP tuning applied (temper=%d, sinter=%d, drc=%d)",
			DEFAULT_TEMPER_STRENGTH, DEFAULT_SINTER_STRENGTH,
			DEFAULT_DRC_STRENGTH);
	return 0;
}

/* --- Temporal denoising --- */

int meteor_isp_set_temper_strength(uint32_t strength)
{
	int ret;

	ret = IMP_ISP_Tuning_SetTemperStrength(strength);
	if (ret) {
		METEOR_LOG_ERR("SetTemperStrength(%u) failed: %d", strength, ret);
		return ret;
	}
	METEOR_LOG_DBG("temporal denoise strength set to %u", strength);
	return 0;
}

/* --- Spatial denoising --- */

int meteor_isp_set_sinter_strength(uint32_t strength)
{
	int ret;

	ret = IMP_ISP_Tuning_SetSinterStrength(strength);
	if (ret) {
		METEOR_LOG_ERR("SetSinterStrength(%u) failed: %d", strength, ret);
		return ret;
	}
	METEOR_LOG_DBG("spatial denoise strength set to %u", strength);
	return 0;
}

/* --- Gain controls --- */

int meteor_isp_set_max_again(uint32_t gain)
{
	int ret;

	ret = IMP_ISP_Tuning_SetMaxAgain(gain);
	if (ret) {
		METEOR_LOG_ERR("SetMaxAgain(%u) failed: %d", gain, ret);
		return ret;
	}
	METEOR_LOG_DBG("max analog gain set to %u", gain);
	return 0;
}

int meteor_isp_get_max_again(uint32_t *gain)
{
	return IMP_ISP_Tuning_GetMaxAgain(gain);
}

int meteor_isp_set_max_dgain(uint32_t gain)
{
	int ret;

	ret = IMP_ISP_Tuning_SetMaxDgain(gain);
	if (ret) {
		METEOR_LOG_ERR("SetMaxDgain(%u) failed: %d", gain, ret);
		return ret;
	}
	METEOR_LOG_DBG("max digital gain set to %u", gain);
	return 0;
}

int meteor_isp_get_max_dgain(uint32_t *gain)
{
	return IMP_ISP_Tuning_GetMaxDgain(gain);
}

/* --- Sensor FPS --- */

int meteor_isp_set_sensor_fps(uint32_t fps_num, uint32_t fps_den)
{
	int ret;

	ret = IMP_ISP_Tuning_SetSensorFPS(fps_num, fps_den);
	if (ret) {
		METEOR_LOG_ERR("SetSensorFPS(%u/%u) failed: %d",
			       fps_num, fps_den, ret);
		return ret;
	}
	METEOR_LOG_DBG("sensor FPS set to %u/%u", fps_num, fps_den);
	return 0;
}

int meteor_isp_get_sensor_fps(uint32_t *fps_num, uint32_t *fps_den)
{
	return IMP_ISP_Tuning_GetSensorFPS(fps_num, fps_den);
}

/* --- Max exposure (integration time) --- */

int meteor_isp_set_max_exposure(unsigned int it_max)
{
#ifdef PLATFORM_T31
	int ret;

	ret = IMP_ISP_Tuning_SetAe_IT_MAX(it_max);
	if (ret) {
		METEOR_LOG_ERR("SetAe_IT_MAX(%u) failed: %d", it_max, ret);
		return ret;
	}
	METEOR_LOG_DBG("max integration time set to %u", it_max);
	return 0;
#else
	(void)it_max;
	METEOR_LOG_DBG("SetAe_IT_MAX not available on this platform");
	return 0;
#endif
}

int meteor_isp_get_max_exposure(unsigned int *it_max)
{
#ifdef PLATFORM_T31
	return IMP_ISP_Tuning_GetAE_IT_MAX(it_max);
#else
	(void)it_max;
	return -1;
#endif
}

/* --- AE compensation --- */

int meteor_isp_set_ae_comp(int comp)
{
	int ret;

	ret = IMP_ISP_Tuning_SetAeComp(comp);
	if (ret) {
		METEOR_LOG_ERR("SetAeComp(%d) failed: %d", comp, ret);
		return ret;
	}
	METEOR_LOG_DBG("AE compensation set to %d", comp);
	return 0;
}

int meteor_isp_get_ae_comp(int *comp)
{
	return IMP_ISP_Tuning_GetAeComp(comp);
}

/* --- DRC --- */

int meteor_isp_set_drc_strength(unsigned int strength)
{
	int ret;

#ifdef PLATFORM_T31
	ret = IMP_ISP_Tuning_SetDRC_Strength(strength);
#else
	/* T20 uses struct-based API — map scalar to manual mode */
	IMPISPDrcAttr drc;
	drc.mode = IMPISP_DRC_MANUAL;
	drc.drc_strength = (unsigned char)strength;
	drc.dval_max = 0;
	drc.dval_min = 0;
	drc.slop_max = 0;
	drc.slop_min = 0;
	drc.black_level = 0;
	drc.white_level = 0xfff;
	ret = IMP_ISP_Tuning_SetRawDRC(&drc);
#endif
	if (ret) {
		METEOR_LOG_ERR("SetDRC(%u) failed: %d", strength, ret);
		return ret;
	}
	METEOR_LOG_DBG("DRC strength set to %u", strength);
	return 0;
}

int meteor_isp_get_drc_strength(unsigned int *strength)
{
#ifdef PLATFORM_T31
	return IMP_ISP_Tuning_GetDRC_Strength(strength);
#else
	IMPISPDrcAttr drc;
	int ret;

	ret = IMP_ISP_Tuning_GetRawDRC(&drc);
	if (ret)
		return ret;
	*strength = drc.drc_strength;
	return 0;
#endif
}

/* --- Readback / diagnostics --- */

int meteor_isp_get_total_gain(uint32_t *gain)
{
	return IMP_ISP_Tuning_GetTotalGain(gain);
}

int meteor_isp_get_ae_luma(int *luma)
{
#ifdef PLATFORM_T31
	return IMP_ISP_Tuning_GetAeLuma(luma);
#else
	(void)luma;
	return -1;
#endif
}
