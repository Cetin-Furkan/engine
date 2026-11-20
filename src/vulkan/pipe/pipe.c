#include "vulkan/pipe.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Cache file: magic, vendor, device, pipelineCacheUUID, then the blob.
// A mismatch starts an empty cache. A missing file is the same.

enum : uint32_t {
    k_pipe_magic = 0x43504741u, // 'AGPC' little-endian
    k_bank_cap   = 128u,
    k_spec_bytes = VULKAN_GFX_SPEC_BYTES,
    k_spec_maps  = VULKAN_GFX_SPEC_MAPS,
};

typedef struct PipeHeader {
    uint32_t magic;
    uint32_t vendor;
    uint32_t device;
    uint8_t  uuid[VK_UUID_SIZE];
} PipeHeader;

static_assert(sizeof(PipeHeader) == 28u, "pipeline cache header");

// One bank slot is one compiled VkPipeline. Cull mode and depth test/write/compare
// are Vulkan 1.4 Extended Dynamic State, so PSOs only key on SPIR-V, layout,
// attachment formats, MSAA samples, blend, depth-bias enable, and specialization.
typedef struct PipeSlot {
    bool                     used;
    bool                     compute;
    bool                     depth_bias;
    uint32_t                 ref;
    uint64_t                 hash;
    const void              *code_a;
    size_t                   code_a_bytes;
    const void              *code_b;
    size_t                   code_b_bytes;
    VkPipelineLayout         layout;
    VkFormat                 color;
    VkFormat                 depth;
    VkSampleCountFlagBits    samples;
    VkBool32                 blend;
    uint32_t                 spec_bytes;
    uint32_t                 spec_maps;
    uint8_t                  spec[k_spec_bytes];
    VkSpecializationMapEntry maps[k_spec_maps];
    VkPipeline               pipeline;
} PipeSlot;

static PipeSlot s_bank[k_bank_cap];

static void cache_path(char *dst, size_t cap) {
    const char *base = getenv("XDG_CACHE_HOME");
    if (base != nullptr && base[0] != '\0') {
        snprintf(dst, cap, "%s/engine/engine.pipeline", base);
        return;
    }
    const char *home = getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
        snprintf(dst, cap, "%s/.cache/engine/engine.pipeline", home);
        return;
    }
    snprintf(dst, cap, "/tmp/engine.pipeline");
}

static bool read_full(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    size_t got = 0u;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (r == 0) {
            return false;
        }
        got += (size_t)r;
    }
    return true;
}

static bool write_full(int fd, const void *buf, size_t n) {
    const uint8_t *p = buf;
    size_t sent = 0u;
    while (sent < n) {
        ssize_t w = write(fd, p + sent, n - sent);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        sent += (size_t)w;
    }
    return true;
}

static void cache_dir(char *dst, size_t cap) {
    char path[512];
    cache_path(path, sizeof path);
    snprintf(dst, cap, "%s", path);
    char *slash = strrchr(dst, '/');
    if (slash != nullptr) {
        *slash = '\0';
    }
}

static void mkdir_p(const char *dir) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", dir);
    size_t n = strlen(tmp);
    if (n == 0u) {
        return;
    }
    for (size_t i = 1u; i < n; ++i) {
        if (tmp[i] != '/') {
            continue;
        }
        tmp[i] = '\0';
        mkdir(tmp, 0700);
        tmp[i] = '/';
    }
    mkdir(tmp, 0700);
}

static bool header_matches(const PipeHeader *hdr, const VulkanDeviceInfo *info) {
    return hdr->magic == k_pipe_magic &&
           hdr->vendor == info->vendor_id &&
           hdr->device == info->device_id &&
           memcmp(hdr->uuid, info->pipeline_cache_uuid, VK_UUID_SIZE) == 0;
}

