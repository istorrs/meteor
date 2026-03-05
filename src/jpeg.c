#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include <jpeglib.h>

#include <meteor/jpeg.h>
#include <meteor/log.h>

int meteor_jpeg_write_gray(const char *path, const uint8_t *data, int width,
                           int height, int quality) {
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
    row_ptr[0] = (JSAMPROW)(data + (ptrdiff_t)r * (ptrdiff_t)width);
    (void)jpeg_write_scanlines(&cinfo, row_ptr, 1);
  }
  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  (void)fclose(fp);
  return 0;
}

static inline uint8_t clamp8(int v) {
  if (v < 0)
    return 0;
  if (v > 255)
    return 255;
  return (uint8_t)v;
}

int meteor_jpeg_write_nv12(const char *path, const uint8_t *y,
                           const uint8_t *uv, int width, int height,
                           int quality) {
  FILE *fp;
  struct jpeg_compress_struct cinfo;
  struct jpeg_error_mgr jerr;
  uint8_t *row;
  JSAMPROW row_ptr[1];
  int x, r, ret = 0;

  fp = fopen(path, "wb"); /* flawfinder: ignore */
  if (!fp) {
    METEOR_LOG_ERR("cannot open %s for writing", path);
    return -1;
  }

  row = malloc((size_t)width * 3);
  if (!row) {
    METEOR_LOG_ERR("cannot allocate row buffer");
    (void)fclose(fp);
    return -1;
  }

  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_compress(&cinfo);
  jpeg_stdio_dest(&cinfo, fp);

  cinfo.image_width = (JDIMENSION)width;
  cinfo.image_height = (JDIMENSION)height;
  cinfo.input_components = 3;
  cinfo.in_color_space = JCS_RGB;

  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, quality, TRUE);
  jpeg_start_compress(&cinfo, TRUE);

  row_ptr[0] = row;

  for (r = 0; r < height; r++) {
    for (x = 0; x < width; x++) {
      int yi = r * width + x;
      /* NV12: UV plane is half-res, interleaved U,V */
      int uvi = (r / 2) * width + (x & ~1);
      int yv = y[yi];
      int u = uv[uvi] - 128;
      int v = uv[uvi + 1] - 128;

      row[x * 3] = clamp8(yv + ((v * 1436) >> 10));
      row[x * 3 + 1] = clamp8(yv - ((u * 352 + v * 731) >> 10));
      row[x * 3 + 2] = clamp8(yv + ((u * 1815) >> 10));
    }

    (void)jpeg_write_scanlines(&cinfo, row_ptr, 1);
  }

  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  (void)fclose(fp);
  free(row);
  return ret;
}
