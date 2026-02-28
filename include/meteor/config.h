#ifndef METEOR_CONFIG_H
#define METEOR_CONFIG_H

typedef struct {
	int  sensitivity;         /* -s, 0-4, default 3 */
	int  grid_cols;           /* -g, default 8 */
	int  grid_rows;           /* -r, default 6 */
	int  cooldown_secs;       /* -c, seconds w/o motion before event ends, default 5 */
	int  capture_interval_ms; /* -f, min ms between captures, default 500 (~2fps) */
	const char *output_dir;   /* -o, default /mnt/mmcblk0p1/meteor */
	/* RMS meteor detector parameters */
	char server_ip[64];       /* flawfinder: ignore */ /* -S, N100 receiver IP, default 192.168.1.100 */
	char station_id[20];      /* flawfinder: ignore */ /* -I, RMS station ID,   default XX0001 */
	/* Storage management */
	int max_event_frames;     /* -m, max JPEGs per event, 0=unlimited, default 30 */
	int retention_days;       /* -R, delete events older than N days, 0=off, default 7 */
} meteor_config;

/* Set all fields to defaults. */
void meteor_config_defaults(meteor_config *cfg);

/*
 * Parse command-line arguments into cfg.
 * Returns 0 on success, -1 on error (prints usage to stderr).
 */
int meteor_config_parse(meteor_config *cfg, int argc, char **argv);

#endif /* METEOR_CONFIG_H */
