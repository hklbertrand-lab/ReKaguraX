// Rendu Vulkan des dessins Xenos : état lu dans les registres GPU, shaders traduits par XenosRecomp.
// EDRAM : une image Vulkan par cible (base, format, pitch) + un tampon de profondeur par taille.
// Un "resolve" copie la cible dans une image indexée par son adresse de destination ; une texture lue à
// cette adresse utilise directement cette image (pas de relecture par le CPU), et VdSwap présente celle
// du tampon d'affichage.
#include "render.h"
#include "settings.h"
#include "guest.h"
#include "shaders.h"
#include "vulkan.h"
#include <algorithm>
#include <atomic>
#include <array>
#include <string>
#include <cmath>
#include <unordered_map>
#include <vector>
#define XXH_INLINE_ALL
#include <xxhash.h>

VkCommandBuffer VulkanFrameCmd();
uint32_t VulkanFrameSlot();
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
    RB_DEPTH_INFO = 0x2002, RB_DEPTHCONTROL = 0x2200, PA_CL_VPORT_ZSCALE = 0x2113, PA_CL_VPORT_ZOFFSET = 0x2114,
    RB_COPY_DEST_BASE = 0x2319, RB_COPY_DEST_PITCH = 0x231A, RB_DEPTH_CLEAR = 0x231D,
    ALU_CONSTANTS = 0x4000, FETCH_CONSTANTS = 0x4800, BOOL_CONSTANTS = 0x4900,
};

static constexpr uint32_t kTargetHeight = 1024;   // hauteur des cibles (l'EDRAM n'en fixe pas)

// Rendu interne : les cibles, viewports, scissors et copies sont multipliés par s_scale (1 = 720p d'origine).
// Les adresses, pas et tuiles restent en coordonnées du jeu.
static float s_scale = 1.0f;

// Mode large (expérimental) : la zone de jeu verticale (cibles de pas 600 et 560, viewport 540x720) est rendue sur
// 1280 de large ; la composante X de la projection (ligne c0 de g_mWVP) est multipliée par 540/1280 pour élargir le
// champ sans étirer. Dans l'image finale, les panneaux latéraux sont omis et la zone de jeu (x 370-910) étirée sur
// toute la largeur. Spécifique à Ikaruga.
static bool s_wide = false;
static constexpr uint32_t kWideWidth = 1280, kFieldWidth = 540, kFieldLeft = 370;
static bool IsFieldPitch(uint32_t pitch) { return pitch == 600 || pitch == 560; }
static bool s_wideFieldThisFrame = false;   // une zone de jeu large a été copiée dans l'image en cours
static inline uint32_t S(uint32_t v) { return uint32_t(v * s_scale + 0.5f); }
static inline int32_t S(int32_t v) { return int32_t(v * s_scale + (v >= 0 ? 0.5f : -0.5f)); }
static constexpr VkDeviceSize kUploadSize = 64 << 20;
extern std::atomic<uint64_t> g_dumpFrame;   // débogage en jeu (F9/F10, RenderDraw)
extern std::atomic<bool> g_debugNoDepth;

struct Image
{
    VkImage image = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED; VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
};

static bool s_ready;
static VkDevice s_dev;
static Image s_white;

// width/height : taille côté jeu ; hw/hh : taille réelle de l'image (rendu interne mis à l'échelle)
struct ColorTarget { Image image; VkFormat format; uint32_t width, height, hw, hh; bool wide = false; };
struct DepthTarget { Image image; uint32_t width, height, hw, hh; };
struct Resolved { Image image; VkFormat format = VK_FORMAT_UNDEFINED; uint32_t width = 0, height = 0, descriptor = 0, bpp = 4, hw = 0, hh = 0; bool wide = false; };
struct TextureFormatInfo { VkFormat vk; uint32_t blockSize, bytesPerBlock; };
static TextureFormatInfo TextureFormat(uint32_t fmt);
static std::unordered_map<uint64_t, ColorTarget> s_colorTargets;       // (base EDRAM, format, pitch)
static std::unordered_map<uint64_t, DepthTarget> s_depthTargets;       // (base EDRAM, taille)
static std::unordered_map<uint32_t, VkRenderPass> s_renderPasses;      // par format couleur (+ profondeur D32S8)
static std::unordered_map<uint64_t, VkFramebuffer> s_framebuffers;
static std::unordered_map<uint32_t, Resolved> s_resolved;              // adresse physique de destination -> copie
static uint32_t s_lastResolved;
static Resolved* s_presented;                                          // à remettre en lecture après présentation
static ColorTarget* s_curColor;
static DepthTarget* s_curDepth;
static VkDescriptorSetLayout s_setLayouts[4];
static VkDescriptorPool s_descPool;
static VkDescriptorSet s_sets[4];
static VkSampler s_sampler;
// Samplers : bit 0 point, bits 1-2 adressage U, bits 3-4 adressage V, bit 5 mip linéaire (64 combinaisons)
static VkSampler s_samplers[64];
static VkPipelineLayout s_pipelineLayout;
static std::unordered_map<uint64_t, VkPipeline> s_pipelines;
static std::unordered_map<const TranslatedShader*, VkShaderModule> s_modules;

static VkBuffer s_upload;
static VkDeviceMemory s_uploadMem;
static uint8_t* s_uploadPtr;
static VkDeviceAddress s_uploadAddr;
static uint64_t s_frameId;
uint32_t g_statPipelines, g_statTextures, g_statTextureKb;   // créations de l'image en cours (IKARUGA_TIMING)   // incrémenté à chaque image enregistrée
static VkDeviceSize s_uploadOffset, s_uploadBegin, s_uploadEnd;   // moitié du tampon propre à l'image en vol

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
    VkResult res = vkAllocateMemory(s_dev, &info, nullptr, &mem);
    static VkDeviceSize total = 0;
    if (res != VK_SUCCESS) KLOG("rendu : allocation de %llu Mo refusée (%d), %llu Mo déjà alloués", (unsigned long long)(req.size >> 20), int(res), (unsigned long long)(total >> 20));
    else if ((total += req.size) >> 26 != (total - req.size) >> 26) KLOG("rendu : %llu Mo de mémoire GPU alloués", (unsigned long long)(total >> 20));
    return mem;
}

// Mémoire des images : sous-allocation linéaire dans des blocs de 64 Mo par type de mémoire (les images ne sont
// jamais libérées). Un vkAllocateMemory par texture coûtait ~1 ms : 30 nouvelles textures dans une image (explosions,
// ennemis qui apparaissent) faisaient rater 2 ou 3 vblanks.
static VkDeviceMemory BindImageMemory(VkImage image)
{
    struct Block { VkDeviceMemory memory; VkDeviceSize used, size; };
    static std::unordered_map<uint32_t, Block> blocks;   // type de mémoire -> bloc courant
    static constexpr VkDeviceSize kBlock = 64ull << 20;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(s_dev, image, &req);
    uint32_t type = VulkanFindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (req.size > kBlock / 4)   // très grande image : allocation dédiée
    {
        VkDeviceMemory mem = Allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkBindImageMemory(s_dev, image, mem, 0);
        return mem;   // dédiée : libérée quand l'échelle change
    }
    Block& b = blocks[type];
    VkDeviceSize at = (b.used + req.alignment - 1) & ~(req.alignment - 1);
    if (!b.memory || at + req.size > b.size)
    {
        VkMemoryRequirements whole = req;
        whole.size = kBlock;
        b = { Allocate(whole, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT), 0, kBlock };
        at = 0;
    }
    vkBindImageMemory(s_dev, image, b.memory, at);
    b.used = at + req.size;
    return VK_NULL_HANDLE;
}

static Image CreateImage(uint32_t w, uint32_t h, VkImageUsageFlags usage, VkFormat format = VK_FORMAT_R8G8B8A8_UNORM,
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT)
{
    Image img;
    img.aspect = aspect;
    VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = { w, h, 1 };
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    vkCreateImage(s_dev, &ci, nullptr, &img.image);
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(s_dev, img.image, &req);
    img.memory = BindImageMemory(img.image);
    VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ci.format;
    vi.subresourceRange = { aspect == VK_IMAGE_ASPECT_COLOR_BIT ? aspect : VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT), 0, 1, 0, 1 };
    vkCreateImageView(s_dev, &vi, nullptr, &img.view);
    return img;
}

static void Transition(Image& img, VkImageLayout to)
{
    if (img.layout == to) return;
    VulkanImageBarrier(s_cmd, img.image, img.layout, to, img.aspect);
    img.layout = to;
}

