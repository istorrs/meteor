/*
 * ftp.h — Four-frame Temporal Pixel (FTP) accumulation.
 *
 * Accumulates per-pixel max/average/std luminance statistics across a
 * 256-frame block, equivalent to RMS FTP compression (Jenniskens et al. 2011).
 * Only the Y (luma) plane is processed; chroma is ignored.
 */
#ifndef METEOR_FTP_H
#define METEOR_FTP_H

#include <stdint.h>

/*
 * Per-pixel state for one accumulation block.
 *
 * Memory layout (8 bytes/pixel, naturally aligned):
 *   maxpixel  uint8   offset 0
 *   maxframe  uint8   offset 1
 *   sum       uint16  offset 2  (max = 256*255 = 65280, fits uint16)
 *   sum_sq    uint32  offset 4  (max = 256*255^2 = 16,646,400, fits uint32)
 */
typedef struct {
	uint8_t  maxpixel;  /* brightest luma seen across the block */
	uint8_t  maxframe;  /* frame index (0-255) at which max occurred */
	uint16_t sum;       /* sum of luma values */
	uint32_t sum_sq;    /* sum of squared luma values */
} FTPPixel;

/*
 * One 256-frame accumulation block for a given detection resolution.
 */
typedef struct {
	int       width;           /* detection width  (e.g. 640) */
	int       height;          /* detection height (e.g. 480) */
	FTPPixel *pixels;          /* flat array [height * width] */
	uint8_t   block_index;     /* rolling block counter (0-255) */
	uint64_t  timestamp_ms;    /* wall-clock ms of first frame in block */
	int       frame_count;     /* frames accumulated so far (0-256) */
} FTPBlock;

/*
 * Allocate and zero-initialise an FTPBlock for the given detection resolution.
 * Returns NULL on allocation failure.
 */
FTPBlock *ftp_block_create(int width, int height);

/* Free all memory owned by block. No-op if block is NULL. */
void ftp_block_destroy(FTPBlock *block);

/*
 * Reset block for a new 256-frame accumulation cycle.
 * start_timestamp_ms: wall-clock timestamp of the first frame.
 */
void ftp_block_reset(FTPBlock *block, uint64_t start_timestamp_ms);

/*
 * Accumulate one luma frame into the block.
 *   y_plane   : pointer to top-left of the Y plane at detection resolution
 *   stride    : row stride in bytes (>= width)
 *   frame_idx : position of this frame in the current block (0-255)
 */
void ftp_block_update(FTPBlock *block, const uint8_t *y_plane,
		      int stride, uint8_t frame_idx);

/*
 * Finalise the block: compute avgpixel and stdpixel from accumulated sums.
 * All four output arrays must be caller-allocated with (width * height) bytes.
 * Integer square root is used; no floating-point in this path.
 */
void ftp_block_finalize(const FTPBlock *block,
			uint8_t *out_maxpixel,
			uint8_t *out_maxframe,
			uint8_t *out_avgpixel,
			uint8_t *out_stdpixel);

#endif /* METEOR_FTP_H */
