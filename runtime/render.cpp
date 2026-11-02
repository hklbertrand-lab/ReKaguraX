// Rendu Vulkan des dessins Xenos : état lu dans les registres GPU, shaders traduits par XenosRecomp.
// EDRAM émulée par une image 1280x720 ; un "resolve" la copie dans l'image du tampon d'affichage.
// Première version : pas de profondeur, textures remplacées par une texture blanche.
#include "render.h"
#include "guest.h"
#include "shaders.h"
#include "vulkan.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>
#define XXH_INLINE_ALL
#include <xxhash.h>

VkCommandBuffer VulkanFrameCmd();
void VulkanImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkImageAspectFlags aspect);
VkDevice VulkanDevice();
uint32_t VulkanFindMemoryType(uint32_t bits, VkMemoryPropertyFlags flags);
void VulkanPresentFrame(VkImage src, uint32_t width, uint32_t height);

enum : uint32_t
{
    RB_SURFACE_INFO = 0x2000, RB_COLOR_INFO = 0x2001, RB_COLOR_MASK = 0x2104, RB_ALPHA_REF = 0x210E,
    PA_CL_VPORT_XSCALE = 0x210F, PA_CL_VPORT_XOFFSET = 0x2110, PA_CL_VPORT_YSCALE = 0x2111, PA_CL_VPORT_YOFFSET = 0x2112,
    PA_SC_WINDOW_SCISSOR_TL = 0x2081, PA_SC_WINDOW_SCISSOR_BR = 0x2082,
    RB_BLENDCONTROL0 = 0x2201, RB_COLORCONTROL = 0x2202, PA_SU_SC_MODE_CNTL = 0x2205, PA_CL_VTE_CNTL = 0x2206,
    RB_MODECONTROL = 0x2208, RB_COPY_CONTROL = 0x2318, RB_COLOR_CLEAR = 0x231E,
    ALU_CONSTANTS = 0x4000, FETCH_CONSTANTS = 0x4800, BOOL_CONSTANTS = 0x4900,
};

static constexpr uint32_t kWidth = 1280, kHeight = 720;
static constexpr VkDeviceSize kUploadSize = 64 << 20;

struct Image { VkImage image = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED; };

static bool s_ready;
static VkDevice s_dev;
static Image s_edram, s_front, s_white;
static VkRenderPass s_renderPass;
static VkFramebuffer s_framebuffer;
static VkDescriptorSetLayout s_setLayouts[4];
static VkDescriptorPool s_descPool;
static VkDescriptorSet s_sets[4];
static VkSampler s_sampler;
static VkSampler s_samplers[4];   // [linéaire|point] x [clamp|wrap], indices 0-3 du tableau de samplers
static VkPipelineLayout s_pipelineLayout;
static std::unordered_map<uint64_t, VkPipeline> s_pipelines;
static std::unordered_map<const TranslatedShader*, VkShaderModule> s_modules;

static VkBuffer s_upload;
static VkDeviceMemory s_uploadMem;
static uint8_t* s_uploadPtr;
static VkDeviceAddress s_uploadAddr;
static VkDeviceSize s_uploadOffset;

static bool s_inRenderPass;
static VkCommandBuffer s_cmd;
static bool s_frameStarted;

// Shaders actifs (fixés par IM_LOAD)
static const TranslatedShader* s_vs;
static const TranslatedShader* s_ps;
static std::vector<uint8_t> s_vsUcode;

// ------------------------------------------------------------------ création des ressources

static VkDeviceMemory Allocate(VkMemoryRequirements req, VkMemoryPropertyFlags flags, bool deviceAddress = false)
{
    VkMemoryAllocateFlagsInfo flagsInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO };
    flagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo info{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    info.pNext = deviceAddress ? &flagsInfo : nullptr;
    info.allocationSize = req.size;
    info.memoryTypeIndex = VulkanFindMemoryType(req.memoryTypeBits, flags);
    VkDeviceMemory mem = VK_NULL_HANDLE;
    vkAllocateMemory(s_dev, &info, nullptr, &mem);
    return mem;
}

static Image CreateImage(uint32_t w, uint32_t h, VkImageUsageFlags usage)
{
    Image img;
    VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = { w, h, 1 };
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    vkCreateImage(s_dev, &ci, nullptr, &img.image);
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(s_dev, img.image, &req);
    img.memory = Allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkBindImageMemory(s_dev, img.image, img.memory, 0);
    VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ci.format;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCreateImageView(s_dev, &vi, nullptr, &img.view);
    return img;
}

