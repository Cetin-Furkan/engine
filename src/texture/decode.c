#include "texture/texture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <jpeglib.h>
#include <png.h>
#include <webp/decode.h>

enum : uint32_t {
    k_max_tex_dim = 16384u,
};

typedef struct PngMemReader {
    const uint8_t *data;
    size_t         len;
    size_t         pos;
} PngMemReader;

static void png_read_mem_cb(png_structp png_ptr, png_bytep out, png_size_t count) {
    PngMemReader *r = (PngMemReader *)png_get_io_ptr(png_ptr);
    if (r == nullptr || r->pos + count > r->len) {
        png_error(png_ptr, "unexpected EOF in PNG buffer");
        return;
    }
    memcpy(out, r->data + r->pos, count);
    r->pos += count;
}

static int decode_png_mem(const uint8_t *bytes, size_t len,
                          uint8_t **out_rgba, uint32_t *out_w, uint32_t *out_h) {
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (png == nullptr) {
        return -1;
    }
    png_infop info = png_create_info_struct(png);
    if (info == nullptr) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        return -1;
    }

    uint8_t   *volatile pixels = nullptr;
    png_bytep *volatile rows   = nullptr;
    if (setjmp(png_jmpbuf(png)) != 0) {
        free(rows);
        free(pixels);
        png_destroy_read_struct(&png, &info, nullptr);
        return -1;
    }

    PngMemReader reader = { .data = bytes, .len = len, .pos = 0u };
    png_set_read_fn(png, &reader, png_read_mem_cb);
    png_read_info(png, info);

    uint32_t w = png_get_image_width(png, info);
    uint32_t h = png_get_image_height(png, info);
    int bit_depth  = png_get_bit_depth(png, info);
    int color_type = png_get_color_type(png, info);

    if (w == 0u || h == 0u || w > k_max_tex_dim || h > k_max_tex_dim) {
        png_destroy_read_struct(&png, &info, nullptr);
        return -1;
    }

    if (bit_depth == 16) {
        png_set_strip_16(png);
    }
    if (color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(png);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
        png_set_expand_gray_1_2_4_to_8(png);
    }
    if (png_get_valid(png, info, PNG_INFO_tRNS) != 0u) {
        png_set_tRNS_to_alpha(png);
    }
    if (color_type == PNG_COLOR_TYPE_RGB ||
        color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_filler(png, 0xFFu, PNG_FILLER_AFTER);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png);
    }
    png_read_update_info(png, info);

    size_t row_bytes = (size_t)w * 4u;
    pixels = malloc(row_bytes * (size_t)h);
    rows   = malloc((size_t)h * sizeof(*rows));
    if (pixels == nullptr || rows == nullptr) {
        free(rows);
        free(pixels);
        png_destroy_read_struct(&png, &info, nullptr);
        return -1;
    }
    for (uint32_t y = 0u; y < h; ++y) {
        rows[y] = pixels + (size_t)y * row_bytes;
    }
    png_read_image(png, rows);
    png_read_end(png, nullptr);

    free(rows);
    png_destroy_read_struct(&png, &info, nullptr);

    *out_rgba = pixels;
    *out_w    = w;
    *out_h    = h;
    return 0;
}

typedef struct JpegSafeErr {
    struct jpeg_error_mgr pub;
    jmp_buf               env;
} JpegSafeErr;

static void jpeg_err_exit(j_common_ptr cinfo) {
    JpegSafeErr *err = (JpegSafeErr *)cinfo->err;
    longjmp(err->env, 1);
}

static void jpeg_output_noop(j_common_ptr cinfo) {
    (void)cinfo;
}

