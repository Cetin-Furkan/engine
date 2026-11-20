#include "graph/graph.h"
#include <stdio.h>
#include <string.h>

static int s_failures = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            fprintf(stderr, "  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, (msg));     \
            s_failures++;                                                           \
        }                                                                           \
    } while (0)

static void dummy_pass(VkCommandBuffer cmd, void *user) {
    (void)cmd;
    (void)user;
}

static void test_shadow_rar_merge_and_war(void) {
    RgGraph g = {0};
    rg_begin_frame(&g, 0u);

    VkImageLayout shadow_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    RgResHandle shadow = rg_import_image(&g, "ShadowMap", (VkImage)(uintptr_t)0x1001,
                                         VK_IMAGE_ASPECT_DEPTH_BIT, 0u, 1u,
                                         VK_IMAGE_LAYOUT_UNDEFINED, &shadow_layout, false);
    RgResHandle hdr = rg_import_image(&g, "SceneHDR", (VkImage)(uintptr_t)0x1002,
                                      VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u,
                                      VK_IMAGE_LAYOUT_UNDEFINED, nullptr, false);
    RgResHandle gi_bda = rg_import_buffer(&g, "GiSlab", VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);

    // Pass 0: Shadow write
    RgAccess a0[] = { { shadow, RG_USE_DEPTH_WRITE } };
    rg_add_pass(&g, "Shadow", RG_PASS_SHADOW, a0, 1u, dummy_pass, nullptr);

    // Pass 1: GI compute samples shadow + writes GI BDA
    RgAccess a1[] = { { shadow, RG_USE_COMPUTE_SAMPLED }, { gi_bda, RG_USE_COMPUTE_WRITE } };
    rg_add_pass(&g, "GI", RG_PASS_GI, a1, 2u, dummy_pass, nullptr);

    // Pass 2: Scene reads shadow (RAR!), reads GI BDA (RAW), writes SceneHDR
    RgAccess a2[] = {
        { shadow, RG_USE_FRAG_SAMPLED },
        { gi_bda, RG_USE_GRAPHICS_READ },
        { hdr,    RG_USE_COLOR_WRITE },
    };
    rg_add_pass(&g, "SceneHDR", RG_PASS_SCENE, a2, 3u, dummy_pass, nullptr);

    // Pass 3: Next-frame shadow write (WAR against COMPUTE + FRAG readers, zero cache flush!)
    RgAccess a3[] = { { shadow, RG_USE_DEPTH_WRITE } };
    rg_add_pass(&g, "ShadowNext", RG_PASS_SHADOW, a3, 1u, dummy_pass, nullptr);

    RgBarrierBatch b0 = {0}, b1 = {0}, b2 = {0}, b3 = {0};
    rg_plan_pass_barriers(&g, 0u, &b0);
    CHECK(b0.image_count == 1u, "Shadow pass transitions UNDEFINED -> DEPTH_ATTACHMENT");
    CHECK(b0.image_barriers[0].oldLayout == VK_IMAGE_LAYOUT_UNDEFINED, "Shadow discard oldLayout");
    CHECK(b0.image_barriers[0].newLayout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, "Shadow newLayout");

    rg_plan_pass_barriers(&g, 1u, &b1);
    CHECK(b1.image_count == 1u, "GI pass emits RAW transition DEPTH -> SHADER_READ_ONLY");
    CHECK(b1.image_barriers[0].oldLayout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, "GI oldLayout");
    CHECK(b1.image_barriers[0].newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, "GI newLayout");
    CHECK((b1.image_barriers[0].srcAccessMask & VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT) != 0u,
          "GI srcAccess flushes depth write");
    CHECK((b1.image_barriers[0].dstStageMask & VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT) != 0u,
          "GI dstStage covers compute reader");
    CHECK((b1.image_barriers[0].dstStageMask & VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT) != 0u,
          "Look-ahead RAR merge covers downstream fragment reader in single barrier");
    CHECK(!b1.has_mem_barrier, "First BDA write needs no prior barrier");

    rg_plan_pass_barriers(&g, 2u, &b2);
    // RAR on shadow map (same layout SHADER_READ_ONLY_OPTIMAL) MUST emit ZERO barrier for shadow!
    CHECK(b2.image_count == 1u, "SceneHDR emits only HDR image barrier (shadow RAR merged!)");
    CHECK(b2.image_barriers[0].image == (VkImage)(uintptr_t)0x1002, "Only HDR transitioned");
    CHECK(b2.has_mem_barrier, "SceneHDR emits BDA RAW barrier for GI slab");
    CHECK(b2.mem_barrier.srcStageMask == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, "GI srcStage compute");
    CHECK((b2.mem_barrier.dstStageMask & VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT) != 0u, "GI dstStage frag");

    rg_plan_pass_barriers(&g, 3u, &b3);
    CHECK(b3.image_count == 1u, "ShadowNext emits WAR execution barrier");
    CHECK(b3.image_barriers[0].srcAccessMask == VK_ACCESS_2_NONE, "WAR uses execution-only barrier (no cache flush)");
    CHECK((b3.image_barriers[0].srcStageMask & VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT) != 0u, "WAR tracks compute reader");
    CHECK((b3.image_barriers[0].srcStageMask & VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT) != 0u, "WAR tracks frag reader");
}

static void test_skipped_producer_undefined_safety(void) {
    RgGraph g = {0};
    rg_begin_frame(&g, 1u);

    // Simulates 2D mode: ShadowMap starts UNDEFINED and ShadowPass is NEVER added;
    // Plane/Scene still declares RG_USE_FRAG_SAMPLED -> graph MUST transition UNDEFINED -> SHADER_READ_ONLY.
    RgResHandle shadow = rg_import_image(&g, "Shadow2D", (VkImage)(uintptr_t)0x2001,
                                         VK_IMAGE_ASPECT_DEPTH_BIT, 0u, 1u,
                                         VK_IMAGE_LAYOUT_UNDEFINED, nullptr, false);
    RgAccess a0[] = { { shadow, RG_USE_FRAG_SAMPLED } };
    rg_add_pass(&g, "Plane2D", RG_PASS_SCENE, a0, 1u, dummy_pass, nullptr);

    RgBarrierBatch b0 = {0};
    rg_plan_pass_barriers(&g, 0u, &b0);
    CHECK(b0.image_count == 1u, "Skipped producer still triggers safe UNDEFINED -> SHADER_READ_ONLY");
    CHECK(b0.image_barriers[0].oldLayout == VK_IMAGE_LAYOUT_UNDEFINED, "oldLayout UNDEFINED");
    CHECK(b0.image_barriers[0].newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, "newLayout SHADER_READ_ONLY");
    CHECK(b0.image_barriers[0].srcStageMask == VK_PIPELINE_STAGE_2_NONE, "srcStage NONE");
}

static void test_unified_image_layouts(void) {
    RgGraph g = {0};
    rg_begin_frame(&g, 0u);

    RgResHandle img = rg_import_image(&g, "UnifiedHDR", (VkImage)(uintptr_t)0x3001,
                                      VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u,
                                      VK_IMAGE_LAYOUT_GENERAL, nullptr, true);
    RgAccess a0[] = { { img, RG_USE_COLOR_WRITE } };
    RgAccess a1[] = { { img, RG_USE_COMPUTE_SAMPLED } };
    rg_add_pass(&g, "Write", RG_PASS_SCENE, a0, 1u, dummy_pass, nullptr);
    rg_add_pass(&g, "Read",  RG_PASS_POST,  a1, 1u, dummy_pass, nullptr);

    RgBarrierBatch b0 = {0}, b1 = {0};
    rg_plan_pass_barriers(&g, 0u, &b0);
    CHECK(b0.image_count == 0u, "Unified GENERAL already in layout and no prior hazard -> 0 barriers");

    rg_plan_pass_barriers(&g, 1u, &b1);
    CHECK(b1.image_count == 1u, "Unified RAW barrier keeps GENERAL -> GENERAL");
    CHECK(b1.image_barriers[0].oldLayout == VK_IMAGE_LAYOUT_GENERAL, "Unified oldLayout GENERAL");
    CHECK(b1.image_barriers[0].newLayout == VK_IMAGE_LAYOUT_GENERAL, "Unified newLayout GENERAL");
}

int main(void) {
    test_shadow_rar_merge_and_war();
    test_skipped_producer_undefined_safety();
    test_unified_image_layouts();

    if (s_failures != 0) {
        fprintf(stderr, "[test_graph] %d check(s) failed!\n", s_failures);
        return 1;
    }
    printf("test_graph: all checks passed\n");
    return 0;
}
