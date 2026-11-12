#include "frame/mesh.h"
#include "frame/plot_file.h"
#include "frame/material.h"
#include "scene/scene.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The box sits above the cosine chart and left of the icosahedron.
// Z is centered on 0 so the click sphere does not need a center z.
static const float k_fit_x  = 280.0f;
static const float k_fit_y  = 170.0f;
static const float k_fit_hx = 90.0f;
static const float k_fit_hy = 70.0f;
static const float k_fit_hz = 48.0f;

enum : uint32_t {
    k_slice_corners = k_obj_tri_cap * 3u,
};

static VulkanContext *s_vk         = nullptr;
static RingCqeFn      s_init_cqe   = nullptr;
static void          *s_init_user  = nullptr;
static bool           s_mesh_ready = false;
static bool           s_logged     = false;
static uint32_t       s_mesh_id    = K_SCENE_INVALID_MESH;
static uint32_t       s_count      = 0u;
static uint64_t       s_hash       = 0ull;
static uint32_t       s_gen        = 0u;
static uint32_t       s_shown_gen  = 0u;
static float          s_radius     = 0.0f;

static uint64_t hash_corners_fnv1a(const ObjCorner *src, uint32_t corners) {
    uint64_t h = 14695981039346656037ull;
    const uint8_t *bytes = (const uint8_t *)src;
    size_t n = (size_t)corners * sizeof(ObjCorner);
    for (size_t i = 0u; i < n; ++i) {
        h ^= (uint64_t)bytes[i];
        h *= 1099511628211ull;
    }
    return h;
}

static float fit_corners_local(ObjCorner *dst, uint32_t corners) {
    if (corners == 0u || dst == nullptr) {
        return 0.0f;
    }
    float minx = dst[0].x, maxx = minx;
    float miny = dst[0].y, maxy = miny;
    float minz = dst[0].z, maxz = minz;
    for (uint32_t i = 1u; i < corners; ++i) {
        float x = dst[i].x;
        float y = dst[i].y;
        float z = dst[i].z;
        if (x < minx) minx = x;
        if (x > maxx) maxx = x;
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
        if (z < minz) minz = z;
        if (z > maxz) maxz = z;
    }
    float dx = maxx - minx;
    float dy = maxy - miny;
    float dz = maxz - minz;
    float s = 1.0e30f;
    if (dx > 1.0e-8f) {
        s = (k_fit_hx * 2.0f) / dx;
    }
    if (dy > 1.0e-8f) {
        float sy = (k_fit_hy * 2.0f) / dy;
        if (sy < s) {
            s = sy;
        }
    }
    if (dz > 1.0e-8f) {
        float sz = (k_fit_hz * 2.0f) / dz;
        if (sz < s) {
            s = sz;
        }
    }
    if (s > 1.0e29f) {
        s = 1.0f;
    }
    float cx = (minx + maxx) * 0.5f;
    float cy = (miny + maxy) * 0.5f;
    float cz = (minz + maxz) * 0.5f;
    for (uint32_t i = 0u; i < corners; ++i) {
        dst[i].x = (dst[i].x - cx) * s;
        dst[i].y = (dst[i].y - cy) * s;
        dst[i].z = (dst[i].z - cz) * s;
    }
    float hx = dx * 0.5f * s;
    float hy = dy * 0.5f * s;
    float hz = dz * 0.5f * s;
    return sqrtf(hx * hx + hy * hy + hz * hz);
}

static void log_load(bool found, int corners, bool capped, bool crossed) {
    if (!found) {
        printf("[Mesh] plots/mesh.obj was not found.\n");
        fflush(stdout);
        return;
    }
    if (crossed) {
        printf("[Mesh] A face crosses itself. That face was left out.\n");
        fflush(stdout);
    }
    if (capped) {
        printf("[Mesh] Triangle cap is %u. The rest was left out.\n", k_obj_tri_cap);
        fflush(stdout);
    }
    if (corners < 3) {
        printf("[Mesh] plots/mesh.obj has no faces.\n");
        fflush(stdout);
        return;
    }
    printf("[Mesh] %u triangles from plots/mesh.obj clusterized into 16B/vert meshlets.\n",
           (uint32_t)corners / 3u);
    fflush(stdout);
}

