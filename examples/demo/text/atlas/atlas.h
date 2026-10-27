#ifndef ENGINE_TEXT_ATLAS_H
#define ENGINE_TEXT_ATLAS_H

#include "vulkan/types.h"
#include <stdbool.h>
#include <stdint.h>

// One append-only R8 atlas in a host-visible device-address buffer.
// Published texels are never rewritten, so a frame already in flight
// keeps sampling the bytes it was submitted with.

enum : uint32_t { TEXT_ATLAS_DIM = 2048 };

typedef struct TextAtlasRect {
    uint32_t x;
    uint32_t y;
    uint32_t w;
    uint32_t h;
} TextAtlasRect;

[[nodiscard]] int text_atlas_init(VulkanContext *vk);
void text_atlas_shutdown(VulkanContext *vk);

// Copies one SDF bitmap. A repeated (face, epoch, glyph) returns the
// rect already published. An empty bitmap is a zero rect and success.
[[nodiscard]] bool text_atlas_pack(uint32_t face, uint32_t epoch, uint32_t glyph,
                                   const uint8_t *bytes, uint32_t width, uint32_t rows,
                                   TextAtlasRect *out);

uint64_t text_atlas_address(void);

#endif /* ENGINE_TEXT_ATLAS_H */
