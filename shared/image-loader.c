/*
 * Copyright © 2008-2012 Kristian Høgsberg
 * Copyright © 2012 Intel Corporation
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#include "../config.h"

#include <stdlib.h>
#include <stdio.h>
#include <jpeglib.h>
#include <png.h>
#include <pixman.h>

#include "config-parser.h"

#define ARRAY_LENGTH(a) (sizeof (a) / sizeof (a)[0])

#ifdef HAVE_WEBP
#include <webp/decode.h>
#endif

static int
stride_for_width(int width)
{
	return width * 4;
}

static void
swizzle_row(JSAMPLE *row, JDIMENSION width)
{
	JSAMPLE *s;
	uint32_t *d;

	s = row + (width - 1) * 3;
	d = (uint32_t *) (row + (width - 1) * 4);
	while (s >= row) {
		*d = 0xff000000 | (s[0] << 16) | (s[1] << 8) | (s[2] << 0);
		s -= 3;
		d--;
	}
}

static void
error_exit(j_common_ptr cinfo)
{
	longjmp(cinfo->client_data, 1);
}

static pixman_image_t *
load_jpeg(FILE *fp)
{
	struct jpeg_decompress_struct cinfo;
	struct jpeg_error_mgr jerr;
	unsigned int i;
	int stride, first;
	JSAMPLE *data, *rows[4];
	jmp_buf env;

	cinfo.err = jpeg_std_error(&jerr);
	jerr.error_exit = error_exit;
	cinfo.client_data = env;
	if (setjmp(env))
		return NULL;

	jpeg_create_decompress(&cinfo);

	jpeg_stdio_src(&cinfo, fp);

	jpeg_read_header(&cinfo, TRUE);

	cinfo.out_color_space = JCS_RGB;
	jpeg_start_decompress(&cinfo);

	stride = cinfo.output_width * 4;
	data = malloc(stride * cinfo.output_height);
	if (data == NULL) {
		fprintf(stderr, "couldn't allocate image data\n");
		return NULL;
	}

	while (cinfo.output_scanline < cinfo.output_height) {
		first = cinfo.output_scanline;
		for (i = 0; i < ARRAY_LENGTH(rows); i++)
			rows[i] = data + (first + i) * stride;

		jpeg_read_scanlines(&cinfo, rows, ARRAY_LENGTH(rows));
		for (i = 0; first + i < cinfo.output_scanline; i++)
			swizzle_row(rows[i], cinfo.output_width);
	}

	jpeg_finish_decompress(&cinfo);

	jpeg_destroy_decompress(&cinfo);

	return pixman_image_create_bits(PIXMAN_a8r8g8b8,
					cinfo.output_width,
					cinfo.output_height,
					(uint32_t *) data, stride);
}

static inline int
multiply_alpha(int alpha, int color)
{
    int temp = (alpha * color) + 0x80;

    return ((temp + (temp >> 8)) >> 8);
}

static void
premultiply_data(png_structp   png,
		 png_row_infop row_info,
		 png_bytep     data)
{
    unsigned int i;
    png_bytep p;

    for (i = 0, p = data; i < row_info->rowbytes; i += 4, p += 4) {
	png_byte  alpha = p[3];
	uint32_t w;

	if (alpha == 0) {
		w = 0;
	} else {
		png_byte red   = p[0];
		png_byte green = p[1];
		png_byte blue  = p[2];

		if (alpha != 0xff) {
			red   = multiply_alpha(alpha, red);
			green = multiply_alpha(alpha, green);
			blue  = multiply_alpha(alpha, blue);
		}
		w = (alpha << 24) | (red << 16) | (green << 8) | (blue << 0);
	}

	* (uint32_t *) p = w;
    }
}

static void
read_func(png_structp png, png_bytep data, png_size_t size)
{
	FILE *fp = png_get_io_ptr(png);

	if (fread(data, 1, size, fp) != size)
		png_error(png, NULL);
}

static void
png_error_callback(png_structp png, png_const_charp error_msg)
{
    longjmp (png_jmpbuf (png), 1);
}

static pixman_image_t *
load_png(FILE *fp)
{
	png_struct *png;
	png_info *info;
	png_byte *data = NULL;
	png_byte **row_pointers = NULL;
	png_uint_32 width, height;
	int depth, color_type, interlace, stride;
	unsigned int i;

	png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL,
				     png_error_callback, NULL);
	if (!png)
		return NULL;

	info = png_create_info_struct(png);
	if (!info) {
		png_destroy_read_struct(&png, &info, NULL);
		return NULL;
	}

	if (setjmp(png_jmpbuf(png))) {
		if (data)
			free(data);
		if (row_pointers)
			free(row_pointers);
		png_destroy_read_struct(&png, &info, NULL);
		return NULL;
	}

	png_set_read_fn(png, fp, read_func);
	png_read_info(png, info);
	png_get_IHDR(png, info,
		     &width, &height, &depth,
		     &color_type, &interlace, NULL, NULL);

	if (color_type == PNG_COLOR_TYPE_PALETTE)
		png_set_palette_to_rgb(png);

	if (color_type == PNG_COLOR_TYPE_GRAY)
		png_set_expand_gray_1_2_4_to_8(png);

	if (png_get_valid(png, info, PNG_INFO_tRNS))
		png_set_tRNS_to_alpha(png);

	if (depth == 16)
		png_set_strip_16(png);

	if (depth < 8)
		png_set_packing(png);

	if (color_type == PNG_COLOR_TYPE_GRAY ||
	    color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
		png_set_gray_to_rgb(png);

	if (interlace != PNG_INTERLACE_NONE)
		png_set_interlace_handling(png);

	png_set_filler(png, 0xff, PNG_FILLER_AFTER);
	png_set_read_user_transform_fn(png, premultiply_data);
	png_read_update_info(png, info);
	png_get_IHDR(png, info,
		     &width, &height, &depth,
		     &color_type, &interlace, NULL, NULL);


	stride = stride_for_width(width);
	data = malloc(stride * height);
	if (!data) {
		png_destroy_read_struct(&png, &info, NULL);
		return NULL;
	}

	row_pointers = malloc(height * sizeof row_pointers[0]);
	if (row_pointers == NULL) {
		free(data);
		png_destroy_read_struct(&png, &info, NULL);
		return NULL;
	}

	for (i = 0; i < height; i++)
		row_pointers[i] = &data[i * stride];

	png_read_image(png, row_pointers);
	png_read_end(png, info);

	free(row_pointers);
	png_destroy_read_struct(&png, &info, NULL);

	return pixman_image_create_bits(PIXMAN_a8r8g8b8, width, height,
					(uint32_t *) data, stride);
}

#ifdef HAVE_WEBP

static pixman_image_t *
load_webp(FILE *fp)
{
	WebPDecoderConfig config;
	uint8_t buffer[16 * 1024];
	int len;
	VP8StatusCode status;
	WebPIDecoder *idec;

	if (!WebPInitDecoderConfig(&config)) {
		fprintf(stderr, "Library version mismatch!\n");
		return NULL;
	}

	/* webp decoding api doesn't seem to specify a min size that's
	   usable for GetFeatures, but 256 works... */
	len = fread(buffer, 1, 256, fp);
	status = WebPGetFeatures(buffer, len, &config.input);
	if (status != VP8_STATUS_OK) {
		fprintf(stderr, "failed to parse webp header\n");
		WebPFreeDecBuffer(&config.output);
		return NULL;
	}

	config.output.colorspace = MODE_BGRA;
	config.output.u.RGBA.stride = stride_for_width(config.input.width);
	config.output.u.RGBA.size =
		config.output.u.RGBA.stride * config.input.height;
	config.output.u.RGBA.rgba =
		malloc(config.output.u.RGBA.stride * config.input.height);
	config.output.is_external_memory = 1;
	if (!config.output.u.RGBA.rgba) {
		WebPFreeDecBuffer(&config.output);
		return NULL;
	}

	rewind(fp);
	idec = WebPINewDecoder(&config.output);
	if (!idec) {
		WebPFreeDecBuffer(&config.output);
		return NULL;
	}

	while (!feof(fp)) {
		len = fread(buffer, 1, sizeof buffer, fp);
		status = WebPIAppend(idec, buffer, len);
		if (status != VP8_STATUS_OK) {
			fprintf(stderr, "webp decode status %d\n", status);
			WebPIDelete(idec);
			WebPFreeDecBuffer(&config.output);
			return NULL;
		}
	}

	WebPIDelete(idec);
	WebPFreeDecBuffer(&config.output);

	return pixman_image_create_bits(PIXMAN_a8r8g8b8,
					config.input.width,
					config.input.height,
					(uint32_t *) config.output.u.RGBA.rgba,
					config.output.u.RGBA.stride);
}

