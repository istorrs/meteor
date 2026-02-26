#ifndef METEOR_JPEG_H
#define METEOR_JPEG_H

#include <stdint.h>

/*
 * Write a grayscale JPEG image.
 * quality: 1-100 (higher = better quality, larger file).
 * Returns 0 on success, -1 on error.
 */
int meteor_jpeg_write_gray(const char *path, const uint8_t *data,
			   int width, int height, int quality);

#endif /* METEOR_JPEG_H */