void vulkan_pipe_cache_open(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || vk->pipe_cache != VK_NULL_HANDLE) {
        return;
    }

    void *blob = nullptr;
    size_t blob_bytes = 0u;
    char path[512];
    cache_path(path, sizeof path);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) == 0 && (size_t)st.st_size > sizeof(PipeHeader)) {
            PipeHeader hdr = {0};
            if (read_full(fd, &hdr, sizeof hdr) && header_matches(&hdr, &vk->device_info)) {
                blob_bytes = (size_t)st.st_size - sizeof hdr;
                blob = malloc(blob_bytes);
                if (blob != nullptr) {
                    if (!read_full(fd, blob, blob_bytes)) {
                        free(blob);
                        blob = nullptr;
                        blob_bytes = 0u;
                    }
                }
            }
        }
        close(fd);
    }

    VkPipelineCacheCreateInfo info = {
        .sType           = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        .flags           = vk->device_info.supports_pipeline_cache_control
                               ? VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT
                               : 0u,
        .initialDataSize = blob_bytes,
        .pInitialData    = blob,
    };
    VkResult res = vkCreatePipelineCache(vk->device, &info, nullptr, &vk->pipe_cache);
    free(blob);
    if (res != VK_SUCCESS) {
        vk->pipe_cache = VK_NULL_HANDLE;
        fprintf(stderr, "[Vulkan Warn] Pipeline cache was not created: %s (%d)\n",
                vulkan_vkresult_to_str(res), (int)res);
    }
}

static bool bytes_match(const void *a, size_t a_n, const void *b, size_t b_n) {
    if (a_n != b_n) {
        return false;
    }
    if (a_n == 0u || a == b) {
        return true;
    }
    if (a == nullptr || b == nullptr) {
        return false;
    }
    return memcmp(a, b, a_n) == 0;
}

static uint64_t fnv_mem(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0u; i < n; ++i) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
    return h;
}

static bool spec_valid(const VkSpecializationInfo *spec) {
    if (spec == nullptr || spec->dataSize == 0u) {
        return spec == nullptr || spec->mapEntryCount == 0u;
    }
    if (spec->pData == nullptr || spec->dataSize > k_spec_bytes ||
        spec->mapEntryCount == 0u || spec->mapEntryCount > k_spec_maps ||
        spec->pMapEntries == nullptr) {
        return false;
    }
    for (uint32_t i = 0u; i < spec->mapEntryCount; ++i) {
        const VkSpecializationMapEntry *e = &spec->pMapEntries[i];
        if (e->offset > spec->dataSize || e->size == 0u || e->size > spec->dataSize - e->offset) {
            return false;
        }
    }
    return true;
}

static bool spec_store(const VkSpecializationInfo *spec, PipeSlot *slot) {
    slot->spec_bytes = 0u;
    slot->spec_maps = 0u;
    if (spec == nullptr || (spec->dataSize == 0u && spec->mapEntryCount == 0u)) {
        return true;
    }
    if (!spec_valid(spec)) {
        fprintf(stderr, "[Vulkan Error] Invalid specialization (%zu bytes, %u map entries; bank max %u/%u)\n",
                spec->dataSize, spec->mapEntryCount, k_spec_bytes, k_spec_maps);
        return false;
    }
    memcpy(slot->spec, spec->pData, spec->dataSize);
    slot->spec_bytes = (uint32_t)spec->dataSize;
    memcpy(slot->maps, spec->pMapEntries, spec->mapEntryCount * sizeof(VkSpecializationMapEntry));
    slot->spec_maps = spec->mapEntryCount;
    return true;
}

static bool spec_match(const PipeSlot *slot, const VkSpecializationInfo *spec) {
    uint32_t bytes = 0u;
    uint32_t maps = 0u;
    const void *data = nullptr;
    const VkSpecializationMapEntry *entries = nullptr;
    if (spec != nullptr && spec->dataSize > 0u) {
        bytes = (uint32_t)spec->dataSize;
        maps = spec->mapEntryCount;
        data = spec->pData;
        entries = spec->pMapEntries;
    }
    if (slot->spec_bytes != bytes || slot->spec_maps != maps) {
        return false;
    }
    if (bytes > 0u && (data == nullptr || memcmp(slot->spec, data, bytes) != 0)) {
        return false;
    }
    if (maps > 0u && (entries == nullptr ||
                      memcmp(slot->maps, entries, maps * sizeof(VkSpecializationMapEntry)) != 0)) {
        return false;
    }
    return true;
}

