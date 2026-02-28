/*
 * meteor_config.h — compile-time tuning parameters for the FTP/Hough
 * meteor detection pipeline.  Adjust these constants and recompile;
 * no source-code changes elsewhere are needed.
 */
#ifndef METEOR_DETECT_CONFIG_H
#define METEOR_DETECT_CONFIG_H

/* Detection resolution — Y-plane is downsampled to this before FTP */
#define DETECT_WIDTH           640
#define DETECT_HEIGHT          480

/* FTP accumulation */
#define FTP_BLOCK_FRAMES       256      /* frames per detection block (RMS standard) */
#define FTP_FPS                25.0f   /* expected camera frame rate */

/* Threshold: pixel is a candidate if (maxpixel - avgpixel) > K * stdpixel
 * K=3 is RMS default for low-noise sensors; raise to 5-6 for high-gain
 * embedded cameras where dark-frame noise is significant. */
#define METEOR_FTP_K           5

/* Hough transform */
#define HOUGH_THETA_STEPS      180     /* angular resolution (1 degree per step) */
/* rho range: [-HOUGH_RHO_MAX, +HOUGH_RHO_MAX]
 * Must be >= ceil(sqrt(DETECT_WIDTH^2 + DETECT_HEIGHT^2)) = 800; use 900 for margin */
#define HOUGH_RHO_MAX          900
#define HOUGH_PEAK_THRESHOLD   8       /* min votes to extract a Hough peak */

/* Meteor candidate filters */
#define METEOR_MIN_LENGTH_PX   15      /* minimum streak length in pixels */
#define METEOR_MIN_VOTES       10      /* minimum Hough votes for a valid line */
#define METEOR_MIN_CANDIDATES  5       /* min threshold pixels before running Hough */

/* Network — N100 receiver */
#define DETECTOR_SERVER_PORT      8765
#define DETECTOR_HTTP_TIMEOUT_MS  5000

/* FF metadata defaults (overridden at runtime via config/CLI) */
#define DETECTOR_DEFAULT_STATION_ID  "XX0001"
#define DETECTOR_DEFAULT_SERVER_IP   "192.168.1.245"

/* Temporary directory on SD card for FF file staging */
#define DETECTOR_FF_TMP_DIR    "/mnt/mmcblk0p1/meteor_ff_tmp"

#endif /* METEOR_DETECT_CONFIG_H */