static bool load_file(RingCqeFn on_cqe, void *on_user, bool *has) {
    ObjCorner *cpu = malloc((size_t)k_slice_corners * sizeof(*cpu));
    if (cpu == nullptr) {
        if (has != nullptr) {
            *has = s_count >= 3u;
        }
        return false;
    }

    bool found = false;
    bool capped = false;
    bool crossed = false;
    int corners = plot_file_load_mesh(cpu, k_slice_corners, &found, &capped, &crossed,
                                      on_cqe, on_user);
    if (corners < 0) {
        free(cpu);
        if (has != nullptr) {
            *has = s_count >= 3u;
        }
        return false;
    }
    log_load(found, corners, capped, crossed);

    uint32_t n = corners >= 3 ? (uint32_t)corners : 0u;
    if (n > k_slice_corners) {
        n = k_slice_corners;
    }
    float next_radius = n > 0u ? fit_corners_local(cpu, n) : 0.0f;
    uint64_t next_hash = n > 0u ? hash_corners_fnv1a(cpu, n) : 0ull;

    if (n == s_count && next_hash == s_hash &&
        (n == 0u || s_mesh_id != K_SCENE_INVALID_MESH)) {
        free(cpu);
        if (has != nullptr) {
            *has = s_count >= 3u;
        }
        return false;
    }

    bool ok = true;
    if (s_mesh_id == K_SCENE_INVALID_MESH) {
        if (n > 0u) {
            s_mesh_id = scene_register_mesh((const SceneCorner *)cpu, n, 0.42f);
            ok = (s_mesh_id != K_SCENE_INVALID_MESH);
        }
    } else {
        ok = (scene_update_mesh(s_mesh_id, n > 0u ? (const SceneCorner *)cpu : nullptr,
                                n, 0.42f) == 0);
    }
    free(cpu);

    if (!ok) {
        if (has != nullptr) {
            *has = s_count >= 3u;
        }
        return false;
    }

    s_count  = n;
    s_hash   = next_hash;
    s_radius = next_radius;
    s_gen++;
    if (has != nullptr) {
        *has = s_count >= 3u;
    }
    return true;
}

static bool mesh_ensure_ready(void) {
    if (s_mesh_ready) {
        return true;
    }
    if (s_vk == nullptr || s_vk->device == VK_NULL_HANDLE) {
        return false;
    }
    if (!plot_file_mesh_check()) {
        fprintf(stderr, "[Mesh Error] Triangulation self-check failed\n");
    }
    s_mesh_ready = true;
    (void)load_file(s_init_cqe, s_init_user, nullptr);
    if (!s_logged) {
        printf("[Mesh] OBJ faces are a mesh. L reloads plots/mesh.obj.\n");
        fflush(stdout);
        s_logged = true;
    }
    return true;
}

int mesh_init(VulkanContext *vk, RingCqeFn on_cqe, void *on_user) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || vk->physical_device == VK_NULL_HANDLE) {
        return -1;
    }
    s_vk         = vk;
    s_init_cqe   = on_cqe;
    s_init_user  = on_user;
    s_mesh_ready = false;
    s_mesh_id    = K_SCENE_INVALID_MESH;
    s_count      = 0u;
    s_hash       = 0ull;
    s_radius     = 0.0f;
    return 0;
}

void mesh_shutdown(VulkanContext *vk) {
    (void)vk;
    s_vk         = nullptr;
    s_init_cqe   = nullptr;
    s_init_user  = nullptr;
    s_mesh_ready = false;
    s_mesh_id    = K_SCENE_INVALID_MESH;
    s_count      = 0u;
    s_hash       = 0ull;
    s_radius     = 0.0f;
}

bool mesh_reload(RingCqeFn on_cqe, void *on_user, bool *has) {
    if (!s_mesh_ready && !mesh_ensure_ready()) {
        if (has != nullptr) {
            *has = false;
        }
        return false;
    }
    return load_file(on_cqe, on_user, has);
}

bool mesh_has_triangles(void) {
    if (!s_mesh_ready && s_vk != nullptr) {
        (void)mesh_ensure_ready();
    }
    return s_count >= 3u;
}

uint32_t mesh_vertex_count(void) {
    return s_count;
}

bool mesh_sphere(float *cx, float *cy, float *radius) {
    if (!s_mesh_ready && s_vk != nullptr) {
        (void)mesh_ensure_ready();
    }
    if (s_count < 3u || s_radius <= 0.0f) {
        return false;
    }
    if (cx != nullptr) {
        *cx = k_fit_x;
    }
    if (cy != nullptr) {
        *cy = k_fit_y;
    }
    if (radius != nullptr) {
        *radius = s_radius;
    }
    return true;
}

bool mesh_wants_frame(void) {
    return s_gen != s_shown_gen;
}

void mesh_commit(void) {
    s_shown_gen = s_gen;
}

void mesh_submit_scene(float yaw_deg, bool selected) {
    if (!mesh_ensure_ready() || s_count < 3u || s_mesh_id == K_SCENE_INVALID_MESH) {
        return;
    }
    const float k_deg_to_rad = 0.017453292519943295f;
    scene_submit_instance(s_mesh_id,
                          k_fit_x, k_fit_y, 0.0f,
                          1.0f,
                          0.0f,
                          yaw_deg * k_deg_to_rad,
                          0.0f,
                          0.42f,
                          selected ? 1.0f : 0.0f,
                          0.0f,
                          k_mat_mesh,
                          0u);
}
