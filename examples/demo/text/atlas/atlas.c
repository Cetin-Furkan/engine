#include "atlas.h"

#include "vulkan/bda.h"
#include <stdio.h>
#include <string.h>

enum : uint32_t { k_slots = 2048, k_gap = 1 };
enum : uint8_t { k_empty = 0, k_used = 1, k_tomb = 2 };

typedef struct PackSlot {
    uint32_t face;
    uint32_t epoch;
    uint32_t glyph;
    uint32_t x, y, w, h;
    uint8_t  state;
} PackSlot;

static DaBuffer  s_atlas;
static uint32_t *s_words = nullptr;
static uint64_t  s_addr = 0;
static PackSlot       s_slot[k_slots];
static uint32_t       s_cursor_x = 0;
static uint32_t       s_shelf_y = 0;
static uint32_t       s_shelf_h = 0;
static bool           s_full = false;

static uint32_t pack_hash(uint32_t face, uint32_t epoch, uint32_t glyph) {
    uint32_t h = face * 0x9E3779B1u ^ (epoch * 0x85EBCA6Bu) ^ (glyph + 0x7F4A7C15u);
    h ^= h >> 16;
    return h;
}

static PackSlot *pack_find(uint32_t face, uint32_t epoch, uint32_t glyph, bool insert) {
    uint32_t index = pack_hash(face, epoch, glyph) & (k_slots - 1u);
    PackSlot *tomb = nullptr;
    for (uint32_t n = 0; n < k_slots; ++n) {
        PackSlot *slot = &s_slot[index];
        if (slot->state == k_used && slot->face == face && slot->epoch == epoch && slot->glyph == glyph) {
            return slot;
        }
        if (slot->state == k_tomb && tomb == nullptr) {
            tomb = slot;
        }
        if (slot->state == k_empty) {
            if (!insert) {
                return nullptr;
            }
            return tomb != nullptr ? tomb : slot;
        }
        index = (index + 1u) & (k_slots - 1u);
    }
    return insert ? tomb : nullptr;
}

static void put_byte(uint32_t x, uint32_t y, uint8_t value) {
    uint32_t linear = y * TEXT_ATLAS_DIM + x;
    uint32_t shift = (linear & 3u) * 8u;
    uint32_t *word = s_words + (linear >> 2);
    *word = (*word & ~(0xFFu << shift)) | ((uint32_t)value << shift);
}

void text_atlas_shutdown(VulkanContext *vk) {
    s_words = nullptr;
    s_addr = 0;
    s_cursor_x = 0;
    s_shelf_y = 0;
    s_shelf_h = 0;
    s_full = false;
    memset(s_slot, 0, sizeof s_slot);
    da_buffer_destroy(vk, &s_atlas);
}

int text_atlas_init(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE) {
        return -1;
    }
    if (s_atlas.buffer != VK_NULL_HANDLE) {
        return 0;
    }
    VkDeviceSize bytes = (VkDeviceSize)TEXT_ATLAS_DIM * TEXT_ATLAS_DIM;
    if (da_buffer_create(vk, bytes, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true, &s_atlas) != 0) {
        return -1;
    }
    if (s_atlas.map == nullptr || s_atlas.address == 0 || (s_atlas.address & 3u) != 0u) {
        fprintf(stderr, "[Text Error] Atlas device address is null\n");
        text_atlas_shutdown(vk);
        return -1;
    }
    memset(s_atlas.map, 0, (size_t)bytes);
    s_words = s_atlas.map;
    s_addr = s_atlas.address;
    return 0;
}

bool text_atlas_pack(uint32_t face, uint32_t epoch, uint32_t glyph,
                     const uint8_t *bytes, uint32_t width, uint32_t rows,
                     TextAtlasRect *out) {
    if (out == nullptr || s_words == nullptr) {
        return false;
    }
    *out = (TextAtlasRect){0};
    if (width == 0u || rows == 0u || bytes == nullptr) {
        return true;
    }
    PackSlot *found = pack_find(face, epoch, glyph, false);
    if (found != nullptr) {
        out->x = found->x;
        out->y = found->y;
        out->w = found->w;
        out->h = found->h;
        return true;
    }
    if (width > TEXT_ATLAS_DIM || rows > TEXT_ATLAS_DIM) {
        return false;
    }
    if (s_cursor_x + width + k_gap > TEXT_ATLAS_DIM) {
        s_shelf_y += s_shelf_h;
        s_cursor_x = 0;
        s_shelf_h = 0;
    }
    if (s_shelf_y + rows + k_gap > TEXT_ATLAS_DIM) {
        if (!s_full) {
            fprintf(stderr, "[Text Error] Atlas is full\n");
            s_full = true;
        }
        return false;
    }
    PackSlot *slot = pack_find(face, epoch, glyph, true);
    if (slot == nullptr) {
        return false;
    }
    uint32_t x = s_cursor_x;
    uint32_t y = s_shelf_y;
    for (uint32_t row = 0; row < rows; ++row) {
        const uint8_t *src = bytes + row * width;
        for (uint32_t col = 0; col < width; ++col) {
            put_byte(x + col, y + row, src[col]);
        }
    }
    s_cursor_x += width + k_gap;
    if (rows + k_gap > s_shelf_h) {
        s_shelf_h = rows + k_gap;
    }
    slot->face = face;
    slot->epoch = epoch;
    slot->glyph = glyph;
    slot->x = x;
    slot->y = y;
    slot->w = width;
    slot->h = rows;
    slot->state = k_used;
    out->x = x;
    out->y = y;
    out->w = width;
    out->h = rows;
    return true;
}

uint64_t text_atlas_address(void) {
    return s_addr;
}