static int decode_jpeg_mem(const uint8_t *bytes, size_t len,
                           uint8_t **out_rgba, uint32_t *out_w, uint32_t *out_h) {
    struct jpeg_decompress_struct cinfo;
    JpegSafeErr jerr;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit     = jpeg_err_exit;
    jerr.pub.output_message = jpeg_output_noop;

    uint8_t *volatile pixels = nullptr;
    uint8_t *volatile rowbuf = nullptr;
    if (setjmp(jerr.env) != 0) {
        free(rowbuf);
        free(pixels);
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, bytes, (unsigned long)len);
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    // Request RGB output when possible (converts YCbCr & Grayscale cleanly).
    if (cinfo.jpeg_color_space != JCS_CMYK && cinfo.jpeg_color_space != JCS_YCCK) {
        cinfo.out_color_space = JCS_RGB;
    }
    jpeg_start_decompress(&cinfo);

    uint32_t w     = cinfo.output_width;
    uint32_t h     = cinfo.output_height;
    int      comps = cinfo.output_components;
    if (w == 0u || h == 0u || w > k_max_tex_dim || h > k_max_tex_dim ||
        (comps != 1 && comps != 3 && comps != 4)) {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    pixels = malloc((size_t)w * (size_t)h * 4u);
    rowbuf = malloc((size_t)w * (size_t)comps);
    if (pixels == nullptr || rowbuf == nullptr) {
        free(rowbuf);
        free(pixels);
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    while (cinfo.output_scanline < h) {
        uint32_t y = cinfo.output_scanline;
        JSAMPROW row_ptr = rowbuf;
        (void)jpeg_read_scanlines(&cinfo, &row_ptr, 1u);
        uint8_t *dst = pixels + (size_t)y * (size_t)w * 4u;
        if (comps == 3) {
            for (uint32_t x = 0u; x < w; ++x) {
                dst[x * 4u + 0u] = rowbuf[x * 3u + 0u];
                dst[x * 4u + 1u] = rowbuf[x * 3u + 1u];
                dst[x * 4u + 2u] = rowbuf[x * 3u + 2u];
                dst[x * 4u + 3u] = 255u;
            }
        } else if (comps == 1) {
            for (uint32_t x = 0u; x < w; ++x) {
                uint8_t v = rowbuf[x];
                dst[x * 4u + 0u] = v;
                dst[x * 4u + 1u] = v;
                dst[x * 4u + 2u] = v;
                dst[x * 4u + 3u] = 255u;
            }
        } else {
            // CMYK inverted Adobe/JPEG convention -> RGB
            for (uint32_t x = 0u; x < w; ++x) {
                uint32_t c = rowbuf[x * 4u + 0u];
                uint32_t m = rowbuf[x * 4u + 1u];
                uint32_t yk = rowbuf[x * 4u + 2u];
                uint32_t k = rowbuf[x * 4u + 3u];
                dst[x * 4u + 0u] = (uint8_t)((c * k) / 255u);
                dst[x * 4u + 1u] = (uint8_t)((m * k) / 255u);
                dst[x * 4u + 2u] = (uint8_t)((yk * k) / 255u);
                dst[x * 4u + 3u] = 255u;
            }
        }
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    free(rowbuf);

    *out_rgba = pixels;
    *out_w    = w;
    *out_h    = h;
    return 0;
}

static int decode_webp_mem(const uint8_t *bytes, size_t len,
                           uint8_t **out_rgba, uint32_t *out_w, uint32_t *out_h) {
    int w = 0;
    int h = 0;
    if (WebPGetInfo(bytes, len, &w, &h) == 0 ||
        w <= 0 || h <= 0 || (uint32_t)w > k_max_tex_dim || (uint32_t)h > k_max_tex_dim) {
        return -1;
    }
    size_t total = (size_t)w * (size_t)h * 4u;
    uint8_t *pixels = malloc(total);
    if (pixels == nullptr) {
        return -1;
    }
    if (WebPDecodeRGBAInto(bytes, len, pixels, total, w * 4) == nullptr) {
        free(pixels);
        return -1;
    }
    *out_rgba = pixels;
    *out_w    = (uint32_t)w;
    *out_h    = (uint32_t)h;
    return 0;
}

int texture_decode_memory(const uint8_t *bytes, size_t len,
                          uint8_t **out_rgba, uint32_t *out_w, uint32_t *out_h) {
    if (bytes == nullptr || len < 12u || out_rgba == nullptr || out_w == nullptr || out_h == nullptr) {
        return -1;
    }
    *out_rgba = nullptr;
    *out_w    = 0u;
    *out_h    = 0u;

    // PNG signature: 89 50 4E 47 0D 0A 1A 0A
    if (bytes[0] == 0x89u && bytes[1] == 0x50u && bytes[2] == 0x4Eu && bytes[3] == 0x47u &&
        bytes[4] == 0x0Du && bytes[5] == 0x0Au && bytes[6] == 0x1Au && bytes[7] == 0x0Au) {
        return decode_png_mem(bytes, len, out_rgba, out_w, out_h);
    }
    // JPEG SOI signature: FF D8 FF
    if (bytes[0] == 0xFFu && bytes[1] == 0xD8u && bytes[2] == 0xFFu) {
        return decode_jpeg_mem(bytes, len, out_rgba, out_w, out_h);
    }
    // WebP signature: "RIFF" .... "WEBP"
    if (bytes[0] == 'R' && bytes[1] == 'I' && bytes[2] == 'F' && bytes[3] == 'F' &&
        bytes[8] == 'W' && bytes[9] == 'E' && bytes[10] == 'B' && bytes[11] == 'P') {
        return decode_webp_mem(bytes, len, out_rgba, out_w, out_h);
    }
    return -1;
}

int texture_decode_file(const char *path,
                        uint8_t **out_rgba, uint32_t *out_w, uint32_t *out_h) {
    if (path == nullptr || path[0] == '\0' ||
        out_rgba == nullptr || out_w == nullptr || out_h == nullptr) {
        return -1;
    }
    FILE *fp = fopen(path, "rb");
    if (fp == nullptr) {
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    long sz = ftell(fp);
    if (sz <= 0 || sz > 256l * 1024l * 1024l || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }
    uint8_t *buf = malloc((size_t)sz);
    if (buf == nullptr) {
        fclose(fp);
        return -1;
    }
    size_t got = fread(buf, 1u, (size_t)sz, fp);
    fclose(fp);
    if (got != (size_t)sz) {
        free(buf);
        return -1;
    }
    int rc = texture_decode_memory(buf, got, out_rgba, out_w, out_h);
    free(buf);
    return rc;
}
