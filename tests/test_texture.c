#include "texture/texture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <jpeglib.h>
#include <png.h>
#include <webp/encode.h>

static int s_failures = 0;

#define CHECK(cond, msg)                                                           \
    do {                                                                           \
        if (!(cond)) {                                                             \
            fprintf(stderr, "  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, (msg));    \
            s_failures++;                                                          \
        }                                                                          \
    } while (0)

typedef struct MemBuf {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} MemBuf;

static void png_mem_write(png_structp png, png_bytep bytes, png_size_t count) {
    MemBuf *mb = (MemBuf *)png_get_io_ptr(png);
    if (mb->len + count > mb->cap) {
        size_t ncap = (mb->cap == 0u ? 1024u : mb->cap * 2u);
        while (ncap < mb->len + count) ncap *= 2u;
        uint8_t *nb = realloc(mb->data, ncap);
        if (nb == nullptr) {
            png_error(png, "oom");
            return;
        }
        mb->data = nb;
        mb->cap  = ncap;
    }
    memcpy(mb->data + mb->len, bytes, count);
    mb->len += count;
}

static void png_mem_flush(png_structp png) {
    (void)png;
}

static void test_sampler_and_material_abi(void) {
    CHECK(sizeof(PbrMaterialGpu) == 128u, "PbrMaterialGpu == 128 bytes");
    bool seen[K_TEX_SAMPLER_COUNT] = {0};
    for (uint32_t ws = 0u; ws < 3u; ++ws) {
        for (uint32_t wt = 0u; wt < 3u; ++wt) {
            for (uint32_t fl = 0u; fl < 2u; ++fl) {
                uint32_t idx = texture_sampler_index((TextureWrap)ws, (TextureWrap)wt, (TextureFilter)fl);
                CHECK(idx < K_TEX_SAMPLER_COUNT, "sampler index in range");
                CHECK(!seen[idx], "sampler index bijective");
                seen[idx] = true;
            }
        }
    }
    PbrMaterialGpu def = texture_pbr_material_default();
    CHECK(def.tex_base == K_TEX_WHITE_SRGB, "default base tex");
    CHECK(def.tex_mr == K_TEX_WHITE_LINEAR, "default mr tex");
    CHECK(def.tex_normal == K_TEX_FLAT_NORMAL, "default flat normal tex");
    CHECK(def.ior == 1.5f, "default dielectric IOR 1.5");
}

static void test_png_jpeg_webp_decoders(void) {
    const uint32_t w = 4u, h = 4u;
    uint8_t src_rgba[4u * 4u * 4u];
    uint8_t src_rgb[4u * 4u * 3u];
    for (uint32_t i = 0u; i < w * h; ++i) {
        src_rgba[i * 4u + 0u] = 220u;
        src_rgba[i * 4u + 1u] = 80u;
        src_rgba[i * 4u + 2u] = 40u;
        src_rgba[i * 4u + 3u] = 180u;
        src_rgb[i * 3u + 0u]  = 220u;
        src_rgb[i * 3u + 1u]  = 80u;
        src_rgb[i * 3u + 2u]  = 40u;
    }

    // 1. PNG round-trip (memory + file)
    MemBuf png_buf = {0};
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop   inf = png_create_info_struct(png);
    CHECK(png != nullptr && inf != nullptr, "libpng write structs");
    if (png != nullptr && inf != nullptr) {
        png_set_write_fn(png, &png_buf, png_mem_write, png_mem_flush);
        png_set_IHDR(png, inf, w, h, 8, PNG_COLOR_TYPE_RGBA,
                     PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
        png_write_info(png, inf);
        png_bytep rows[4] = {
            src_rgba + 0u * 16u, src_rgba + 1u * 16u,
            src_rgba + 2u * 16u, src_rgba + 3u * 16u,
        };
        png_write_image(png, rows);
        png_write_end(png, nullptr);
        png_destroy_write_struct(&png, &inf);

        uint8_t *dec = nullptr;
        uint32_t dw = 0u, dh = 0u;
        CHECK(texture_decode_memory(png_buf.data, png_buf.len, &dec, &dw, &dh) == 0, "decode PNG memory");
        CHECK(dw == w && dh == h && dec != nullptr, "PNG dimensions match");
        if (dec != nullptr) {
            CHECK(dec[0] == 220u && dec[1] == 80u && dec[2] == 40u && dec[3] == 180u, "PNG exact RGBA");
            free(dec);
        }

        const char *tmp_png = "/tmp/engine_v2_test_tex.png";
        FILE *fp = fopen(tmp_png, "wb");
        if (fp != nullptr) {
            fwrite(png_buf.data, 1u, png_buf.len, fp);
            fclose(fp);
            dec = nullptr;
            CHECK(texture_decode_file(tmp_png, &dec, &dw, &dh) == 0, "decode PNG file");
            CHECK(dw == w && dh == h && dec != nullptr && dec[3] == 180u, "PNG file exact alpha");
            free(dec);
            unlink(tmp_png);
        }
    }
    free(png_buf.data);

    // 2. JPEG round-trip (memory)
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    unsigned char *jpg_data = nullptr;
    unsigned long  jpg_len  = 0ul;
    jpeg_mem_dest(&cinfo, &jpg_data, &jpg_len);
    cinfo.image_width      = w;
    cinfo.image_height     = h;
    cinfo.input_components = 3;
    cinfo.in_color_space   = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, 95, TRUE);
    jpeg_start_compress(&cinfo, TRUE);
    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW rp = src_rgb + cinfo.next_scanline * w * 3u;
        jpeg_write_scanlines(&cinfo, &rp, 1u);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);

    uint8_t *jdec = nullptr;
    uint32_t jw = 0u, jh = 0u;
    CHECK(texture_decode_memory(jpg_data, (size_t)jpg_len, &jdec, &jw, &jh) == 0, "decode JPEG memory");
    CHECK(jw == w && jh == h && jdec != nullptr, "JPEG dimensions match");
    if (jdec != nullptr) {
        CHECK(abs((int)jdec[0] - 220) <= 10 && abs((int)jdec[1] - 80) <= 10 && jdec[3] == 255u,
              "JPEG RGB accuracy + opaque alpha");
        free(jdec);
    }
    free(jpg_data);

    // 3. WebP lossless round-trip (memory)
    uint8_t *webp_data = nullptr;
    size_t   webp_len  = WebPEncodeLosslessRGBA(src_rgba, (int)w, (int)h, (int)(w * 4u), &webp_data);
    CHECK(webp_len > 12u && webp_data != nullptr, "encode WebP lossless");
    if (webp_data != nullptr) {
        uint8_t *wdec = nullptr;
        uint32_t ww = 0u, wh = 0u;
        CHECK(texture_decode_memory(webp_data, webp_len, &wdec, &ww, &wh) == 0, "decode WebP memory");
        CHECK(ww == w && wh == h && wdec != nullptr, "WebP dimensions match");
        if (wdec != nullptr) {
            CHECK(wdec[0] == 220u && wdec[1] == 80u && wdec[2] == 40u && wdec[3] == 180u, "WebP exact RGBA");
            free(wdec);
        }
        WebPFree(webp_data);
    }

    // 4. Corrupt / truncated payload rejection
    const uint8_t garbage[16] = { 0xDE, 0xAD, 0xBE, 0xEF, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
    uint8_t *bad = nullptr;
    uint32_t bw = 0u, bh = 0u;
    CHECK(texture_decode_memory(garbage, sizeof garbage, &bad, &bw, &bh) != 0 && bad == nullptr,
          "reject invalid image magic");
}

int main(void) {
    printf("[Test] Running Vulkan 1.4 Bindless PBR Texture & Multi-Codec Decoder Suite...\n");
    test_sampler_and_material_abi();
    test_png_jpeg_webp_decoders();
    if (s_failures != 0) {
        fprintf(stderr, "[FAIL] test_texture: %d check(s) failed\n", s_failures);
        return 1;
    }
    printf("[PASS] test_texture: all PNG/JPEG/WebP, sampler, and PBR material ABI checks passed!\n");
    return 0;
}
