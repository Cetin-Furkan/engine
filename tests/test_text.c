#include "input/pointer.h"
#include "text/atlas/atlas.h"
#include "text/face/face.h"
#include "text/shape/shape.h"
#include "text/text_int.h"
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <string.h>

// CPU text: the sealed face cannot be retargeted, the SDF stroke is inside
// the outline, a shaped word has a real advance, and text_set_visible(false)
// suppresses overlay quads and editing in World 0. No Vulkan and no window.

static int s_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            s_failures++;                                               \
        }                                                               \
    } while (0)

uint64_t text_atlas_address(void) {
    return 0u;
}

bool text_atlas_pack(uint32_t face, uint32_t epoch, uint32_t glyph,
                     const uint8_t *bytes, uint32_t width, uint32_t rows,
                     TextAtlasRect *out) {
    (void)face;
    (void)epoch;
    (void)glyph;
    (void)bytes;
    (void)width;
    (void)rows;
    (void)out;
    return false;
}

void pointer_set_clipboard_text(const char *utf8, uint32_t len) {
    (void)utf8;
    (void)len;
}

int main(void) {
    CHECK(text_face_init() == 0);
    CHECK(!text_face_apply_line("sealed file /tmp/not-a-face.ttf"));
    CHECK(strstr(text_face_family(TEXT_FACE_SEALED), "Inter") != nullptr);
    CHECK(text_face_family(TEXT_FACE_SYSTEM)[0] != '\0');
    CHECK(text_face_family(TEXT_FACE_TERMINAL)[0] != '\0');
    CHECK(text_face_sdf_check());
    float adv = text_shape_advance(TEXT_FACE_SEALED, "Hello", 32.0f);
    CHECK(adv > 20.0f);
    CHECK(adv < 400.0f);
    float mono = text_shape_advance(TEXT_FACE_KNOWN_MONO, "Hello", 32.0f);
    CHECK(mono > 20.0f);
    uint32_t e0 = text_face_epoch(TEXT_FACE_SYSTEM);
    CHECK(text_face_apply_line("system pattern sans"));
    CHECK(text_face_epoch(TEXT_FACE_SYSTEM) == e0 + 1u);
    CHECK(text_face_sdf_check());

    // Verify text visibility gate (World 0 vs Worlds 1..9)
    text_set_visible(true);
    text_place_boot();
    CHECK(text_is_visible());
    CHECK(!text_busy());
    text_arm_tick(true, true, 100, 100);
    CHECK(text_click(-100.0f, 50.0f, true));
    CHECK(text_click(100.0f, -20.0f, true));
    CHECK(text_editing());

    // Hiding text (World 0) cancels active editing/anchors and suppresses clicks & quads
    text_set_visible(false);
    CHECK(!text_is_visible());
    CHECK(!text_editing());
    text_arm_tick(true, true, 100, 100);
    CHECK(!text_click(0.0f, 0.0f, true));
    CHECK(!text_on_key(KEY_A));
    CHECK(!text_insert("x", 1u));
    TextQuad quads[16];
    CHECK(text_fill(quads, 16u, 1280u, 720u) == 0u);
    text_commit();
    CHECK(!text_busy());

    // Re-enabling text restores interactive placement
    text_set_visible(true);
    CHECK(text_is_visible());
    CHECK(text_busy());
    text_commit();
    CHECK(!text_busy());

    text_face_shutdown();
    if (s_failures != 0) {
        fprintf(stderr, "%d text checks failed\n", s_failures);
        return 1;
    }
    printf("text ok\n");
    return 0;
}
