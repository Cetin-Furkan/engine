#include "vulkan/pipe.h"
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

static void cache_path(char *dst, size_t cap) {
    const char *base = getenv("XDG_CACHE_HOME");
    if (base != nullptr && base[0] != '\0') {
        snprintf(dst, cap, "%s/antigravity/engine.pipeline", base);
        return;
    }
    const char *home = getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
        snprintf(dst, cap, "%s/.cache/antigravity/engine.pipeline", home);
        return;
    }
    snprintf(dst, cap, "/tmp/antigravity-engine.pipeline");
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
    FILE *in = fopen(path, "rb");
    if (in != nullptr) {
        PipeHeader hdr = {0};
        if (fread(&hdr, sizeof hdr, 1, in) == 1 && header_matches(&hdr, &props)) {
            if (fseek(in, 0, SEEK_END) == 0) {
                long end = ftell(in);
                if (end > (long)sizeof hdr) {
                    blob_bytes = (size_t)end - sizeof hdr;
                    if (fseek(in, (long)sizeof hdr, SEEK_SET) == 0) {
                        blob = malloc(blob_bytes);
                        if (blob != nullptr && fread(blob, 1, blob_bytes, in) != blob_bytes) {
                            free(blob);
                            blob = nullptr;
                            blob_bytes = 0;
                        }
                    }
                }
            }
        }
        fclose(in);
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

void vulkan_pipe_cache_close(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || vk->pipe_cache == VK_NULL_HANDLE) {
        return;
    }
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
                FILE *out = fopen(tmp, "wb");
                if (out != nullptr) {
                    PipeHeader hdr = {
                        .magic  = k_pipe_magic,
                        .vendor = props.vendorID,
                        .device = props.deviceID,
                    };
                    memcpy(hdr.uuid, props.pipelineCacheUUID, VK_UUID_SIZE);
                    bool ok = fwrite(&hdr, sizeof hdr, 1, out) == 1 &&
                              fwrite(blob, 1, bytes, out) == bytes;
                    fclose(out);
                    if (!ok || rename(tmp, path) != 0) {
                        unlink(tmp);
                    }
                }
            }
            free(blob);
        }
    }
    vkDestroyPipelineCache(vk->device, vk->pipe_cache, nullptr);
    vk->pipe_cache = VK_NULL_HANDLE;
}

static VkShaderModuleCreateInfo module_info(const void *code, size_t bytes) {
    VkShaderModuleCreateInfo info = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = bytes,
        .pCode    = (const uint32_t *)code,
    };
    return info;
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
    VkShaderModuleCreateInfo vert_mod = module_info(info->vert, info->vert_bytes);
    VkShaderModuleCreateInfo frag_mod = module_info(info->frag, info->frag_bytes);
    VkPipelineShaderStageCreateInfo stages[4][2] = {0};
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
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode    = info->cull,
        .frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth   = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo sample_state = {
        .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = info->samples == 0 ? VK_SAMPLE_COUNT_1_BIT : info->samples,
    };
    VkPipelineDepthStencilStateCreateInfo depth = {
        .sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable  = info->depth_test,
        .depthWriteEnable = info->depth_write,
        .depthCompareOp   = info->depth_compare == 0 ? VK_COMPARE_OP_LESS : info->depth_compare,
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

    VkPipelineRenderingCreateInfo rendering[4] = {0};
    VkGraphicsPipelineCreateInfo pipes[4] = {0};
    VkFormat colors[4] = {0};
    for (uint32_t i = 0u; i < count; ++i) {
        colors[i] = formats[i];
        const VkSpecializationInfo *spec = info->frag_spec;
        if (specs != nullptr) {
            spec = specs[i];
        }
        stages[i][0] = (VkPipelineShaderStageCreateInfo){
            .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext  = &vert_mod,
            .stage  = VK_SHADER_STAGE_VERTEX_BIT,
            .module = VK_NULL_HANDLE,
            .pName  = "main",
        };
        stages[i][1] = (VkPipelineShaderStageCreateInfo){
            .sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext               = &frag_mod,
            .stage               = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module              = VK_NULL_HANDLE,
            .pName               = "main",
            .pSpecializationInfo = spec,
        };
        rendering[i] = (VkPipelineRenderingCreateInfo){
            .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount    = 1,
            .pColorAttachmentFormats = &colors[i],
            .depthAttachmentFormat   = info->depth_format,
        };
        pipes[i] = (VkGraphicsPipelineCreateInfo){
            .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext               = &rendering[i],
            .stageCount          = 2,
            .pStages             = stages[i],
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
    }
    VkResult res = vkCreateGraphicsPipelines(vk->device, vk->pipe_cache, count, pipes, nullptr, out);
    if (res != VK_SUCCESS) {
        for (uint32_t i = 0u; i < count; ++i) {
            if (out[i] != VK_NULL_HANDLE) {
                vkDestroyPipeline(vk->device, out[i], nullptr);
                out[i] = VK_NULL_HANDLE;
            }
        }
        fprintf(stderr, "[Vulkan Error] vkCreateGraphicsPipelines failed: %s (%d)\n",
                vulkan_vkresult_to_str(res), (int)res);
        return -1;
    }
    return 0;
}

VkPipeline vulkan_compute_pipeline(VulkanContext *vk, const void *spv, size_t bytes,
                                   VkPipelineLayout layout) {
    if (vk == nullptr || spv == nullptr || bytes == 0 || layout == VK_NULL_HANDLE) {
        return VK_NULL_HANDLE;
    }
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
        fprintf(stderr, "[Vulkan Error] vkCreateComputePipelines failed: %s (%d)\n",
                vulkan_vkresult_to_str(res), (int)res);
        return VK_NULL_HANDLE;
    }
    return out;
}
