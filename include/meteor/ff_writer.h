/*
 * ff_writer.h — Write RMS-compatible FF binary files (version 2 format).
 *
 * The N100 server can feed these files directly to unmodified RMS software
 * (RMS.DetectStarsAndMeteors) for astrometric calibration and GMN upload.
 *
 * Binary layout (little-endian, all fields packed):
 *   int32_t  : -1          (version marker)
 *   uint32_t : nrows       (frame height)
 *   uint32_t : ncols       (frame width)
 *   uint32_t : nframes     (always 256)
 *   uint32_t : first       (first frame number, 0)
 *   uint32_t : camno       (numeric camera identifier)
 *   uint32_t : decimation  (1)
 *   uint32_t : interleave  (0)
 *   uint32_t : fps_milli   (fps * 1000)
 *   uint8_t[nrows*ncols] : maxpixel
 *   uint8_t[nrows*ncols] : maxframe
 *   uint8_t[nrows*ncols] : avepixel
 *   uint8_t[nrows*ncols] : stdpixel
 *
 * Filename convention: FF_<stationid>_<YYYYMMDD>_<HHMMSS>_<mmm>_000000.bin
 */
#ifndef METEOR_FF_WRITER_H
#define METEOR_FF_WRITER_H

#include <stddef.h>
#include <stdint.h>

/* Metadata for one FF file. */
typedef struct {
	char     station_id[20]; /* flawfinder: ignore */ /* e.g. "XX0001" */
	uint16_t year;
	uint8_t  month;
	uint8_t  day;
	uint8_t  hour;
	uint8_t  minute;
	uint8_t  second;
	uint16_t millisecond;
	uint16_t width;
	uint16_t height;
	uint16_t nframes;        /* always 256 */
	float    fps;
	uint32_t camno;          /* numeric camera id */
} FFHeader;

/*
 * Generate the canonical RMS filename into buf (must be >= 64 bytes).
 * Format: FF_<stationid>_<YYYYMMDD>_<HHMMSS>_<mmm>_000000.bin
 */
void ff_make_filename(char *buf, size_t bufsz, const FFHeader *hdr);

/*
 * Write an FF binary file to path.
 * Returns 0 on success, -1 on error.
 * maxpixel/maxframe/avgpixel/stdpixel: flat uint8 arrays [height * width].
 */
int ff_write(const char *path, const FFHeader *hdr,
	     const uint8_t *maxpixel, const uint8_t *maxframe,
	     const uint8_t *avgpixel, const uint8_t *stdpixel);

#endif /* METEOR_FF_WRITER_H */