#endif


struct image_loader {
	unsigned char header[4];
	int header_size;
	pixman_image_t *(*load)(FILE *fp);
};

static const struct image_loader loaders[] = {
	{ { 0x89, 'P', 'N', 'G' }, 4, load_png },
	{ { 0xff, 0xd8 }, 2, load_jpeg },
#ifdef HAVE_WEBP
	{ { 'R', 'I', 'F', 'F' }, 4, load_webp }
#endif
};

pixman_image_t *
load_image(const char *filename)
{
	pixman_image_t *image;
	unsigned char header[4];
	FILE *fp;
	unsigned int i;

	fp = fopen(filename, "rb");
	if (fp == NULL)
		return NULL;

	if (fread(header, sizeof header, 1, fp) != 1)
		return NULL;

	rewind(fp);
	for (i = 0; i < ARRAY_LENGTH(loaders); i++) {
		if (memcmp(header, loaders[i].header,
			   loaders[i].header_size) == 0) {
			image = loaders[i].load(fp);
			break;
		}
	}

	fclose(fp);

	if (i == ARRAY_LENGTH(loaders)) {
		fprintf(stderr, "unrecognized file header for %s: "
			"0x%02x 0x%02x 0x%02x 0x%02x\n",
			filename, header[0], header[1], header[2], header[3]);
		image = NULL;
	}

	return image;
}
