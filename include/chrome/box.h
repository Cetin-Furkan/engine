#ifndef ENGINE_CHROME_BOX_H
#define ENGINE_CHROME_BOX_H

#include "chrome/spec.h"
#include <stdint.h>

// The chrome spec is the tree. A layer docks to the window. A control is a
// child of its layer. The client is what remains. Paint and hit tests read
// these rectangles, so a dock change in the spec moves both.

typedef enum BoxDock : uint8_t {
    BOX_DOCK_WINDOW = 0,
    BOX_DOCK_TOP    = 1,
    BOX_DOCK_BOTTOM = 2,
    BOX_DOCK_CLIENT = 3,
    BOX_DOCK_CONTROL = 4,
} BoxDock;

typedef struct BoxRect {
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
} BoxRect;

typedef struct BoxNode {
    char     name[ENG_CHROME_NAME];
    BoxDock  dock;
    BoxRect  rect;
    uint16_t parent;
    uint16_t child_count;
} BoxNode;

enum : uint32_t {
    BOX_NONE = 0xffffu,
    BOX_CAP  = 1u + ENG_CHROME_LAYERS + ENG_CHROME_CONTROLS + 1u,
};

// Lays out spec into nodes. nodes[0] is the window. Top layers stack downward
// and bottom layers stack upward, in declaration order. Controls on one layer
// pack from align start on the left and align end on the right. The client
// rect is what remains. Returns the node count.
uint32_t box_layout(const ChromeSpec *spec,
                    uint32_t buf_w, uint32_t buf_h,
                    uint32_t surf_w, uint32_t surf_h,
                    BoxNode *nodes, uint32_t cap,
                    BoxRect *client);

[[nodiscard]] bool box_layer_rect(const ChromeSpec *spec, uint32_t layer_index,
                                  uint32_t buf_w, uint32_t buf_h,
                                  uint32_t surf_w, uint32_t surf_h,
                                  BoxRect *out);

[[nodiscard]] bool box_control_rect(const ChromeSpec *spec, uint32_t control_index,
                                    uint32_t buf_w, uint32_t buf_h,
                                    uint32_t surf_w, uint32_t surf_h,
                                    BoxRect *out);

#endif /* ENGINE_CHROME_BOX_H */
