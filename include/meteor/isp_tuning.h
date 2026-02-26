#ifndef METEOR_ISP_TUNING_H
#define METEOR_ISP_TUNING_H

#include <stdint.h>

/*
 * ISP tuning for meteor (astronomical) detection.
 *
 * The defaults are optimized for detecting brief, faint streaks of light
 * against a dark night sky:
 *   - Temporal denoising disabled (preserve single-frame events)
 *   - Spatial denoising low (reduce noise without smoothing faint meteors)
 *   - Gain capped (sensitivity vs. noise floor trade-off)
 *   - DRC moderate (pull faint detail from dark background)
 *   - ISP locked to night mode
 *
 * All Set functions return 0 on success, negative on error.
 */

/*
 * Apply all ISP tuning parameters for meteor detection.
 * Call once after meteor_isp_init().
 */
int meteor_isp_tuning_init(void);

/* --- Temporal denoising (3D NR) --- */

/*
 * Set temporal denoise strength.
 * Range: 0 (off) to 255 (max). 128 = firmware default.
 * For meteor detection, use 0 to preserve single-frame events.
 */
int meteor_isp_set_temper_strength(uint32_t strength);

/* --- Spatial denoising (2D NR) --- */

/*
 * Set spatial denoise strength.
 * Range: 0 (off) to 255 (max). 128 = firmware default.
 * For meteor detection, use a low value (~32-64) to reduce noise
 * without smoothing away faint streaks.
 */
int meteor_isp_set_sinter_strength(uint32_t strength);

/* --- Sensor gain limits --- */

/*
 * Set maximum analog gain.
 * Higher gain = more sensitivity to faint meteors, but more noise.
 * The value is in ISP gain units (not dB).
 */
int meteor_isp_set_max_again(uint32_t gain);
int meteor_isp_get_max_again(uint32_t *gain);

/*
 * Set maximum digital gain.
 * Digital gain amplifies noise more than analog — keep this lower.
 */
int meteor_isp_set_max_dgain(uint32_t gain);
int meteor_isp_get_max_dgain(uint32_t *gain);

/* --- Sensor frame rate --- */

/*
 * Set sensor FPS as a fraction (num/den).
 * Higher FPS = shorter per-frame exposure = better temporal resolution
 * for fast-moving meteors. 25/1 is a good starting point.
 */
int meteor_isp_set_sensor_fps(uint32_t fps_num, uint32_t fps_den);
int meteor_isp_get_sensor_fps(uint32_t *fps_num, uint32_t *fps_den);

/* --- Exposure control --- */

/*
 * Set maximum integration (exposure) time.
 * Caps how long each frame is exposed. Prevents motion blur on fast
 * meteors but reduces sensitivity to faint ones.
 * T31 only — no-op on T20.
 */
int meteor_isp_set_max_exposure(unsigned int it_max);
int meteor_isp_get_max_exposure(unsigned int *it_max);

/*
 * Set AE compensation.
 * Shifts auto-exposure target up (brighter) or down (darker).
 * Range: 0-255 (T31), 90-150 recommended (T20).
 */
int meteor_isp_set_ae_comp(int comp);
int meteor_isp_get_ae_comp(int *comp);

/* --- Dynamic range compression --- */

/*
 * Set DRC strength.
 * Expands shadow detail in dark frames, helping reveal faint meteors.
 * T31: 0-255 scalar (128 = default).
 * T20: uses preset modes internally.
 */
int meteor_isp_set_drc_strength(unsigned int strength);
int meteor_isp_get_drc_strength(unsigned int *strength);

/* --- Readback / diagnostics --- */

/*
 * Read the current total sensor gain (analog + digital combined).
 * Returns 0 in manual AE mode.
 */
int meteor_isp_get_total_gain(uint32_t *gain);

/*
 * Read the current AE luminance value.
 * T31 only — returns -1 on T20 (not available).
 */
int meteor_isp_get_ae_luma(int *luma);

#endif /* METEOR_ISP_TUNING_H */
