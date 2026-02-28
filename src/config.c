#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <meteor/config.h>
#include <meteor/meteor_config.h>

#define DEFAULT_SENSITIVITY       3
#define DEFAULT_GRID_COLS         8
#define DEFAULT_GRID_ROWS         6
#define DEFAULT_COOLDOWN_SECS     5
#define DEFAULT_CAPTURE_INTERVAL  500
#define DEFAULT_OUTPUT_DIR        "/mnt/mmcblk0p1/meteor"
#define DEFAULT_MAX_EVENT_FRAMES  30
#define DEFAULT_RETENTION_DAYS    7

#define MAX_ROI_COUNT             52

void meteor_config_defaults(meteor_config *cfg)
{
	cfg->sensitivity = DEFAULT_SENSITIVITY;
	cfg->grid_cols = DEFAULT_GRID_COLS;
	cfg->grid_rows = DEFAULT_GRID_ROWS;
	cfg->cooldown_secs = DEFAULT_COOLDOWN_SECS;
	cfg->capture_interval_ms = DEFAULT_CAPTURE_INTERVAL;
	cfg->output_dir = DEFAULT_OUTPUT_DIR;
	(void)snprintf(cfg->server_ip,   sizeof(cfg->server_ip),
		       "%s", DETECTOR_DEFAULT_SERVER_IP);
	(void)snprintf(cfg->station_id,  sizeof(cfg->station_id),
		       "%s", DETECTOR_DEFAULT_STATION_ID);
	cfg->max_event_frames = DEFAULT_MAX_EVENT_FRAMES;
	cfg->retention_days   = DEFAULT_RETENTION_DAYS;
}

static void usage(const char *prog)
{
	(void)fprintf(stderr,
		"Usage: %s [options]\n"
		"  -s SENSE  Motion sensitivity 0-4 (default: %d)\n"
		"  -g COLS   Grid columns (default: %d)\n"
		"  -r ROWS   Grid rows (default: %d)\n"
		"  -c SECS   Cooldown seconds before event ends (default: %d)\n"
		"  -f MS     Min ms between frame captures (default: %d)\n"
		"  -o DIR    Output directory (default: %s)\n"
		"  -S IP     N100 receiver IP for RMS detector (default: %s)\n"
		"  -I ID     RMS station ID, e.g. XX0001 (default: %s)\n"
		"  -m N      Max JPEG frames per event, 0=unlimited (default: %d)\n"
		"  -R DAYS   Delete events older than DAYS days, 0=off (default: %d)\n"
		"  -h        Show this help\n",
		prog, DEFAULT_SENSITIVITY, DEFAULT_GRID_COLS,
		DEFAULT_GRID_ROWS, DEFAULT_COOLDOWN_SECS,
		DEFAULT_CAPTURE_INTERVAL, DEFAULT_OUTPUT_DIR,
		DETECTOR_DEFAULT_SERVER_IP, DETECTOR_DEFAULT_STATION_ID,
		DEFAULT_MAX_EVENT_FRAMES, DEFAULT_RETENTION_DAYS);
}

int meteor_config_parse(meteor_config *cfg, int argc, char **argv)
{
	int opt;

	meteor_config_defaults(cfg);

	while ((opt = getopt(argc, argv, "s:g:r:c:f:o:S:I:m:R:h")) != -1) { /* flawfinder: ignore */
		switch (opt) {
		case 's':
			cfg->sensitivity = (int)strtol(optarg, NULL, 10);
			if (cfg->sensitivity < 0 || cfg->sensitivity > 4) {
				(void)fprintf(stderr,
					      "sensitivity must be 0-4\n");
				return -1;
			}
			break;
		case 'g':
			cfg->grid_cols = (int)strtol(optarg, NULL, 10);
			if (cfg->grid_cols < 1) {
				(void)fprintf(stderr,
					      "grid columns must be >= 1\n");
				return -1;
			}
			break;
		case 'r':
			cfg->grid_rows = (int)strtol(optarg, NULL, 10);
			if (cfg->grid_rows < 1) {
				(void)fprintf(stderr,
					      "grid rows must be >= 1\n");
				return -1;
			}
			break;
		case 'c':
			cfg->cooldown_secs = (int)strtol(optarg, NULL, 10);
			if (cfg->cooldown_secs < 1) {
				(void)fprintf(stderr,
					      "cooldown must be >= 1\n");
				return -1;
			}
			break;
		case 'f':
			cfg->capture_interval_ms = (int)strtol(optarg, NULL, 10);
			if (cfg->capture_interval_ms < 0) {
				(void)fprintf(stderr,
					      "capture interval must be >= 0\n");
				return -1;
			}
			break;
		case 'o':
			cfg->output_dir = optarg;
			break;
		case 'S':
			(void)snprintf(cfg->server_ip, sizeof(cfg->server_ip),
				       "%s", optarg);
			break;
		case 'I':
			(void)snprintf(cfg->station_id, sizeof(cfg->station_id),
				       "%s", optarg);
			break;
		case 'm':
			cfg->max_event_frames = (int)strtol(optarg, NULL, 10);
			if (cfg->max_event_frames < 0) {
				(void)fprintf(stderr,
					      "max event frames must be >= 0\n");
				return -1;
			}
			break;
		case 'R':
			cfg->retention_days = (int)strtol(optarg, NULL, 10);
			if (cfg->retention_days < 0) {
				(void)fprintf(stderr,
					      "retention days must be >= 0\n");
				return -1;
			}
			break;
		case 'h': /* fall through */
		default:
			usage(argv[0]);
			return -1;
		}
	}

	if (cfg->grid_cols * cfg->grid_rows > MAX_ROI_COUNT) {
		(void)fprintf(stderr,
			      "grid_cols * grid_rows (%d) exceeds max ROI count (%d)\n",
			      cfg->grid_cols * cfg->grid_rows, MAX_ROI_COUNT);
		return -1;
	}

	return 0;
}
