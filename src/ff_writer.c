#include <meteor/ff_writer.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

void ff_make_filename(char *buf, size_t bufsz, const FFHeader *hdr)
{
	(void)snprintf(buf, bufsz,
		"FF_%s_%04u%02u%02u_%02u%02u%02u_%03u_000000.bin",
		hdr->station_id,
		(unsigned)hdr->year, (unsigned)hdr->month, (unsigned)hdr->day,
		(unsigned)hdr->hour, (unsigned)hdr->minute, (unsigned)hdr->second,
		(unsigned)hdr->millisecond);
}

/*
 * Write a single little-endian uint32 to a file.
 * Returns 0 on success, -1 on error.
 */
static int write_u32(FILE *f, uint32_t v)
{
	uint8_t buf[4];

	buf[0] = (uint8_t)(v & 0xFFu);
	buf[1] = (uint8_t)((v >>  8) & 0xFFu);
	buf[2] = (uint8_t)((v >> 16) & 0xFFu);
	buf[3] = (uint8_t)((v >> 24) & 0xFFu);
	return (fwrite(buf, 1, 4, f) == 4) ? 0 : -1;
}

int ff_write(const char *path, const FFHeader *hdr,
	     const uint8_t *maxpixel, const uint8_t *maxframe,
	     const uint8_t *avgpixel, const uint8_t *stdpixel)
{
	FILE    *f;
	size_t   plane_sz = (size_t)hdr->width * (size_t)hdr->height;
	uint32_t fps_milli = (uint32_t)(hdr->fps * 1000.0f);
	int      rc = 0;

	f = fopen(path, "wb"); /* flawfinder: ignore */
	if (!f)
		return -1;

	/* Version marker: int32 = -1, stored as two's complement uint32 */
	if (write_u32(f, (uint32_t)(-1)) != 0)  { rc = -1; goto done; }
	if (write_u32(f, (uint32_t)hdr->height)  != 0)  { rc = -1; goto done; }
	if (write_u32(f, (uint32_t)hdr->width)   != 0)  { rc = -1; goto done; }
	if (write_u32(f, (uint32_t)hdr->nframes) != 0)  { rc = -1; goto done; }
	if (write_u32(f, 0u)                     != 0)  { rc = -1; goto done; } /* first frame */
	if (write_u32(f, hdr->camno)             != 0)  { rc = -1; goto done; }
	if (write_u32(f, 1u)                     != 0)  { rc = -1; goto done; } /* decimation */
	if (write_u32(f, 0u)                     != 0)  { rc = -1; goto done; } /* interleave */
	if (write_u32(f, fps_milli)              != 0)  { rc = -1; goto done; }

	/* Four image planes */
	if (fwrite(maxpixel, 1, plane_sz, f) != plane_sz) { rc = -1; goto done; }
	if (fwrite(maxframe,  1, plane_sz, f) != plane_sz) { rc = -1; goto done; }
	if (fwrite(avgpixel,  1, plane_sz, f) != plane_sz) { rc = -1; goto done; }
	if (fwrite(stdpixel,  1, plane_sz, f) != plane_sz) { rc = -1; goto done; }

done:
	if (fclose(f) != 0)
		rc = -1;
	return rc;
}