static uint64_t hash_key(bool compute, bool depth_bias, const void *a, size_t a_n,
                         const void *b, size_t b_n, VkPipelineLayout layout,
                         VkFormat color, VkFormat depth, VkSampleCountFlagBits samples,
                         VkBool32 blend, const PipeSlot *spec_slot) {
    uint64_t h = 14695981039346656037ull;
    uint32_t flags = (compute ? 1u : 0u) | (depth_bias ? 2u : 0u);
    h = fnv_mem(h, &flags, sizeof flags);
    h = fnv_mem(h, a, a_n);
    h = fnv_mem(h, b, b_n);
    h = fnv_mem(h, &layout, sizeof layout);
    h = fnv_mem(h, &color, sizeof color);
    h = fnv_mem(h, &depth, sizeof depth);
    h = fnv_mem(h, &samples, sizeof samples);
    h = fnv_mem(h, &blend, sizeof blend);
    if (spec_slot != nullptr && spec_slot->spec_bytes > 0u) {
        h = fnv_mem(h, spec_slot->spec, spec_slot->spec_bytes);
        h = fnv_mem(h, spec_slot->maps, spec_slot->spec_maps * sizeof(VkSpecializationMapEntry));
    }
    return h;
}

static PipeSlot *bank_find(uint64_t hash, bool compute, bool depth_bias, const void *a, size_t a_n,
                           const void *b, size_t b_n, VkPipelineLayout layout,
                           VkFormat color, VkFormat depth, VkSampleCountFlagBits samples,
                           VkBool32 blend, const VkSpecializationInfo *spec) {
    for (uint32_t i = 0u; i < k_bank_cap; ++i) {
        PipeSlot *slot = &s_bank[i];
        if (!slot->used || slot->hash != hash || slot->compute != compute ||
            slot->depth_bias != depth_bias) {
            continue;
        }
        if (slot->layout != layout || slot->color != color || slot->depth != depth ||
            slot->samples != samples || slot->blend != blend) {
            continue;
        }
        if (!bytes_match(slot->code_a, slot->code_a_bytes, a, a_n) ||
            !bytes_match(slot->code_b, slot->code_b_bytes, b, b_n) ||
            !spec_match(slot, spec)) {
            continue;
        }
        return slot;
    }
    return nullptr;
}

static PipeSlot *bank_empty(void) {
    for (uint32_t i = 0u; i < k_bank_cap; ++i) {
        if (!s_bank[i].used) {
            return &s_bank[i];
        }
    }
    fprintf(stderr, "[Vulkan Error] Pipeline bank is full (%u)\n", k_bank_cap);
    return nullptr;
}

static void bank_drop_all(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE) {
        return;
    }
    for (uint32_t i = 0u; i < k_bank_cap; ++i) {
        if (!s_bank[i].used || s_bank[i].pipeline == VK_NULL_HANDLE) {
            s_bank[i] = (PipeSlot){0};
            continue;
        }
        vkDestroyPipeline(vk->device, s_bank[i].pipeline, nullptr);
        s_bank[i] = (PipeSlot){0};
    }
}

void vulkan_pipe_release(VulkanContext *vk, VkPipeline pipeline) {
    if (pipeline == VK_NULL_HANDLE) {
        return;
    }
    for (uint32_t i = 0u; i < k_bank_cap; ++i) {
        PipeSlot *slot = &s_bank[i];
        if (!slot->used || slot->pipeline != pipeline) {
            continue;
        }
        if (slot->ref > 0u) {
            slot->ref--;
        }
        if (slot->ref == 0u) {
            if (vk != nullptr && vk->device != VK_NULL_HANDLE) {
                vkDestroyPipeline(vk->device, slot->pipeline, nullptr);
            }
            *slot = (PipeSlot){0};
        }
        return;
    }
    fprintf(stderr, "[Vulkan Error] Pipeline release missed the bank\n");
}

static VkShaderModuleCreateInfo module_info(const void *code, size_t bytes) {
    return (VkShaderModuleCreateInfo){
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = bytes,
        .pCode    = code,
    };
}

