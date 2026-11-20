// Slang 2026 1x R32_UINT Visibility Buffer Fragment Shader
// Writes 32-bit packed ((instance_id + 1) : 11) | (meshlet_id : 14) | (prim_id : 7).
// Supports ALPHA_MASK cutout discard and two-sided / mirrored single-sided backface discard.

#include "scene_common.slang"
#include "pbr_bindless.slang"

[[vk::push_constant]] SceneDrawPush pc;

struct VisPSIn {
    [[vk::location(0)]] nointerpolation uint vis_id       : TEXCOORD0;
    [[vk::location(1)]] float2               v_uv         : TEXCOORD1;
    [[vk::location(2)]] float                v_alpha      : TEXCOORD2;
    [[vk::location(3)]] nointerpolation uint v_pbr_mat_id : TEXCOORD3;
    [[vk::location(4)]] nointerpolation uint v_inst_flags : TEXCOORD4;
};

[shader("fragment")]
uint main(VisPSIn pin, bool is_front : SV_IsFrontFace) : SV_Target0 {
    if ((pc.draw_flags & 1u) != 0u) {
        bool eff_front = is_front;
        if ((pin.v_inst_flags & k_scene_flag_flip_winding) != 0u) {
            eff_front = !eff_front;
        }
        bool two_sided = (pin.v_inst_flags & k_scene_flag_two_sided) != 0u;
        if (!two_sided && !eff_front) {
            discard;
        }
    }

    if ((pin.v_inst_flags & k_scene_flag_alpha_mask) != 0u &&
        pin.v_pbr_mat_id != 0u && pc.pbr_mats_bda != 0ull) {
        PbrMaterialGpu mat = ((PbrMaterialGpu *)pc.pbr_mats_bda)[pin.v_pbr_mat_id];
        float alpha = mat.base_color_factor.a * pin.v_alpha;
        if ((mat.flags & k_pbr_flag_has_base_tex) != 0u) {
            float2 uv = pbr_transform_uv(pin.v_uv, mat);
            alpha *= pbr_sample_tex(mat.tex_base, mat.samp_base, uv).a;
        }
        if (alpha < mat.alpha_cutoff) {
            discard;
        }
    }

    return pin.vis_id;
}
