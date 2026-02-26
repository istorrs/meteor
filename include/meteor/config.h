#ifndef METEOR_CONFIG_H
#define METEOR_CONFIG_H

typedef struct {
	int sensitivity;         /* -s, 0-4, default 3 */
	int grid_cols;           /* -g, default 8 */
	int grid_rows;           /* -r, default 6 */
	int cooldown_secs;       /* -c, seconds w/o motion before event ends, default 5 */
	int capture_interval_ms; /* -f, min ms between captures, default 500 (~2fps) */
	const char *output_dir;  /* -o, default /mnt/mmcblk0p1/meteor */
} meteor_config;

/* Set all fields to defaults. */
void meteor_config_defaults(meteor_config *cfg);

/*
 * Parse command-line arguments into cfg.
 * Returns 0 on success, -1 on error (prints usage to stderr).
 */
int meteor_config_parse(meteor_config *cfg, int argc, char **argv);

#endif /* METEOR_CONFIG_H */