static VkPipeline create_graphics(VulkanContext *vk, const VulkanGfxPipe *info, VkFormat format,
                                  const VkSpecializationInfo *spec) {
    const bool has_color = (format != VK_FORMAT_UNDEFINED);
    const bool has_frag  = (info->frag != nullptr && info->frag_bytes > 0u);
    VkShaderModuleCreateInfo vert_mod = module_info(info->vert, info->vert_bytes);
    VkShaderModuleCreateInfo frag_mod = module_info(info->frag, info->frag_bytes);
    VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext  = &vert_mod,
            .stage  = VK_SHADER_STAGE_VERTEX_BIT,
            .module = VK_NULL_HANDLE,
            .pName  = "main",
        },
        {
            .sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext               = &frag_mod,
            .stage               = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module              = VK_NULL_HANDLE,
            .pName               = "main",
            .pSpecializationInfo = spec,
        },
    };
    VkPipelineVertexInputStateCreateInfo vertex = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkPipelineViewportStateCreateInfo viewport = {
        .sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1u,
        .scissorCount  = 1u,
    };
    VkSampleCountFlagBits samples = info->samples == 0 ? VK_SAMPLE_COUNT_1_BIT : info->samples;
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType           = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode     = VK_POLYGON_MODE_FILL,
        .cullMode        = info->cull,
        .frontFace       = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depthBiasEnable = info->depth_bias_enable ? VK_TRUE : VK_FALSE,
        .lineWidth       = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo sample_state = {
        .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = samples,
    };
    VkPipelineDepthStencilStateCreateInfo depth = {
        .sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable  = info->depth_test,
        .depthWriteEnable = info->depth_write,
        .depthCompareOp   = VK_COMPARE_OP_LESS,
    };
    VkPipelineColorBlendAttachmentState blend_att = {
        .blendEnable         = info->blend,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp        = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp        = VK_BLEND_OP_ADD,
        .colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo blend = {
        .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = has_color ? 1u : 0u,
        .pAttachments    = has_color ? &blend_att : nullptr,
    };
    VkDynamicState dynamics[8] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_CULL_MODE,
        VK_DYNAMIC_STATE_FRONT_FACE,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_DEPTH_BIAS,
    };
    VkPipelineDynamicStateCreateInfo dynamic = {
        .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = info->depth_bias_enable ? 8u : 7u,
        .pDynamicStates    = dynamics,
    };
    VkPipelineRenderingCreateInfo rendering = {
        .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount    = has_color ? 1u : 0u,
        .pColorAttachmentFormats = has_color ? &format : nullptr,
        .depthAttachmentFormat   = info->depth_format,
    };
    VkGraphicsPipelineCreateInfo pipe = {
        .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext               = &rendering,
        .stageCount          = has_frag ? 2u : 1u,
        .pStages             = stages,
        .pVertexInputState   = &vertex,
        .pInputAssemblyState = &assembly,
        .pViewportState      = &viewport,
        .pRasterizationState = &raster,
        .pMultisampleState   = &sample_state,
        .pDepthStencilState  = &depth,
        .pColorBlendState    = &blend,
        .pDynamicState       = &dynamic,
        .layout              = info->layout,
    };
    VkPipeline out = VK_NULL_HANDLE;
    VkResult res = vkCreateGraphicsPipelines(vk->device, vk->pipe_cache, 1u, &pipe, nullptr, &out);
    if (res != VK_SUCCESS) {
        if (out != VK_NULL_HANDLE) {
            vkDestroyPipeline(vk->device, out, nullptr);
        }
        fprintf(stderr, "[Vulkan Error] vkCreateGraphicsPipelines failed: %s (%d)\n",
                vulkan_vkresult_to_str(res), (int)res);
        return VK_NULL_HANDLE;
    }
    return out;
}

static VkPipeline create_compute(VulkanContext *vk, const void *spv, size_t bytes,
                                 VkPipelineLayout layout) {
    VkShaderModuleCreateInfo mod = module_info(spv, bytes);
    VkPipelineShaderStageCreateInfo stage = {
        .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .pNext  = &mod,
        .stage  = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = VK_NULL_HANDLE,
        .pName  = "main",
    };
    VkComputePipelineCreateInfo pipe = {
        .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage  = stage,
        .layout = layout,
    };
    VkPipeline out = VK_NULL_HANDLE;
    VkResult res = vkCreateComputePipelines(vk->device, vk->pipe_cache, 1u, &pipe, nullptr, &out);
    if (res != VK_SUCCESS) {
        if (out != VK_NULL_HANDLE) {
            vkDestroyPipeline(vk->device, out, nullptr);
        }
        fprintf(stderr, "[Vulkan Error] vkCreateComputePipelines failed: %s (%d)\n",
                vulkan_vkresult_to_str(res), (int)res);
        return VK_NULL_HANDLE;
    }
    return out;
}

