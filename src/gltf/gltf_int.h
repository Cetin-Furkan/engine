#pragma once

#include "gltf/gltf.h"
#include <stddef.h>
#include <stdint.h>

enum : uint32_t {
    K_GLTF_COMP_I8  = 5120u,
    K_GLTF_COMP_U8  = 5121u,
    K_GLTF_COMP_I16 = 5122u,
    K_GLTF_COMP_U16 = 5123u,
    K_GLTF_COMP_U32 = 5125u,
    K_GLTF_COMP_F32 = 5126u,
};

typedef struct GltfBufData {
    uint8_t *bytes;
    size_t   len;
    bool     owned;
} GltfBufData;

typedef struct GltfBufView {
    uint32_t buffer;
    size_t   byte_offset;
    size_t   byte_length;
    size_t   byte_stride;
} GltfBufView;

typedef struct GltfAccessor {
    int32_t  buffer_view;
    size_t   byte_offset;
    uint32_t component_type;
    uint32_t count;
    uint32_t num_comps;
    bool     normalized;
    bool     has_sparse;
    uint32_t sparse_count;
    int32_t  sparse_idx_bv;
    size_t   sparse_idx_off;
    uint32_t sparse_idx_ctype;
    int32_t  sparse_val_bv;
    size_t   sparse_val_off;
} GltfAccessor;

typedef struct GltfSamplerInfo {
    TextureWrap   wrap_s;
    TextureWrap   wrap_t;
    TextureFilter filter;
} GltfSamplerInfo;

typedef struct GltfTexRef {
    int32_t source;
    int32_t sampler;
} GltfTexRef;

uint8_t *gltf_decode_base64(const char *src, size_t len, size_t *out_len);
void     gltf_resolve_uri_path(const char *base_dir, const char *uri, char *out, size_t cap);
uint32_t gltf_type_comps(const char *type_str);
float   *gltf_decode_accessor_f32(const GltfAccessor *acc,
                                  const GltfBufView *bvs, uint32_t bv_count,
                                  const GltfBufData *bufs, uint32_t buf_count);
uint32_t *gltf_decode_accessor_indices(const GltfAccessor *acc,
                                       const GltfBufView *bvs, uint32_t bv_count,
                                       const GltfBufData *bufs, uint32_t buf_count);
void     gltf_decompose_colmajor_mat4(const float cm[16],
                                      float out_t[3], float out_q[4], float out_s[3],
                                      bool *out_neg_det);
void     gltf_compute_scene_zup_bounds(GltfAsset *asset);
