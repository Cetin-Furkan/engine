#include "vulkan/pipe.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Cache file: magic, vendor, device, pipelineCacheUUID, then the blob.
// A mismatch starts an empty cache. A missing file is the same.

enum : uint32_t { k_pipe_magic = 0x43504741u }; // 'AGPC' little-endian

typedef struct PipeHeader {
    uint32_t magic;
    uint32_t vendor;
    uint32_t device;
    uint8_t  uuid[VK_UUID_SIZE];
} PipeHeader;

static_assert(sizeof(PipeHeader) == 28u, "pipeline cache header");

// One bank slot is one VkPipeline. The key is the SPIR-V, the layout, and
// the fixed state. Two draws of the same situation share the pipeline.
// A full bank fails the create. In-use pipelines are not evicted.
enum : uint32_t {
    k_bank_cap   = 64u,
    k_spec_bytes = 32u,
    k_spec_maps  = 4u,
};

typedef struct PipeSlot {
    bool                  used;
    bool                  compute;
    uint32_t              ref;
    uint64_t              hash;
    const void           *code_a;
    size_t                code_a_bytes;
    const void           *code_b;
    size_t                code_b_bytes;
    VkPipelineLayout      layout;
    VkFormat              color;
    VkFormat              depth;
    VkSampleCountFlagBits samples;
    VkCullModeFlags       cull;
    VkBool32              depth_test;
    VkBool32              depth_write;
    VkCompareOp           compare;
    VkBool32              blend;
    uint32_t              spec_bytes;
    uint32_t              spec_maps;
    uint8_t               spec[k_spec_bytes];
    VkSpecializationMapEntry maps[k_spec_maps];
    VkPipeline            pipeline;
} PipeSlot;

static PipeSlot s_bank[k_bank_cap];

static void bank_drop_all(VulkanContext *vk);

static void cache_path(char *dst, size_t cap) {
    const char *base = getenv("XDG_CACHE_HOME");
    if (base != nullptr && base[0] != '\0') {
        snprintf(dst, cap, "%s/engine-v2/engine.pipeline", base);
        return;
    }
    const char *home = getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
        snprintf(dst, cap, "%s/.cache/engine-v2/engine.pipeline", home);
        return;
    }
    snprintf(dst, cap, "/tmp/engine-v2.pipeline");
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
    if (n == 0) {
        return;
    }
    for (size_t i = 1; i < n; ++i) {
        if (tmp[i] != '/') {
            continue;
        }
        tmp[i] = '\0';
        mkdir(tmp, 0700);
        tmp[i] = '/';
    }
    mkdir(tmp, 0700);
}

static bool header_matches(const PipeHeader *hdr, const VkPhysicalDeviceProperties *props) {
    return hdr->magic == k_pipe_magic &&
           hdr->vendor == props->vendorID &&
           hdr->device == props->deviceID &&
           memcmp(hdr->uuid, props->pipelineCacheUUID, VK_UUID_SIZE) == 0;
}

void vulkan_pipe_cache_open(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || vk->pipe_cache != VK_NULL_HANDLE) {
        return;
    }
    VkPhysicalDeviceProperties props = {0};
    vkGetPhysicalDeviceProperties(vk->physical_device, &props);

    void *blob = nullptr;
    size_t blob_bytes = 0;
    char path[512];
    cache_path(path, sizeof path);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) == 0 && (size_t)st.st_size > sizeof(PipeHeader)) {
            PipeHeader hdr = {0};
            if (read(fd, &hdr, sizeof hdr) == (ssize_t)sizeof hdr && header_matches(&hdr, &props)) {
                blob_bytes = (size_t)st.st_size - sizeof hdr;
                blob = malloc(blob_bytes);
                if (blob != nullptr) {
                    if (read(fd, blob, blob_bytes) != (ssize_t)blob_bytes) {
                        free(blob);
                        blob = nullptr;
                        blob_bytes = 0;
                    }
                }
            }
        }
        close(fd);
    }

    VkPipelineCacheCreateInfo info = {
        .sType           = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
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
    if (a_n == 0u) {
        return true;
    }
    if (a == b) {
        return true;
    }
    if (a == nullptr || b == nullptr) {
        return false;
    }
    return memcmp(a, b, a_n) == 0;
}