static VkPipeline bank_hold_graphics(VulkanContext *vk, const VulkanGfxPipe *info, VkFormat format,
                                     const VkSpecializationInfo *spec) {
    VkSampleCountFlagBits samples = info->samples == 0 ? VK_SAMPLE_COUNT_1_BIT : info->samples;
    PipeSlot spec_tmp = {0};
    if (!spec_store(spec, &spec_tmp)) {
        return VK_NULL_HANDLE;
    }
    uint64_t hash = hash_key(false, info->depth_bias_enable,
                             info->vert, info->vert_bytes, info->frag, info->frag_bytes,
                             info->layout, format, info->depth_format, samples, info->blend, &spec_tmp);
    PipeSlot *hit = bank_find(hash, false, info->depth_bias_enable,
                              info->vert, info->vert_bytes, info->frag, info->frag_bytes,
                              info->layout, format, info->depth_format, samples, info->blend, spec);
    if (hit != nullptr) {
        hit->ref++;
        return hit->pipeline;
    }
    PipeSlot *slot = bank_empty();
    if (slot == nullptr) {
        return VK_NULL_HANDLE;
    }
    VkPipeline pipe = create_graphics(vk, info, format, spec);
    if (pipe == VK_NULL_HANDLE) {
        return VK_NULL_HANDLE;
    }
    *slot = spec_tmp;
    slot->used         = true;
    slot->compute      = false;
    slot->depth_bias   = info->depth_bias_enable;
    slot->ref          = 1u;
    slot->hash         = hash;
    slot->code_a       = info->vert;
    slot->code_a_bytes = info->vert_bytes;
    slot->code_b       = info->frag;
    slot->code_b_bytes = info->frag_bytes;
    slot->layout       = info->layout;
    slot->color        = format;
    slot->depth        = info->depth_format;
    slot->samples      = samples;
    slot->blend        = info->blend;
    slot->pipeline     = pipe;
    printf("[Vulkan] Graphics pipeline created (color %d, depth %d, EDS 1.4)\n",
           (int)format, (int)info->depth_format);
    fflush(stdout);
    return pipe;
}

static VkPipeline bank_hold_compute(VulkanContext *vk, const void *spv, size_t bytes,
                                    VkPipelineLayout layout) {
    uint64_t hash = hash_key(true, false, spv, bytes, nullptr, 0u, layout, VK_FORMAT_UNDEFINED,
                             VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT, VK_FALSE, nullptr);
    PipeSlot *hit = bank_find(hash, true, false, spv, bytes, nullptr, 0u, layout, VK_FORMAT_UNDEFINED,
                              VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT, VK_FALSE, nullptr);
    if (hit != nullptr) {
        hit->ref++;
        return hit->pipeline;
    }
    PipeSlot *slot = bank_empty();
    if (slot == nullptr) {
        return VK_NULL_HANDLE;
    }
    VkPipeline pipe = create_compute(vk, spv, bytes, layout);
    if (pipe == VK_NULL_HANDLE) {
        return VK_NULL_HANDLE;
    }
    *slot = (PipeSlot){
        .used         = true,
        .compute      = true,
        .ref          = 1u,
        .hash         = hash,
        .code_a       = spv,
        .code_a_bytes = bytes,
        .layout       = layout,
        .color        = VK_FORMAT_UNDEFINED,
        .depth        = VK_FORMAT_UNDEFINED,
        .samples      = VK_SAMPLE_COUNT_1_BIT,
        .pipeline     = pipe,
    };
    printf("[Vulkan] Compute pipeline created\n");
    fflush(stdout);
    return pipe;
}

void vulkan_pipe_cache_close(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE) {
        return;
    }
    if (vk->pipe_cache != VK_NULL_HANDLE) {
        size_t bytes = 0u;
        VkResult res = vkGetPipelineCacheData(vk->device, vk->pipe_cache, &bytes, nullptr);
        if (res == VK_SUCCESS && bytes > 0u) {
            void *blob = malloc(bytes);
            if (blob != nullptr) {
                res = vkGetPipelineCacheData(vk->device, vk->pipe_cache, &bytes, blob);
                if (res == VK_SUCCESS) {
                    char dir[512];
                    char path[512];
                    cache_dir(dir, sizeof dir);
                    cache_path(path, sizeof path);
                    mkdir_p(dir);
                    char tmp[560];
                    snprintf(tmp, sizeof tmp, "%s.%d.tmp", path, (int)getpid());
                    int out_fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
                    if (out_fd >= 0) {
                        PipeHeader hdr = {
                            .magic  = k_pipe_magic,
                            .vendor = vk->device_info.vendor_id,
                            .device = vk->device_info.device_id,
                        };
                        memcpy(hdr.uuid, vk->device_info.pipeline_cache_uuid, VK_UUID_SIZE);
                        bool ok = write_full(out_fd, &hdr, sizeof hdr) &&
                                  write_full(out_fd, blob, bytes);
                        close(out_fd);
                        if (!ok || rename(tmp, path) != 0) {
                            unlink(tmp);
                        }
                    }
                }
                free(blob);
            }
        }
    }
    bank_drop_all(vk);
    if (vk->pipe_cache != VK_NULL_HANDLE) {
        vkDestroyPipelineCache(vk->device, vk->pipe_cache, nullptr);
        vk->pipe_cache = VK_NULL_HANDLE;
    }
}