static void Transition(Image& img, VkImageLayout to)
{
    if (img.layout == to) return;
    VulkanImageBarrier(s_cmd, img.image, img.layout, to, VK_IMAGE_ASPECT_COLOR_BIT);
    img.layout = to;
}

static bool Init()
{
    s_dev = VulkanDevice();
    if (!s_dev) return false;

    s_edram = CreateImage(kWidth, kHeight, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    s_front = CreateImage(kWidth, kHeight, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    s_white = CreateImage(1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);

    // Tampon d'envoi : sommets + constantes, lu par les shaders via son adresse GPU
    VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = kUploadSize;
    bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    vkCreateBuffer(s_dev, &bi, nullptr, &s_upload);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(s_dev, s_upload, &req);
    s_uploadMem = Allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);
    vkBindBufferMemory(s_dev, s_upload, s_uploadMem, 0);
    vkMapMemory(s_dev, s_uploadMem, 0, VK_WHOLE_SIZE, 0, (void**)&s_uploadPtr);
    VkBufferDeviceAddressInfo ai{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
    ai.buffer = s_upload;
    s_uploadAddr = vkGetBufferDeviceAddress(s_dev, &ai);

    // Passe de rendu : une cible couleur conservée entre les dessins
    VkAttachmentDescription att{};
    att.format = VK_FORMAT_R8G8B8A8_UNORM;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkRenderPassCreateInfo rp{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rp.attachmentCount = 1;
    rp.pAttachments = &att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    vkCreateRenderPass(s_dev, &rp, nullptr, &s_renderPass);
    VkFramebufferCreateInfo fb{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fb.renderPass = s_renderPass;
    fb.attachmentCount = 1;
    fb.pAttachments = &s_edram.view;
    fb.width = kWidth;
    fb.height = kHeight;
    fb.layers = 1;
    vkCreateFramebuffer(s_dev, &fb, nullptr, &s_framebuffer);

    // Descripteurs "bindless" attendus par shader_common.h : textures 2D/3D/cube (sets 0-2), samplers (set 3)
    VkDescriptorType types[4] = { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER };
    uint32_t counts[4] = { 4096, 256, 256, 256 };
    VkDescriptorPoolSize poolSizes[2] = { { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4096 + 512 }, { VK_DESCRIPTOR_TYPE_SAMPLER, 256 } };
    VkDescriptorPoolCreateInfo dp{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dp.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    dp.maxSets = 4;
    dp.poolSizeCount = 2;
    dp.pPoolSizes = poolSizes;
    vkCreateDescriptorPool(s_dev, &dp, nullptr, &s_descPool);
    for (int i = 0; i < 4; i++)
    {
        VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo bf{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
        bf.bindingCount = 1;
        bf.pBindingFlags = &flags;
        VkDescriptorSetLayoutBinding b{ 0, types[i], counts[i], VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        li.pNext = &bf;
        li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        li.bindingCount = 1;
        li.pBindings = &b;
        vkCreateDescriptorSetLayout(s_dev, &li, nullptr, &s_setLayouts[i]);
    }
    VkDescriptorSetAllocateInfo da{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    da.descriptorPool = s_descPool;
    da.descriptorSetCount = 4;
    da.pSetLayouts = s_setLayouts;
    vkAllocateDescriptorSets(s_dev, &da, s_sets);

    VkDescriptorImageInfo samplerInfos[4];
    for (int i = 0; i < 4; i++)
    {
        VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        si.magFilter = si.minFilter = (i & 1) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = (i & 2) ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = VK_LOD_CLAMP_NONE;
        vkCreateSampler(s_dev, &si, nullptr, &s_samplers[i]);
        samplerInfos[i] = { s_samplers[i], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    }
    s_sampler = s_samplers[0];

    VkDescriptorImageInfo whiteInfo{ VK_NULL_HANDLE, s_white.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet writes[2]{};
    writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, s_sets[0], 0, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &whiteInfo };
    writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, s_sets[3], 0, 0, 4, VK_DESCRIPTOR_TYPE_SAMPLER, samplerInfos };
    vkUpdateDescriptorSets(s_dev, 2, writes, 0, nullptr);

    VkPushConstantRange push{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 24 };
    VkPipelineLayoutCreateInfo pl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pl.setLayoutCount = 4;
    pl.pSetLayouts = s_setLayouts;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    vkCreatePipelineLayout(s_dev, &pl, nullptr, &s_pipelineLayout);
    return true;
}

// Command buffer de l'image en cours ; au premier appel d'une image, réinitialise le tampon d'envoi
static bool BeginCommands()
{
    if (!s_ready) { if (!(s_ready = Init())) return false; }
    s_cmd = VulkanFrameCmd();
    if (!s_frameStarted)
    {
        s_frameStarted = true;
        s_uploadOffset = 0;
        static bool first = true;
        if (first)
        {
            first = false;
            VkClearColorValue black{}, white{ { 1, 0, 1, 1 } };   // magenta temporaire : rend visibles les textures manquantes
            VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            Transition(s_edram, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdClearColorImage(s_cmd, s_edram.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
            Transition(s_front, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdClearColorImage(s_cmd, s_front.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
            Transition(s_white, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdClearColorImage(s_cmd, s_white.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &range);
            Transition(s_white, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    }
    return true;
}

static VkDeviceSize Upload(const void* data, VkDeviceSize size, VkDeviceSize align = 16)
{
    s_uploadOffset = (s_uploadOffset + align - 1) & ~(align - 1);
    if (s_uploadOffset + size > kUploadSize) { KLOG("rendu : tampon d'envoi plein"); s_uploadOffset = 0; }
    VkDeviceSize at = s_uploadOffset;
    if (data) memcpy(s_uploadPtr + at, data, size);
    s_uploadOffset += size;
    return at;
}

static void EndRenderPass()
{
    if (s_inRenderPass) { vkCmdEndRenderPass(s_cmd); s_inRenderPass = false; }
}

// ------------------------------------------------------------------ état

void RenderSetShader(bool pixel, const uint8_t* ucode, uint32_t sizeBytes, const TranslatedShader* shader)
{
    if (pixel) s_ps = shader;
    else { s_vs = shader; s_vsUcode.assign(ucode, ucode + sizeBytes); }
}

static VkShaderModule Module(const TranslatedShader* s)
{
    auto& m = s_modules[s];
    if (!m)
    {
        VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        ci.codeSize = s->spirv.size() * 4;
        ci.pCode = s->spirv.data();
        vkCreateShaderModule(s_dev, &ci, nullptr, &m);
    }
    return m;
}

static VkBlendFactor BlendFactor(uint32_t f)
{
    static const VkBlendFactor map[] = {
        VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ZERO,
        VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
        VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR, VK_BLEND_FACTOR_CONSTANT_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
        VK_BLEND_FACTOR_SRC_ALPHA_SATURATE };
    return f < 17 ? map[f] : VK_BLEND_FACTOR_ONE;
}

static VkBlendOp BlendOp(uint32_t op)
{
    static const VkBlendOp map[] = { VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX, VK_BLEND_OP_REVERSE_SUBTRACT };
    return op < 5 ? map[op] : VK_BLEND_OP_ADD;
}

// Format de sommet Xenos -> format Vulkan (données déjà remises en little-endian par mot de 32 bits)
static VkFormat VertexFormat(uint32_t fmt, bool isSigned, bool normalized)
{
    switch (fmt)
    {
    case 6:  return normalized ? (isSigned ? VK_FORMAT_R8G8B8A8_SNORM : VK_FORMAT_R8G8B8A8_UNORM) : (isSigned ? VK_FORMAT_R8G8B8A8_SSCALED : VK_FORMAT_R8G8B8A8_USCALED);
    case 7:  return normalized ? (isSigned ? VK_FORMAT_A2B10G10R10_SNORM_PACK32 : VK_FORMAT_A2B10G10R10_UNORM_PACK32) : VK_FORMAT_A2B10G10R10_USCALED_PACK32;
    case 25: return normalized ? (isSigned ? VK_FORMAT_R16G16_SNORM : VK_FORMAT_R16G16_UNORM) : (isSigned ? VK_FORMAT_R16G16_SSCALED : VK_FORMAT_R16G16_USCALED);
    case 26: return normalized ? (isSigned ? VK_FORMAT_R16G16B16A16_SNORM : VK_FORMAT_R16G16B16A16_UNORM) : (isSigned ? VK_FORMAT_R16G16B16A16_SSCALED : VK_FORMAT_R16G16B16A16_USCALED);
    case 31: return VK_FORMAT_R16G16_SFLOAT;
    case 32: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 36: return VK_FORMAT_R32_SFLOAT;
    case 37: return VK_FORMAT_R32G32_SFLOAT;
    case 38: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 57: return VK_FORMAT_R32G32B32_SFLOAT;
    default: return VK_FORMAT_UNDEFINED;
    }
}

struct VertexLayout
{
    VkVertexInputBindingDescription bindings[16];
    VkVertexInputAttributeDescription attributes[16];
    uint32_t bindingCount = 0, attributeCount = 0;
    uint32_t fetchConst[16];      // constante de fetch de chaque binding
    bool allFloat32 = true;       // nécessaire pour générer le 4e sommet des rectangles
};

static bool DecodeVertexLayout(const uint32_t* regs, VertexLayout& layout)
{
    bool ok = true;
    ForEachVertexFetch(s_vsUcode.data(), uint32_t(s_vsUcode.size()), [&](uint32_t address, const uint32_t* w) {
        uint32_t location = UINT32_MAX;
        for (auto& [a, loc] : s_vs->fetchLocations) if (a == address) location = loc;
        if (location == UINT32_MAX || layout.attributeCount >= 16) return;   // mini-fetch ou attribut non déclaré
        uint32_t constIndex = ((w[0] >> 20) & 0x1F) * 3 + ((w[0] >> 25) & 3);
        uint32_t format = (w[1] >> 16) & 0x3F, stride = (w[2] & 0xFF) * 4;
        int32_t offset = int32_t(w[2] << 1) >> 9;   // 23 bits signés, en dwords
        VkFormat vk = VertexFormat(format, (w[1] >> 12) & 1, ((w[1] >> 13) & 1) == 0);
        if (vk == VK_FORMAT_UNDEFINED) { KLOG("rendu : format de sommet %u non géré", format); ok = false; return; }
        if (format < 36) layout.allFloat32 = false;
        uint32_t binding = 0;
        while (binding < layout.bindingCount && layout.fetchConst[binding] != constIndex) binding++;
        if (binding == layout.bindingCount)
        {
            layout.fetchConst[binding] = constIndex;
            layout.bindings[binding] = { binding, stride, VK_VERTEX_INPUT_RATE_VERTEX };
            layout.bindingCount++;
        }
        layout.attributes[layout.attributeCount++] = { location, binding, vk, uint32_t(offset * 4) };
    });
    return ok;
}

static VkPipeline GetPipeline(const uint32_t* regs, const VertexLayout& layout, VkPrimitiveTopology topology, uint32_t spec)
{
    uint32_t blend = regs[RB_BLENDCONTROL0], mask = regs[RB_COLOR_MASK] & 0xF;
    struct Key { uint64_t vs, ps; uint32_t blend, mask, topology, spec; uint64_t layoutHash; } key{
        s_vs->containerHash, s_ps->containerHash, blend, mask, uint32_t(topology), spec,
        XXH3_64bits(&layout, offsetof(VertexLayout, fetchConst)) };
    uint64_t h = XXH3_64bits(&key, sizeof(key));
    if (auto it = s_pipelines.find(h); it != s_pipelines.end()) return it->second;

    VkSpecializationMapEntry specEntry{ 0, 0, 4 };
    VkSpecializationInfo specInfo{ 1, &specEntry, 4, &spec };
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, Module(s_vs), "main", &specInfo };
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, Module(s_ps), "main", &specInfo };

    VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount = layout.bindingCount;
    vi.pVertexBindingDescriptions = layout.bindings;
    vi.vertexAttributeDescriptionCount = layout.attributeCount;
    vi.pVertexAttributeDescriptions = layout.attributes;
    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = topology;
    VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;   // TODO : PA_SU_SC_MODE_CNTL
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState att{};
    uint32_t srcC = blend & 0x1F, opC = (blend >> 5) & 7, dstC = (blend >> 8) & 0x1F;
    uint32_t srcA = (blend >> 16) & 0x1F, opA = (blend >> 21) & 7, dstA = (blend >> 24) & 0x1F;
    att.blendEnable = !(srcC == 1 && dstC == 0 && opC == 0 && srcA == 1 && dstA == 0 && opA == 0);
    att.srcColorBlendFactor = BlendFactor(srcC); att.dstColorBlendFactor = BlendFactor(dstC); att.colorBlendOp = BlendOp(opC);
    att.srcAlphaBlendFactor = BlendFactor(srcA); att.dstAlphaBlendFactor = BlendFactor(dstA); att.alphaBlendOp = BlendOp(opA);
    att.colorWriteMask = mask;
    VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1;
    cb.pAttachments = &att;
    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkPipelineDepthStencilStateCreateInfo dss{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };

    VkGraphicsPipelineCreateInfo pci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pDepthStencilState = &dss;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &ds;
    pci.layout = s_pipelineLayout;
    pci.renderPass = s_renderPass;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult r = vkCreateGraphicsPipelines(s_dev, VK_NULL_HANDLE, 1, &pci, nullptr, &pipeline);
    if (r != VK_SUCCESS) KLOG("rendu : échec de création du pipeline (%d)", r);
    s_pipelines[h] = pipeline;
    return pipeline;
}

// ------------------------------------------------------------------ resolve (copie EDRAM -> tampon d'affichage)

static void Resolve(const uint32_t* regs)
{
    EndRenderPass();
    uint32_t control = regs[RB_COPY_CONTROL];
    Transition(s_edram, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Transition(s_front, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy copy{};
    copy.srcSubresource = copy.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.extent = { kWidth, kHeight, 1 };
    vkCmdCopyImage(s_cmd, s_edram.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_front.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    if (control & (1 << 8))   // effacement couleur demandé avec la copie
    {
        uint32_t c = regs[RB_COLOR_CLEAR];
        VkClearColorValue color{ { ((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f, (c >> 24) / 255.0f } };
        VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        Transition(s_edram, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(s_cmd, s_edram.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
    }
}

// ------------------------------------------------------------------ textures

struct TextureFormatInfo { VkFormat vk; uint32_t blockSize, bytesPerBlock; };

static TextureFormatInfo TextureFormat(uint32_t fmt)
{
    switch (fmt)
    {
    case 2:  return { VK_FORMAT_R8_UNORM, 1, 1 };                    // k_8
    case 4:  return { VK_FORMAT_R5G6B5_UNORM_PACK16, 1, 2 };         // k_5_6_5
    case 6:  return { VK_FORMAT_B8G8R8A8_UNORM, 1, 4 };              // k_8_8_8_8 (ARGB big-endian)
    case 15: return { VK_FORMAT_B4G4R4A4_UNORM_PACK16, 1, 2 };       // k_4_4_4_4
    case 18: return { VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 4, 8 };        // k_DXT1
    case 19: return { VK_FORMAT_BC2_UNORM_BLOCK, 4, 16 };            // k_DXT2_3
    case 20: return { VK_FORMAT_BC3_UNORM_BLOCK, 4, 16 };            // k_DXT4_5
    default: return { VK_FORMAT_UNDEFINED, 1, 4 };
    }
}

static uint32_t SwapEndian(uint32_t v, uint32_t endian)
{
    switch (endian)
    {
    case 1: return ((v & 0x00FF00FF) << 8) | ((v >> 8) & 0x00FF00FF);
    case 2: return __builtin_bswap32(v);
    case 3: return (v << 16) | (v >> 16);
    default: return v;
    }
}

// Adresse d'un bloc dans une texture 2D tuilée (Tiled2D de Xenia, texture_address.h)
static uint32_t Tiled2D(uint32_t x, uint32_t y, uint32_t pitchAligned, uint32_t bppLog2)
{
    uint32_t outer = ((y >> 5) * (pitchAligned >> 5) + (x >> 5)) << 6;
    uint32_t inner = (((y >> 1) & 7) << 3) | (x & 7);
    uint32_t bytes = (outer | inner) << bppLog2;
    uint32_t bank = (y >> 4) & 1, pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1);
    return ((y & 1) << 4) | (pipe << 6) | (bank << 11) | (bytes & 15) | (((bytes >> 4) & 1) << 5) | (((bytes >> 5) & 7) << 8) | ((bytes >> 8) << 12);
}

struct CachedTexture { Image image; uint32_t descriptor = 0; uint64_t contentHash = 0; };
static std::unordered_map<uint64_t, CachedTexture> s_textures;
static uint32_t s_nextDescriptor = 1;   // 0 = texture de remplacement

static VkComponentSwizzle Swizzle(uint32_t sel)
{
    static const VkComponentSwizzle map[] = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A,
        VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO };
    return map[sel & 7];
}

// Texture décrite par une fetch constant (6 dwords) -> indice dans le tableau de descripteurs (0 si non gérée)
static uint32_t GetTexture(const uint32_t* f, uint32_t (*phys)(uint32_t))
{
    if ((f[0] & 3) != 2) return 0;
    uint32_t format = f[1] & 0x3F, endian = (f[1] >> 6) & 3, base = (f[1] >> 12) << 12;
    uint32_t width = (f[2] & 0x1FFF) + 1, height = ((f[2] >> 13) & 0x1FFF) + 1, dim = (f[5] >> 9) & 3;
    uint32_t pitch = ((f[0] >> 22) & 0x1FF) << 5;
    bool tiled = f[0] >> 31;
    TextureFormatInfo info = TextureFormat(format);
    if (dim != 1 || info.vk == VK_FORMAT_UNDEFINED || !base)
    {
        static std::unordered_map<uint32_t, int> warned;
        if (!warned[format * 4 + dim]++) KLOG("rendu : texture format %u dimension %u non gérée", format, dim);
        return 0;
    }

    uint32_t bw = (width + info.blockSize - 1) / info.blockSize, bh = (height + info.blockSize - 1) / info.blockSize;
    uint32_t pitchBlocks = std::max((pitch ? pitch : width) / info.blockSize, bw);
    uint32_t pitchAligned = (pitchBlocks + 31) & ~31u, heightAligned = (bh + 31) & ~31u;
    uint32_t bppLog2 = __builtin_ctz(info.bytesPerBlock);
    uint32_t srcSize = tiled ? pitchAligned * heightAligned * info.bytesPerBlock : pitchBlocks * bh * info.bytesPerBlock;
    const uint8_t* src = g_base + phys(base);

    uint64_t key = XXH3_64bits(f, 24);
    uint64_t content = XXH3_64bits(src, srcSize);
    auto it = s_textures.find(key);
    if (it != s_textures.end() && it->second.contentHash == content) return it->second.descriptor;

    // Dé-tuilage + remise en ordre des octets dans le tampon d'envoi
    EndRenderPass();
    uint32_t rowBytes = bw * info.bytesPerBlock;
    VkDeviceSize at = Upload(nullptr, VkDeviceSize(rowBytes) * bh, 16);
    uint8_t* dst = s_uploadPtr + at;
    for (uint32_t y = 0; y < bh; y++)
        for (uint32_t x = 0; x < bw; x++)
        {
            uint32_t off = tiled ? Tiled2D(x, y, pitchAligned, bppLog2) : (y * pitchBlocks + x) * info.bytesPerBlock;
            uint8_t* d = dst + y * rowBytes + x * info.bytesPerBlock;
            if (off + info.bytesPerBlock > srcSize) { memset(d, 0, info.bytesPerBlock); continue; }
            memcpy(d, src + off, info.bytesPerBlock);
            if (info.bytesPerBlock >= 4)
                for (uint32_t i = 0; i < info.bytesPerBlock; i += 4) { uint32_t v; memcpy(&v, d + i, 4); v = SwapEndian(v, endian); memcpy(d + i, &v, 4); }
            else if (info.bytesPerBlock == 2 && (endian == 1 || endian == 2))
                std::swap(d[0], d[1]);
        }

    bool created = it == s_textures.end();
    CachedTexture& t = s_textures[key];
    if (created)
    {
        VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = info.vk;
        ci.extent = { width, height, 1 };
        ci.mipLevels = ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        vkCreateImage(s_dev, &ci, nullptr, &t.image.image);
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(s_dev, t.image.image, &req);
        t.image.memory = Allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkBindImageMemory(s_dev, t.image.image, t.image.memory, 0);
        uint32_t sw = (f[3] >> 1) & 0xFFF;
        VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = t.image.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = info.vk;
        vi.components = { Swizzle(sw), Swizzle(sw >> 3), Swizzle(sw >> 6), Swizzle(sw >> 9) };
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCreateImageView(s_dev, &vi, nullptr, &t.image.view);
        t.descriptor = s_nextDescriptor++;
        VkDescriptorImageInfo ii{ VK_NULL_HANDLE, t.image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, s_sets[0], 0, t.descriptor, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &ii };
        vkUpdateDescriptorSets(s_dev, 1, &w, 0, nullptr);
        if (getenv("IKARUGA_DRAWLOG"))
            KLOG("texture %ux%u format %u %s swizzle %03X -> descripteur %u", width, height, format, tiled ? "tuilée" : "linéaire", sw, t.descriptor);
    }
    t.contentHash = content;
    Transition(t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.bufferOffset = at;
    copy.bufferRowLength = bw * info.blockSize;
    copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.imageExtent = { width, height, 1 };
    vkCmdCopyBufferToImage(s_cmd, s_upload, t.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    Transition(t.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return t.descriptor;
}

// Sampler : filtre (mag) et adressage (X) de la fetch constant
static uint32_t GetSampler(const uint32_t* f)
{
    bool point = ((f[3] >> 19) & 3) == 0;
    bool wrap = ((f[0] >> 10) & 7) == 0;
    return (point ? 1 : 0) | (wrap ? 2 : 0);
}

// ------------------------------------------------------------------ dessin

void RenderDraw(const uint32_t* regs, uint32_t initiator, uint32_t (*phys)(uint32_t))
{
    static int logged;
    if (getenv("IKARUGA_DRAWLOG") && logged++ < 400)
        KLOG("RenderDraw init=%08X mode=%u vs=%016llx ps=%016llx copy=%08X clear=%08X", initiator, regs[RB_MODECONTROL] & 7,
            s_vs ? (unsigned long long)s_vs->containerHash : 0ull, s_ps ? (unsigned long long)s_ps->containerHash : 0ull,
            regs[RB_COPY_CONTROL], regs[RB_COLOR_CLEAR]);
    if (!BeginCommands()) return;
    uint32_t edramMode = regs[RB_MODECONTROL] & 7;
    if (edramMode == 6) { Resolve(regs); return; }   // mode copie : c'est un resolve
    if (edramMode != 4) return;                      // 5 = profondeur seule (pas encore de tampon de profondeur)
    if (!s_vs || !s_ps) return;

    uint32_t prim = initiator & 0x3F, source = (initiator >> 6) & 3, count = initiator >> 16;
    if (source != 2) { static int warned; if (!warned++) KLOG("rendu : dessins indexés pas encore gérés"); return; }

    VertexLayout layout;
    if (!DecodeVertexLayout(regs, layout)) return;

    VkPrimitiveTopology topology;
    std::vector<uint32_t> indices;
    uint32_t vertexCount = count;
    switch (prim)
    {
    case 1: topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
    case 2: topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
    case 3: topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
    case 4: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
    case 5: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
    case 6: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
    case 13:   // liste de quads -> triangles indexés
        topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        for (uint32_t q = 0; q + 3 < count; q += 4) indices.insert(indices.end(), { q, q + 1, q + 2, q, q + 2, q + 3 });
        break;
    case 8:    // liste de rectangles : le 4e sommet est déduit (v1 + v2 - v0), ce qui exige des attributs float32
        topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        if (!layout.allFloat32) { static int warned; if (!warned++) KLOG("rendu : rectangles avec attributs non float32"); return; }
        break;
    default:
        { static int warned; if (!warned++) KLOG("rendu : primitive %u non gérée", prim); }
        return;
    }

    // Sommets : copie de chaque flux en remettant les mots de 32 bits en little-endian
    VkBuffer buffers[16];
    VkDeviceSize offsets[16];
    for (uint32_t b = 0; b < layout.bindingCount; b++)
    {
        uint32_t d0 = regs[FETCH_CONSTANTS + layout.fetchConst[b] * 2], d1 = regs[FETCH_CONSTANTS + layout.fetchConst[b] * 2 + 1];
        uint32_t base = d0 & ~3u, sizeBytes = ((d1 >> 2) & 0xFFFFFF) * 4, stride = layout.bindings[b].stride;
        uint32_t needed = stride ? stride * count : sizeBytes;
        uint32_t bytes = std::min(needed, sizeBytes ? sizeBytes : needed);
        uint32_t outVertices = prim == 8 ? count / 3 * 4 : count;
        VkDeviceSize at = Upload(nullptr, VkDeviceSize(std::max(stride, 4u)) * outVertices + 16);
        uint32_t* dst = (uint32_t*)(s_uploadPtr + at);
        const uint8_t* src = g_base + phys(base);
        if (prim == 8)
        {
            // Pour chaque rectangle : v0 v1 v2 puis v3 = v1 + v2 - v0, composante par composante (float32)
            uint32_t words = stride / 4;
            for (uint32_t r = 0; r * 3 + 2 < count; r++)
            {
                float v[4][64];
                for (int k = 0; k < 3; k++)
                    for (uint32_t i = 0; i < words && i < 64; i++)
                    {
                        uint32_t x = __builtin_bswap32(*(const uint32_t*)(src + (r * 3 + k) * stride + i * 4));
                        memcpy(&v[k][i], &x, 4);
                    }
                for (uint32_t i = 0; i < words && i < 64; i++) v[3][i] = v[1][i] + v[2][i] - v[0][i];
                for (int k = 0; k < 4; k++) memcpy(dst + (r * 4 + k) * words, v[k], words * 4);
            }
        }
        else
            for (uint32_t i = 0; i < bytes / 4; i++) dst[i] = __builtin_bswap32(((const uint32_t*)src)[i]);
        buffers[b] = s_upload;
        offsets[b] = at;
    }
    if (prim == 8)
    {
        for (uint32_t r = 0; r * 3 + 2 < count; r++) { uint32_t q = r * 4; indices.insert(indices.end(), { q, q + 1, q + 2, q + 2, q + 1, q + 3 }); }
    }

    // Constantes : VS = registres ALU 0-255, PS = 256-479 (float4, valeurs déjà en ordre hôte)
    VkDeviceSize vsConst = Upload(regs + ALU_CONSTANTS, 4096, 256);
    VkDeviceSize psConst = Upload(regs + ALU_CONSTANTS + 256 * 4, 3584, 256);
    uint8_t shared[512]{};   // indices de descripteurs (0 = texture de remplacement), booléens, seuil alpha
    // Textures du pixel shader : l'échantillonneur n lit la fetch constant n ; indices 2D en c0-c3, samplers en c12-c15
    for (uint32_t slot = 0; slot < 16; slot++)
    {
        const uint32_t* f = regs + FETCH_CONSTANTS + slot * 6;
        if ((f[0] & 3) != 2) continue;
        uint32_t tex = GetTexture(f, phys), smp = GetSampler(f);
        memcpy(shared + slot * 4, &tex, 4);
        memcpy(shared + 192 + slot * 4, &smp, 4);
    }
    uint32_t booleans = (regs[BOOL_CONSTANTS] & 0xFFFF) | ((regs[BOOL_CONSTANTS + 4] & 0xFFFF) << 16);
    memcpy(shared + 256, &booleans, 4);
    memcpy(shared + 272, &regs[RB_ALPHA_REF], 4);
    VkDeviceSize sharedConst = Upload(shared, sizeof(shared), 256);
    uint64_t push[3] = { s_uploadAddr + vsConst, s_uploadAddr + psConst, s_uploadAddr + sharedConst };

    uint32_t colorControl = regs[RB_COLORCONTROL];
    uint32_t spec = 0;
    if ((colorControl & 8) && (s_ps->specConstantsMask & 2)) spec |= 2;   // test alpha (fonction "supérieur ou égal" supposée)
    VkPipeline pipeline = GetPipeline(regs, layout, topology, spec | (s_vs->specConstantsMask & 1));
    if (!pipeline) return;

    if (!s_inRenderPass)
    {
        Transition(s_edram, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderPassBeginInfo rb{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        rb.renderPass = s_renderPass;
        rb.framebuffer = s_framebuffer;
        rb.renderArea = { { 0, 0 }, { kWidth, kHeight } };
        vkCmdBeginRenderPass(s_cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
        s_inRenderPass = true;
    }

    // Viewport Xenos (échelle/décalage) ; une hauteur négative retourne l'axe Y comme D3D
    float xs, xo, ys, yo;
    memcpy(&xs, &regs[PA_CL_VPORT_XSCALE], 4); memcpy(&xo, &regs[PA_CL_VPORT_XOFFSET], 4);
    memcpy(&ys, &regs[PA_CL_VPORT_YSCALE], 4); memcpy(&yo, &regs[PA_CL_VPORT_YOFFSET], 4);
    VkViewport viewport{ xo - xs, yo - ys, 2 * xs, 2 * ys, 0.0f, 1.0f };
    if (!(regs[PA_CL_VTE_CNTL] & 1) || xs == 0) viewport = { 0, float(kHeight), float(kWidth), -float(kHeight), 0, 1 };
    uint32_t tl = regs[PA_SC_WINDOW_SCISSOR_TL], br = regs[PA_SC_WINDOW_SCISSOR_BR];
    VkRect2D scissor{ { int32_t(tl & 0x7FFF), int32_t((tl >> 16) & 0x7FFF) }, { (br & 0x7FFF) - (tl & 0x7FFF), ((br >> 16) & 0x7FFF) - ((tl >> 16) & 0x7FFF) } };
    if (scissor.extent.width == 0 || scissor.extent.width > kWidth || scissor.extent.height > kHeight) scissor = { { 0, 0 }, { kWidth, kHeight } };
    vkCmdSetViewport(s_cmd, 0, 1, &viewport);
    vkCmdSetScissor(s_cmd, 0, 1, &scissor);

    vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipelineLayout, 0, 4, s_sets, 0, nullptr);
    vkCmdPushConstants(s_cmd, s_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
    if (layout.bindingCount) vkCmdBindVertexBuffers(s_cmd, 0, layout.bindingCount, buffers, offsets);
    if (!indices.empty())
    {
        VkDeviceSize at = Upload(indices.data(), indices.size() * 4, 4);
        vkCmdBindIndexBuffer(s_cmd, s_upload, at, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(s_cmd, uint32_t(indices.size()), 1, 0, 0, 0);
    }
    else
        vkCmdDraw(s_cmd, vertexCount, 1, 0, 0);
}

// Paquet de présentation inséré par VdSwap : affiche le tampon d'affichage
void RenderPresent()
{
    static int logged;
    if (logged++ < 5) KLOG("RenderPresent #%d", logged);
    if (!BeginCommands()) return;
    EndRenderPass();
    Transition(s_front, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VulkanPresentFrame(s_front.image, kWidth, kHeight);
    s_frameStarted = false;
}