static bool Init()
{
    s_dev = VulkanDevice();
    if (!s_dev) return false;

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

    VkDescriptorImageInfo samplerInfos[64];
    const VkSamplerAddressMode modes[4] = { VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER };
    for (int i = 0; i < 64; i++)
    {
        VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        si.magFilter = si.minFilter = (i & 1) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        si.mipmapMode = (i & 32) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = modes[(i >> 1) & 3];
        si.addressModeV = si.addressModeW = modes[(i >> 3) & 3];
        si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        si.maxLod = VK_LOD_CLAMP_NONE;
        vkCreateSampler(s_dev, &si, nullptr, &s_samplers[i]);
        samplerInfos[i] = { s_samplers[i], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    }
    s_sampler = s_samplers[0];

    VkDescriptorImageInfo whiteInfo{ VK_NULL_HANDLE, s_white.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet writes[2]{};
    writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, s_sets[0], 0, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &whiteInfo };
    writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, s_sets[3], 0, 0, 64, VK_DESCRIPTOR_TYPE_SAMPLER, samplerInfos };
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

// Changement de résolution interne : toutes les cibles et copies sont recréées à la nouvelle taille au fil des
// dessins. Appelé en début d'image, avant tout enregistrement ; les grandes images (allocation dédiée) sont libérées.
static void RebuildForScale(float scale)
{
    vkDeviceWaitIdle(s_dev);
    auto destroy = [](Image& img) {
        if (img.view) vkDestroyImageView(s_dev, img.view, nullptr);
        if (img.image) vkDestroyImage(s_dev, img.image, nullptr);
        if (img.memory) vkFreeMemory(s_dev, img.memory, nullptr);
        img = {};
    };
    for (auto& [k, fb] : s_framebuffers) vkDestroyFramebuffer(s_dev, fb, nullptr);
    s_framebuffers.clear();
    for (auto& [k, t] : s_colorTargets) destroy(t.image);
    for (auto& [k, t] : s_depthTargets) destroy(t.image);
    s_colorTargets.clear();
    s_depthTargets.clear();
    for (auto& [k, r] : s_resolved) destroy(r.image);   // descripteur conservé : réécrit à la recréation
    s_curColor = nullptr; s_curDepth = nullptr; s_presented = nullptr;
    KLOG("rendu : résolution interne x%.2f", scale);
    s_scale = scale;
}

// Command buffer de l'image en cours ; au premier appel d'une image, réinitialise le tampon d'envoi
static bool BeginCommands()
{
    if (!s_ready) { if (!(s_ready = Init())) return false; }
    s_cmd = VulkanFrameCmd();
    if (!s_frameStarted)
    {
        if (float want = SettingsScale(); want != s_scale || g_settings.wide != s_wide) { s_wide = g_settings.wide; RebuildForScale(want); }
        s_wideFieldThisFrame = false;
        s_frameStarted = true;
        s_frameId++;
        s_uploadBegin = s_uploadOffset = VulkanFrameSlot() * (kUploadSize / 2);
        s_uploadEnd = s_uploadBegin + kUploadSize / 2;
        if (s_presented) { Transition(s_presented->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); s_presented = nullptr; }
        static bool first = true;
        if (first)
        {
            first = false;
            VkClearColorValue black{}, white{ { 1, 0, 1, 1 } };   // magenta temporaire : rend visibles les textures manquantes
            VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
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
    if (s_uploadOffset + size > s_uploadEnd) { KLOG("rendu : tampon d'envoi plein"); s_uploadOffset = s_uploadBegin; }
    VkDeviceSize at = s_uploadOffset;
    if (data) memcpy(s_uploadPtr + at, data, size);
    s_uploadOffset += size;
    return at;
}

// Constantes : la plupart des dessins consécutifs gardent les mêmes ; on réutilise alors la copie précédente
// de l'image en cours au lieu de réécrire ~9 Ko par dessin dans la mémoire partagée avec la carte
static VkDeviceSize UploadConstants(int slot, const void* data, VkDeviceSize size)
{
    static std::vector<uint8_t> last[3];
    static VkDeviceSize lastAt[3];
    static uint64_t lastFrame[3];
    auto& l = last[slot];
    if (lastFrame[slot] == s_frameId && l.size() == size && !memcmp(l.data(), data, size))
        return lastAt[slot];
    l.assign((const uint8_t*)data, (const uint8_t*)data + size);
    lastFrame[slot] = s_frameId;
    return lastAt[slot] = Upload(data, size, 256);
}

static void EndRenderPass()
{
    if (s_inRenderPass) { vkCmdEndRenderPass(s_cmd); s_inRenderPass = false; }
}

// ------------------------------------------------------------------ cibles de rendu (EDRAM)

static VkFormat ColorFormat(uint32_t f)
{
    switch (f)
    {
    case 0: case 1: return VK_FORMAT_R8G8B8A8_UNORM;                 // 8_8_8_8 (+ gamma)
    case 2: case 10: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;      // 2_10_10_10
    case 3: case 12: return VK_FORMAT_R16G16B16A16_SFLOAT;           // 2_10_10_10_FLOAT (7e3) -> flottant 16 bits
    case 4: case 6: return VK_FORMAT_R16G16_SFLOAT;                  // 16_16 / 16_16_FLOAT
    case 5: case 7: return VK_FORMAT_R16G16B16A16_SFLOAT;            // 16_16_16_16 / FLOAT
    case 14: return VK_FORMAT_R32_SFLOAT;
    case 15: return VK_FORMAT_R32G32_SFLOAT;
    default: return VK_FORMAT_R8G8B8A8_UNORM;
    }
}

static VkRenderPass RenderPassFor(VkFormat format)
{
    auto& rp = s_renderPasses[uint32_t(format)];
    if (rp) return rp;
    VkAttachmentDescription att[2]{};
    att[0].format = format;
    att[1].format = VK_FORMAT_D32_SFLOAT_S8_UINT;
    for (auto& a : att)
    {
        a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        a.storeOp = a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    att[0].initialLayout = att[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att[1].initialLayout = att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference color{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL }, depth{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &color;
    sub.pDepthStencilAttachment = &depth;
    VkRenderPassCreateInfo ci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    ci.attachmentCount = 2;
    ci.pAttachments = att;
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    vkCreateRenderPass(s_dev, &ci, nullptr, &rp);
    return rp;
}

static ColorTarget& GetColorTarget(const uint32_t* regs)
{
    uint32_t pitch = regs[RB_SURFACE_INFO] & 0x3FFF, info = regs[RB_COLOR_INFO];
    uint32_t base = info & 0xFFF, format = (info >> 16) & 0xF;
    uint32_t width = pitch ? pitch : 1280;
    uint64_t key = uint64_t(base) | uint64_t(format) << 12 | uint64_t(width) << 16;
    auto& t = s_colorTargets[key];
    if (!t.image.image)
    {
        EndRenderPass();   // effacement initial impossible dans une passe de rendu
        t.format = ColorFormat(format);
        t.width = width;
        t.height = kTargetHeight;
        t.wide = s_wide && IsFieldPitch(width);
        t.hw = S(t.wide ? kWideWidth + 64 : width); t.hh = S(kTargetHeight);
        t.image = CreateImage(t.hw, t.hh, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, t.format);
        VkClearColorValue black{};
        VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        Transition(t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(s_cmd, t.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        KLOG("rendu : cible couleur base %u format %u (%ux%u)", base, format, width, kTargetHeight);
    }
    return t;
}

static DepthTarget& GetDepthTarget(const uint32_t* regs, uint32_t width, uint32_t height)
{
    uint32_t base = regs[RB_DEPTH_INFO] & 0xFFF;
    uint64_t key = uint64_t(base) | uint64_t(width) << 12 | uint64_t(height) << 32;
    auto& t = s_depthTargets[key];
    if (!t.image.image)
    {
        EndRenderPass();
        t.width = width; t.height = height;
        t.hw = S(width); t.hh = S(height);
        t.image = CreateImage(t.hw, t.hh, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
        VkClearDepthStencilValue clear{ 1.0f, 0 };
        VkImageSubresourceRange range{ VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1 };
        Transition(t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearDepthStencilImage(s_cmd, t.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    }
    return t;
}

// Démarre (ou continue) la passe de rendu sur les cibles désignées par les registres
static void BeginTargets(const uint32_t* regs)
{
    ColorTarget& color = GetColorTarget(regs);
    DepthTarget& depth = GetDepthTarget(regs, color.wide ? kWideWidth + 64 : color.width, color.height);
    if (s_inRenderPass && s_curColor == &color && s_curDepth == &depth) return;
    EndRenderPass();
    s_curColor = &color;
    s_curDepth = &depth;
    VkRenderPass rp = RenderPassFor(color.format);
    uint64_t key = XXH3_64bits(std::array<uint64_t, 2>{ uint64_t(color.image.image), uint64_t(depth.image.image) }.data(), 16);
    auto& fb = s_framebuffers[key];
    if (!fb)
    {
        VkImageView views[2] = { color.image.view, depth.image.view };
        VkFramebufferCreateInfo ci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        ci.renderPass = rp;
        ci.attachmentCount = 2;
        ci.pAttachments = views;
        ci.width = color.hw;
        ci.height = color.hh;
        ci.layers = 1;
        vkCreateFramebuffer(s_dev, &ci, nullptr, &fb);
    }
    Transition(color.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    Transition(depth.image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    VkRenderPassBeginInfo rb{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rb.renderPass = rp;
    rb.framebuffer = fb;
    rb.renderArea = { { 0, 0 }, { color.hw, color.hh } };
    vkCmdBeginRenderPass(s_cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    s_inRenderPass = true;
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
    bool packedNormal = false;    // normale compressée 11-11-10 (constante de spécialisation R11G11B10_NORMAL)
};

static bool DecodeVertexLayout(const uint32_t* regs, VertexLayout& layout)
{
    bool ok = true;
    // Un mini-fetch (dword 1 bit 30) n'a ni constante de fetch ni pas : il reprend ceux du dernier fetch complet
    uint32_t lastConst = 0, lastStride = 0;
    // Emplacements déjà pris par une correspondance exacte : un repli par registre seul ne doit pas y retomber.
    // Sol de forêt : position et couleur lues toutes deux dans r1 ; la position retombait sur l'emplacement de la
    // couleur et l'entrée 0 (position) restait sans flux (NVIDIA tolère, AMD dessine tout en noir)
    uint32_t exactTaken = 0;
    ForEachVertexFetch(s_vsUcode.data(), uint32_t(s_vsUcode.size()), [&](uint32_t, const uint32_t* w) {
        for (auto& [k, loc] : s_vs->fetchRegLocations) if (k == VertexFetchKey(w) && loc < 32) exactTaken |= 1u << loc;
    });
    ForEachVertexFetch(s_vsUcode.data(), uint32_t(s_vsUcode.size()), [&](uint32_t address, const uint32_t* w) {
        bool mini = (w[1] >> 30) & 1;
        if (!mini) { lastConst = ((w[0] >> 20) & 0x1F) * 3 + ((w[0] >> 25) & 3); lastStride = (w[2] & 0xFF) * 4; }
        // Correspondance par registre et composantes écrites ; à défaut, par registre seul (hors emplacements pris,
        // de préférence celui de la même adresse d'instruction)
        uint32_t location = UINT32_MAX, fetchKey = VertexFetchKey(w);
        for (auto& [k, loc] : s_vs->fetchRegLocations) if (k == fetchKey) location = loc;
        if (location == UINT32_MAX)
        {
            for (auto& [k, loc] : s_vs->fetchRegLocations)
                if ((k & 0xFF) == (fetchKey & 0xFF) && !(loc < 32 && (exactTaken >> loc) & 1)) location = loc;
            for (auto& [a, loc] : s_vs->fetchLocations)
                if (a == address && !(loc < 32 && (exactTaken >> loc) & 1))
                    for (auto& [k, l2] : s_vs->fetchRegLocations) if (l2 == loc && (k & 0xFF) == (fetchKey & 0xFF)) location = loc;
            if (location == UINT32_MAX)   // ancien repli
                for (auto& [k, loc] : s_vs->fetchRegLocations) if ((k & 0xFF) == (fetchKey & 0xFF)) location = loc;
        }
        static bool flog = getenv("IKARUGA_FETCHLOG");
        if (flog)
        {
            static std::unordered_map<uint64_t, int> seen;
            static std::unordered_map<uint64_t, int> seenVs;
            if (!seenVs[s_vs->containerHash]++)
                for (auto& [fa, fl] : s_vs->fetchLocations) KLOG("  table vs %016llx : adresse %u -> emplacement %u", (unsigned long long)s_vs->containerHash, fa, fl);
            if (!seen[s_vs->containerHash ^ (uint64_t(address) << 40)]++)
                KLOG("vfetch vs %016llx adr %u mini %d format %u offset %d dst r%u swz %03X stride %u -> emplacement %d | %08X %08X %08X", (unsigned long long)s_vs->containerHash, address, mini,
                    (w[1] >> 16) & 0x3F, int32_t(w[2] << 1) >> 9, (w[0] >> 12) & 0x3F, w[1] & 0xFFF, w[2] & 0xFF, int(location), w[0], w[1], w[2]);
        }
        if (location == UINT32_MAX || layout.attributeCount >= 16) return;   // attribut non déclaré
        uint32_t constIndex = lastConst;
        uint32_t format = (w[1] >> 16) & 0x3F, stride = lastStride;
        int32_t offset = int32_t(w[2] << 1) >> 9;   // 23 bits signés, en dwords
        VkFormat vk = VertexFormat(format, (w[1] >> 12) & 1, ((w[1] >> 13) & 1) == 0);
        // NORMAL, TANGENT, BINORMAL (emplacements 1-3) et BLENDINDICES (9) sont des uint4 dans le SPIR-V : bits bruts
        if (location >= 1 && location <= 3)
        {
            if (format == 16 || format == 17 || format == 7) { layout.packedNormal |= location == 1; vk = VK_FORMAT_R32_UINT; }
            else if (format == 36) vk = VK_FORMAT_R32_UINT;
            else if (format == 37) vk = VK_FORMAT_R32G32_UINT;
            else if (format == 57) vk = VK_FORMAT_R32G32B32_UINT;
            else if (format == 38) vk = VK_FORMAT_R32G32B32A32_UINT;
        }
        else if (location == 9 && format == 6) vk = VK_FORMAT_R8G8B8A8_UINT;
        if (vk == VK_FORMAT_UNDEFINED) { KLOG("rendu : format de sommet %u non géré", format); ok = false; return; }
        if (format < 36) layout.allFloat32 = false;
        static bool vlog = getenv("IKARUGA_VTXLOG");
        if (vlog)
        {
            static std::unordered_map<uint32_t, int> seen;
            uint32_t endian = regs[FETCH_CONSTANTS + constIndex * 2 + 1] & 3;
            if (!seen[format | endian << 8]++) KLOG("sommets : format %u endian %u (fetch %u, ps %016llx)", format, endian, constIndex, (unsigned long long)(s_ps ? s_ps->containerHash : 0));
        }
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

static VkPipeline GetPipeline(const uint32_t* regs, const VertexLayout& layout, VkPrimitiveTopology topology, uint32_t spec, bool depthOnly)
{
    uint32_t blend = regs[RB_BLENDCONTROL0], mask = depthOnly ? 0 : regs[RB_COLOR_MASK] & 0xF;
    uint32_t depthControl = regs[RB_DEPTHCONTROL] & 0x76;   // test, écriture, fonction
    // Depth clamp toujours : le sol de forêt du chapitre 1 est posé pile sur le plan lointain (PA_CL_CLIP_CNTL ne
    // désactive pourtant rien, 00080000) ; NVIDIA et AMDVLK le gardent, RADV (Steam Deck) l'élimine. Caméra fixe
    // vue de dessus : pas de géométrie derrière le plan proche à craindre. IKARUGA_NOCLAMP=1 pour comparer.
    static const bool noClamp = getenv("IKARUGA_NOCLAMP");
    if (!noClamp) depthControl |= 1;
    if (g_debugNoDepth) depthControl |= 0x80;
    VkFormat format = s_curColor->format;
    struct Key { uint64_t vs, ps; uint32_t blend, mask, topology, spec, depthControl, format; uint64_t layoutHash; } key{
        s_vs->containerHash, s_ps->containerHash, blend, mask, uint32_t(topology), spec, depthControl, uint32_t(format),
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
    ia.primitiveRestartEnable = topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN ||
        topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;   // TODO : PA_SU_SC_MODE_CNTL
    rs.lineWidth = 1.0f;
    rs.depthClampEnable = depthControl & 1;
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
    dss.depthTestEnable = (depthControl >> 1) & 1;
    static const bool noDepth = getenv("IKARUGA_NODEPTH");   // débogage
    if (noDepth || (depthControl & 0x80)) dss.depthTestEnable = false;
    dss.depthWriteEnable = dss.depthTestEnable && ((depthControl >> 2) & 1);
    static const VkCompareOp ops[] = { VK_COMPARE_OP_NEVER, VK_COMPARE_OP_LESS, VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_LESS_OR_EQUAL,
        VK_COMPARE_OP_GREATER, VK_COMPARE_OP_NOT_EQUAL, VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS };
    dss.depthCompareOp = ops[(depthControl >> 4) & 7];

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
    pci.renderPass = RenderPassFor(format);
    VkPipeline pipeline = VK_NULL_HANDLE;
    g_statPipelines++;
    VkResult r = vkCreateGraphicsPipelines(s_dev, VK_NULL_HANDLE, 1, &pci, nullptr, &pipeline);
    if (r != VK_SUCCESS) KLOG("rendu : échec de création du pipeline (%d)", r);
    s_pipelines[h] = pipeline;
    return pipeline;
}

// ------------------------------------------------------------------ resolve (copie EDRAM -> tampon d'affichage)

static uint32_t s_nextDescriptor = 1;   // 0 = texture de remplacement

static void Resolve(const uint32_t* regs, uint32_t (*phys)(uint32_t))
{
    EndRenderPass();
    // Rectangle du resolve : 3 sommets (x, y) flottants big-endian désignés par la constante de fetch 0
    uint32_t va = regs[FETCH_CONSTANTS] & 0x1FFFFFFC;
    float v[6] = {};
    for (int i = 0; i < 6; i++) { uint32_t w = __builtin_bswap32(*(const uint32_t*)(g_base + phys(va + i * 4))); memcpy(&v[i], &w, 4); }
    static uint64_t logFrom = getenv("IKARUGA_RESOLVELOG") ? strtoull(getenv("IKARUGA_RESOLVELOG"), nullptr, 10) : 0;
    extern uint64_t g_presentCount;
    static int logged;
    if (logFrom && g_presentCount >= logFrom && logged++ < 200)
    {
        KLOG("resolve img=%llu woff=%08X sc=%08X/%08X mode=%08X surf=%08X color=%08X ctl=%08X dest=%08X pitch=%08X info=%08X rect=(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f)",
            (unsigned long long)g_presentCount, regs[0x2080], regs[PA_SC_WINDOW_SCISSOR_TL], regs[PA_SC_WINDOW_SCISSOR_BR], regs[PA_SU_SC_MODE_CNTL],
            regs[RB_SURFACE_INFO], regs[RB_COLOR_INFO], regs[RB_COPY_CONTROL], regs[RB_COPY_DEST_BASE], regs[RB_COPY_DEST_PITCH], regs[0x231B],
            v[0], v[1], v[2], v[3], v[4], v[5]);
    }
    uint32_t control = regs[RB_COPY_CONTROL];
    ColorTarget& color = GetColorTarget(regs);
    DepthTarget& depth = GetDepthTarget(regs, color.wide ? kWideWidth + 64 : color.width, color.height);   // même tampon que BeginTargets
    uint32_t source = control & 7, command = (control >> 20) & 3;
    uint32_t destBase = regs[RB_COPY_DEST_BASE] & 0x1FFFFFFF, destPitch = regs[RB_COPY_DEST_PITCH] & 0x3FFF;
    uint32_t destHeight = (regs[RB_COPY_DEST_PITCH] >> 16) & 0x3FFF;
    uint32_t w = destPitch ? destPitch : color.width, h = std::min(destHeight ? destHeight : 720u, kTargetHeight);

    // Rendu par tuiles : chaque tuile est dessinée à sa place dans la cible hôte (assez grande, on n'applique pas
    // PA_SC_WINDOW_OFFSET), puis copiée vers une base décalée d'autant de lignes dans la même texture invitée.
    int32_t x0 = int32_t(std::min({ v[0], v[2], v[4] })), y0 = int32_t(std::min({ v[1], v[3], v[5] }));
    int32_t x1 = int32_t(std::max({ v[0], v[2], v[4] })), y1 = int32_t(std::max({ v[1], v[3], v[5] }));
    if (x1 <= x0 || y1 <= y0) { x0 = y0 = 0; x1 = int32_t(w); y1 = int32_t(h); }
    x0 = std::clamp(x0, 0, int32_t(color.width)); x1 = std::clamp(x1, x0, int32_t(color.width));
    y0 = std::clamp(y0, 0, int32_t(color.height)); y1 = std::clamp(y1, y0, int32_t(color.height));
    int32_t dx = x0, dy = y0;
    if (regs[PA_SU_SC_MODE_CNTL] & (1 << 16))   // vtx_window_offset_enable : position dans la tuile
    {
        uint32_t woff = regs[0x2080];
        dx += int32_t(woff << 17) >> 17; dy += int32_t(woff << 1) >> 17;
    }
    uint32_t bpp = TextureFormat((regs[0x231B] >> 7) & 0x3F).bytesPerBlock;
    if (!bpp) bpp = 4;
    uint32_t key = destBase;
    for (auto& [base, res] : s_resolved)   // base décalée à l'intérieur d'une copie existante (tuile suivante)
    {
        uint32_t row = res.width * res.bpp;
        if (base < destBase && res.image.image && row && destBase < base + row * res.height && (destBase - base) % row == 0)
        { key = base; dy += int32_t((destBase - base) / row); w = res.width; h = res.height; break; }
    }

    // Resolve de la profondeur (source 4) : le décor relit la profondeur de la scène (texture 24_8_FLOAT) ;
    // sans elle il échantillonnait la texture blanche (profondeur 1 partout) et ses surfaces devenaient unies.
    // Vulkan ne copie pas une image de profondeur vers une image couleur : on passe par un tampon.
    // Mode large : la copie couvre toute la largeur rendue, la destination devient large elle aussi
    uint32_t hostW = S(w);
    if (color.wide) { x0 = 0; x1 = int32_t(kWideWidth); hostW = S(kWideWidth); s_wideFieldThisFrame = true; }
    if (source == 4 && command != 3 && destBase && w && h && x1 > x0 && y1 > y0)
    {
        Resolved& r = s_resolved[key];
        if (!r.image.image || r.width != w || r.height != h || r.format != VK_FORMAT_R32_SFLOAT || r.hw != hostW)
        {
            r.image = CreateImage(hostW, S(h), VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_FORMAT_R32_SFLOAT);
            r.width = w; r.height = h; r.hw = hostW; r.hh = S(h); r.format = VK_FORMAT_R32_SFLOAT; r.bpp = bpp; r.wide = color.wide;
            if (!r.descriptor) r.descriptor = s_nextDescriptor++;
            VkDescriptorImageInfo ii{ VK_NULL_HANDLE, r.image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkWriteDescriptorSet wr{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, s_sets[0], 0, r.descriptor, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &ii };
            vkUpdateDescriptorSets(s_dev, 1, &wr, 0, nullptr);
            KLOG("rendu : resolve profondeur -> %08X (%ux%u)", destBase, w, h);
        }
        static VkBuffer buf;
        static VkDeviceMemory bufMem;
        static float bufScale;
        const VkDeviceSize kSize = VkDeviceSize(S(1280u)) * S(kTargetHeight) * 4;
        if (buf && bufScale != s_scale) { vkDestroyBuffer(s_dev, buf, nullptr); vkFreeMemory(s_dev, bufMem, nullptr); buf = VK_NULL_HANDLE; }
        if (!buf)
        {
            bufScale = s_scale;
            VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bi.size = kSize;
            bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            vkCreateBuffer(s_dev, &bi, nullptr, &buf);
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(s_dev, buf, &req);
            bufMem = Allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            vkBindBufferMemory(s_dev, buf, bufMem, 0);
        }
        dx = std::max(dx, 0); dy = std::max(dy, 0);
        uint32_t cw = uint32_t(std::min(x1 - x0, int32_t(color.wide ? kWideWidth : w) - dx)), ch = uint32_t(std::min(y1 - y0, int32_t(h) - dy));
        // en pixels hôte (rendu interne mis à l'échelle)
        int32_t sx0 = S(x0), sy0 = S(y0), sdx = S(dx), sdy = S(dy);
        cw = uint32_t(std::min(S(x0 + int32_t(cw)) - sx0, int32_t(r.hw) - sdx)); ch = uint32_t(std::min(S(y0 + int32_t(ch)) - sy0, int32_t(r.hh) - sdy));
        if (int32_t(cw) > 0 && int32_t(ch) > 0 && VkDeviceSize(cw) * ch * 4 <= kSize)
        {
            Transition(depth.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy c{};
            c.imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
            c.imageOffset = { sx0, sy0, 0 };
            c.imageExtent = { cw, ch, 1 };
            vkCmdCopyImageToBuffer(s_cmd, depth.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
            VkBufferMemoryBarrier bb{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
            bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; bb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bb.buffer = buf; bb.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(s_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &bb, 0, nullptr);
            Transition(r.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            c.imageOffset = { sdx, sdy, 0 };
            vkCmdCopyBufferToImage(s_cmd, buf, r.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            Transition(r.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    }
    if (source < 4 && command != 3 && destBase && w && h && x1 > x0 && y1 > y0)
    {
        Resolved& r = s_resolved[key];
        if (!r.image.image || r.width != w || r.height != h || r.format != color.format || r.hw != hostW)
        {
            r.image = CreateImage(hostW, S(h), VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, color.format);
            r.width = w; r.height = h; r.hw = hostW; r.hh = S(h); r.format = color.format; r.bpp = bpp; r.wide = color.wide;
            if (!r.descriptor) r.descriptor = s_nextDescriptor++;
            VkDescriptorImageInfo ii{ VK_NULL_HANDLE, r.image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkWriteDescriptorSet wr{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, s_sets[0], 0, r.descriptor, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &ii };
            vkUpdateDescriptorSets(s_dev, 1, &wr, 0, nullptr);
            KLOG("rendu : resolve -> %08X (%ux%u)", destBase, w, h);
        }
        Transition(color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        Transition(r.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy copy{};
        copy.srcSubresource = copy.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        dx = std::max(dx, 0); dy = std::max(dy, 0);
        int32_t ex = std::min(x1 - x0, int32_t(color.wide ? kWideWidth : w) - dx), ey = std::min(y1 - y0, int32_t(h) - dy);
        copy.srcOffset = { S(x0), S(y0), 0 };
        copy.dstOffset = { S(dx), S(dy), 0 };
        copy.extent = { uint32_t(std::min(S(x0 + ex) - S(x0), int32_t(r.hw) - S(dx))), uint32_t(std::min(S(y0 + ey) - S(y0), int32_t(r.hh) - S(dy))), 1 };
        if (int32_t(copy.extent.width) > 0 && int32_t(copy.extent.height) > 0)
            vkCmdCopyImage(s_cmd, color.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        Transition(r.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        s_lastResolved = key;
    }
    if (control & (1 << 8))   // effacement couleur
    {
        uint32_t c = regs[RB_COLOR_CLEAR];
        VkClearColorValue value{ { ((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f, (c >> 24) / 255.0f } };
        VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        Transition(color.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(s_cmd, color.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
    }
    if (control & (1 << 9))   // effacement profondeur/stencil (D24S8)
    {
        uint32_t d = regs[RB_DEPTH_CLEAR];
        VkClearDepthStencilValue value{ (d >> 8) / 16777215.0f, d & 0xFF };
        VkImageSubresourceRange range{ VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1 };
        Transition(depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearDepthStencilImage(s_cmd, depth.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
    }
}

// ------------------------------------------------------------------ textures

static TextureFormatInfo TextureFormat(uint32_t fmt)
{
    switch (fmt)
    {
    case 2:  return { VK_FORMAT_R8_UNORM, 1, 1 };                    // k_8
    case 4:  return { VK_FORMAT_R5G6B5_UNORM_PACK16, 1, 2 };         // k_5_6_5
    case 6:  return { VK_FORMAT_R8G8B8A8_UNORM, 1, 4 };              // k_8_8_8_8 (ordre des canaux : swizzle de la fetch constant, comme Xenia)
    case 10: return { VK_FORMAT_R8G8_UNORM, 1, 2 };                  // k_8_8
    case 15: return { VK_FORMAT_B4G4R4A4_UNORM_PACK16, 1, 2 };       // k_4_4_4_4
    case 24: return { VK_FORMAT_R16_UNORM, 1, 2 };                   // k_16
    case 25: return { VK_FORMAT_R16G16_UNORM, 1, 4 };                // k_16_16
    case 26: return { VK_FORMAT_R16G16B16A16_UNORM, 1, 8 };          // k_16_16_16_16
    case 30: return { VK_FORMAT_R16_SFLOAT, 1, 2 };                  // k_16_FLOAT
    case 31: return { VK_FORMAT_R16G16_SFLOAT, 1, 4 };               // k_16_16_FLOAT
    case 32: return { VK_FORMAT_R16G16B16A16_SFLOAT, 1, 8 };         // k_16_16_16_16_FLOAT
    case 36: return { VK_FORMAT_R32_SFLOAT, 1, 4 };                  // k_32_FLOAT
    case 37: return { VK_FORMAT_R32G32_SFLOAT, 1, 8 };               // k_32_32_FLOAT
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

struct CachedTexture { Image image, staging; uint32_t descriptor = 0, levels = 1; uint64_t contentHash = 0; };

VkPhysicalDevice VulkanPhysicalDevice();

// Fonctionnalités d'un format (blit filtré pour générer les mips)
static bool CanBlit(VkFormat format, VkFormatFeatureFlags need)
{
    static std::unordered_map<uint32_t, VkFormatFeatureFlags> cache;
    auto it = cache.find(format);
    if (it == cache.end())
    {
        VkFormatProperties p{};
        vkGetPhysicalDeviceFormatProperties(VulkanPhysicalDevice(), format, &p);
        it = cache.emplace(format, p.optimalTilingFeatures).first;
    }
    return (it->second & need) == need;
}
static std::unordered_map<uint64_t, CachedTexture> s_textures;
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
    static bool texLog = getenv("IKARUGA_TEXLOG");
    static bool formatsLogged = false;
    if (texLog && !formatsLogged)
    {
        // Fonctionnalités de chaque format de texture selon le pilote (comparaison entre GPU)
        formatsLogged = true;
        for (uint32_t fmt = 0; fmt < 64; fmt++)
            if (VkFormat vk = TextureFormat(fmt).vk; vk != VK_FORMAT_UNDEFINED)
            {
                VkFormatProperties p{};
                vkGetPhysicalDeviceFormatProperties(VulkanPhysicalDevice(), vk, &p);
                KLOG("format %u (vk %d) : optimal %08X", fmt, int(vk), p.optimalTilingFeatures);
            }
    }
    if (texLog)
    {
        // Combinaisons distinctes : format, signes (3 = gamma), exp_adjust, filtres mag/min/mip, niveaux de mip
        static std::unordered_map<uint64_t, int> seen;
        int exp = int32_t(f[3] << 13) >> 26;
        uint64_t k = uint64_t((f[0] >> 10) & 0x1FF) << 48 | (f[1] & 0x3F) | uint64_t((f[0] >> 2) & 0xFF) << 8 | uint64_t(exp & 0x3F) << 16 | uint64_t((f[3] >> 19) & 0x3F) << 24 | uint64_t((f[4] >> 2) & 0xFF) << 32;
        if (!seen[k]++) KLOG("texture : adressage %o format %u signes %02X exp %d filtres mag %u min %u mip %u, mips %u-%u, %ux%u, pix_center %u",
            (f[0] >> 10) & 0x1FF, f[1] & 0x3F, (f[0] >> 2) & 0xFF, exp, (f[3] >> 19) & 3, (f[3] >> 21) & 3, (f[3] >> 23) & 3, (f[4] >> 2) & 15, (f[4] >> 6) & 15,
            (f[2] & 0x1FFF) + 1, ((f[2] >> 13) & 0x1FFF) + 1, 0u);
    }
    uint32_t format = f[1] & 0x3F, endian = (f[1] >> 6) & 3, base = (f[1] >> 12) << 12;
    uint32_t width = (f[2] & 0x1FFF) + 1, height = ((f[2] >> 13) & 0x1FFF) + 1, dim = (f[5] >> 9) & 3;
    if (dim == 0) { width = (f[2] & 0xFFFFFF) + 1; height = 1; dim = 1; }   // 1D : texture 2D d'une ligne
    uint32_t pitch = ((f[0] >> 22) & 0x1FF) << 5;
    bool tiled = f[0] >> 31;
    if (auto it = s_resolved.find(base & 0x1FFFFFFF); it != s_resolved.end() && it->second.image.image)
        return it->second.descriptor;   // rendue par le GPU : on échantillonne directement la copie
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
    g_statTextures++; g_statTextureKb += srcSize >> 10;
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

    // Mips : le jeu fournit les siennes (souvent 0 à 8-10), on les régénère sur le GPU à partir du niveau 0 par
    // blits filtrés ; sans elles, les textures réduites (polices en particulier) crénellent.
    // Les formats compressés passent par une image intermédiaire et sont décompressés en RGBA8.
    uint32_t mipMax = (f[4] >> 6) & 15, levels = 1;
    if (((f[3] >> 23) & 3) != 2 && mipMax)
        levels = std::min(mipMax + 1, 32u - __builtin_clz(std::max(width, height)));
    VkFormat finalFormat = info.vk;
    const VkFormatFeatureFlags blitDst = VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    bool viaStaging = false;
    if (levels > 1 && !CanBlit(info.vk, blitDst))
    {
        if (CanBlit(info.vk, VK_FORMAT_FEATURE_BLIT_SRC_BIT) && CanBlit(VK_FORMAT_R8G8B8A8_UNORM, blitDst)) { finalFormat = VK_FORMAT_R8G8B8A8_UNORM; viaStaging = true; }
        else levels = 1;
    }

    // Débogage : IKARUGA_TEXDUMP=1 -> écrit chaque texture dé-tuilée (build/tex/<format>_<l>x<h>_<n>.bin)
    static bool texDump = getenv("IKARUGA_TEXDUMP");
    if (texDump)
    {
        static int n;
        char name[128]; snprintf(name, sizeof name, "build/tex/%u_%ux%u_%04d_%s.bin", format, width, height, n++, tiled ? "t" : "l");
        if (FILE* fd = fopen(name, "wb")) { fwrite(dst, 1, size_t(rowBytes) * bh, fd); fclose(fd); }
    }
    bool created = it == s_textures.end();
    CachedTexture& t = s_textures[key];
    if (created)
    {
        t.levels = levels;
        VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = finalFormat;
        ci.extent = { width, height, 1 };
        ci.mipLevels = levels;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (viaStaging) t.staging = CreateImage(width, height, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, info.vk);
        vkCreateImage(s_dev, &ci, nullptr, &t.image.image);
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(s_dev, t.image.image, &req);
        BindImageMemory(t.image.image);
        uint32_t sw = (f[3] >> 1) & 0xFFF;
        VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = t.image.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = finalFormat;
        vi.components = { Swizzle(sw), Swizzle(sw >> 3), Swizzle(sw >> 6), Swizzle(sw >> 9) };
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1 };
        vkCreateImageView(s_dev, &vi, nullptr, &t.image.view);
        t.descriptor = s_nextDescriptor++;
        VkDescriptorImageInfo ii{ VK_NULL_HANDLE, t.image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, s_sets[0], 0, t.descriptor, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &ii };
        vkUpdateDescriptorSets(s_dev, 1, &w, 0, nullptr);
        static const bool drawLog = getenv("IKARUGA_DRAWLOG");
        if (drawLog)
            KLOG("texture %ux%u format %u %s swizzle %03X -> descripteur %u", width, height, format, tiled ? "tuilée" : "linéaire", sw, t.descriptor);
    }
    t.contentHash = content;
    Transition(t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.bufferOffset = at;
    copy.bufferRowLength = bw * info.blockSize;
    copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.imageExtent = { width, height, 1 };
    if (t.staging.image)
    {
        Transition(t.staging, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyBufferToImage(s_cmd, s_upload, t.staging.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        Transition(t.staging, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkImageBlit b{};
        b.srcSubresource = b.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        b.srcOffsets[1] = b.dstOffsets[1] = { int32_t(width), int32_t(height), 1 };
        vkCmdBlitImage(s_cmd, t.staging.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
    }
    else
        vkCmdCopyBufferToImage(s_cmd, s_upload, t.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    for (uint32_t level = 1; level < t.levels; level++)
    {
        // niveau précédent : écriture terminée -> source du blit
        VkImageMemoryBarrier mb{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        mb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; mb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        mb.srcQueueFamilyIndex = mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        mb.image = t.image.image;
        mb.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 1, 0, 1 };
        vkCmdPipelineBarrier(s_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &mb);
        VkImageBlit b{};
        b.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1 };
        b.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 };
        b.srcOffsets[1] = { int32_t(std::max(1u, width >> (level - 1))), int32_t(std::max(1u, height >> (level - 1))), 1 };
        b.dstOffsets[1] = { int32_t(std::max(1u, width >> level)), int32_t(std::max(1u, height >> level)), 1 };
        vkCmdBlitImage(s_cmd, t.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
    }
    if (t.levels > 1)
    {
        // niveaux 0..n-2 en source, le dernier en destination : tout passe en lecture
        VkImageMemoryBarrier mb[2]{};
        for (auto& m : mb)
        {
            m.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            m.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT; m.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            m.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            m.srcQueueFamilyIndex = m.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            m.image = t.image.image;
        }
        mb[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; mb[0].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, t.levels - 1, 0, 1 };
        mb[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; mb[1].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, t.levels - 1, 1, 0, 1 };
        vkCmdPipelineBarrier(s_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 2, mb);
        t.image.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    else
        Transition(t.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return t.descriptor;
}

// Sampler : filtre (mag), adressage X/Y et filtre de mip (1 = linéaire) de la fetch constant.
// Adressage Xenos (3 bits par axe) : 0 répétition, 1 miroir, 2/4 bord, 3/5 miroir puis bord, 6/7 bordure.
// Le jeu utilise le miroir pour ses textures répétées (rails, panneaux) : en "bord" elles s'étiraient.
static uint32_t AddressMode(uint32_t clamp)
{
    static const uint32_t map[8] = { 0, 1, 2, 1, 2, 1, 3, 3 };
    return map[clamp & 7];
}

static uint32_t GetSampler(const uint32_t* f)
{
    bool point = ((f[3] >> 19) & 3) == 0;
    bool mipLinear = ((f[3] >> 23) & 3) == 1;
    return (point ? 1 : 0) | AddressMode(f[0] >> 10) << 1 | AddressMode(f[0] >> 13) << 3 | (mipLinear ? 32 : 0);
}

// ------------------------------------------------------------------ dessin

// Débogage en jeu : F10 -> structure complète de l'image suivante dans le journal, F9 -> test de profondeur coupé
std::atomic<uint64_t> g_dumpFrame{ 0 };
std::atomic<bool> g_debugNoDepth{ false };
static bool DumpFrame() { extern uint64_t g_presentCount; return g_dumpFrame && g_presentCount == g_dumpFrame; }

void RenderDraw(const uint32_t* regs, uint32_t initiator, uint32_t (*phys)(uint32_t), uint32_t dmaBase, uint32_t dmaSize)
{
    static int logged;
    static uint64_t drawFrom = getenv("IKARUGA_DRAWLOG") ? strtoull(getenv("IKARUGA_DRAWLOG"), nullptr, 10) : 0;
    extern uint64_t g_presentCount;
    if ((drawFrom && g_presentCount >= drawFrom && logged++ < 400) || DumpFrame())
        KLOG("RenderDraw init=%08X mode=%u vs=%016llx ps=%016llx copy=%08X clear=%08X", initiator, regs[RB_MODECONTROL] & 7,
            s_vs ? (unsigned long long)s_vs->containerHash : 0ull, s_ps ? (unsigned long long)s_ps->containerHash : 0ull,
            regs[RB_COPY_CONTROL], regs[RB_COLOR_CLEAR]);
    if (!BeginCommands()) return;
    uint32_t edramMode = regs[RB_MODECONTROL] & 7;
    if (edramMode == 6) { Resolve(regs, phys); return; }   // mode copie : c'est un resolve
    if (edramMode != 4 && edramMode != 5) return;    // 4 = couleur + profondeur, 5 = profondeur seule
    if (!s_vs || !s_ps)
    {
        // Débogage : dessins perdus faute de shader traduit (IKARUGA_DRAWTEX=n)
        extern uint64_t g_presentCount;
        static uint64_t texFrom = getenv("IKARUGA_DRAWTEX") ? strtoull(getenv("IKARUGA_DRAWTEX"), nullptr, 10) : 0;
        if ((texFrom && g_presentCount == texFrom) || DumpFrame())
        {
            const uint32_t* f = regs + FETCH_CONSTANTS;
            KLOG("dessin perdu (vs %s, ps %s) prim %u, %u sommets, texture 0 : base %08X format %u %ux%u", s_vs ? "ok" : "absent", s_ps ? "ok" : "absent",
                initiator & 0x3F, initiator >> 16, (f[1] >> 12) << 12, f[1] & 0x3F, (f[2] & 0x1FFF) + 1, ((f[2] >> 13) & 0x1FFF) + 1);
        }
        return;
    }

    uint32_t prim = initiator & 0x3F, source = (initiator >> 6) & 3, count = initiator >> 16;
    if (source == 1) { static int warned; if (!warned++ || DumpFrame()) KLOG("rendu : index immédiats non gérés"); return; }

    // Index en mémoire (source DMA) : 16 ou 32 bits, ordre des octets dans VGT_DMA_SIZE[31:30], décalage VGT_INDX_OFFSET
    bool indexed = source == 0;
    std::vector<uint32_t> guestIndices;
    uint32_t fetchCount = count;   // nombre de sommets à copier
    if (indexed)
    {
        bool is32 = (initiator >> 11) & 1;
        uint32_t endian = dmaSize >> 30, indexOffset = regs[0x2102];
        const uint8_t* src = g_base + phys(dmaBase & ~(is32 ? 3u : 1u));
        uint32_t maxIndex = 0;
        guestIndices.resize(count);
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t v;
            if (is32) { memcpy(&v, src + i * 4, 4); v = SwapEndian(v, endian); }
            else { uint16_t h; memcpy(&h, src + i * 2, 2); v = (endian == 1 || endian == 2) ? __builtin_bswap16(h) : h; if (v == 0xFFFF) v = 0xFFFFFFFF; }
            if (v != 0xFFFFFFFF) { v += indexOffset; maxIndex = std::max(maxIndex, v); }
            guestIndices[i] = v;
        }
        fetchCount = count ? maxIndex + 1 : 0;
        if (prim == 8) { static int warned; if (!warned++ || DumpFrame()) KLOG("rendu : rectangles indexés non gérés"); return; }
    }

    VertexLayout layout{};   // zéro : la clé des pipelines hache aussi les entrées inutilisées
    if (!DecodeVertexLayout(regs, layout)) { if (DumpFrame()) KLOG("dessin perdu : disposition des sommets"); return; }

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
        for (uint32_t q = 0; q + 3 < count; q += 4)
        {
            auto at = [&](uint32_t k) { return indexed ? guestIndices[q + k] : q + k; };
            indices.insert(indices.end(), { at(0), at(1), at(2), at(0), at(2), at(3) });
        }
        break;
    case 8:    // liste de rectangles : le 4e sommet est déduit (v1 + v2 - v0), ce qui exige des attributs float32
        topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        if (!layout.allFloat32) { static int warned; if (!warned++ || DumpFrame()) KLOG("rendu : rectangles avec attributs non float32"); return; }
        break;
    default:
        { static int warned; if (!warned++) KLOG("rendu : primitive %u non gérée", prim); }
        return;
    }
    if (indexed && prim != 13) indices = guestIndices;

    // Sommets : copie de chaque flux en remettant les mots de 32 bits en little-endian
    VkBuffer buffers[16];
    VkDeviceSize offsets[16];
    for (uint32_t b = 0; b < layout.bindingCount; b++)
    {
        uint32_t d0 = regs[FETCH_CONSTANTS + layout.fetchConst[b] * 2], d1 = regs[FETCH_CONSTANTS + layout.fetchConst[b] * 2 + 1];
        uint32_t base = d0 & ~3u, sizeBytes = ((d1 >> 2) & 0xFFFFFF) * 4, stride = layout.bindings[b].stride;
        // Dessin non indexé : les index générés partent de VGT_INDX_OFFSET (sommet de départ de DrawPrimitive) ;
        // sans lui, deux icônes d'un même tampon lisaient toutes deux les sommets de la première
        if (!indexed && stride)
        {
            uint32_t skip = std::min(regs[0x2102] * stride, sizeBytes);
            base += skip; sizeBytes -= skip;
        }
        uint32_t needed = stride ? stride * fetchCount : sizeBytes;
        uint32_t bytes = std::min(needed, sizeBytes ? sizeBytes : needed);
        uint32_t outVertices = prim == 8 ? count / 3 * 4 : fetchCount;
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
        if (DumpFrame())
        {
            std::string attrs;
            for (uint32_t a = 0; a < layout.attributeCount; a++)
                if (layout.attributes[a].binding == layout.bindings[b].binding)
                {
                    char t[48]; snprintf(t, sizeof t, " [loc %u vk %d +%u]", layout.attributes[a].location, int(layout.attributes[a].format), layout.attributes[a].offset);
                    attrs += t;
                }
            KLOG("  sommets flux %u : base %08X pas %u taille %u, 1er sommet %08X %08X %08X %08X %08X %08X %08X %08X%s", b, base, stride, sizeBytes,
                dst[0], dst[1], dst[2], dst[3], dst[4], dst[5], dst[6], dst[7], attrs.c_str());
        }
        buffers[b] = s_upload;
        offsets[b] = at;
    }
    if (prim == 8)
    {
        for (uint32_t r = 0; r * 3 + 2 < count; r++) { uint32_t q = r * 4; indices.insert(indices.end(), { q, q + 1, q + 2, q + 2, q + 1, q + 3 }); }
    }

    // Constantes : VS = registres ALU 0-255, PS = 256-479 (float4, valeurs déjà en ordre hôte)
    static const bool fadeLog = getenv("IKARUGA_FADELOG");
    if (fadeLog && s_ps->containerHash == 0xd721fb273c54220aull)
    {
        static int n;
        float c[4]; memcpy(c, regs + ALU_CONSTANTS + 256 * 4, 16);
        if (n++ % 60 == 0) KLOG("remplissage g_vMCol = %.3f %.3f %.3f %.3f, blend %08X", c[0], c[1], c[2], c[3], regs[RB_BLENDCONTROL0]);
    }
    VkDeviceSize vsConst;
    if (s_wide && IsFieldPitch(regs[RB_SURFACE_INFO] & 0x3FFF) && (regs[RB_SURFACE_INFO] & 0x3FFF) == 600)
    {
        // Passe 3D en mode large : X de clip multiplié par 540/1280 (ligne c0 de g_mWVP) -> champ élargi, sans étirement
        float c[1024];
        memcpy(c, regs + ALU_CONSTANTS, 4096);
        for (int i = 0; i < 4; i++) c[i] *= float(kFieldWidth) / float(kWideWidth);
        vsConst = UploadConstants(0, c, 4096);
    }
    else
        vsConst = UploadConstants(0, regs + ALU_CONSTANTS, 4096);
    VkDeviceSize psConst = UploadConstants(1, regs + ALU_CONSTANTS + 256 * 4, 3584);
    uint8_t shared[1024]{};   // indices de descripteurs (0 = texture de remplacement), booléens, seuil alpha, boucles
    // Textures du pixel shader : l'échantillonneur n lit la fetch constant n ; indices 2D en c0-c3, samplers en c12-c15
    for (uint32_t slot = 0; slot < 16; slot++)
    {
        const uint32_t* f = regs + FETCH_CONSTANTS + slot * 6;
        if ((f[0] & 3) != 2) continue;
        uint32_t tex = GetTexture(f, phys), smp = GetSampler(f);
        extern uint64_t g_presentCount;
        static uint64_t texFrom = getenv("IKARUGA_DRAWTEX") ? strtoull(getenv("IKARUGA_DRAWTEX"), nullptr, 10) : 0;
        if ((texFrom && g_presentCount == texFrom) || DumpFrame())
            KLOG("dessin ps %016llx emplacement %u : base %08X format %u %ux%u swizzle %03X adressage %o -> descripteur %u sampler %u",
                (unsigned long long)s_ps->containerHash, slot, (f[1] >> 12) << 12, f[1] & 0x3F, (f[2] & 0x1FFF) + 1, ((f[2] >> 13) & 0x1FFF) + 1, (f[3] >> 1) & 0xFFF, (f[0] >> 10) & 0x1FF, tex, smp);
        memcpy(shared + slot * 4, &tex, 4);
        memcpy(shared + 192 + slot * 4, &smp, 4);
    }
    uint32_t booleans = (regs[BOOL_CONSTANTS] & 0xFFFF) | ((regs[BOOL_CONSTANTS + 4] & 0xFFFF) << 16);
    memcpy(shared + 256, &booleans, 4);
    memcpy(shared + 272, &regs[RB_ALPHA_REF], 4);
    // Demi-pixel D3D9 (PA_SU_VTX_CNTL.pix_center = 0) : Xenos place les centres de pixels sur les entiers,
    // Vulkan à +0,5 ; sans décalage, un texte affiché 1:1 est échantillonné entre deux texels (rayures)
    if (!(regs[0x2302] & 1))
    {
        float vxs, vys;
        memcpy(&vxs, &regs[PA_CL_VPORT_XSCALE], 4); memcpy(&vys, &regs[PA_CL_VPORT_YSCALE], 4);
        if (!(regs[PA_CL_VTE_CNTL] & 1) || vxs == 0) { vxs = 640; vys = -360; }
        static float sx = 1, sy = -1;
        static bool parsed = [] { if (const char* e = getenv("IKARUGA_HPO")) sscanf(e, "%f,%f", &sx, &sy); return true; }();
        (void)parsed;
        float hpo[2] = { sx * 0.5f / std::fabs(vxs), sy * 0.5f / std::fabs(vys) };
        memcpy(shared + 264, hpo, 8);
    }
    // Constantes de boucle (g_LoopConst) : nombre, début, pas signé, à partir de l'octet 512
    for (uint32_t n = 0; n < 32; n++)
    {
        uint32_t v = regs[BOOL_CONSTANTS + 8 + n];
        int32_t loop[4] = { int32_t(v & 0xFF), int32_t((v >> 8) & 0xFF), int32_t(int8_t((v >> 16) & 0xFF)), 0 };
        memcpy(shared + 512 + n * 16, loop, 16);
    }
    VkDeviceSize sharedConst = UploadConstants(2, shared, sizeof(shared));
    uint64_t push[3] = { s_uploadAddr + vsConst, s_uploadAddr + psConst, s_uploadAddr + sharedConst };

    uint32_t colorControl = regs[RB_COLORCONTROL];
    uint32_t spec = 0;
    if ((colorControl & 8) && (s_ps->specConstantsMask & 2)) spec |= 2;   // test alpha (fonction "supérieur ou égal" supposée)
    BeginTargets(regs);
    VkPipeline pipeline = GetPipeline(regs, layout, topology, spec | ((s_vs->specConstantsMask & 1) && layout.packedNormal ? 1 : 0), edramMode == 5);
    if (!pipeline) { if (DumpFrame()) KLOG("dessin perdu : pipeline"); return; }
    uint32_t tw = s_curColor->width, th = s_curColor->height;

    // Viewport Xenos (échelle/décalage) ; une hauteur négative retourne l'axe Y comme D3D
    float xs, xo, ys, yo;
    memcpy(&xs, &regs[PA_CL_VPORT_XSCALE], 4); memcpy(&xo, &regs[PA_CL_VPORT_XOFFSET], 4);
    memcpy(&ys, &regs[PA_CL_VPORT_YSCALE], 4); memcpy(&yo, &regs[PA_CL_VPORT_YOFFSET], 4);
    // XenosRecomp suit la convention D3D (y vers le haut) : on inverse le signe Xenos pour Vulkan (y vers le bas)
    float zs, zo;
    memcpy(&zs, &regs[PA_CL_VPORT_ZSCALE], 4); memcpy(&zo, &regs[PA_CL_VPORT_ZOFFSET], 4);
    float zmin = std::clamp(zo, 0.0f, 1.0f), zmax = std::clamp(zo + zs, 0.0f, 1.0f);
    if (zs == 0) { zmin = 0; zmax = 1; }
    VkViewport viewport{ xo - xs, yo + ys, 2 * xs, -2 * ys, zmin, zmax };
    if (!(regs[PA_CL_VTE_CNTL] & 1) || xs == 0) viewport = { 0, 720.0f, 1280.0f, -720.0f, 0, 1 };
    bool fieldComposite = false;
    if (s_curColor->wide)
    {
        // zone de jeu large : viewport et scissor sur toute la largeur
        viewport.x = 0; viewport.width = float(kWideWidth);
    }
    else if (s_wideFieldThisFrame)
    {
        // Image finale : le rectangle qui pose la zone de jeu (x 370-910) est étiré sur toute la largeur ;
        // les autres rectangles plein écran de la même passe (panneaux latéraux) sont omis
        // (texture 0 seulement : les emplacements 10-11 gardent les copies de la passe 3D d'un dessin à l'autre)
        bool readsField = false;
        const uint32_t* f0 = regs + FETCH_CONSTANTS;
        if ((initiator & 0x3F) == 8 && (f0[0] & 3) == 2)
        {
            auto it = s_resolved.find(((f0[1] >> 12) << 12) & 0x1FFFFFFF);
            readsField = it != s_resolved.end() && it->second.wide;
        }
        if (readsField)
        {
            float k = float(kWideWidth) / float(kFieldWidth);
            viewport.x = viewport.x * k - float(kFieldLeft) * k;
            viewport.width *= k;
            fieldComposite = true;
        }
        else if ((initiator & 0x3F) == 8 && s_ps->containerHash == 0x149be5ee809351beull)
            return;   // panneau latéral
    }
    viewport.x *= s_scale; viewport.y *= s_scale; viewport.width *= s_scale; viewport.height *= s_scale;
    (void)fieldComposite;
    uint32_t tl = regs[PA_SC_WINDOW_SCISSOR_TL], br = regs[PA_SC_WINDOW_SCISSOR_BR];
    VkRect2D scissor{ { int32_t(tl & 0x7FFF), int32_t((tl >> 16) & 0x7FFF) }, { (br & 0x7FFF) - (tl & 0x7FFF), ((br >> 16) & 0x7FFF) - ((tl >> 16) & 0x7FFF) } };
    if (scissor.extent.width == 0 || scissor.offset.x + scissor.extent.width > tw || scissor.offset.y + scissor.extent.height > th)
        scissor = { { 0, 0 }, { std::min(tw, 1280u), std::min(th, 720u) } };
    if (s_curColor->wide) { scissor.offset.x = 0; scissor.extent.width = kWideWidth; }
    {
        int32_t x0 = S(scissor.offset.x), y0 = S(scissor.offset.y);
        int32_t x1 = S(scissor.offset.x + int32_t(scissor.extent.width)), y1 = S(scissor.offset.y + int32_t(scissor.extent.height));
        scissor = { { x0, y0 }, { uint32_t(x1 - x0), uint32_t(y1 - y0) } };
    }
    vkCmdSetViewport(s_cmd, 0, 1, &viewport);
    vkCmdSetScissor(s_cmd, 0, 1, &scissor);
    {
        // Débogage : IKARUGA_PASSLOG=n -> structure de l'image n (cible, shaders, viewport, textures copiées lues)
        extern uint64_t g_presentCount;
        static uint64_t passFrom = getenv("IKARUGA_PASSLOG") ? strtoull(getenv("IKARUGA_PASSLOG"), nullptr, 10) : 0;
        if ((passFrom && g_presentCount == passFrom) || DumpFrame())
        {
            std::string tex;
            for (uint32_t slot = 0; slot < 16; slot++)
            {
                const uint32_t* f = regs + FETCH_CONSTANTS + slot * 6;
                if ((f[0] & 3) != 2) continue;
                uint32_t b = (f[1] >> 12) << 12 & 0x1FFFFFFF;
                if (s_resolved.count(b)) { char t[32]; snprintf(t, sizeof t, " t%u=%08X", slot, b); tex += t; }
            }
            KLOG("passe depth %02X z %.3f-%.3f cible %ux%u fmt %d vs %016llx ps %016llx vp (%.0f,%.0f %.0fx%.0f) sc (%d,%d %ux%u) prim %u n %u%s",
                regs[RB_DEPTHCONTROL] & 0x77, viewport.minDepth, viewport.maxDepth, s_curColor->width, s_curColor->height, int(s_curColor->format), (unsigned long long)s_vs->containerHash, (unsigned long long)s_ps->containerHash,
                viewport.x, viewport.y, viewport.width, viewport.height, scissor.offset.x, scissor.offset.y, scissor.extent.width, scissor.extent.height,
                initiator & 0x3F, initiator >> 16, tex.c_str());
        }
    }

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
uint64_t g_presentCount;

void RenderPresent(uint32_t frontBuffer)
{
    if (!BeginCommands()) return;
    EndRenderPass();
    auto it = s_resolved.find(frontBuffer & 0x1FFFFFFF);
    if (it == s_resolved.end()) it = s_resolved.find(s_lastResolved);
    if (it != s_resolved.end())
    {
        Transition(it->second.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VulkanPresentFrame(it->second.image.image, it->second.hw, it->second.hh);
        s_presented = &it->second;
    }
    else
        VulkanPresentFrame(VK_NULL_HANDLE, 1280, 720);
    s_frameStarted = false;
    g_presentCount++;
}
