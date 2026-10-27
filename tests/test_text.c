#include "text/face/face.h"
#include "text/shape/shape.h"
#include <stdio.h>
#include <string.h>

// CPU text: the sealed face cannot be retargeted, the SDF stroke is inside
// the outline, and a shaped word has a real advance. No Vulkan and no window.

static int s_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            s_failures++;                                               \
        }                                                               \
    } while (0)

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
    text_face_shutdown();
    if (s_failures != 0) {
        fprintf(stderr, "%d text checks failed\n", s_failures);
        return 1;
    }
    printf("text ok\n");
    return 0;
}
