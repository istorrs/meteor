#include <stdio.h>

#include <jpeglib.h>

#include <meteor/jpeg.h>
#include <meteor/log.h>

int meteor_jpeg_write_gray(const char *path, const uint8_t *data,
			   int width, int height, int quality)
{
	FILE *fp;
	struct jpeg_compress_struct cinfo;
	struct jpeg_error_mgr jerr;
	JSAMPROW row_ptr[1];
	int r;

	fp = fopen(path, "wb"); /* flawfinder: ignore */
	if (!fp) {
		METEOR_LOG_ERR("cannot open %s for writing", path);
		return -1;
	}

	cinfo.err = jpeg_std_error(&jerr);
	jpeg_create_compress(&cinfo);
	jpeg_stdio_dest(&cinfo, fp);

	cinfo.image_width = (JDIMENSION)width;
	cinfo.image_height = (JDIMENSION)height;
	cinfo.input_components = 1;
	cinfo.in_color_space = JCS_GRAYSCALE;

	jpeg_set_defaults(&cinfo);
	jpeg_set_quality(&cinfo, quality, TRUE);
	jpeg_start_compress(&cinfo, TRUE);

	for (r = 0; r < height; r++) {
		row_ptr[0] = (JSAMPROW)(data + r * width);
		(void)jpeg_write_scanlines(&cinfo, row_ptr, 1);
	}

	jpeg_finish_compress(&cinfo);
	jpeg_destroy_compress(&cinfo);
	(void)fclose(fp);
	return 0;
}
