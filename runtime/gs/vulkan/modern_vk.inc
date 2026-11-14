// modern_vk.inc - the EOD_MODERN pass (modern_scene.h) for the Vulkan GS backend,
// included at the end of gs_vulkan.cpp inside gs::hw so it shares the device, the
// surfaces and the texture cache. A port of runtime/gs/d3d11/modern_d3d11.inc:
// the frame's sky, world and objects from the decoded batches at the internal
// resolution with 4x MSAA and 16x anisotropic filtering, a cascaded sun shadow
// atlas, Filament-style shading, AO, bloom and a filmic grade; the result is
// copied into the game's frame buffer before the GS draws the HUD on top.
// The pass records raw Vulkan into the backend's command buffer (between full
// barriers) with its own pipelines and descriptor sets; every image stays in
// VK_IMAGE_LAYOUT_GENERAL, as the backend's own images do.

namespace {

#include "modern_vk_spv.inc"

struct MImage {
    VkImage img{};
    VkImageView view{};
    VkDeviceMemory mem{};
    VkFormat fmt{};
    uint32_t w = 0, h = 0;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
};

bool is_depth_format(VkFormat f) { return f == VK_FORMAT_D32_SFLOAT; }

void mimage_free(MImage &m) {
    if (!m.img) return;
    auto d = g_dev->device;
    auto img = m.img;
    auto view = m.view;
    auto mem = m.mem;
    g_dev->garbage.push_back([=] {
        vkDestroyImageView(d, view, nullptr);
        vkDestroyImage(d, img, nullptr);
        vkFreeMemory(d, mem, nullptr);
    });
    m = MImage();
}

// An image in VK_IMAGE_LAYOUT_GENERAL (sampled, attachment, transfer source and destination).
bool mimage(MImage &m, uint32_t w, uint32_t h, VkFormat fmt, VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT) {
    mimage_free(m);
    const bool depth = is_depth_format(fmt);
    VkImageCreateInfo i{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    i.imageType = VK_IMAGE_TYPE_2D;
    i.format = fmt;
    i.extent = {w, h, 1};
    i.mipLevels = 1;
    i.arrayLayers = 1;
    i.samples = samples;
    i.tiling = VK_IMAGE_TILING_OPTIMAL;
    i.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
              (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (vkCreateImage(g_dev->device, &i, nullptr, &m.img) != VK_SUCCESS) return false;
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(g_dev->device, m.img, &r);
    VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    a.allocationSize = r.size;
    a.memoryTypeIndex = g_dev->memory_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(g_dev->device, &a, nullptr, &m.mem) != VK_SUCCESS) {
        vkDestroyImage(g_dev->device, m.img, nullptr);
        m.img = VK_NULL_HANDLE;
        return false;
    }
    vkBindImageMemory(g_dev->device, m.img, m.mem, 0);
    VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    v.image = m.img;
    v.viewType = VK_IMAGE_VIEW_TYPE_2D;
    v.format = fmt;
    v.subresourceRange = {(VkImageAspectFlags)(depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 1, 0, 1};
    vkCreateImageView(g_dev->device, &v, nullptr, &m.view);
    m.fmt = fmt;
    m.w = w;
    m.h = h;
    m.samples = samples;
    g_ctx->begin();
    g_ctx->end_render(6);
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = m.img;
    b.subresourceRange = v.subresourceRange;
    vkCmdPipelineBarrier(g_ctx->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &b);
    return true;
}

// Device-local arena for the mesh cache (vertex and index data), filled through the upload chunks.
struct MArena {
    VkBuffer buf{};
    VkDeviceMemory mem{};
    VkDeviceSize size = 0, used = 0;
};
std::vector<MArena> g_marena;
constexpr VkDeviceSize kArenaBlock = 32ull << 20;

bool marena_alloc(VkDeviceSize bytes, VkBuffer &buf, VkDeviceSize &off) {
    bytes = (bytes + 255) & ~VkDeviceSize(255);
    for (MArena &a : g_marena)
        if (a.used + bytes <= a.size) {
            buf = a.buf;
            off = a.used;
            a.used += bytes;
            return true;
        }
    MArena a;
    a.size = std::max(kArenaBlock, bytes);
    VkBufferCreateInfo i{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    i.size = a.size;
    i.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(g_dev->device, &i, nullptr, &a.buf) != VK_SUCCESS) return false;
    VkMemoryRequirements r;
    vkGetBufferMemoryRequirements(g_dev->device, a.buf, &r);
    VkMemoryAllocateInfo m{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    m.allocationSize = r.size;
    m.memoryTypeIndex = g_dev->memory_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(g_dev->device, &m, nullptr, &a.mem) != VK_SUCCESS) {
        vkDestroyBuffer(g_dev->device, a.buf, nullptr);
        return false;
    }
    vkBindBufferMemory(g_dev->device, a.buf, a.mem, 0);
    a.used = bytes;
    buf = a.buf;
    off = 0;
    g_marena.push_back(a);
    return true;
}

struct MeshGpu {
    VkBuffer buf{};
    VkDeviceSize vb = 0, ib = 0;  // offsets in buf
    uint32_t count = 0;
    bool normals = false;
};
std::unordered_map<uint64_t, MeshGpu> g_mmesh;
std::vector<vkgs::Texture *> g_mtex;  // this frame's batch textures (one reference each)
std::vector<uint32_t> g_matest;      // per batch: ate | atst << 1 | afail << 4 | aref << 8
std::vector<uint32_t> g_mblend_key;  // per batch: the GS blend and texture state, ~0 unknown
std::vector<uint64_t> g_malpha;      // per batch: vertex alpha sum << 24 | draws

struct ModernFrameCB {
    float clip_x[4], clip_y[4], clip_w[4], params[4], cam_pos[4], up[4], sh_x[4], sh_y[4], sh_z[4], fog[4], sun[4], gsz[4];
    float c0_x[4], c0_y[4], c0_z[4], c1_x[4], c1_y[4], c1_z[4], cs[4], post[4], post2[4], set[4], set2[4];
};
struct ModernBatchCB {
    float m[12], light[4], info[4], info2[4], atest[4], zrow[4];
};
static_assert(sizeof(ModernBatchCB) == 128, "push constants");

// descriptor set 0 bindings (modern_vk.hlsl)
enum : uint32_t { MB_FRAME = 0, MB_SHADOW = 1, MB_GSFRAME = 2, MB_DEPTHMS = 3, MB_SRC = 4, MB_AO = 5, MB_BLOOM = 6,
                  MB_IMAGES_END = 7, MB_SHADOW_SMP = 20, MB_LIN = 21 };

VkDescriptorSetLayout g_mset0, g_mset1;
VkPipelineLayout g_mlayout;
VkSampler g_msamplers[4], g_mshadow_smp, g_mlin_clamp;
MImage g_mdummy, g_mdummy_ms;
vkgs::HostBuffer g_mones;  // vertex alpha 0x80 and "blends" for meshes without per-vertex alpha

enum MShader { VS_MODERN, PS_MODERN, VS_SHADOW, PS_SHADOW, VS_GSDEPTH, VS_FULL, PS_FULL, PS_AO, PS_BRIGHT, PS_BLUR,
               PS_COMP, kMShaders };
VkShaderModule g_mshader[kMShaders];
const char *const k_mentry[kMShaders] = {"vs_modern", "ps_modern", "vs_shadow", "ps_shadow", "vs_gsdepth", "vs_full",
                                         "ps_full",   "ps_ao",     "ps_bright", "ps_blur",   "ps_comp"};

// pipeline state: shaders, blend, depth mode, attachments
struct MPipeKey {
    uint8_t vs, ps, depth, bias;  // depth: 0 off, 1 sky (always, no write), 2 GEQUAL + write, 3 LESS + write
    uint8_t blend_on, src, dst, op;
    uint32_t color, zfmt, samples;
    bool operator==(const MPipeKey &o) const { return !memcmp(this, &o, sizeof *this); }
};
struct MPipeHash {
    size_t operator()(const MPipeKey &k) const {
        uint64_t h = 1469598103934665603ull;
        const uint8_t *p = (const uint8_t *)&k;
        for (size_t i = 0; i < sizeof k; i++) h = (h ^ p[i]) * 1099511628211ull;
        return (size_t)h;
    }
};
std::unordered_map<MPipeKey, VkPipeline, MPipeHash> g_mpipes;

VkPipeline mpipeline(const MPipeKey &k) {
    auto it = g_mpipes.find(k);
    if (it != g_mpipes.end()) return it->second;
    VkPipelineShaderStageCreateInfo st[2]{};
    for (int i = 0; i < 2; i++) {
        st[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
        st[i].module = g_mshader[i ? k.ps : k.vs];
        st[i].pName = k_mentry[i ? k.ps : k.vs];
    }
    const bool mesh = k.vs != VS_FULL;
    VkVertexInputBindingDescription bind[2] = {{0, sizeof(modern::Vertex), VK_VERTEX_INPUT_RATE_VERTEX},
                                               {1, 2, VK_VERTEX_INPUT_RATE_VERTEX}};
    VkVertexInputAttributeDescription attr[5] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
                                                 {1, 0, VK_FORMAT_R32G32_SFLOAT, 12},
                                                 {2, 0, VK_FORMAT_R8G8B8A8_SNORM, 20},
                                                 {3, 0, VK_FORMAT_R8G8B8A8_UNORM, 24},
                                                 {4, 1, VK_FORMAT_R8G8_UNORM, 0}};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (mesh) {
        vi.vertexBindingDescriptionCount = 2;
        vi.pVertexBindingDescriptions = bind;
        vi.vertexAttributeDescriptionCount = 5;
        vi.pVertexAttributeDescriptions = attr;
    }
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1;
    if (k.bias) {
        rs.depthBiasEnable = VK_TRUE;
        rs.depthBiasConstantFactor = 100;
        rs.depthBiasSlopeFactor = 1.5f;
        rs.depthBiasClamp = 0.01f;
    }
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = (VkSampleCountFlagBits)k.samples;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = k.depth != 0;
    ds.depthWriteEnable = k.depth >= 2;
    ds.depthCompareOp = k.depth == 1 ? VK_COMPARE_OP_ALWAYS : k.depth == 2 ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState cb{};
    cb.blendEnable = k.blend_on;
    cb.srcColorBlendFactor = (VkBlendFactor)k.src;
    cb.dstColorBlendFactor = (VkBlendFactor)k.dst;
    cb.colorBlendOp = (VkBlendOp)k.op;
    // the frame buffer's alpha is the GS's 0x80 (1.0): the blend constant's alpha
    cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA;
    cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    cb.alphaBlendOp = VK_BLEND_OP_ADD;
    cb.colorWriteMask = 15;
    VkPipelineColorBlendStateCreateInfo bs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    bs.attachmentCount = k.color ? 1 : 0;
    bs.pAttachments = &cb;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 3;
    dy.pDynamicStates = dyn;
    VkFormat color = (VkFormat)k.color;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = k.color ? 1 : 0;
    ri.pColorAttachmentFormats = &color;
    ri.depthAttachmentFormat = (VkFormat)k.zfmt;
    VkGraphicsPipelineCreateInfo p{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    p.pNext = &ri;
    p.stageCount = 2;
    p.pStages = st;
    p.pVertexInputState = &vi;
    p.pInputAssemblyState = &ia;
    p.pViewportState = &vp;
    p.pRasterizationState = &rs;
    p.pMultisampleState = &ms;
    p.pDepthStencilState = &ds;
    p.pColorBlendState = &bs;
    p.pDynamicState = &dy;
    p.layout = g_mlayout;
    VkPipeline pipe = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g_dev->device, VK_NULL_HANDLE, 1, &p, nullptr, &pipe) != VK_SUCCESS)
        WARN("modern: pipeline %s/%s failed", k_mentry[k.vs], k_mentry[k.ps]);
    g_mpipes[k] = pipe;
    return pipe;
}

// render targets of the pass
MImage g_mcolor, g_mdepth, g_mresolve, g_mfinal, g_mao, g_mb1, g_mb2, g_mshadow;
constexpr int kShadowSize = 4096;  // atlas width: two 2048x2048 cascades side by side
int g_mw, g_mh;
VkSampleCountFlagBits g_msamples = VK_SAMPLE_COUNT_4_BIT;
bool g_mready, g_mfailed;

VkSampler msampler(VkFilter f, VkSamplerAddressMode u, VkSamplerAddressMode v, float aniso, bool compare) {
    VkSamplerCreateInfo i{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    i.magFilter = i.minFilter = f;
    i.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    i.addressModeU = u;
    i.addressModeV = v;
    i.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    i.maxLod = 1000;
    if (aniso > 1 && g_dev->anisotropy) {
        i.anisotropyEnable = VK_TRUE;
        i.maxAnisotropy = aniso;
    }
    if (compare) {
        i.compareEnable = VK_TRUE;
        i.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        i.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        i.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    }
    VkSampler s = VK_NULL_HANDLE;
    vkCreateSampler(g_dev->device, &i, nullptr, &s);
    return s;
}

bool modern_init() {
    if (g_mready || g_mfailed) return g_mready;
    g_mfailed = true;
    try {
        static const struct { const uint32_t *code; size_t bytes; } k_code[kMShaders] = {
            {k_mspv_vs_modern, sizeof k_mspv_vs_modern},   {k_mspv_ps_modern, sizeof k_mspv_ps_modern},
            {k_mspv_vs_shadow, sizeof k_mspv_vs_shadow},   {k_mspv_ps_shadow, sizeof k_mspv_ps_shadow},
            {k_mspv_vs_gsdepth, sizeof k_mspv_vs_gsdepth}, {k_mspv_vs_full, sizeof k_mspv_vs_full},
            {k_mspv_ps_full, sizeof k_mspv_ps_full},       {k_mspv_ps_ao, sizeof k_mspv_ps_ao},
            {k_mspv_ps_bright, sizeof k_mspv_ps_bright},   {k_mspv_ps_blur, sizeof k_mspv_ps_blur},
            {k_mspv_ps_comp, sizeof k_mspv_ps_comp}};
        for (int i = 0; i < kMShaders; i++) {
            VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            info.codeSize = k_code[i].bytes;
            info.pCode = k_code[i].code;
            vkgs::require(vkCreateShaderModule(g_dev->device, &info, nullptr, &g_mshader[i]), k_mentry[i]);
        }
        const VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        std::vector<VkDescriptorSetLayoutBinding> b0;
        b0.push_back({MB_FRAME, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, nullptr});
        for (uint32_t i = 1; i < MB_IMAGES_END; i++)
            b0.push_back({i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
        b0.push_back({MB_SHADOW_SMP, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
        b0.push_back({MB_LIN, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dl.bindingCount = (uint32_t)b0.size();
        dl.pBindings = b0.data();
        vkgs::require(vkCreateDescriptorSetLayout(g_dev->device, &dl, nullptr, &g_mset0), "modern set 0");
        VkDescriptorSetLayoutBinding b1[2] = {{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                              {1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
        dl.bindingCount = 2;
        dl.pBindings = b1;
        vkgs::require(vkCreateDescriptorSetLayout(g_dev->device, &dl, nullptr, &g_mset1), "modern set 1");
        VkDescriptorSetLayout sets[2] = {g_mset0, g_mset1};
        VkPushConstantRange pc{both, 0, sizeof(ModernBatchCB)};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 2;
        pl.pSetLayouts = sets;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &pc;
        vkgs::require(vkCreatePipelineLayout(g_dev->device, &pl, nullptr, &g_mlayout), "modern layout");
        for (int k = 0; k < 4; k++)
            g_msamplers[k] = msampler(VK_FILTER_LINEAR,
                                      (k & 1) ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                      (k & 2) ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT, 16, false);
        g_mshadow_smp = msampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
                                 VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, 1, true);
        g_mlin_clamp = msampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 1, false);
        VkImageFormatProperties ifp{};
        if (vkGetPhysicalDeviceImageFormatProperties(g_dev->physical, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
                                                     VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0,
                                                     &ifp) != VK_SUCCESS ||
            !(ifp.sampleCounts & VK_SAMPLE_COUNT_4_BIT))
            g_msamples = VK_SAMPLE_COUNT_1_BIT;
        if (!mimage(g_mdummy, 1, 1, VK_FORMAT_R8G8B8A8_UNORM) ||
            !mimage(g_mdummy_ms, 1, 1, VK_FORMAT_D32_SFLOAT, g_msamples) ||
            !mimage(g_mshadow, kShadowSize, kShadowSize / 2, VK_FORMAT_D32_SFLOAT))
            throw std::runtime_error("modern images");
        g_mones = g_dev->host_buffer(1 << 19);
        uint8_t *ones = (uint8_t *)g_mones.map;
        for (size_t k = 0; k < (1 << 19); k += 2) ones[k] = 0x80, ones[k + 1] = 0xFF;
    } catch (const std::exception &e) {
        WARN("modern renderer: %s", e.what());
        return false;
    }
    g_mready = true;
    g_mfailed = false;
    fprintf(stderr, "[modern] renderer ready (vulkan): %ux MSAA, %s anisotropic, %d sun shadow atlas, scale %d\n",
            (unsigned)g_msamples, g_dev->anisotropy ? "16x" : "no", kShadowSize, g_scale);
    return true;
}

bool modern_targets(int w, int h) {
    if (g_mcolor.img && g_mw == w && g_mh == h) return true;
    const uint32_t W = (uint32_t)w, H = (uint32_t)h, hw = std::max(1u, W / 2), hh = std::max(1u, H / 2);
    bool ok = mimage(g_mcolor, W, H, VK_FORMAT_R8G8B8A8_UNORM, g_msamples) &&
              mimage(g_mdepth, W, H, VK_FORMAT_D32_SFLOAT, g_msamples) &&
              mimage(g_mresolve, W, H, VK_FORMAT_R8G8B8A8_UNORM) && mimage(g_mfinal, W, H, VK_FORMAT_R8G8B8A8_UNORM) &&
              mimage(g_mao, hw, hh, VK_FORMAT_R8_UNORM) && mimage(g_mb1, hw, hh, VK_FORMAT_R8G8B8A8_UNORM) &&
              mimage(g_mb2, hw, hh, VK_FORMAT_R8G8B8A8_UNORM);
    g_mw = ok ? w : 0;
    g_mh = ok ? h : 0;
    return ok;
}

void modern_upload(const modern::Mesh &m) {
    if (m.idx.empty() || m.v.empty() || g_mmesh.count(m.id)) return;
    const VkDeviceSize vbytes = m.v.size() * sizeof(modern::Vertex), ibytes = m.idx.size() * 4;
    MeshGpu g;
    VkDeviceSize off;
    if (!marena_alloc(vbytes + ibytes + 256, g.buf, off)) return;
    g.vb = off;
    g.ib = (off + vbytes + 255) & ~VkDeviceSize(255);
    auto sv = g_ctx->upload(m.v.data(), (size_t)vbytes);
    VkBufferCopy c{sv.second, g.vb, vbytes};
    vkCmdCopyBuffer(g_ctx->cmd, sv.first->buffer, g.buf, 1, &c);
    auto si = g_ctx->upload(m.idx.data(), (size_t)ibytes);
    c = {si.second, g.ib, ibytes};
    vkCmdCopyBuffer(g_ctx->cmd, si.first->buffer, g.buf, 1, &c);
    g.count = (uint32_t)m.idx.size();
    g.normals = m.v[0].n[3] != 0;
    g_mmesh.emplace(m.id, g);
}

void modern_release_textures() {
    for (vkgs::Texture *&t : g_mtex) release(t);
    g_mtex.clear();
    g_mblend_key.clear();
    g_malpha.clear();
    g_matest.clear();
}

float modern_gloss(const modern::Batch &b) {
    switch (b.pc) {
    case 0x2e0: return 0.45f;
    case 0xd48: return 0.2f;
    case 0x15a8: return 0.1f;
    }
    return b.role == modern::OBJECT ? 0.08f : 0.0f;
}

void norm3(float *v) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0)
        for (int i = 0; i < 3; i++) v[i] /= l;
}
float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
void cross3(const float *a, const float *b, float *o) {
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}

// The GS blend (A - B) * C + D (A, B, D: 0 Cs, 1 Cd, 2 zero; C: 0 As, 1 Ad, 2 FIX) as a Vulkan blend.
// raw: the layer is drawn unlit (add / subtract layers). Ad is taken as 1.0.
void modern_blend(uint32_t key, bool &raw, MPipeKey &k) {
    raw = false;
    k.blend_on = 1;
    k.op = VK_BLEND_OP_ADD;
    if (key != ~0u && !(key & 1)) {  // the game draws it without blending
        k.src = VK_BLEND_FACTOR_ONE;
        k.dst = VK_BLEND_FACTOR_ZERO;
        return;
    }
    if (key == ~0u) key = 1 | 0 << 1 | 1 << 3 | 0 << 5 | 1 << 7;  // unknown: (Cs - Cd) * As + Cd
    const int A = (key >> 1) & 3, B = (key >> 3) & 3, C = (key >> 5) & 3, D = (key >> 7) & 3;
    const VkBlendFactor c = C == 0 ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_CONSTANT_COLOR;
    const VkBlendFactor ic = C == 0 ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    VkBlendFactor sf = c, df = ic;
    if (A == 0 && B == 1 && D == 1) {  // mix
    } else if (A == 0 && B == 2 && D == 1) {  // add
        df = VK_BLEND_FACTOR_ONE;
        raw = true;
    } else if (A == 2 && B == 0 && D == 1) {  // subtract
        df = VK_BLEND_FACTOR_ONE;
        k.op = VK_BLEND_OP_REVERSE_SUBTRACT;
        raw = true;
    } else if (A == 1 && B == 0 && D == 0) {  // reverse mix
        sf = ic;
        df = c;
    } else if (A == 0 && B == 2 && D == 2) {  // scaled source
        df = VK_BLEND_FACTOR_ZERO;
    }
    k.src = (uint8_t)sf;
    k.dst = (uint8_t)df;
}

// ------------------------------------------------------------ recording helpers
VkCommandBuffer g_mcmd;
bool g_mrendering;
VkPipeline g_mbound;
uint64_t g_mset1_serial;
std::unordered_map<uint64_t, VkDescriptorSet> g_mset1_cache;  // (view, sampler) -> set, this command buffer

void mend() {
    if (g_mrendering) vkCmdEndRendering(g_mcmd);
    g_mrendering = false;
}
// a full barrier between the pass's steps (all earlier writes visible)
void mbarrier() {
    mend();
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(g_mcmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0,
                         nullptr, 0, nullptr);
}
// Starts rendering into color (optional) and depth (optional); clear < 0: load.
void mbegin(VkImageView color, VkImageView depth, uint32_t w, uint32_t h, float clear_depth = -1) {
    mbarrier();
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = color;
    ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo za = ca;
    za.imageView = depth;
    if (clear_depth >= 0) {
        za.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        za.clearValue.depthStencil = {clear_depth, 0};
    }
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {w, h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = color ? 1 : 0;
    ri.pColorAttachments = &ca;
    ri.pDepthAttachment = depth ? &za : nullptr;
    vkCmdBeginRendering(g_mcmd, &ri);
    g_mrendering = true;
    g_mbound = VK_NULL_HANDLE;
}
// viewport (x, y, w, h) in the attachment, Direct3D orientation (negative height)
void mviewport(float x, float y, float w, float h) {
    VkViewport v{x, y + h, w, -h, 0, 1};
    vkCmdSetViewport(g_mcmd, 0, 1, &v);
    VkRect2D r{{(int32_t)x, (int32_t)y}, {(uint32_t)w, (uint32_t)h}};
    vkCmdSetScissor(g_mcmd, 0, 1, &r);
}
void mbind(const MPipeKey &k) {
    VkPipeline p = mpipeline(k);
    if (p != g_mbound) vkCmdBindPipeline(g_mcmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    g_mbound = p;
}
void mblend_constants(const float *f) { vkCmdSetBlendConstants(g_mcmd, f); }

// set 0: the frame constants and up to six frame images (null: a dummy)
void mset0(VkBuffer ubo, VkDeviceSize ubo_off, std::initializer_list<std::pair<uint32_t, VkImageView>> images) {
    VkDescriptorSet ds = g_ctx->alloc_set(g_mset0);
    VkDescriptorBufferInfo bi{ubo, ubo_off, sizeof(ModernFrameCB)};
    VkDescriptorImageInfo ii[MB_IMAGES_END]{};
    for (uint32_t i = 1; i < MB_IMAGES_END; i++) {
        ii[i].imageView = i == MB_DEPTHMS ? g_mdummy_ms.view : g_mdummy.view;
        ii[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    ii[MB_SHADOW].imageView = g_mshadow.view;
    for (auto &p : images)
        if (p.second) ii[p.first].imageView = p.second;
    VkDescriptorImageInfo si[2]{};
    si[0].sampler = g_mshadow_smp;
    si[1].sampler = g_mlin_clamp;
    VkWriteDescriptorSet w[MB_IMAGES_END + 2]{};
    uint32_t n = 0;
    w[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, MB_FRAME, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &bi, nullptr};
    n++;
    for (uint32_t i = 1; i < MB_IMAGES_END; i++, n++)
        w[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, i, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &ii[i], nullptr, nullptr};
    w[n++] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, MB_SHADOW_SMP, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, &si[0], nullptr, nullptr};
    w[n++] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, MB_LIN, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, &si[1], nullptr, nullptr};
    vkUpdateDescriptorSets(g_dev->device, n, w, 0, nullptr);
    vkCmdBindDescriptorSets(g_mcmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_mlayout, 0, 1, &ds, 0, nullptr);
}
// set 1: a batch's texture and sampler
void mset1(VkImageView view, VkSampler smp) {
    if (g_mset1_serial != g_ctx->serial) {
        g_mset1_cache.clear();
        g_mset1_serial = g_ctx->serial;
    }
    const uint64_t key = (uint64_t)(uintptr_t)view * 31 + (uint64_t)(uintptr_t)smp;
    VkDescriptorSet &ds = g_mset1_cache[key];
    if (!ds) {
        ds = g_ctx->alloc_set(g_mset1);
        VkDescriptorImageInfo ii{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL}, si{smp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        VkWriteDescriptorSet w[2] = {
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &ii, nullptr, nullptr},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, 1, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, &si, nullptr, nullptr}};
        vkUpdateDescriptorSets(g_dev->device, 2, w, 0, nullptr);
    }
    vkCmdBindDescriptorSets(g_mcmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_mlayout, 1, 1, &ds, 0, nullptr);
}
// the frame constants into this command buffer's upload memory
std::pair<VkBuffer, VkDeviceSize> mframe(const ModernFrameCB &fc) {
    auto u = g_ctx->upload(&fc, sizeof fc);
    return {u.first->buffer, u.second};
}
void mfullscreen(MShader ps, VkFormat color, VkImageView view, uint32_t w, uint32_t h,
                 std::pair<VkBuffer, VkDeviceSize> ubo, std::initializer_list<std::pair<uint32_t, VkImageView>> images) {
    mbegin(view, VK_NULL_HANDLE, w, h);
    MPipeKey k{};
    k.vs = VS_FULL;
    k.ps = (uint8_t)ps;
    k.color = (uint32_t)color;
    k.samples = 1;
    mbind(k);
    mviewport(0, 0, (float)w, (float)h);
    mset0(ubo.first, ubo.second, images);
    mset1(g_mdummy.view, g_mlin_clamp);
    vkCmdDraw(g_mcmd, 3, 1, 0, 0);
}

// The batches of the given roles. pass: 0 the scene (lit), 1 shadow casters, 2 GS depth.
void modern_batches(const modern::Frame &f, ModernBatchCB &bc, int pass, std::initializer_list<uint32_t> roles,
                    uint32_t &drawn, MPipeKey base) {
    for (uint32_t role : roles) {
        for (size_t i = 0; i < f.batches.size(); i++) {
            const modern::Batch &b = f.batches[i];
            if (b.role != role || !b.drawn) continue;
            static const uint32_t skip_pc = getenv("EOD_MODERN_SKIP") ? (uint32_t)strtoul(getenv("EOD_MODERN_SKIP"), nullptr, 16) : 0;
            if (b.pc == skip_pc) continue;
            // car passes without the per-vertex alpha (clipped): the damage overlay is left out
            const bool car = (b.pc == 0x2e0 || b.pc == 0xd48 || b.pc == 0x15a8) && b.valpha.empty();
            if (car && b.pc == 0x15a8) continue;
            const bool per_vertex = !b.valpha.empty();
            auto it = g_mmesh.find(b.mesh);
            if (it == g_mmesh.end()) continue;
            vkgs::Texture *t = i < g_mtex.size() ? g_mtex[i] : nullptr;
            memcpy(bc.m, b.model, 48);
            memcpy(bc.light, b.light, 16);
            memcpy(bc.zrow, b.zrow, 16);
            bc.info[0] = (float)b.role;
            bc.info[1] = (float)b.layer;
            bc.info[2] = t ? 1.0f : 0.0f;
            bc.info[3] = modern_gloss(b);
            const uint32_t key = i < g_mblend_key.size() ? g_mblend_key[i] : ~0u;
            bool raw = false;
            MPipeKey k = base;
            if (pass == 0) {
                k.depth = role == modern::SKY ? 1 : 2;
                if (role != modern::SKY) {
                    const uint32_t bkey = per_vertex && key != ~0u ? (key | 1u) : key;
                    modern_blend(bkey, raw, k);
                    const float fix = key == ~0u ? 1.0f : (float)((key >> 9) & 0xFF) / 128.0f;
                    const float bf[4] = {fix, fix, fix, 128.0f / 255.0f};
                    mblend_constants(bf);
                } else {
                    k.blend_on = 1, k.src = VK_BLEND_FACTOR_SRC_ALPHA, k.dst = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, k.op = VK_BLEND_OP_ADD;
                    const float bf[4] = {0, 0, 0, 128.0f / 255.0f};
                    mblend_constants(bf);
                }
            }
            bc.info2[0] = raw ? 1.0f : 0.0f;
            const uint64_t va = i < g_malpha.size() ? g_malpha[i] : 0;
            const bool by_as = key != ~0u && (key & 1) && ((key >> 5) & 3) == 0;
            const int tfx = (int)((key >> 17) & 3);
            const float av = (va & 0xFFFFFF) ? std::min(2.0f, (float)(va >> 24) / (float)(va & 0xFFFFFF) / 128.0f) : 1.0f;
            bc.info2[1] = by_as && tfx != 1 ? av : 1.0f;
            bc.info2[2] = key != ~0u && !((key >> 19) & 1) ? 1.0f : 0.0f;
            if (key != ~0u && tfx != 1) bc.info2[1] = b.valpha.empty() ? av : 1.0f;
            bc.info2[3] = key == ~0u ? 0.0f : (float)tfx;
            const uint32_t wm = key == ~0u ? 0 : (key >> 21) & 15;
            const int su = (wm & 3) == 1 || (wm & 3) == 2 ? 1 : 0, sv = ((wm >> 2) & 3) == 1 || ((wm >> 2) & 3) == 2 ? 1 : 0;
            const uint32_t at = i < g_matest.size() ? g_matest[i] : 0;
            bc.atest[0] = (at & 1) ? (float)((at >> 1) & 7) : -1.0f;
            bc.atest[1] = (float)((at >> 8) & 0xFF);
            bc.atest[2] = ((at >> 4) & 3) == 0 ? 1.0f : 0.0f;
            static const bool no_atest = getenv("EOD_MODERN_NOATEST") != nullptr;
            if (no_atest || car) bc.atest[0] = -1.0f;
            if (car) bc.info2[1] = 1.0f, bc.info2[2] = 0.0f;
            if (raw) bc.info[3] = 0;
            mbind(k);
            vkCmdPushConstants(g_mcmd, g_mlayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof bc, &bc);
            mset1(t ? t->view : g_mdummy.view, g_msamplers[su | sv << 1]);
            VkBuffer vbs[2] = {it->second.buf, g_mones.buffer};
            VkDeviceSize offs[2] = {it->second.vb, 0};
            const uint32_t nv = (uint32_t)b.valpha.size();
            if (nv) {
                auto u = g_ctx->upload(b.valpha.data(), nv);
                vbs[1] = u.first->buffer;
                offs[1] = u.second;
            }
            vkCmdBindVertexBuffers(g_mcmd, 0, 2, vbs, offs);
            vkCmdBindIndexBuffer(g_mcmd, it->second.buf, it->second.ib, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(g_mcmd, it->second.count, 1, 0, 0, 0);
            drawn++;
        }
    }
}

}  // namespace

void modern_alpha(int32_t batch, uint32_t alpha) {
    if (batch < 0) return;
    if ((size_t)batch >= g_malpha.size()) g_malpha.resize((size_t)batch + 1, 0);
    g_malpha[(size_t)batch] += (uint64_t)alpha << 24 | 1;
}

void modern_texture(int32_t batch, const Env &e) {
    if (!g_on || batch < 0) return;
    if ((size_t)batch >= g_mblend_key.size()) g_mblend_key.resize((size_t)batch + 1, ~0u);
    if ((size_t)batch >= g_matest.size()) g_matest.resize((size_t)batch + 1, 0);
    g_matest[(size_t)batch] = (e.ate ? 1u : 0u) | (uint32_t)(e.atst & 7) << 1 | (uint32_t)(e.afail & 3) << 4 | (e.aref & 0xFF) << 8;
    g_mblend_key[(size_t)batch] = (e.abe ? 1u : 0u) | (uint32_t)(e.ba & 3) << 1 | (uint32_t)(e.bb & 3) << 3 |
                                  (uint32_t)(e.bc & 3) << 5 | (uint32_t)(e.bd & 3) << 7 | (e.bfix & 0xFF) << 9 |
                                  (uint32_t)(e.tfx & 3) << 17 | (e.tcc ? 1u : 0u) << 19 | (e.tme ? 1u : 0u) << 20 |
                                  (uint32_t)(e.wms & 3) << 21 | (uint32_t)(e.wmt & 3) << 23;
    if (!e.tme) return;
    // render-to-texture sources (a surface owns the texture's memory) are left untextured here
    const uint32_t p0 = (e.tbp >> 5) % kPages;
    if (g_owner[p0] && !g_vram_ok[p0]) return;
    alpha_order::Range alpha;
    float scale = 1;
    uint32_t flags = 0;
    vkgs::Texture *t = vram_texture(e, alpha, scale, flags);
    if (!t) return;
    if ((size_t)batch >= g_mtex.size()) g_mtex.resize((size_t)batch + 1, nullptr);
    if (g_mtex[(size_t)batch]) return;
    t->AddRef();
    g_mtex[(size_t)batch] = t;
}

void modern_draw(modern::Frame *frame) {
    std::unique_ptr<modern::Frame> f(frame);
    if (!modern_init()) {  // the GS draws everything again
        modern_release_textures();
        modern::disable();
        return;
    }
    flush();
    g_ctx->barrier(2);  // the GS's commands so far, then the pass's own
    g_mcmd = g_ctx->cmd;
    for (const modern::Mesh &m : f->meshes) modern_upload(m);
    if (f->batches.empty() || g_skip_frame || !modern::g_target.valid) {
        modern_release_textures();
        return;
    }
    const modern::Target &t = modern::g_target;
    const int cols = (int)((t.scissor >> 16) & 0x7FF) + 1, rows = (int)((t.scissor >> 48) & 0x7FF) + 1;
    Surface *s = get_surface(t.fbp, t.fbw, t.fpsm, false, rows);
    if (!s || !s->tex || cols <= 0 || rows <= 0) {
        modern_release_textures();
        return;
    }
    const int W = s->w * g_scale, H = s->h * g_scale;
    if (!modern_targets(W, H)) {
        modern_release_textures();
        return;
    }
    acquire(s, 0, 0, std::min(cols, s->w), std::min(rows, s->h));
    s->version++;
    g_ctx->barrier(2);
    g_mcmd = g_ctx->cmd;

    // world -> GS screen (rows x, y, w of the camera) -> clip for this surface
    const float *cm = f->cam;  // column-major: element (r, c) = cm[c * 4 + r]
    const float ox = (float)(t.xyoffset & 0xFFFF) / 16.0f, oy = (float)((t.xyoffset >> 32) & 0xFFFF) / 16.0f;
    const float sw = (float)s->w, sh = (float)s->h;
    ModernFrameCB fc = {};
    for (int c = 0; c < 4; c++) {
        const float x = cm[c * 4 + 0], y = cm[c * 4 + 1], w = cm[c * 4 + 3];
        fc.clip_x[c] = 2.0f / sw * x - (1.0f + 2.0f * ox / sw) * w;
        fc.clip_y[c] = -2.0f / sh * y + (1.0f + 2.0f * oy / sh) * w;
        fc.clip_w[c] = w;
    }
    // camera position: where the x, y and w rows all vanish
    double A[3][3], b3[3];
    const int rr[3] = {0, 1, 3};
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) A[r][c] = cm[c * 4 + rr[r]];
        b3[r] = -cm[12 + rr[r]];
    }
    auto det3 = [](double m[3][3]) {
        return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
               m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    };
    const double D = det3(A);
    float cam[3] = {0, 0, 0};
    if (std::fabs(D) > 1e-12)
        for (int k = 0; k < 3; k++) {
            double M2[3][3];
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 3; c++) M2[r][c] = c == k ? b3[r] : A[r][c];
            cam[k] = (float)(det3(M2) / D);
        }
    float fwd[3] = {cm[0 * 4 + 3], cm[1 * 4 + 3], cm[2 * 4 + 3]};
    norm3(fwd);
    const float cy = oy + sh * 0.5f;
    float up[3] = {-(cm[0 * 4 + 1] - cy * cm[0 * 4 + 3]), -(cm[1 * 4 + 1] - cy * cm[1 * 4 + 3]),
                   -(cm[2 * 4 + 1] - cy * cm[2 * 4 + 3])};
    {
        const float d = dot3(up, fwd);
        for (int i = 0; i < 3; i++) up[i] -= d * fwd[i];
        norm3(up);
    }
    std::vector<float> dists;
    float L[3] = {0, 1, 0};
    bool have_l = false;
    for (const modern::Batch &b : f->batches) {
        if (!b.drawn) continue;
        if (!have_l && b.role != modern::SKY) {
            memcpy(L, b.light, 12);
            have_l = true;
        }
        if (b.role != modern::OBJECT) continue;
        const float d[3] = {b.model[3] - cam[0], b.model[7] - cam[1], b.model[11] - cam[2]};
        dists.push_back(std::sqrt(dot3(d, d)));
    }
    norm3(L);
    float med = 40.0f;
    if (!dists.empty()) {
        std::nth_element(dists.begin(), dists.begin() + (long)dists.size() / 2, dists.end());
        med = std::max(dists[dists.size() / 2], 1.0f);
    }
    static const float k_shadow = getenv("EOD_MODERN_SHADOW") ? (float)atof(getenv("EOD_MODERN_SHADOW")) : 1.2f;
    const float k_haze = 40.0f;
    const Config &gc = g_config;
    auto pct = [](int v) { return std::clamp(v, 0, 200) / 100.0f; };
    static float s_scale = 0;
    s_scale = s_scale == 0 ? med : s_scale + (med - s_scale) * 0.002f;
    float u[3], v[3];
    float ref[3] = {0, 0, 1};
    if (std::fabs(dot3(ref, L)) > 0.9f) ref[0] = 1, ref[2] = 0;
    cross3(L, ref, u);
    norm3(u);
    cross3(L, u, v);
    float casc[2][3][4];
    for (int c = 0; c < 2; c++) {
        const float half = s_scale * k_shadow * (c == 0 ? 0.35f : 2.0f);
        float ctr[3] = {cam[0] + fwd[0] * half * 0.6f, cam[1] + fwd[1] * half * 0.6f, cam[2] + fwd[2] * half * 0.6f};
        const float texel = 2 * half / (kShadowSize / 2);
        float cu = std::floor(dot3(u, ctr) / texel) * texel, cv = std::floor(dot3(v, ctr) / texel) * texel;
        const float cl = dot3(L, ctr);
        for (int i = 0; i < 3; i++) {
            casc[c][0][i] = u[i] / half;
            casc[c][1][i] = v[i] / half;
            casc[c][2][i] = -L[i] / (6 * half);
        }
        casc[c][0][3] = -cu / half;
        casc[c][1][3] = -cv / half;
        casc[c][2][3] = 0.5f + cl / (6 * half);
        fc.cs[c] = texel;
        fc.cs[2 + c] = 6 * half;
    }
    memcpy(fc.c0_x, casc[0][0], 16);
    memcpy(fc.c0_y, casc[0][1], 16);
    memcpy(fc.c0_z, casc[0][2], 16);
    memcpy(fc.c1_x, casc[1][0], 16);
    memcpy(fc.c1_y, casc[1][1], 16);
    memcpy(fc.c1_z, casc[1][2], 16);
    fc.params[0] = 0.05f;
    fc.params[1] = 1.0f / (kShadowSize / 2);
    const float k_ao = 0.05f;
    fc.set[0] = pct(gc.modern_shadows);
    fc.set[1] = pct(gc.modern_reflections);
    fc.set[2] = 0.75f * pct(gc.modern_ao);
    fc.set[3] = 0.6f * pct(gc.modern_vignette);
    fc.set2[0] = pct(gc.modern_bloom);
    fc.set2[1] = pct(gc.modern_grade);
    fc.set2[2] = gc.modern_soft_shadows ? 0.03f : 0.0f;
    fc.set2[3] = pct(gc.modern_haze);
    fc.post[0] = std::sqrt(fc.clip_x[0] * fc.clip_x[0] + fc.clip_x[1] * fc.clip_x[1] + fc.clip_x[2] * fc.clip_x[2]) * W * 0.5f;
    fc.post[1] = s_scale * k_ao;
    fc.post[2] = 0.72f;
    fc.post[3] = 0.35f * pct(gc.modern_bloom);
    fc.post2[0] = 1.0f / W;
    fc.post2[1] = 1.0f / H;
    fc.params[2] = med * k_haze;
    fc.params[3] = pct(gc.modern_exposure);
    memcpy(fc.cam_pos, cam, 12);
    memcpy(fc.up, up, 12);
    const uint64_t fog = st.fogcol;
    fc.fog[0] = (fog & 0xFF) / 255.0f;
    fc.fog[1] = ((fog >> 8) & 0xFF) / 255.0f;
    fc.fog[2] = ((fog >> 16) & 0xFF) / 255.0f;
    memcpy(fc.sun, L, 12);
    static const bool dbg = getenv("EOD_MODERN_DEBUG") != nullptr;
    fc.fog[3] = dbg ? 1.0f : 0.0f;
    fc.sun[3] = f->batches[0].light[3];
    static bool logged;
    if (!logged) {
        logged = true;
        fprintf(stderr, "[modern] camera (%.1f %.1f %.1f) up (%.2f %.2f %.2f) sun (%.2f %.2f %.2f) object median %.1f\n",
                cam[0], cam[1], cam[2], up[0], up[1], up[2], L[0], L[1], L[2], med);
    }
    ModernBatchCB bc = {};
    uint32_t drawn = 0;

    // sun shadow cascades: the atlas' left half (near) and right half (far)
    if (fc.set[0] > 0) {
        mbegin(VK_NULL_HANDLE, g_mshadow.view, g_mshadow.w, g_mshadow.h, 1.0f);
        MPipeKey k{};
        k.vs = VS_SHADOW;
        k.ps = PS_SHADOW;
        k.depth = 3;
        k.bias = 1;
        k.zfmt = VK_FORMAT_D32_SFLOAT;
        k.samples = 1;
        for (int c = 0; c < 2; c++) {
            memcpy(fc.sh_x, casc[c][0], 16);
            memcpy(fc.sh_y, casc[c][1], 16);
            memcpy(fc.sh_z, casc[c][2], 16);
            mbind(k);
            mviewport((float)(c * kShadowSize / 2), 0, (float)(kShadowSize / 2), (float)(kShadowSize / 2));
            auto ubo = mframe(fc);
            mset0(ubo.first, ubo.second, {});
            modern_batches(*f, bc, 1, {(uint32_t)modern::WORLD, (uint32_t)modern::OBJECT}, drawn, k);
        }
    }
    auto ubo = mframe(fc);

    // the scene, over the GS frame so far (the game's sky)
    mbegin(g_mcolor.view, g_mdepth.view, (uint32_t)W, (uint32_t)H, 0.0f);
    {
        MPipeKey k{};
        k.vs = VS_FULL;
        k.ps = PS_FULL;
        k.depth = 1;
        k.color = VK_FORMAT_R8G8B8A8_UNORM;
        k.zfmt = VK_FORMAT_D32_SFLOAT;
        k.samples = (uint32_t)g_msamples;
        mbind(k);
        mviewport(0, 0, (float)W, (float)H);
        mset0(ubo.first, ubo.second, {{MB_GSFRAME, s->tex->view}});
        mset1(g_mdummy.view, g_mlin_clamp);
        vkCmdDraw(g_mcmd, 3, 1, 0, 0);
        k.vs = VS_MODERN;
        k.ps = PS_MODERN;
        mset0(ubo.first, ubo.second, {});
        // sky first (no depth), then the world in draw order (layers), then objects
        modern_batches(*f, bc, 0, {(uint32_t)modern::SKY, (uint32_t)modern::WORLD, (uint32_t)modern::OBJECT}, drawn, k);
    }
    mbarrier();
    {
        VkImageResolve r{};
        r.srcSubresource = r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.extent = {(uint32_t)W, (uint32_t)H, 1};
        if (g_msamples != VK_SAMPLE_COUNT_1_BIT)
            vkCmdResolveImage(g_mcmd, g_mcolor.img, VK_IMAGE_LAYOUT_GENERAL, g_mresolve.img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        else {
            VkImageCopy c{r.srcSubresource, {}, r.dstSubresource, {}, r.extent};
            vkCmdCopyImage(g_mcmd, g_mcolor.img, VK_IMAGE_LAYOUT_GENERAL, g_mresolve.img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        }
    }
    static const bool no_post = getenv("EOD_MODERN_NOPOST") != nullptr;
    MImage *out = &g_mresolve;
    if (!no_post && g_msamples != VK_SAMPLE_COUNT_1_BIT) {
        const uint32_t hw = g_mao.w, hh = g_mao.h;
        mfullscreen(PS_AO, VK_FORMAT_R8_UNORM, g_mao.view, hw, hh, ubo, {{MB_DEPTHMS, g_mdepth.view}});
        mfullscreen(PS_BRIGHT, VK_FORMAT_R8G8B8A8_UNORM, g_mb1.view, hw, hh, ubo, {{MB_SRC, g_mresolve.view}});
        fc.post2[2] = 2.0f / W;
        fc.post2[3] = 0;
        mfullscreen(PS_BLUR, VK_FORMAT_R8G8B8A8_UNORM, g_mb2.view, hw, hh, mframe(fc), {{MB_SRC, g_mb1.view}});
        fc.post2[2] = 0;
        fc.post2[3] = 2.0f / H;
        mfullscreen(PS_BLUR, VK_FORMAT_R8G8B8A8_UNORM, g_mb1.view, hw, hh, mframe(fc), {{MB_SRC, g_mb2.view}});
        fc.post2[3] = 0;
        mfullscreen(PS_COMP, VK_FORMAT_R8G8B8A8_UNORM, g_mfinal.view, (uint32_t)W, (uint32_t)H, ubo,
                    {{MB_SRC, g_mresolve.view}, {MB_AO, g_mao.view}, {MB_BLOOM, g_mb1.view}});
        out = &g_mfinal;
    }
    mbarrier();
    {
        VkImageCopy c{};
        c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.extent = {(uint32_t)(std::min(cols, s->w) * g_scale), (uint32_t)(std::min(rows, s->h) * g_scale), 1};
        vkCmdCopyImage(g_mcmd, out->img, VK_IMAGE_LAYOUT_GENERAL, s->tex->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    }

    // the GS depth the replaced draws would have written, so what the GS draws next (particles,
    // the flag, the camera-space batches before the HUD) is hidden behind the world correctly
    if (Surface *zs = get_surface(t.zbp, t.fbw, t.zpsm, true, rows)) {
        if (zs->tex) {
            mbarrier();
            acquire(zs, 0, 0, std::min(cols, zs->w), std::min(rows, zs->h));
            zs->version++;
            g_ctx->barrier(2);
            g_mcmd = g_ctx->cmd;
            const int bits = t.zpsm == 0x30 ? 32 : t.zpsm == 0x31 ? 24 : 16;
            fc.gsz[0] = (float)(1.0 / (double)(1ull << bits));
            auto zubo = mframe(fc);
            const uint32_t zw = (uint32_t)(zs->w * g_scale), zh = (uint32_t)(zs->h * g_scale);
            mbegin(VK_NULL_HANDLE, zs->tex->view, zw, zh);
            MPipeKey k{};
            k.vs = VS_GSDEPTH;
            k.ps = PS_SHADOW;
            k.depth = 2;
            k.zfmt = VK_FORMAT_D32_SFLOAT;
            k.samples = 1;
            mbind(k);
            mviewport(0, 0, (float)zw, (float)zh);
            mset0(zubo.first, zubo.second, {});
            uint32_t dz = 0;
            modern_batches(*f, bc, 2, {(uint32_t)modern::WORLD, (uint32_t)modern::OBJECT}, dz, k);
        }
    }
    mbarrier();
    g_stats.draws += drawn;
    modern_release_textures();
    // the GS's next batch rebinds its own state
    g_ctx->boundPipeline = VK_NULL_HANDLE;
    g_ctx->dynamicValid = g_ctx->imagesValid = g_ctx->constantsValid = g_ctx->vertexValid = false;
    unbind_all();
    g_bound_rt = g_bound_ds = nullptr;
    g_state_valid = false;
}
