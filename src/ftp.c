#include <meteor/ftp.h>
#include <stdlib.h>
#include <string.h>

FTPBlock *ftp_block_create(int width, int height)
{
	FTPBlock *block;

	block = malloc(sizeof(*block));
	if (!block)
		return NULL;

	block->pixels = malloc((size_t)(width * height) * sizeof(FTPPixel));
	if (!block->pixels) {
		free(block);
		return NULL;
	}

	block->width       = width;
	block->height      = height;
	block->block_index = 0;
	block->timestamp_ms = 0;
	block->frame_count = 0;

	(void)memset(block->pixels, 0,
		     (size_t)(width * height) * sizeof(FTPPixel));

	return block;
}

void ftp_block_destroy(FTPBlock *block)
{
	if (!block)
		return;
	free(block->pixels);
	free(block);
}

void ftp_block_reset(FTPBlock *block, uint64_t start_timestamp_ms)
{
	(void)memset(block->pixels, 0,
		     (size_t)(block->width * block->height) * sizeof(FTPPixel));
	block->frame_count   = 0;
	block->timestamp_ms  = start_timestamp_ms;
	block->block_index++;
}

void ftp_block_update(FTPBlock *block, const uint8_t *y_plane,
		      int stride, uint8_t frame_idx)
{
	int x, y;

	for (y = 0; y < block->height; y++) {
		const uint8_t *row = y_plane + y * stride;
		FTPPixel      *pix = block->pixels + y * block->width;

		for (x = 0; x < block->width; x++) {
			uint8_t luma = row[x];

			if (luma > pix[x].maxpixel) {
				pix[x].maxpixel = luma;
				pix[x].maxframe = frame_idx;
			}
			pix[x].sum    += luma;
			pix[x].sum_sq += (uint32_t)luma * (uint32_t)luma;
		}
	}
	block->frame_count++;
}

/* Integer square root via Newton's method. */
static uint32_t isqrt32(uint32_t n)
{
	uint32_t x, x1;

	if (n == 0)
		return 0;

	x  = n;
	x1 = (x + 1u) / 2u;
	while (x1 < x) {
		x  = x1;
		x1 = (x + n / x) / 2u;
	}
	return x;
}

void ftp_block_finalize(const FTPBlock *block,
			uint8_t *out_maxpixel,
			uint8_t *out_maxframe,
			uint8_t *out_avgpixel,
			uint8_t *out_stdpixel)
{
	int      n   = block->width * block->height;
	uint32_t fc  = (block->frame_count > 0) ?
			(uint32_t)block->frame_count : 1u;
	int      i;

	for (i = 0; i < n; i++) {
		const FTPPixel *p       = &block->pixels[i];
		uint32_t        avg     = (uint32_t)p->sum / fc;
		uint32_t        avg_sq  = p->sum_sq / fc;
		uint32_t        var, std_val;

		out_maxpixel[i] = p->maxpixel;
		out_maxframe[i] = p->maxframe;
		out_avgpixel[i] = (avg > 255u) ? 255u : (uint8_t)avg;

		/* variance = E[x²] − E[x]², clamped to 0 to avoid underflow */
		var     = (avg_sq > avg * avg) ? (avg_sq - avg * avg) : 0u;
		std_val = isqrt32(var);
		out_stdpixel[i] = (std_val > 255u) ? 255u : (uint8_t)std_val;
	}
}