VkPipeline vulkan_compute_pipeline(VulkanContext *vk, const void *spv, size_t bytes,
                                   VkPipelineLayout layout) {
    if (vk == nullptr || spv == nullptr || bytes == 0u || layout == VK_NULL_HANDLE) {
        return VK_NULL_HANDLE;
    }
    return bank_hold_compute(vk, spv, bytes, layout);
}

void vulkan_gfx_arm(VulkanGfxLive *live, const VulkanGfxPipe *info, VkFormat format,
                    const VkSpecializationInfo *spec) {
    if (live == nullptr || info == nullptr) {
        return;
    }
    *live = (VulkanGfxLive){0};
    live->info = *info;
    live->info.color_format = format;
    live->info.frag_spec = nullptr;
    live->format = format;
    live->armed = true;

    const VkSpecializationInfo *src_spec = (spec != nullptr) ? spec : info->frag_spec;
    if (src_spec != nullptr && (src_spec->dataSize > 0u || src_spec->mapEntryCount > 0u)) {
        if (!spec_valid(src_spec)) {
            fprintf(stderr, "[Vulkan Error] Invalid specialization passed to vulkan_gfx_arm\n");
            live->failed = true;
            return;
        }
        memcpy(live->spec_data, src_spec->pData, src_spec->dataSize);
        memcpy(live->spec_maps, src_spec->pMapEntries,
               src_spec->mapEntryCount * sizeof(VkSpecializationMapEntry));
        live->owned_spec = (VkSpecializationInfo){
            .mapEntryCount = src_spec->mapEntryCount,
            .pMapEntries   = live->spec_maps,
            .dataSize      = src_spec->dataSize,
            .pData         = live->spec_data,
        };
        live->has_spec = true;
    }
}

bool vulkan_gfx_bind(VulkanContext *vk, VkCommandBuffer cmd, VulkanGfxLive *live) {
    if (vk == nullptr || cmd == VK_NULL_HANDLE || live == nullptr || !live->armed || live->failed) {
        return false;
    }
    if (live->pipeline == VK_NULL_HANDLE) {
        if (live->info.vert == nullptr || live->info.layout == VK_NULL_HANDLE ||
            (live->format != VK_FORMAT_UNDEFINED && live->info.frag == nullptr)) {
            live->failed = true;
            return false;
        }
        const VkSpecializationInfo *spec = nullptr;
        if (live->has_spec) {
            live->owned_spec.pMapEntries = live->spec_maps;
            live->owned_spec.pData       = live->spec_data;
            spec                         = &live->owned_spec;
        }
        live->pipeline = bank_hold_graphics(vk, &live->info, live->format, spec);
        if (live->pipeline == VK_NULL_HANDLE) {
            live->failed = true;
            return false;
        }
    }

    VkCompareOp compare = live->info.custom_depth_compare
        ? live->info.depth_compare
        : (live->info.depth_compare == 0 ? VK_COMPARE_OP_LESS : live->info.depth_compare);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, live->pipeline);
    vkCmdSetCullMode(cmd, live->info.cull);
    vkCmdSetFrontFace(cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetDepthTestEnable(cmd, live->info.depth_test);
    vkCmdSetDepthWriteEnable(cmd, live->info.depth_write);
    vkCmdSetDepthCompareOp(cmd, compare);
    return true;
}

void vulkan_gfx_disarm(VulkanContext *vk, VulkanGfxLive *live) {
    if (live == nullptr) {
        return;
    }
    if (live->pipeline != VK_NULL_HANDLE) {
        vulkan_pipe_release(vk, live->pipeline);
    }
    *live = (VulkanGfxLive){0};
}