static uint64_t fnv_mem(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; ++i) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
    return h;
}

static bool spec_store(const VkSpecializationInfo *spec, PipeSlot *slot) {
    slot->spec_bytes = 0u;
    slot->spec_maps = 0u;
    if (spec == nullptr || spec->dataSize == 0u) {
        return true;
    }
    if (spec->pData == nullptr || spec->dataSize > k_spec_bytes || spec->mapEntryCount > k_spec_maps) {
        fprintf(stderr, "[Vulkan Error] Specialization is %zu bytes, the bank holds %u\n",
                spec->dataSize, k_spec_bytes);
        return false;
    }
    memcpy(slot->spec, spec->pData, spec->dataSize);
    slot->spec_bytes = (uint32_t)spec->dataSize;
    if (spec->mapEntryCount > 0u && spec->pMapEntries != nullptr) {
        memcpy(slot->maps, spec->pMapEntries, spec->mapEntryCount * sizeof(VkSpecializationMapEntry));
        slot->spec_maps = spec->mapEntryCount;
    }
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

static uint64_t hash_key(bool compute, const void *a, size_t a_n, const void *b, size_t b_n,
                         VkPipelineLayout layout, VkFormat color, VkFormat depth,
                         VkSampleCountFlagBits samples, VkCullModeFlags cull,
                         VkBool32 depth_test, VkBool32 depth_write, VkCompareOp compare,
                         VkBool32 blend, const PipeSlot *spec_slot) {
    uint64_t h = 14695981039346656037ull;
    uint32_t flag = compute ? 1u : 0u;
    h = fnv_mem(h, &flag, sizeof flag);
    h = fnv_mem(h, a, a_n);
    h = fnv_mem(h, b, b_n);
    h = fnv_mem(h, &layout, sizeof layout);
    h = fnv_mem(h, &color, sizeof color);
    h = fnv_mem(h, &depth, sizeof depth);
    h = fnv_mem(h, &samples, sizeof samples);
    h = fnv_mem(h, &cull, sizeof cull);
    h = fnv_mem(h, &depth_test, sizeof depth_test);
    h = fnv_mem(h, &depth_write, sizeof depth_write);
    h = fnv_mem(h, &compare, sizeof compare);
    h = fnv_mem(h, &blend, sizeof blend);
    if (spec_slot != nullptr && spec_slot->spec_bytes > 0u) {
        h = fnv_mem(h, spec_slot->spec, spec_slot->spec_bytes);
        h = fnv_mem(h, spec_slot->maps, spec_slot->spec_maps * sizeof(VkSpecializationMapEntry));
    }
    return h;
}

static PipeSlot *bank_find(uint64_t hash, bool compute, const void *a, size_t a_n,
                           const void *b, size_t b_n, VkPipelineLayout layout,
                           VkFormat color, VkFormat depth, VkSampleCountFlagBits samples,
                           VkCullModeFlags cull, VkBool32 depth_test, VkBool32 depth_write,
                           VkCompareOp compare, VkBool32 blend, const VkSpecializationInfo *spec) {
    for (uint32_t i = 0u; i < k_bank_cap; ++i) {
        PipeSlot *slot = &s_bank[i];
        if (!slot->used || slot->hash != hash || slot->compute != compute) {
            continue;
        }
        if (slot->layout != layout || slot->color != color || slot->depth != depth ||
            slot->samples != samples || slot->cull != cull || slot->depth_test != depth_test ||
            slot->depth_write != depth_write || slot->compare != compare || slot->blend != blend) {
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
    VkShaderModuleCreateInfo info = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = bytes,
        .pCode    = (const uint32_t *)code,
    };
    return info;
}

static VkPipeline create_graphics(VulkanContext *vk, const VulkanGfxPipe *info, VkFormat format,
                                  const VkSpecializationInfo *spec) {
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
        .viewportCount = 1,
        .scissorCount  = 1,
    };
    VkSampleCountFlagBits samples = info->samples == 0 ? VK_SAMPLE_COUNT_1_BIT : info->samples;
    VkCompareOp compare = info->depth_compare == 0 ? VK_COMPARE_OP_LESS : info->depth_compare;
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode    = info->cull,
        .frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth   = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo sample_state = {
        .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = samples,
    };
    VkPipelineDepthStencilStateCreateInfo depth = {
        .sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable  = info->depth_test,
        .depthWriteEnable = info->depth_write,
        .depthCompareOp   = compare,
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
        .attachmentCount = 1,
        .pAttachments    = &blend_att,
    };
    VkDynamicState dynamics[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic = {
        .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates    = dynamics,
    };
    VkPipelineRenderingCreateInfo rendering = {
        .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount    = 1,
        .pColorAttachmentFormats = &format,
        .depthAttachmentFormat   = info->depth_format,
    };
    VkGraphicsPipelineCreateInfo pipe = {
        .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext               = &rendering,
        .stageCount          = 2,
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
    VkResult res = vkCreateGraphicsPipelines(vk->device, vk->pipe_cache, 1, &pipe, nullptr, &out);
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
    VkResult res = vkCreateComputePipelines(vk->device, vk->pipe_cache, 1, &pipe, nullptr, &out);
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
    VkCompareOp compare = info->depth_compare == 0 ? VK_COMPARE_OP_LESS : info->depth_compare;
    PipeSlot spec_tmp = {0};
    if (!spec_store(spec, &spec_tmp)) {
        return VK_NULL_HANDLE;
    }
    uint64_t hash = hash_key(false, info->vert, info->vert_bytes, info->frag, info->frag_bytes,
                             info->layout, format, info->depth_format, samples, info->cull,
                             info->depth_test, info->depth_write, compare, info->blend, &spec_tmp);
    PipeSlot *hit = bank_find(hash, false, info->vert, info->vert_bytes, info->frag, info->frag_bytes,
                              info->layout, format, info->depth_format, samples, info->cull,
                              info->depth_test, info->depth_write, compare, info->blend, spec);
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
    slot->used = true;
    slot->compute = false;
    slot->ref = 1u;
    slot->hash = hash;
    slot->code_a = info->vert;
    slot->code_a_bytes = info->vert_bytes;
    slot->code_b = info->frag;
    slot->code_b_bytes = info->frag_bytes;
    slot->layout = info->layout;
    slot->color = format;
    slot->depth = info->depth_format;
    slot->samples = samples;
    slot->cull = info->cull;
    slot->depth_test = info->depth_test;
    slot->depth_write = info->depth_write;
    slot->compare = compare;
    slot->blend = info->blend;
    slot->pipeline = pipe;
    printf("[Vulkan] Graphics pipeline created (color %d, depth %d)\n", (int)format, (int)info->depth_format);
    fflush(stdout);
    return pipe;
}

static VkPipeline bank_hold_compute(VulkanContext *vk, const void *spv, size_t bytes,
                                    VkPipelineLayout layout) {
    uint64_t hash = hash_key(true, spv, bytes, nullptr, 0u, layout, VK_FORMAT_UNDEFINED,
                             VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT, 0, VK_FALSE, VK_FALSE,
                             VK_COMPARE_OP_NEVER, VK_FALSE, nullptr);
    PipeSlot *hit = bank_find(hash, true, spv, bytes, nullptr, 0u, layout, VK_FORMAT_UNDEFINED,
                              VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT, 0, VK_FALSE, VK_FALSE,
                              VK_COMPARE_OP_NEVER, VK_FALSE, nullptr);
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
    *slot = (PipeSlot){0};
    slot->used = true;
    slot->compute = true;
    slot->ref = 1u;
    slot->hash = hash;
    slot->code_a = spv;
    slot->code_a_bytes = bytes;
    slot->layout = layout;
    slot->color = VK_FORMAT_UNDEFINED;
    slot->depth = VK_FORMAT_UNDEFINED;
    slot->samples = VK_SAMPLE_COUNT_1_BIT;
    slot->compare = VK_COMPARE_OP_NEVER;
    slot->pipeline = pipe;
    printf("[Vulkan] Compute pipeline created\n");
    fflush(stdout);
    return pipe;
}

void vulkan_pipe_cache_close(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE) {
        return;
    }
    if (vk->pipe_cache != VK_NULL_HANDLE) {
        size_t bytes = 0;
        VkResult res = vkGetPipelineCacheData(vk->device, vk->pipe_cache, &bytes, nullptr);
        if (res == VK_SUCCESS && bytes > 0) {
            void *blob = malloc(bytes);
            if (blob != nullptr) {
                res = vkGetPipelineCacheData(vk->device, vk->pipe_cache, &bytes, blob);
                if (res == VK_SUCCESS) {
                    VkPhysicalDeviceProperties props = {0};
                    vkGetPhysicalDeviceProperties(vk->physical_device, &props);
                    char dir[512];
                    char path[512];
                    cache_dir(dir, sizeof dir);
                    cache_path(path, sizeof path);
                    mkdir_p(dir);
                    char tmp[540];
                    snprintf(tmp, sizeof tmp, "%s.tmp", path);
                    int out_fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
                    if (out_fd >= 0) {
                        PipeHeader hdr = {
                            .magic  = k_pipe_magic,
                            .vendor = props.vendorID,
                            .device = props.deviceID,
                        };
                        memcpy(hdr.uuid, props.pipelineCacheUUID, VK_UUID_SIZE);
                        bool ok = write(out_fd, &hdr, sizeof hdr) == (ssize_t)sizeof hdr &&
                                  write(out_fd, blob, bytes) == (ssize_t)bytes;
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

int vulkan_gfx_pipelines(VulkanContext *vk, const VulkanGfxPipe *info,
                         const VkFormat *formats, const VkSpecializationInfo *const *specs,
                         uint32_t count, VkPipeline *out) {
    if (vk == nullptr || info == nullptr || formats == nullptr || out == nullptr ||
        count == 0u || count > 4u || info->vert == nullptr || info->frag == nullptr ||
        info->layout == VK_NULL_HANDLE) {
        return -1;
    }
    for (uint32_t i = 0u; i < count; ++i) {
        out[i] = VK_NULL_HANDLE;
    }
    for (uint32_t i = 0u; i < count; ++i) {
        const VkSpecializationInfo *spec = info->frag_spec;
        if (specs != nullptr) {
            spec = specs[i];
        }
        out[i] = bank_hold_graphics(vk, info, formats[i], spec);
        if (out[i] == VK_NULL_HANDLE) {
            for (uint32_t j = 0u; j < i; ++j) {
                vulkan_pipe_release(vk, out[j]);
                out[j] = VK_NULL_HANDLE;
            }
            return -1;
        }
    }
    return 0;
}

VkPipeline vulkan_compute_pipeline(VulkanContext *vk, const void *spv, size_t bytes,
                                   VkPipelineLayout layout) {
    if (vk == nullptr || spv == nullptr || bytes == 0 || layout == VK_NULL_HANDLE) {
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
    live->info.frag_spec = spec;
    live->format = format;
    live->spec = spec;
    live->armed = true;
}

VkPipeline vulkan_gfx_use(VulkanContext *vk, VulkanGfxLive *live) {
    if (live == nullptr || !live->armed || live->failed) {
        return VK_NULL_HANDLE;
    }
    if (live->pipeline != VK_NULL_HANDLE) {
        return live->pipeline;
    }
    VkPipeline out = VK_NULL_HANDLE;
    const VkSpecializationInfo *specs[1] = { live->spec };
    if (vulkan_gfx_pipelines(vk, &live->info, &live->format, live->spec != nullptr ? specs : nullptr,
                             1u, &out) != 0) {
        live->failed = true;
        return VK_NULL_HANDLE;
    }
    live->pipeline = out;
    return out;
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
