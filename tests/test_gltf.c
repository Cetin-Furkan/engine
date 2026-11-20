#include "gltf/gltf.h"
#include "ecs/ecs.h"
#include "scene/scene.h"
#include "texture/texture.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int s_failures = 0;

#define CHECK(cond, msg)                                                           \
    do {                                                                           \
        if (!(cond)) {                                                             \
            fprintf(stderr, "  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, (msg));    \
            s_failures++;                                                          \
        }                                                                          \
    } while (0)

static void write_u32_le(uint8_t *dst, uint32_t v) {
    dst[0] = (uint8_t)(v & 0xFFu);
    dst[1] = (uint8_t)((v >> 8u) & 0xFFu);
    dst[2] = (uint8_t)((v >> 16u) & 0xFFu);
    dst[3] = (uint8_t)((v >> 24u) & 0xFFu);
}

static void test_yup_to_zup_and_tangents(void) {
    float q[4];
    gltf_yup_to_zup_quat(q);

    // glTF +Y (up) -> Engine +Z (up)
    float up_gltf[3] = { 0.0f, 1.0f, 0.0f };
    float up_eng[3]  = {0};
    ecs_quat_rotate_vec3(q, up_gltf, up_eng);
    CHECK(fabsf(up_eng[0]) < 1.0e-5f && fabsf(up_eng[1]) < 1.0e-5f && fabsf(up_eng[2] - 1.0f) < 1.0e-5f,
          "glTF +Y maps exactly to Engine +Z");

    // glTF +Z (front) -> Engine -Y
    float fwd_gltf[3] = { 0.0f, 0.0f, 1.0f };
    float fwd_eng[3]  = {0};
    ecs_quat_rotate_vec3(q, fwd_gltf, fwd_eng);
    CHECK(fabsf(fwd_eng[0]) < 1.0e-5f && fabsf(fwd_eng[1] + 1.0f) < 1.0e-5f && fabsf(fwd_eng[2]) < 1.0e-5f,
          "glTF +Z maps exactly to Engine -Y (preserves right-handedness)");

    // Tangent generation test: right-handed UV (+1) and mirrored UV (-1)
    ScenePbrCorner tri[3] = {
        { .x = 0, .y = 0, .z = 0, .nx = 0, .ny = 0, .nz = 1, .u = 0, .v = 0, .r = 1, .g = 1, .b = 1, .a = 1 },
        { .x = 1, .y = 0, .z = 0, .nx = 0, .ny = 0, .nz = 1, .u = 1, .v = 0, .r = 1, .g = 1, .b = 1, .a = 1 },
        { .x = 0, .y = 1, .z = 0, .nx = 0, .ny = 0, .nz = 1, .u = 0, .v = 1, .r = 1, .g = 1, .b = 1, .a = 1 },
    };
    gltf_generate_tangents(tri, 3u);
    CHECK(fabsf(tri[0].tx - 1.0f) < 1.0e-4f && fabsf(tri[0].ty) < 1.0e-4f && fabsf(tri[0].tz) < 1.0e-4f,
          "standard triangle tangent aligns with +X");
    CHECK(tri[0].tw > 0.5f, "standard UV bitangent sign is +1");

    ScenePbrCorner mir[3] = {
        { .x = 0, .y = 0, .z = 0, .nx = 0, .ny = 0, .nz = 1, .u = 0, .v = 1, .r = 1, .g = 1, .b = 1, .a = 1 },
        { .x = 1, .y = 0, .z = 0, .nx = 0, .ny = 0, .nz = 1, .u = 1, .v = 1, .r = 1, .g = 1, .b = 1, .a = 1 },
        { .x = 0, .y = 1, .z = 0, .nx = 0, .ny = 0, .nz = 1, .u = 0, .v = 0, .r = 1, .g = 1, .b = 1, .a = 1 },
    };
    gltf_generate_tangents(mir, 3u);
    CHECK(mir[0].tw < -0.5f, "mirrored V coordinate flips bitangent handedness to -1");
}

static void test_synthetic_glb_extensions(void) {
    // Build a valid minimal GLB in memory with:
    // - 3 positions + 3 UVs + 3 u16 indices
    // - Material with KHR_materials_emissive_strength (4.5), KHR_materials_ior (2.0),
    //   KHR_texture_transform, doubleSided, alphaMode MASK
    // - Parent Empty node -> Child Mesh node with negative scale matrix
    const float    verts[9] = { 0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,  0.0f, 2.0f, 0.0f };
    const float    uvs[6]   = { 0.0f, 0.0f,  1.0f, 0.0f,  0.0f, 1.0f };
    const uint16_t idx[4]   = { 0u, 1u, 2u, 0u }; // 8 bytes aligned

    uint8_t bin[sizeof verts + sizeof uvs + sizeof idx];
    memcpy(bin, verts, sizeof verts);
    memcpy(bin + 36u, uvs, sizeof uvs);
    memcpy(bin + 60u, idx, sizeof idx);
    const size_t bin_len = sizeof bin; // 68 bytes (4-byte aligned)

    const char *json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":["
          "{\"name\":\"ParentRig\",\"translation\":[0,1,0],\"children\":[1]},"
          "{\"name\":\"MirroredTri\",\"mesh\":0,\"matrix\":[-1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]}"
        "],"
        "\"meshes\":[{\"name\":\"TriMesh\",\"primitives\":[{\"attributes\":{\"POSITION\":0,\"TEXCOORD_0\":1},\"indices\":2,\"material\":0}]}],"
        "\"materials\":[{"
          "\"name\":\"GoldGlow\","
          "\"doubleSided\":true,"
          "\"alphaMode\":\"MASK\",\"alphaCutoff\":0.42,"
          "\"emissiveFactor\":[0.5,0.2,0.1],"
          "\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.9,0.7,0.2,1.0],\"metallicFactor\":0.95,\"roughnessFactor\":0.18},"
          "\"extensions\":{"
            "\"KHR_materials_emissive_strength\":{\"emissiveStrength\":4.5},"
            "\"KHR_materials_ior\":{\"ior\":2.0}"
          "}"
        "}],"
        "\"buffers\":[{\"byteLength\":68}],"
        "\"bufferViews\":["
          "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
          "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":24},"
          "{\"buffer\":0,\"byteOffset\":60,\"byteLength\":6}"
        "],"
        "\"accessors\":["
          "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
          "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
          "{\"bufferView\":2,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}"
        "]"
        "}";

    size_t jraw = strlen(json);
    size_t jpad = (jraw + 3u) & ~3u;
    size_t total = 12u + 8u + jpad + 8u + bin_len;
    uint8_t *glb = calloc(1u, total);
    CHECK(glb != nullptr, "alloc synthetic glb");
    if (glb == nullptr) return;

    write_u32_le(glb + 0u, 0x46546C67u);
    write_u32_le(glb + 4u, 2u);
    write_u32_le(glb + 8u, (uint32_t)total);
    write_u32_le(glb + 12u, (uint32_t)jpad);
    write_u32_le(glb + 16u, 0x4E4F534Au);
    memcpy(glb + 20u, json, jraw);
    for (size_t i = jraw; i < jpad; ++i) glb[20u + i] = ' ';
    size_t bpos = 20u + jpad;
    write_u32_le(glb + bpos, (uint32_t)bin_len);
    write_u32_le(glb + bpos + 4u, 0x004E4942u);
    memcpy(glb + bpos + 8u, bin, bin_len);

    GltfAsset asset = {0};
    CHECK(gltf_load_glb_memory(glb, total, nullptr, &asset) == 0, "load synthetic GLB");
    CHECK(asset.node_count == 2u, "2 nodes parsed");
    CHECK(asset.prim_count == 1u && asset.prims[0].corner_count == 3u, "1 triangle primitive");
    CHECK(asset.prims[0].has_uvs && asset.prims[0].has_tangents, "UVs and auto-generated tangents");
    CHECK(asset.nodes[0].parent_index == -1 && asset.nodes[1].parent_index == 0, "parent-child link preserved");
    CHECK(asset.nodes[1].negative_det, "negative-scale matrix detected");
    // Parent translated +1 in glTF Y, triangle height +2 in glTF Y -> Engine Z max == +3.0!
    CHECK(fabsf(asset.bounds_min[2] - 1.0f) < 1.0e-4f && fabsf(asset.bounds_max[2] - 3.0f) < 1.0e-4f,
          "hierarchical Z-up bounds exact");

    CHECK(asset.material_count == 1u, "1 material");
    if (asset.material_count == 1u) {
        const GltfMaterial *m = &asset.materials[0];
        CHECK((m->flags & PBR_MAT_FLAG_DOUBLE_SIDED) != 0u, "doubleSided flag");
        CHECK((m->flags & PBR_MAT_FLAG_ALPHA_MASK) != 0u, "alphaMode MASK flag");
        CHECK(fabsf(m->alpha_cutoff - 0.42f) < 1.0e-4f, "alphaCutoff preserved");
        CHECK(fabsf(m->ior - 2.0f) < 1.0e-4f, "KHR_materials_ior preserved");
        CHECK(fabsf(m->emissive[0] - 2.25f) < 1.0e-3f, "KHR_materials_emissive_strength multiplied");
    }

    SceneMeshletBuild build = {0};
    CHECK(scene_build_meshlets_pbr(asset.prims[0].corners, asset.prims[0].corner_count,
                                   asset.prims[0].has_uvs, asset.prims[0].has_tangents, &build) == 0,
          "clusterize glTF PBR meshlet");
    CHECK(build.meshlet_count == 1u && build.vertex_count == 3u && build.triangle_count == 1u,
          "meshlet counts exact");
    scene_meshlet_build_free(&build);
    gltf_asset_free(&asset);
    free(glb);
}

static bool has_blender_in_path(void) {
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);
        execlp("blender", "blender", "--version", (char *)nullptr);
        _exit(127);
    }
    int status = 0;
    (void)waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void test_real_blender_blend_roundtrip(void) {
    if (!has_blender_in_path()) {
        printf("  [Skip] 'blender' not installed on PATH — skipping live .blend round-trip.\n");
        return;
    }

    const char *blend_path = "/tmp/engine_v2_blender_test.blend";
    unlink(blend_path);

    // Create a genuine Blender scene exercising Principled BSDF textures, normal map,
    // KHR_texture_transform (Mapping node), KHR_materials_emissive_strength, KHR_materials_ior,
    // ALPHA_MASK, ALPHA_BLEND, doubleSided, Subdivision modifier, mirrored scale, and 3-level hierarchy!
    const char *gen_py =
        "import bpy, math\n"
        "bpy.ops.wm.read_factory_settings(use_empty=True)\n"
        "\n"
        "# 1. Packed 16x16 PNG Albedo + Normal + ORM textures\n"
        "alb = bpy.data.images.new('AlbedoTex', 16, 16, alpha=True)\n"
        "nrm = bpy.data.images.new('NormalTex', 16, 16, alpha=False)\n"
        "nrm.colorspace_settings.name = 'Non-Color'\n"
        "apx = []; npx = []\n"
        "for y in range(16):\n"
        "    for x in range(16):\n"
        "        chk = 1.0 if ((x // 4) + (y // 4)) % 2 == 0 else 0.25\n"
        "        apx.extend([0.85 * chk, 0.45 * chk, 0.18, 1.0 if (x + y) % 3 != 0 else 0.1])\n"
        "        npx.extend([0.62, 0.50, 0.95, 1.0])\n"
        "alb.pixels[:] = apx; alb.pack()\n"
        "nrm.pixels[:] = npx; nrm.pack()\n"
        "\n"
        "# 2. Empty Root -> Subdivided Textured Pedestal -> Neon Orb + Glass Dome + Mirrored Fin\n"
        "rig = bpy.data.objects.new('RigRoot', None)\n"
        "bpy.context.collection.objects.link(rig)\n"
        "\n"
        "bpy.ops.mesh.primitive_cube_add(size=1.2, location=(0, 0, 0.6))\n"
        "ped = bpy.context.active_object; ped.name = 'Pedestal'\n"
        "ped.parent = rig\n"
        "mod = ped.modifiers.new('Subsurf', 'SUBSURF'); mod.levels = 1; mod.render_levels = 1\n"
        "mat_p = bpy.data.materials.new('PaintedMetal'); mat_p.use_nodes = True\n"
        "nt = mat_p.node_tree; bsdf = nt.nodes['Principled BSDF']\n"
        "bsdf.inputs['Metallic'].default_value = 0.85\n"
        "bsdf.inputs['Roughness'].default_value = 0.28\n"
        "bsdf.inputs['IOR'].default_value = 1.75\n"
        "tc = nt.nodes.new('ShaderNodeTexCoord')\n"
        "mp = nt.nodes.new('ShaderNodeMapping'); mp.inputs['Scale'].default_value = (2.0, 2.0, 1.0)\n"
        "ta = nt.nodes.new('ShaderNodeTexImage'); ta.image = alb\n"
        "tn = nt.nodes.new('ShaderNodeTexImage'); tn.image = nrm\n"
        "nm = nt.nodes.new('ShaderNodeNormalMap'); nm.inputs['Strength'].default_value = 0.8\n"
        "nt.links.new(tc.outputs['UV'], mp.inputs['Vector'])\n"
        "nt.links.new(mp.outputs['Vector'], ta.inputs['Vector'])\n"
        "nt.links.new(mp.outputs['Vector'], tn.inputs['Vector'])\n"
        "nt.links.new(ta.outputs['Color'], bsdf.inputs['Base Color'])\n"
        "nt.links.new(tn.outputs['Color'], nm.inputs['Color'])\n"
        "nt.links.new(nm.outputs['Normal'], bsdf.inputs['Normal'])\n"
        "ped.data.materials.append(mat_p)\n"
        "\n"
        "bpy.ops.mesh.primitive_uv_sphere_add(segments=16, ring_count=10, radius=0.35, location=(0, 0, 1.65))\n"
        "orb = bpy.context.active_object; orb.name = 'NeonOrb'; orb.parent = ped\n"
        "mat_e = bpy.data.materials.new('NeonCore'); mat_e.use_nodes = True\n"
        "eb = mat_e.node_tree.nodes['Principled BSDF']\n"
        "eb.inputs['Base Color'].default_value = (0.1, 0.8, 1.0, 1.0)\n"
        "eb.inputs['Emission Color'].default_value = (0.2, 0.9, 1.0, 1.0)\n"
        "eb.inputs['Emission Strength'].default_value = 6.0\n"
        "orb.data.materials.append(mat_e)\n"
        "\n"
        "bpy.ops.mesh.primitive_uv_sphere_add(segments=16, ring_count=10, radius=0.55, location=(0, 0, 1.65))\n"
        "dome = bpy.context.active_object; dome.name = 'GlassDome'; dome.parent = ped\n"
        "mat_g = bpy.data.materials.new('ClearGlass'); mat_g.use_nodes = True\n"
        "gb = mat_g.node_tree.nodes['Principled BSDF']\n"
        "gb.inputs['Base Color'].default_value = (0.7, 0.9, 1.0, 1.0)\n"
        "gb.inputs['Alpha'].default_value = 0.35\n"
        "gb.inputs['Roughness'].default_value = 0.08\n"
        "dome.data.materials.append(mat_g)\n"
        "\n"
        "bpy.ops.mesh.primitive_cone_add(vertices=12, radius1=0.3, depth=0.8, location=(1.1, 0, 0.4))\n"
        "fin = bpy.context.active_object; fin.name = 'MirroredFin'; fin.parent = rig\n"
        "fin.scale = (-1.0, 1.0, 1.0)\n"
        "\n"
        "bpy.ops.wm.save_as_mainfile(filepath='/tmp/engine_v2_blender_test.blend')\n";

    pid_t pid = fork();
    CHECK(pid >= 0, "fork blender generator");
    if (pid == 0) {
        freopen("/dev/null", "w", stdout);
        execlp("blender", "blender", "--background", "--factory-startup",
               "--python-expr", gen_py, (char *)nullptr);
        _exit(127);
    }
    int status = 0;
    (void)waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "Blender generated test .blend file");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return;
    }

    // Load .blend directly (cold export via Blender -> cached GLB)
    GltfAsset asset = {0};
    CHECK(gltf_load_file(blend_path, &asset) == 0, "gltf_load_file loads native .blend directly");
    CHECK(asset.from_blend, "asset flagged from_blend");
    CHECK(asset.node_count >= 5u, "all 5 Blender nodes (Empty + 4 meshes) present");
    CHECK(asset.prim_count >= 4u, "all 4 mesh primitives present");
    CHECK(asset.image_count >= 2u, "packed PNG albedo + normal textures exported");
    CHECK(asset.material_count >= 3u, "Principled BSDF materials exported");

    // Verify hierarchy & specific Blender features!
    bool saw_rig = false, saw_ped = false, saw_orb = false, saw_mirrored = false;
    uint32_t hier_links = 0u;
    for (uint32_t i = 0u; i < asset.node_count; ++i) {
        const GltfNode *n = &asset.nodes[i];
        if (n->parent_index >= 0) hier_links++;
        if (strcmp(n->name, "RigRoot") == 0)     saw_rig = true;
        if (strcmp(n->name, "Pedestal") == 0)    saw_ped = true;
        if (strcmp(n->name, "NeonOrb") == 0)     saw_orb = true;
        if (strcmp(n->name, "MirroredFin") == 0 && n->negative_det) saw_mirrored = true;
    }
    CHECK(saw_rig && saw_ped && saw_orb, "Blender object names preserved (RigRoot, Pedestal, NeonOrb)");
    CHECK(hier_links >= 4u, "3-level Blender parent-child hierarchy preserved");
    CHECK(saw_mirrored, "mirrored Blender Scale X=-1 detected");

    // Verify Subdivision Surface modifier was applied (Pedestal > 12 tris of a plain cube!)
    uint32_t max_tris = 0u;
    for (uint32_t p = 0u; p < asset.prim_count; ++p) {
        uint32_t tris = asset.prims[p].corner_count / 3u;
        if (tris > max_tris) max_tris = tris;
        CHECK(asset.prims[p].has_tangents || !asset.prims[p].has_uvs, "tangents present for UV primitives");
    }
    CHECK(max_tris >= 48u, "Subdivision modifier applied on export");

    // Verify Principled BSDF materials: emissiveStrength > 1, normal map, UV Mapping scale 2x2, ALPHA_BLEND
    bool saw_emissive_hdr = false, saw_normal_map = false, saw_uv_xform = false, saw_blend = false;
    for (uint32_t m = 0u; m < asset.material_count; ++m) {
        const GltfMaterial *gm = &asset.materials[m];
        if (gm->emissive[2] > 2.0f) saw_emissive_hdr = true;
        if ((gm->flags & PBR_MAT_FLAG_HAS_NORMAL) != 0u) saw_normal_map = true;
        if (gm->base_tex.has_transform && fabsf(gm->base_tex.uv_scale[0] - 2.0f) < 0.05f) saw_uv_xform = true;
        if ((gm->flags & PBR_MAT_FLAG_ALPHA_BLEND) != 0u) saw_blend = true;
    }
    CHECK(saw_emissive_hdr, "Blender Emission Strength > 1 exported via KHR_materials_emissive_strength");
    CHECK(saw_normal_map, "Blender Normal Map node exported");
    CHECK(saw_uv_xform, "Blender Mapping node exported via KHR_texture_transform");
    CHECK(saw_blend, "Blender Alpha < 1 exported as ALPHA_BLEND");

    // Verify all packed Blender PNG images decode cleanly via libpng16
    for (uint32_t i = 0u; i < asset.image_count; ++i) {
        uint8_t *rgba = nullptr;
        uint32_t w = 0u, h = 0u;
        CHECK(asset.images[i].encoded != nullptr && asset.images[i].encoded_len > 0u,
              "GLB image bytes embedded");
        CHECK(texture_decode_memory(asset.images[i].encoded, asset.images[i].encoded_len,
                                    &rgba, &w, &h) == 0 && w == 16u && h == 16u,
              "packed Blender PNG texture decoded cleanly");
        free(rgba);
    }

    // Verify Z-up world orientation: NeonOrb sits above Pedestal at Z > 1.8m!
    CHECK(asset.bounds_max[2] > 1.8f && asset.bounds_min[2] > -0.2f,
          "Blender Z-up world bounds upright (top > 1.8m, base near 0m)");

    // Second load must hit the deterministic GLB cache cleanly
    struct timespec t0 = {0}, t1 = {0};
    clock_gettime(CLOCK_MONOTONIC, &t0);
    GltfAsset cached = {0};
    CHECK(gltf_load_file(blend_path, &cached) == 0, "cached .blend reload succeeds");
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double cache_ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 +
                      (double)(t1.tv_nsec - t0.tv_nsec) * 1.0e-6;
    CHECK(cache_ms < 250.0, "cached .blend reload skips Blender subprocess (<250ms)");
    CHECK(cached.node_count == asset.node_count, "cached load matches cold load");

    gltf_asset_free(&cached);
    gltf_asset_free(&asset);
    unlink(blend_path);
}

int main(void) {
    printf("[Test] Running Strict ISO C23 glTF 2.0 + Native Blender (.blend) Suite...\n");
    test_yup_to_zup_and_tangents();
    test_synthetic_glb_extensions();
    test_real_blender_blend_roundtrip();
    if (s_failures != 0) {
        fprintf(stderr, "[FAIL] test_gltf: %d check(s) failed\n", s_failures);
        return 1;
    }
    printf("[PASS] test_gltf: all glTF 2.0, KHR extensions, tangents, and live Blender .blend checks passed!\n");
    return 0;
}
