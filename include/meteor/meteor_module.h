/*
 * meteor_module.h — integration entry point for the FTP meteor detector.
 *
 * Runs a dedicated POSIX thread that continuously pulls raw NV12 frames from
 * the IMP FrameSource channel, downsamples the Y plane to detection resolution,
 * and feeds each frame to the DetectorState pipeline.
 *
 * The existing IVS-based motion detection in main.c is unaffected.
 */
#ifndef METEOR_MODULE_H
#define METEOR_MODULE_H

#include <meteor/config.h>

/*
 * Initialise and start the meteor detection module.
 *   fs_chn  : FrameSource channel to pull frames from (must already be enabled)
 *   cfg     : runtime configuration (server_ip, station_id, output_dir)
 * Returns 0 on success, -1 on failure.
 */
int meteor_module_init(int fs_chn, const meteor_config *cfg);

/*
 * Stop the frame-grabbing thread and release all resources.
 * Safe to call even if meteor_module_init() was never called.
 */
void meteor_module_deinit(void);

#endif /* METEOR_MODULE_H */
