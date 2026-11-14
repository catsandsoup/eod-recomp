// Vulkan GS renderer: the surface ownership, batching and texture cache
// are ported from gs_d3d11.cpp; vk_compat.h maps its small state API to Vulkan.
#ifdef EOD_VULKAN
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "../../core/config.h"
#include "../../core/log.h"
#include "../../platform/platform.h"
#include "../../platform/screenshot_size.h"
#include "../../platform/presentation.h"
#include "../../platform/editor_test_startup.h"
#include "alpha_order.h"
#include "alpha_tiles.h"
#include "alpha_range.h"
#include "../gs_hw.h"
#include "../gs_sprite_edges.h"
#include "../gs_mem.h"
#include "../gs_state.h"
#include "../gs_swizzle.h"
#include "../texture_replacements.h"
#include "vk_compat.h"
#include "../../ui/overlay.h"
#include "gs_page_coverage.h"
#include "gpu_upload_blocks.h"

namespace gs {
extern bool g_skip_frame; // gs_draw.cpp
namespace hw {
#ifdef EOD_HW_SELECT
namespace vulkan {  // one of two backends (gs_hw_select.cpp chooses)
#else
bool g_on;
#endif
void readback_pages(uint32_t bp, uint32_t bw, uint32_t psm, int x, int y, int w, int h);

namespace {

#include "gs_shaders_vk.inc"
#include "overlay_vk.inc"

template <class T> void release(T *&p) {
    if (p)
        p->Release();
    p = nullptr;
}

// Failed adapter calls are logged with their source line.
int g_fails_logged;
HRESULT check(HRESULT hr, int line) {
    if (FAILED(hr) && g_fails_logged < 50) {
        g_fails_logged++;
        WARN("vulkan: call at gs_vulkan.cpp:%d failed (%08lx)", line, (unsigned long)hr);
    }
    return hr;
}
#define CHECK(call) check((call), __LINE__)

// ------------------------------------------------------------------ device
ID3D11Device *g_dev;
ID3D11DeviceContext *g_ctx;
int g_scale = 1; // internal resolution multiplier

ID3D11VertexShader *g_vs_draw, *g_vs_pass;
ID3D11PixelShader *g_ps_draw, *g_ps_convert_color, *g_ps_convert_depth, *g_ps_upload_color, *g_ps_upload_depth,
    *g_ps_readback_color, *g_ps_readback_depth, *g_ps_copy;
ID3D11InputLayout *g_layout;
ID3D11Buffer *g_vb, *g_cb_draw, *g_cb_pass;
constexpr UINT kVBBytes = 8 << 20;
UINT g_vb_pos;
ID3D11SamplerState *g_point_clamp, *g_linear_clamp;
ID3D11RasterizerState *g_rs_scissor, *g_rs_noscissor;
ID3D11DepthStencilState *g_ds_off, *g_ds_write_always;
ID3D11BlendState *g_bs_opaque, *g_bs_none, *g_bs_alpha;

std::thread::id g_gs_thread;
bool g_gs_thread_known;

struct Stats {
    uint32_t draws, prims, flushes, uploads, readbacks, converts, tex_new, tex_hit, tex_rt, tex_bytes, presents;
    uint32_t rb_reason[4]; // vram_needed from: 0 CPU (CLUT, transfers), 1
                           // textures, 2 acquire, 3 display
} g_stats;
int g_rb_reason;

// --------------------------------------------------------------- formats
enum Fmt : uint32_t { FMT_32 = 0, FMT_24 = 1, FMT_16 = 2, FMT_DEPTH = 3 };

bool is_depth_psm(uint32_t psm) { return (psm & 0x30) == 0x30; }
uint32_t color_fmt(uint32_t psm) {
    switch (psm & 0xF) {
    case 0x1:
        return FMT_24;
    case 0x2:
    case 0xA:
        return FMT_16;
    default:
        return FMT_32;
    }
}
int bpp_of(uint32_t psm) { return color_fmt(psm) == FMT_16 ? 16 : 32; }
double depth_range(uint32_t zpsm) { // 2^bits
    switch (zpsm & 0xF) {
    case 0x0:
        return 4294967296.0;
    case 0x1:
        return 16777216.0;
    default:
        return 65536.0;
    }
}
void page_dims(uint32_t psm, int &pw, int &ph) {
    pw = 64;
    ph = bpp_of(psm) == 16 ? 64 : 32;
}

// ---------------------------------------------------------------- shaders
bool create_shaders() {
    g_vs_draw = new vkgs::Shader;
    g_vs_draw->entry = "vs_draw";
    g_vs_draw->id = 0;
    {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = sizeof k_spv_vs_draw;
        info.pCode = k_spv_vs_draw;
        vkgs::require(vkCreateShaderModule(g_dev->device, &info, nullptr, &g_vs_draw->module), "shader vs_draw");
    }
    g_ps_draw = new vkgs::Shader;
    g_ps_draw->entry = "ps_draw";
    g_ps_draw->id = 1;
    {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = sizeof k_spv_ps_draw;
        info.pCode = k_spv_ps_draw;
        vkgs::require(vkCreateShaderModule(g_dev->device, &info, nullptr, &g_ps_draw->module), "shader ps_draw");
    }
    g_vs_pass = new vkgs::Shader;
    g_vs_pass->entry = "vs_pass";
    g_vs_pass->id = 2;
    {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = sizeof k_spv_vs_pass;
        info.pCode = k_spv_vs_pass;
        vkgs::require(vkCreateShaderModule(g_dev->device, &info, nullptr, &g_vs_pass->module), "shader vs_pass");
    }
    g_ps_convert_color = new vkgs::Shader;
    g_ps_convert_color->entry = "ps_convert_color";
    g_ps_convert_color->id = 3;
    {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = sizeof k_spv_ps_convert_color;
        info.pCode = k_spv_ps_convert_color;
        vkgs::require(vkCreateShaderModule(g_dev->device, &info, nullptr, &g_ps_convert_color->module),
                      "shader ps_convert_color");
    }
    g_ps_convert_depth = new vkgs::Shader;
    g_ps_convert_depth->entry = "ps_convert_depth";
    g_ps_convert_depth->id = 4;
    {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = sizeof k_spv_ps_convert_depth;
        info.pCode = k_spv_ps_convert_depth;
        vkgs::require(vkCreateShaderModule(g_dev->device, &info, nullptr, &g_ps_convert_depth->module),
                      "shader ps_convert_depth");
    }
    g_ps_upload_color = new vkgs::Shader;
    g_ps_upload_color->entry = "ps_upload_color";
    g_ps_upload_color->id = 5;
    {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = sizeof k_spv_ps_upload_color;
        info.pCode = k_spv_ps_upload_color;
        vkgs::require(vkCreateShaderModule(g_dev->device, &info, nullptr, &g_ps_upload_color->module),
                      "shader ps_upload_color");
    }
    g_ps_upload_depth = new vkgs::Shader;
    g_ps_upload_depth->entry = "ps_upload_depth";
    g_ps_upload_depth->id = 6;
    {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = sizeof k_spv_ps_upload_depth;
        info.pCode = k_spv_ps_upload_depth;
        vkgs::require(vkCreateShaderModule(g_dev->device, &info, nullptr, &g_ps_upload_depth->module),
                      "shader ps_upload_depth");
    }
    g_ps_readback_color = new vkgs::Shader;
    g_ps_readback_color->entry = "ps_readback_color";
    g_ps_readback_color->id = 7;
    {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = sizeof k_spv_ps_readback_color;
        info.pCode = k_spv_ps_readback_color;
        vkgs::require(vkCreateShaderModule(g_dev->device, &info, nullptr, &g_ps_readback_color->module),
                      "shader ps_readback_color");
    }
    g_ps_readback_depth = new vkgs::Shader;
    g_ps_readback_depth->entry = "ps_readback_depth";
    g_ps_readback_depth->id = 8;
    {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = sizeof k_spv_ps_readback_depth;
        info.pCode = k_spv_ps_readback_depth;
        vkgs::require(vkCreateShaderModule(g_dev->device, &info, nullptr, &g_ps_readback_depth->module),
                      "shader ps_readback_depth");
    }
    g_ps_copy = new vkgs::Shader;
    g_ps_copy->entry = "ps_copy";
    g_ps_copy->id = 9;
    {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = sizeof k_spv_ps_copy;
        info.pCode = k_spv_ps_copy;
        vkgs::require(vkCreateShaderModule(g_dev->device, &info, nullptr, &g_ps_copy->module), "shader ps_copy");
    }
    return true;
}

// --------------------------------------------------------------- surfaces
struct Surface {
    uint32_t bp, bw, psm; // psm: layout and format (a Z format for depth buffers)
    bool depth;
    int w, h; // GS pixels
    ID3D11Texture2D *tex = nullptr;
    ID3D11RenderTargetView *rtv = nullptr;
    ID3D11DepthStencilView *dsv = nullptr;
    ID3D11ShaderResourceView *srv = nullptr;
    uint32_t version = 0; // bumped when drawn to
    uint64_t last_use = 0;
};

std::vector<Surface *> g_surfaces;
Surface *g_owner[kPages]; // nullptr: the CPU copy is current
bool g_vram_ok[kPages];   // a surface owns the page and gs::vram also holds its
                          // contents
uint64_t g_frame_no;
// Proof of CPU/GPU equality for every storage bit in these physical blocks.
// Actual complete CPU uploads add bits; any later GPU draw acquisition clears
// them conservatively. These are independent of diagnostic drawn footprints.
uint32_t g_cpu_valid_blocks[kPages]{};
struct PendingUpload { Surface *owner; uint32_t page, valid_blocks; UploadBlockRect rect; };
std::vector<PendingUpload> g_pending_uploads;
bool gpu_uploads_enabled() {
    static const bool enabled = [] { const char *v = getenv("EOD_GPU_UPLOADS"); return v && atoi(v) != 0; }();
    return enabled;
}

// Diagnostic-only, conservative GS block footprints. These never decide
// ownership, synchronization, or whether a required readback is performed.
uint32_t g_diag_draw_blocks[kPages]{};
void block_dims(uint32_t psm, int &bw, int &bh, int &unit_bits);
bool readback_trace_enabled() {
    static const bool enabled = [] {
        const char *v = getenv("EOD_GS_READBACK_TRACE");
        return v && atoi(v) > 0;
    }();
    return enabled;
}
bool readback_trace_line() {
    static uint64_t lines = 0;
    static const uint64_t limit = [] {
        const char *v = getenv("EOD_GS_READBACK_TRACE");
        return uint64_t(v && atoi(v) > 1 ? std::min(atoi(v), 1000000) : 4096);
    }();
    return lines++ < limit;
}
template <class Fn>
void diagnostic_blocks(uint32_t bp, uint32_t bw, uint32_t psm, int x, int y, int w, int h, Fn fn) {
    int block_w, block_h, unit_bits;
    block_dims(psm, block_w, block_h, unit_bits);
    const SwizzleTab *tab = swizzle_table(psm, bw);
    if (w <= 0 || h <= 0)
        return;
    const int x0 = x & ~(block_w - 1), y0 = y & ~(block_h - 1);
    for (int py = y0; py < y + h; py += block_h)
        for (int px = x0; px < x + w; px += block_w) {
            uint32_t byte_address = uint32_t(uint64_t(tab->addr(bp, px, py)) * unit_bits / 8) % kVramSize;
            fn(byte_address / 8192, 1u << ((byte_address / 256) % 32));
        }
}

bool create_surface_texture(Surface *s) {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)(s->w * g_scale);
    td.Height = (UINT)(s->h * g_scale);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    if (s->depth) {
        td.Format = DXGI_FORMAT_R32_TYPELESS;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    } else {
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    }
    if (FAILED(CHECK(g_dev->CreateTexture2D(&td, nullptr, &s->tex))))
        return false;
    if (s->depth) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dd = {};
        dd.Format = DXGI_FORMAT_D32_FLOAT;
        dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        CHECK(g_dev->CreateDepthStencilView(s->tex, &dd, &s->dsv));
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_R32_FLOAT;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        CHECK(g_dev->CreateShaderResourceView(s->tex, &sd, &s->srv));
        g_ctx->ClearDepthStencilView(s->dsv, D3D11_CLEAR_DEPTH, 0.0f, 0);
    } else {
        CHECK(g_dev->CreateRenderTargetView(s->tex, nullptr, &s->rtv));
        CHECK(g_dev->CreateShaderResourceView(s->tex, nullptr, &s->srv));
        float black[4] = {0, 0, 0, 0};
        g_ctx->ClearRenderTargetView(s->rtv, black);
    }
    return s->rtv || s->dsv;
}

Surface *find_surface(uint32_t bp, uint32_t bw, uint32_t psm, bool depth) {
    for (Surface *s : g_surfaces)
        if (s->bp == bp && s->bw == bw && s->psm == psm && s->depth == depth)
            return s;
    return nullptr;
}

void page_of(const Surface *s, int px, int py, uint32_t &page) {
    page = ((s->bp >> 5) + (uint32_t)py * (s->bw ? s->bw : 1) + (uint32_t)px) % kPages;
}

// the surface's pages in rows [0, h): (page, x, y, w, h in GS pixels)
struct PageRect {
    uint32_t page;
    int x, y, w, h;
};
void surface_pages_rect(const Surface *s, int x0, int y0, int x1, int y1, std::vector<PageRect> &out) {
    int pw, ph;
    page_dims(s->psm, pw, ph);
    int cols = (int)(s->bw ? s->bw : 1);
    x0 = std::max(0, x0);
    y0 = std::max(0, y0);
    x1 = std::min(x1, std::min(s->w, cols * pw));
    y1 = std::min(y1, s->h);
    out.clear();
    if (x0 >= x1 || y0 >= y1)
        return;
    for (int r = y0 / ph; r <= (y1 - 1) / ph; r++)
        for (int c = x0 / pw; c <= (x1 - 1) / pw; c++) {
            PageRect pr;
            page_of(s, c, r, pr.page);
            pr.x = c * pw;
            pr.y = r * ph;
            pr.w = pw;
            pr.h = std::min(ph, s->h - r * ph);
            out.push_back(pr);
        }
}

void surface_pages(const Surface *s, int rows, std::vector<PageRect> &out) {
    surface_pages_rect(s, 0, 0, s->w, rows, out);
}

// ------------------------------------------------------------- pass helper
struct PassCB {
    float dst[4], src[4], misc[4];
    uint32_t fmt[4], page[4], texa[4];
    float color[4];
};

void set_viewport(int w, int h) {
    D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0.0f, 1.0f};
    g_ctx->RSSetViewports(1, &vp);
}

// Draws a rectangle pass into the bound target: dst in target texels, src in
// source texels.
void run_pass(ID3D11PixelShader *ps, int tw, int th, float dx0, float dy0, float dx1, float dy1, float sx0, float sy0,
              float sx1, float sy1, PassCB cb) {
    cb.dst[0] = dx0 / tw * 2.0f - 1.0f;
    cb.dst[1] = 1.0f - dy0 / th * 2.0f;
    cb.dst[2] = dx1 / tw * 2.0f - 1.0f;
    cb.dst[3] = 1.0f - dy1 / th * 2.0f;
    cb.src[0] = sx0;
    cb.src[1] = sy0;
    cb.src[2] = sx1;
    cb.src[3] = sy1;
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(CHECK(g_ctx->Map(g_cb_pass, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))) {
        memcpy(m.pData, &cb, sizeof cb);
        g_ctx->Unmap(g_cb_pass, 0);
    }
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->VSSetShader(g_vs_pass, nullptr, 0);
    g_ctx->VSSetConstantBuffers(1, 1, &g_cb_pass);
    g_ctx->PSSetShader(ps, nullptr, 0);
    g_ctx->PSSetConstantBuffers(1, 1, &g_cb_pass);
    g_ctx->RSSetState(g_rs_noscissor);
    g_ctx->Draw(6, 0);
}

void unbind_all() {
    ID3D11ShaderResourceView *null_srv[4] = {};
    g_ctx->PSSetShaderResources(0, 4, null_srv);
    g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

// ---------------------------------------------------------- draw batching
struct HwVertex {
    float x, y;
    uint32_t z;
    uint32_t rgba;
    float s, t, q;
    float fog;
};
static_assert(sizeof(HwVertex) == 32, "vertex layout");

struct DrawCB {
    float vp_scale[2], vp_offset[2];
    float zscale, zmax, lod_k, lod_l;
    float tex_size[2], stq_size[2];
    float tex_scale;
    uint32_t flags, aref, fix;
    uint32_t fog[4];
    uint32_t texa[4];
    uint32_t blend[4];
    int32_t wrap[4];
};

// what a batch of primitives needs
struct DrawState {
    Surface *rt, *ds;
    ID3D11ShaderResourceView *tex;
    int sampler_mag, sampler_min; // indices into the sampler cache
    int topology;                 // 0 triangles, 1 lines, 2 points
    uint32_t blend_key;           // see blend_state()
    uint32_t depth_key;           // see depth_state()
    int scissor[4];
    alpha_order::Range texture_alpha;
    bool two_pass; // alpha test with a fail mode that still writes something
    bool fb_only_alpha_failure; // candidate for exact constant-depth batching
    uint32_t fail_blend_key, fail_depth_key;
    DrawCB cb;
};

DrawState g_state;
bool g_state_valid;
std::vector<HwVertex> g_verts;

std::unordered_map<uint32_t, ID3D11BlendState *> g_blend_cache;
std::unordered_map<uint32_t, ID3D11DepthStencilState *> g_depth_cache;
std::vector<ID3D11SamplerState *> g_samplers; // index = linear | (wrap_u << 1) | (wrap_v << 2)

// blend key: bit 0 enable, 1-4 src factor, 5-9 dst factor, 10-12 op, 13-16
// write mask
ID3D11BlendState *blend_state(uint32_t key) {
    auto it = g_blend_cache.find(key);
    if (it != g_blend_cache.end())
        return it->second;
    D3D11_BLEND_DESC bd = {};
    D3D11_RENDER_TARGET_BLEND_DESC &r = bd.RenderTarget[0];
    r.BlendEnable = key & 1;
    r.SrcBlend = (D3D11_BLEND)((key >> 1) & 31);
    r.DestBlend = (D3D11_BLEND)((key >> 6) & 31);
    r.BlendOp = (D3D11_BLEND_OP)((key >> 11) & 7);
    r.SrcBlendAlpha = D3D11_BLEND_ONE;
    r.DestBlendAlpha = D3D11_BLEND_ZERO;
    r.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    r.RenderTargetWriteMask = (UINT8)((key >> 14) & 15);
    if (!r.BlendEnable) {
        r.SrcBlend = D3D11_BLEND_ONE;
        r.DestBlend = D3D11_BLEND_ZERO;
        r.BlendOp = D3D11_BLEND_OP_ADD;
    }
    ID3D11BlendState *s = nullptr;
    CHECK(g_dev->CreateBlendState(&bd, &s));
    g_blend_cache[key] = s;
    return s;
}
uint32_t blend_key(bool enable, D3D11_BLEND src, D3D11_BLEND dst, D3D11_BLEND_OP op, uint32_t mask) {
    return (enable ? 1u : 0u) | ((uint32_t)src << 1) | ((uint32_t)dst << 6) | ((uint32_t)op << 11) | (mask << 14);
}

// depth key: bit 0 enable, 1-4 func, 5 write
ID3D11DepthStencilState *depth_state(uint32_t key) {
    auto it = g_depth_cache.find(key);
    if (it != g_depth_cache.end())
        return it->second;
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = key & 1;
    dd.DepthFunc = (D3D11_COMPARISON_FUNC)((key >> 1) & 15);
    dd.DepthWriteMask = (key & 32) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    if (!dd.DepthFunc)
        dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ID3D11DepthStencilState *s = nullptr;
    CHECK(g_dev->CreateDepthStencilState(&dd, &s));
    g_depth_cache[key] = s;
    return s;
}
uint32_t depth_key(bool enable, D3D11_COMPARISON_FUNC f, bool write) {
    return (enable ? 1u : 0u) | ((uint32_t)f << 1) | (write ? 32u : 0u);
}

ID3D11SamplerState *sampler(int idx) {
    if (g_samplers.empty()) {
        for (int i = 0; i < 8; i++) {
            D3D11_SAMPLER_DESC sd = {};
            sd.Filter = (i & 1) ? D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT : D3D11_FILTER_MIN_MAG_MIP_POINT;
            sd.AddressU = (i & 2) ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_WRAP;
            sd.AddressV = (i & 4) ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_WRAP;
            sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.MaxLOD = D3D11_FLOAT32_MAX;
            sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
            ID3D11SamplerState *s = nullptr;
            CHECK(g_dev->CreateSamplerState(&sd, &s));
            g_samplers.push_back(s);
        }
    }
    return g_samplers[(size_t)idx & 7];
}

void bind_targets(Surface *rt, Surface *ds);
void acquire(Surface *s, int x0, int y0, int x1, int y1);

void draw_batch(const DrawState &s, UINT first, UINT count, bool fail_pass) {
    // The alpha-test half (F_ATEST_PASS/F_ATEST_FAIL) is a ps_draw pipeline
    // specialization: both halves of a two-pass group share the constants.
    g_ctx->PSSetSpecialization(s.two_pass ? (fail_pass ? 0x800u : 0x400u) : 0u);
    const DrawCB &cb = s.cb;
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(CHECK(g_ctx->Map(g_cb_draw, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))) {
        memcpy(m.pData, &cb, sizeof cb);
        g_ctx->Unmap(g_cb_draw, 0);
    }
    float zero[4] = {0, 0, 0, 0};
    g_ctx->OMSetBlendState(blend_state(fail_pass ? s.fail_blend_key : s.blend_key), zero, 0xFFFFFFFF);
    g_ctx->OMSetDepthStencilState(depth_state(fail_pass ? s.fail_depth_key : s.depth_key), 0);
    g_ctx->Draw(count, first);
    g_stats.draws++;
}

void flush() {
    if (g_verts.empty())
        return;
    std::vector<HwVertex> verts;
    verts.swap(g_verts); // (the passes below flush too)
    DrawState s = g_state;
    // take the pages under the batch (not the whole scissor: buffers share memory
    // with textures)
    float mx0 = 1e9f, my0 = 1e9f, mx1 = -1e9f, my1 = -1e9f;
    uint32_t min_z=UINT32_MAX,max_z=0;
    unsigned min_alpha=255,max_alpha=0;
    for (const HwVertex &v : verts) {
        mx0 = std::min(mx0, v.x);
        my0 = std::min(my0, v.y);
        mx1 = std::max(mx1, v.x);
        my1 = std::max(my1, v.y);
        min_z=std::min(min_z,v.z);max_z=std::max(max_z,v.z);
        min_alpha=std::min(min_alpha,v.rgba>>24);max_alpha=std::max(max_alpha,v.rgba>>24);
    }
    static const bool alpha_range_enabled=std::getenv("EOD_ALPHA_RANGE") ? std::atoi(std::getenv("EOD_ALPHA_RANGE"))!=0 : true;
    // PCSX2 CalcAlphaMinMax/TryAlphaTest: a uniform outcome needs only its
    // original write state. Mixed ranges retain native pass/fail ordering.
    if(s.two_pass && alpha_range_enabled) {
        const auto source=alpha_order::source_alpha(alpha_order::vertex_range(min_alpha,max_alpha),
            s.texture_alpha,s.cb.flags&1,s.cb.flags&4,(s.cb.flags>>16)&3);
        const unsigned test=(s.cb.flags>>20)&7;
        if(alpha_order::all_pass(test,s.cb.aref,source)) s.two_pass=false;
        else if(alpha_order::all_fail(test,s.cb.aref,source)) {
            s.two_pass=false;
            s.blend_key=s.fail_blend_key;
            s.depth_key=s.fail_depth_key;
        }
    }
    int sc = g_scale;
    int bx0 = std::max((int)floorf(mx0) - 1, s.scissor[0] / sc),
        by0 = std::max((int)floorf(my0) - 1, s.scissor[1] / sc);
    int bx1 = std::min((int)ceilf(mx1) + 2, s.scissor[2] / sc), by1 = std::min((int)ceilf(my1) + 2, s.scissor[3] / sc);
    if (bx0 >= bx1 || by0 >= by1)
        return;
    acquire(s.rt, bx0, by0, bx1, by1);
    s.rt->version++;
    if (s.ds) {
        acquire(s.ds, bx0, by0, bx1, by1);
        s.ds->version++;
    }
    UINT bytes = (UINT)(verts.size() * sizeof(HwVertex));
    if (bytes > kVBBytes) { // split huge batches (multiples of 6 vertices)
        size_t per = (kVBBytes / sizeof(HwVertex)) / 6 * 6;
        for (size_t i = 0; i < verts.size(); i += per) {
            g_verts.assign(verts.begin() + (long)i, verts.begin() + (long)std::min(verts.size(), i + per));
            flush();
        }
        return;
    }
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (g_vb_pos + bytes > kVBBytes) {
        g_vb_pos = 0;
        mode = D3D11_MAP_WRITE_DISCARD;
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(CHECK(g_ctx->Map(g_vb, 0, mode, 0, &m))))
        return;
    memcpy((uint8_t *)m.pData + g_vb_pos, verts.data(), bytes);
    g_ctx->Unmap(g_vb, 0);
    UINT first = g_vb_pos / sizeof(HwVertex), count = (UINT)verts.size();
    g_vb_pos += bytes;
    g_ctx->SetVertexWindow(g_vb, first, count); // one upload for this batch's draws

    bind_targets(s.rt, s.ds);
    UINT stride = sizeof(HwVertex), offset = 0;
    g_ctx->IASetVertexBuffers(0, 1, &g_vb, &stride, &offset);
    g_ctx->IASetInputLayout(g_layout);
    g_ctx->IASetPrimitiveTopology(s.topology == 0   ? D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST
                                  : s.topology == 1 ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST
                                                    : D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    g_ctx->VSSetShader(g_vs_draw, nullptr, 0);
    g_ctx->VSSetConstantBuffers(0, 1, &g_cb_draw);
    g_ctx->PSSetShader(g_ps_draw, nullptr, 0);
    g_ctx->PSSetConstantBuffers(0, 1, &g_cb_draw);
    ID3D11ShaderResourceView *srv = s.tex;
    g_ctx->PSSetShaderResources(0, 1, &srv);
    ID3D11SamplerState *ss[2] = {sampler(s.sampler_mag), sampler(s.sampler_min)};
    g_ctx->PSSetSamplers(0, 2, ss);
    g_ctx->RSSetState(g_rs_scissor);
    D3D11_RECT r = {s.scissor[0], s.scissor[1], s.scissor[2], s.scissor[3]};
    g_ctx->RSSetScissorRects(1, &r);
    const auto ztest=((s.depth_key>>1)&15)==D3D11_COMPARISON_GREATER_EQUAL ? alpha_order::Test::GreaterEqual :
                     ((s.depth_key>>1)&15)==D3D11_COMPARISON_ALWAYS ? alpha_order::Test::Always : alpha_order::Test::Other;
    if (s.two_pass && alpha_order::defer_depth(s.fb_only_alpha_failure,s.depth_key&32,ztest,min_z,max_z)) {
        // FB_ONLY writes color for both alpha outcomes. With a constant Z and
        // GEQUAL/ALWAYS, earlier depth writes cannot change later visibility.
        // Keep ALL color draws in native order, then write passing depth only.
        DrawState color=s;color.two_pass=false;color.depth_key&=~32u;
        draw_batch(color,first,count,false);
        DrawState depth=s;depth.two_pass=false;depth.cb.flags|=0x400u;
        depth.blend_key&=~(15u<<14);
        draw_batch(depth,first,count,false);
    } else if (s.two_pass) {
        // Alpha-fail writes are part of each GS primitive, not a second layer
        // over the whole batch. A later glyph fill must follow an earlier
        // shadow's FB_ONLY pixels, including when the alpha test fails.
        const UINT primitive_size=s.topology==0 ? 3 : s.topology==1 ? 2 : 1;
        static const bool group_disjoint=std::getenv("EOD_ALPHA_GROUP") ? std::atoi(std::getenv("EOD_ALPHA_GROUP"))!=0 : true;
        alpha_order::Tiles tiles(s.rt->w*sc,s.rt->h*sc);
        const alpha_order::Bounds clip{s.scissor[0],s.scissor[1],s.scissor[2],s.scissor[3]};
        UINT group_first=0;
        auto draw_group=[&](UINT end) {
            if(end==group_first) return;
            draw_batch(s,first+group_first,end-group_first,false);
            draw_batch(s,first+group_first,end-group_first,true);
        };
        for(UINT offset=0;offset<count;offset+=primitive_size) {
            float x0=verts[offset].x,y0=verts[offset].y,x1=x0,y1=y0;
            for(UINT i=1;i<primitive_size;i++) {
                const auto &v=verts[offset+i];
                x0=std::min(x0,v.x);y0=std::min(y0,v.y);x1=std::max(x1,v.x);y1=std::max(y1,v.y);
            }
            const auto coverage=alpha_order::bounds(x0,y0,x1,y1,sc,clip);
            if(!group_disjoint || !tiles.append(coverage)) {
                draw_group(offset);group_first=offset;tiles.clear();tiles.append(coverage);
            }
        }
        draw_group(count);
    } else draw_batch(s, first, count, false);
    g_stats.flushes++;
}

Surface *g_bound_rt, *g_bound_ds;

void bind_targets(Surface *rt, Surface *ds) {
    Surface *any = rt ? rt : ds;
    ID3D11RenderTargetView *rtv = rt ? rt->rtv : nullptr;
    // unbind textures first (a target may have been a texture of the previous
    // batch)
    ID3D11ShaderResourceView *null_srv[4] = {};
    g_ctx->PSSetShaderResources(0, 4, null_srv);
    g_ctx->OMSetRenderTargets(rtv ? 1 : 0, rtv ? &rtv : nullptr, ds ? ds->dsv : nullptr);
    if (any)
        set_viewport(any->w * g_scale, any->h * g_scale);
    g_bound_rt = rt;
    g_bound_ds = ds;
}

// ------------------------------------------------------- VRAM <-> surfaces
// raw GS value of pixel (x, y) of buffer (bp, bw, psm) in gs::vram
inline uint32_t vram_raw(const SwizzleTab *T, uint32_t rb, uint32_t rc, int x, uint32_t psm) {
    uint32_t a = T->addr_row(rb, rc, x);
    if (bpp_of(psm) == 16)
        return half(a);
    uint32_t v = word(a);
    return (psm & 0xF) == 0x1 ? (v & 0xFFFFFF) : v;
}

ID3D11Texture2D *g_raw_tex; // R32_UINT staging for uploads (DEFAULT, 2048x2048)
ID3D11ShaderResourceView *g_raw_srv;
ID3D11Texture2D *g_rb_tex,
    *g_rb_staging; // readback target and staging (R32_UINT 2048x2048)
ID3D11RenderTargetView *g_rb_rtv;
// Deferred CPU-write preservation copies (see deferred_schedule).
bool deferred_kick_enabled() {
    static const bool enabled = [] { const char *v = getenv("EOD_DEFER_KICK"); return !v || atoi(v) != 0; }();
    return enabled;
}
constexpr int kSlotPixels = 64;  // a slot holds one owner page rectangle (<= 64x64 GS pixels)
constexpr int kSlots = 256;
ID3D11Texture2D *g_drb_staging;   // kSlots slots of 64x64 R32 values, stacked vertically
std::vector<int> g_free_slots;
uint8_t g_slot_refs[kSlots]; // holders of a slot's copy (a deferred page, CLUT snapshots)
int slot_take() {
    const int slot = g_free_slots.back();
    g_free_slots.pop_back();
    g_slot_refs[slot] = 1;
    return slot;
}
void slot_release(int slot) {
    if (--g_slot_refs[slot] == 0)
        g_free_slots.push_back(slot);
}

bool create_raw_textures() {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = 2048;
    td.Height = 2048;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_UINT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(CHECK(g_dev->CreateTexture2D(&td, nullptr, &g_raw_tex))))
        return false;
    CHECK(g_dev->CreateShaderResourceView(g_raw_tex, nullptr, &g_raw_srv));
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(CHECK(g_dev->CreateTexture2D(&td, nullptr, &g_rb_tex))))
        return false;
    CHECK(g_dev->CreateRenderTargetView(g_rb_tex, nullptr, &g_rb_rtv));
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(CHECK(g_dev->CreateTexture2D(&td, nullptr, &g_rb_staging))))
        return false;
    td.Width = kSlotPixels;
    td.Height = kSlotPixels * kSlots;
    if (FAILED(CHECK(g_dev->CreateTexture2D(&td, nullptr, &g_drb_staging))))
        return false;
    for (int i = kSlots - 1; i >= 0; --i)
        g_free_slots.push_back(i);
    return true;
}

// Uploads rectangle (x, y, w, h) of the surface from gs::vram.
void deferred_before_read(uint32_t bp, uint32_t bw, uint32_t psm, int x, int y, int w, int h, const char *why);
void upload_rect(Surface *s, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0)
        return;
    flush();
    deferred_before_read(s->bp, s->bw, s->psm, x, y, w, h, "upload");
    const SwizzleTab *T = swizzle_table(s->psm, s->bw);
    std::vector<uint32_t> raw((size_t)w * h);
    for (int r = 0; r < h; r++) {
        int py = y + r;
        uint32_t rb = T->row_base(s->bp, py), rc = T->rowC[py & 2047];
        for (int c = 0; c < w; c++)
            raw[(size_t)r * w + c] = vram_raw(T, rb, rc, x + c, s->psm);
    }
    D3D11_BOX box = {0, 0, 0, (UINT)w, (UINT)h, 1};
    g_ctx->UpdateSubresource(g_raw_tex, 0, &box, raw.data(), (UINT)w * 4, 0);
    unbind_all();
    if (s->depth)
        g_ctx->OMSetRenderTargets(0, nullptr, s->dsv);
    else
        g_ctx->OMSetRenderTargets(1, &s->rtv, nullptr);
    set_viewport(s->w * g_scale, s->h * g_scale);
    g_ctx->OMSetBlendState(g_bs_opaque, nullptr, 0xFFFFFFFF);
    g_ctx->OMSetDepthStencilState(s->depth ? g_ds_write_always : g_ds_off, 0);
    g_ctx->PSSetShaderResources(2, 1, &g_raw_srv);
    PassCB cb = {};
    cb.fmt[1] = s->depth ? FMT_DEPTH : color_fmt(s->psm);
    cb.misc[1] = (float)depth_range(s->psm);
    run_pass(s->depth ? g_ps_upload_depth : g_ps_upload_color, s->w * g_scale, s->h * g_scale, (float)x * g_scale,
             (float)y * g_scale, (float)(x + w) * g_scale, (float)(y + h) * g_scale, 0, 0, (float)w, (float)h, cb);
    unbind_all();
    g_bound_rt = g_bound_ds = nullptr;
    g_stats.uploads++;
}

// GPU half of a readback: raw GS values of rectangle (x, y, w, h) of the surface
// into g_rb_tex texels (0, 0, w, h).
void readback_record(Surface *s, int x, int y, int w, int h) {
    flush();
    unbind_all();
    g_ctx->OMSetRenderTargets(1, &g_rb_rtv, nullptr);
    set_viewport(2048, 2048);
    g_ctx->OMSetBlendState(g_bs_opaque, nullptr, 0xFFFFFFFF);
    g_ctx->OMSetDepthStencilState(g_ds_off, 0);
    ID3D11ShaderResourceView *srv = s->srv;
    g_ctx->PSSetShaderResources(s->depth ? 1 : 0, 1, &srv);
    PassCB cb = {};
    cb.fmt[0] = s->depth ? FMT_DEPTH : color_fmt(s->psm);
    cb.misc[0] = (float)depth_range(s->psm);
    // pixel i (1x) samples source texel x*S + i*S + S/2: the middle of the scaled
    // pixel
    float sc = (float)g_scale;
    run_pass(s->depth ? g_ps_readback_depth : g_ps_readback_color, 2048, 2048, 0, 0, (float)w, (float)h, x * sc, y * sc,
             (x + w) * sc, (y + h) * sc, cb);
    unbind_all();
    g_bound_rt = g_bound_ds = nullptr;
}

// Writes rectangle (x, y, w, h) of the surface back to gs::vram.
void readback_rect(Surface *s, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0)
        return;
    readback_record(s, x, y, w, h);
    D3D11_BOX box = {0, 0, 0, (UINT)w, (UINT)h, 1};
    g_ctx->CopySubresourceRegion(g_rb_staging, 0, 0, 0, 0, g_rb_tex, 0, &box);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(CHECK(g_ctx->Map(g_rb_staging, 0, D3D11_MAP_READ, 0, &m))))
        return;
    const SwizzleTab *T = swizzle_table(s->psm, s->bw);
    bool b16 = bpp_of(s->psm) == 16, b24 = (s->psm & 0xF) == 0x1;
    for (int r = 0; r < h; r++) {
        const uint32_t *src = (const uint32_t *)((const uint8_t *)m.pData + (size_t)r * m.RowPitch);
        int py = y + r;
        uint32_t rb = T->row_base(s->bp, py), rc = T->rowC[py & 2047];
        for (int c = 0; c < w; c++) {
            uint32_t a = T->addr_row(rb, rc, x + c), v = src[c];
            if (b16)
                half(a) = (uint16_t)v;
            else if (b24)
                word(a) = (word(a) & 0xFF000000u) | (v & 0xFFFFFF);
            else
                word(a) = v;
        }
    }
    g_ctx->Unmap(g_rb_staging, 0);
    vram_mark(s->bp, s->bw, s->psm, x, y, w, h);
    g_stats.readbacks++;
    g_stats.rb_reason[g_rb_reason & 3]++;
}

// ------------------------------------------- deferred CPU-write preservation
// A CPU write into part of a GPU-owned page used to read the whole page back
// synchronously before the write (submit + fence wait per page). The deferred
// form records the identical readback pass at the identical command position,
// copies its result into a private staging slot and lets the CPU write proceed.
// Ownership metadata changes exactly as before. The page's other bytes stay
// stale in gs::vram until a CPU consumer reads a storage block that still holds
// a bit the CPU has not rewritten since; that read first waits for the copy and
// merges it bit-exactly (GPU value where the CPU has not written, CPU value
// where it has), which is the byte image the synchronous order would produce.
// Bits the CPU rewrites completely never need the GPU copy at all.
// EOD_DEFER_PRESERVE=0 restores the synchronous readback for comparison.
bool deferred_preserve_enabled() {
    static const bool enabled = [] { const char *v = getenv("EOD_DEFER_PRESERVE"); return !v || atoi(v) != 0; }();
    return enabled;
}
struct DeferredPart { uint32_t bp, bw, psm; int x, y, w, h, slot; };
struct DeferredPage {
    uint64_t serial = 0;              // command buffer holding the copies
    std::vector<DeferredPart> parts;  // in the synchronous readback order
    uint8_t written[8192];            // storage bits the CPU wrote since (per byte)
    uint32_t pending_blocks = ~0u;    // 256-byte blocks with a bit still awaiting the GPU
};
DeferredPage *g_deferred[kPages];
uint32_t g_deferred_count;
struct DeferredStats { uint32_t scheduled, merged, waited, overwritten, kicked; } g_dstats;
// Scheduling hint only (never affects data): this page's previous deferral
// had to be merged, so submit its copy right away to have it ready.
bool g_deferred_merged_last[kPages];

void deferred_free(uint32_t page) {
    DeferredPage *d = g_deferred[page];
    for (const DeferredPart &q : d->parts)
        slot_release(q.slot);
    delete d;
    g_deferred[page] = nullptr;
    --g_deferred_count;
}

// Completes page `page`: waits for its copies and merges the untouched bits.
void deferred_resolve(uint32_t page, const char *why = "") {
    DeferredPage *d = g_deferred[page];
    if (!d)
        return;
    if (readback_trace_enabled() && readback_trace_line())
        fprintf(stderr, "[gs-readback] frame=%llu deferred-merge %s page=%u pending=%08x serial=%llu current=%llu completed=%llu\n",
                (unsigned long long)g_frame_no, why, page, d->pending_blocks, (unsigned long long)d->serial,
                (unsigned long long)g_dev->serial, (unsigned long long)g_dev->completed);
    if (g_dev->wait_serial(d->serial))
        g_dstats.waited++;
    const uint8_t *base = (const uint8_t *)g_drb_staging->host.map;
    for (const DeferredPart &q : d->parts) {
        const SwizzleTab *T = swizzle_table(q.psm, q.bw);
        const bool b16 = bpp_of(q.psm) == 16, b24 = (q.psm & 0xF) == 0x1;
        const uint32_t *slot = (const uint32_t *)(base + (size_t)q.slot * kSlotPixels * kSlotPixels * 4);
        for (int r = 0; r < q.h; r++) {
            const uint32_t *src = slot + (size_t)r * kSlotPixels;
            int py = q.y + r;
            uint32_t rb = T->row_base(q.bp, py), rc = T->rowC[py & 2047];
            for (int c = 0; c < q.w; c++) {
                uint32_t a = T->addr_row(rb, rc, q.x + c), v = src[c];
                // The value readback_rect would store, as bytes at `addr`.
                uint32_t addr, n;
                if (b16) {
                    addr = (a & 0x1FFFFF) * 2;
                    n = 2;
                } else {
                    addr = (a & 0xFFFFF) * 4;
                    n = 4;
                    if (b24)
                        v = (word(a) & 0xFF000000u) | (v & 0xFFFFFF);
                }
                for (uint32_t k = 0; k < n; k++) {
                    uint8_t &b = vram[addr + k];
                    const uint32_t off = (addr + k) - page * 8192;
                    const uint8_t keep = off < 8192 ? d->written[off] : 0;
                    b = (uint8_t)((b & keep) | ((v >> (8 * k)) & ~keep & 0xFF));
                }
            }
        }
    }
    g_dstats.merged++;
    g_deferred_merged_last[page] = true;
    deferred_free(page);
}

void deferred_resolve_all() {
    for (uint32_t p = 0; p < kPages && g_deferred_count; ++p)
        if (g_deferred[p])
            deferred_resolve(p);
}

// A CPU consumer is about to read the blocks under this rectangle.
void deferred_before_read(uint32_t bp, uint32_t bw, uint32_t psm, int x, int y, int w, int h, const char *why) {
    if (!g_deferred_count)
        return;
    const PageCoverage c = page_coverage(bp, bw, psm, x, y, w, h);
    for (uint32_t i = 0; i < c.count; ++i) {
        uint32_t p = c.pages[i];
        if (g_deferred[p] && (c.touched[p] & g_deferred[p]->pending_blocks)) {
            if (readback_trace_enabled() && readback_trace_line())
                fprintf(stderr, "[gs-readback] deferred read %s bp=%x bw=%u psm=%x rect=%d,%d,%d,%d touched=%08x\n", why, bp,
                        bw, psm, x, y, w, h, c.touched[p]);
            deferred_resolve(p, why);
        }
    }
}

// True when every bit a texture decode of this level reads (conservatively the
// whole storage unit of each texel) is already final in gs::vram, i.e. not in a
// deferred page or rewritten by the CPU since the deferral.
bool deferred_texels_final(uint32_t bp, uint32_t bw, uint32_t psm, int w, int h) {
    if (!g_deferred_count)
        return true;
    const SwizzleTab *T = swizzle_table(psm, bw);
    for (int y = 0; y < h; y++) {
        uint32_t rb = T->row_base(bp, y), rc = T->rowC[y & 2047];
        for (int x = 0; x < w; x++) {
            uint32_t a = T->addr_row(rb, rc, x), addr, n = 1;
            uint8_t m = 0xFF;
            switch (psm) {
            case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: addr = (a & 0x1FFFFF) * 2; n = 2; break;
            case PSMT8: addr = a & 0x3FFFFF; break;
            case PSMT4: addr = (a >> 1) & 0x3FFFFF; m = (a & 1) ? 0xF0 : 0x0F; break;
            case PSMCT32: case PSMZ32: case PSMCT24: case PSMZ24: case PSMT8H: case PSMT4HL: case PSMT4HH:
                addr = (a & 0xFFFFF) * 4; n = 4; break;
            default: return false;
            }
            const DeferredPage *d = g_deferred[addr / 8192];
            if (!d)
                continue;
            for (uint32_t k = 0; k < n; k++)
                if ((d->written[(addr + k) % 8192] & m) != m)
                    return false;
        }
    }
    return true;
}

// Same synchronization as readback_pages(p * 32, 1, PSMCT32, 0, 0, 64, 32) for
// an owned page, with the host wait and the gs::vram store deferred.
void deferred_schedule(uint32_t page) {
    Surface *o = g_owner[page];
    std::vector<PageRect> pages;
    surface_pages(o, o->h, pages);
    DeferredPage *d = nullptr;
    for (const PageRect &q : pages) {
        if (q.page != page || q.w <= 0 || q.h <= 0)
            continue;
        if (g_free_slots.empty())
            deferred_resolve_all();
        if (!d) {
            d = new DeferredPage;
            memset(d->written, 0, sizeof d->written);
            g_deferred[page] = d;
            ++g_deferred_count;
        }
        const int slot = slot_take();
        readback_record(o, q.x, q.y, q.w, q.h);
        D3D11_BOX box = {0, 0, 0, (UINT)q.w, (UINT)q.h, 1};
        g_ctx->CopySubresourceRegion(g_drb_staging, 0, 0, (UINT)(slot * kSlotPixels), 0, g_rb_tex, 0, &box);
        d->parts.push_back({o->bp, o->bw, o->psm, q.x, q.y, q.w, q.h, slot});
        vram_mark(o->bp, o->bw, o->psm, q.x, q.y, q.w, q.h);
        g_stats.readbacks++;
        g_stats.rb_reason[g_rb_reason & 3]++;
    }
    if (!d) { // not part of the surface's rows (shouldn't happen): give up the page
        g_owner[page] = nullptr;
        return;
    }
    g_ctx->host_read_barrier();
    d->serial = g_dev->serial;
    g_vram_ok[page] = true;
    g_cpu_valid_blocks[page] = 0xffffffffu;
    g_dstats.scheduled++;
    if (g_deferred_merged_last[page] && deferred_kick_enabled()) {
        // Start the GPU on it now (asynchronously) so a later merge rarely waits.
        g_dev->submit(false, false);
        g_dstats.kicked++;
    }
}

// The CPU is about to store these pixels: their bits no longer need GPU data.
void deferred_cpu_written(const PageCoverage &cov, uint32_t bp, uint32_t bw, uint32_t psm, int x, int y, int w,
                          int h) {
    bool any = false;
    for (uint32_t i = 0; i < cov.count && !any; ++i)
        any = g_deferred[cov.pages[i]] != nullptr;
    if (!any)
        return;
    switch (psm) {
    case PSMCT32: case PSMZ32: case PSMCT24: case PSMZ24: case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S:
    case PSMT8: case PSMT4: case PSMT8H: case PSMT4HL: case PSMT4HH:
        break;
    default: // no exact bit model for this format: complete the pages first
        for (uint32_t i = 0; i < cov.count; ++i)
            deferred_resolve(cov.pages[i]);
        return;
    }
    const SwizzleTab *T = swizzle_table(psm, bw);
    auto mark_pixel = [&](int px, int py) {
        uint32_t a = T->addr(bp, px, py), addr = 0, n = 1;
        uint8_t m = 0xFF;
        switch (psm) {
        case PSMCT32: case PSMZ32: addr = (a & 0xFFFFF) * 4; n = 4; break;
        case PSMCT24: case PSMZ24: addr = (a & 0xFFFFF) * 4; n = 3; break;
        case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: addr = (a & 0x1FFFFF) * 2; n = 2; break;
        case PSMT8: addr = a & 0x3FFFFF; break;
        case PSMT4: addr = (a >> 1) & 0x3FFFFF; m = (a & 1) ? 0xF0 : 0x0F; break;
        case PSMT8H: addr = (a & 0xFFFFF) * 4 + 3; break;
        case PSMT4HL: addr = (a & 0xFFFFF) * 4 + 3; m = 0x0F; break;
        case PSMT4HH: addr = (a & 0xFFFFF) * 4 + 3; m = 0xF0; break;
        }
        DeferredPage *d = g_deferred[addr / 8192];
        if (!d)
            return;
        for (uint32_t k = 0; k < n; k++)
            d->written[(addr + k) % 8192] |= m;
    };
    // Block by block (as page_coverage): a block-aligned block inside the
    // rectangle of a whole-storage format replaces all 256 bytes of its block.
    int block_w, block_h, unit_bits;
    block_dims(psm, block_w, block_h, unit_bits);
    const bool whole_storage = psm != PSMCT24 && psm != PSMZ24 && psm != PSMT8H && psm != PSMT4HL && psm != PSMT4HH;
    const int x0 = x & ~(block_w - 1), y0 = y & ~(block_h - 1);
    for (int by = y0; by < y + h; by += block_h)
        for (int bx = x0; bx < x + w; bx += block_w) {
            if (whole_storage && bx >= x && by >= y && bx + block_w <= x + w && by + block_h <= y + h) {
                const uint32_t address = uint32_t(uint64_t(T->addr(bp, bx, by)) * unit_bits / 8) % kVramSize;
                if (DeferredPage *d = g_deferred[address / 8192])
                    memset(d->written + (address % 8192 & ~255u), 0xFF, 256);
                continue;
            }
            for (int py = std::max(by, y); py < std::min(by + block_h, y + h); py++)
                for (int px = std::max(bx, x); px < std::min(bx + block_w, x + w); px++)
                    mark_pixel(px, py);
        }
    for (uint32_t i = 0; i < cov.count; ++i) {
        uint32_t p = cov.pages[i];
        DeferredPage *d = g_deferred[p];
        if (!d)
            continue;
        uint32_t pending = d->pending_blocks;
        for (uint32_t b = 0; b < 32; b++) {
            if (!(pending & cov.touched[p] & (1u << b)))
                continue;
            uint64_t all = ~0ull;
            const uint8_t *blk = d->written + b * 256;
            for (int k = 0; k < 256; k += 8) {
                uint64_t v;
                memcpy(&v, blk + k, 8);
                all &= v;
            }
            if (all == ~0ull)
                pending &= ~(1u << b);
        }
        d->pending_blocks = pending;
        if (!pending) { // every bit rewritten by the CPU: the copy is never needed
            g_dstats.overwritten++;
            g_deferred_merged_last[p] = false;
            deferred_free(p);
        }
    }
}

// ------------------------------------------------- lazy CLUT source snapshots
// A CLUT load reading GPU-rendered (or deferred-preserved) blocks used to do a
// synchronous readback: submit, then wait for all earlier GPU work. The game
// often writes TEX0 with CLD before it uploads the palette, and loads again
// afterwards, so that first CLUT is never used. Here the identical readback
// pass is recorded at the identical command position into private staging
// slots, together with a copy of the CPU bytes of the same pages and the
// deferred-merge masks in force now. The page images are completed (waiting
// for the GPU) only when the loaded CLUT entries are actually read. The image
// of each page is what gs::vram would hold after the synchronous order at the
// load. Ownership metadata is left as it is: later CPU readers synchronize as
// they always do. EOD_LAZY_CLUT=0 restores the synchronous readback.
bool lazy_clut_enabled() {
    static const bool enabled = [] { const char *v = getenv("EOD_LAZY_CLUT"); return !v || atoi(v) != 0; }();
    return enabled;
}
struct SnapshotPage {
    uint32_t page = 0;
    uint64_t serial = 0;              // command buffer holding the GPU copies (0: none)
    std::vector<DeferredPart> readback; // owner readback parts (readback_rect order)
    std::vector<DeferredPart> deferred; // a deferred page's parts, merged with `written`
    std::vector<uint8_t> written;       // deferred-merge mask at the snapshot
    uint8_t bytes[8192];                // gs::vram bytes at the snapshot, completed once
    bool complete = false;
};
} // namespace
struct ClutSnapshot {
    std::vector<SnapshotPage *> pages;
};
namespace {
uint64_t g_clut_lazy, g_clut_lazy_waits;

// The bytes of a GS pixel at swizzled unit address `a` for this format.
inline void pixel_bytes(uint32_t psm, uint32_t a, uint32_t &addr, uint32_t &n) {
    if (bpp_of(psm) == 16) {
        addr = (a & 0x1FFFFF) * 2;
        n = 2;
    } else {
        addr = (a & 0xFFFFF) * 4;
        n = 4;
    }
}

void snapshot_complete(SnapshotPage &sp) {
    if (sp.complete)
        return;
    if (sp.serial && g_dev->wait_serial(sp.serial))
        ++g_clut_lazy_waits;
    const uint8_t *base = (const uint8_t *)g_drb_staging->host.map;
    auto store = [&](const DeferredPart &q, bool merge) {
        const SwizzleTab *T = swizzle_table(q.psm, q.bw);
        const bool b16 = bpp_of(q.psm) == 16, b24 = (q.psm & 0xF) == 0x1;
        const uint32_t *slot = (const uint32_t *)(base + (size_t)q.slot * kSlotPixels * kSlotPixels * 4);
        for (int r = 0; r < q.h; r++) {
            const uint32_t *src = slot + (size_t)r * kSlotPixels;
            int py = q.y + r;
            uint32_t rb = T->row_base(q.bp, py), rc = T->rowC[py & 2047];
            for (int c = 0; c < q.w; c++) {
                uint32_t a = T->addr_row(rb, rc, q.x + c), v = src[c], addr, n;
                pixel_bytes(q.psm, a, addr, n);
                const uint32_t off0 = addr - sp.page * 8192;
                if (off0 >= 8192)
                    continue; // (parts lie in their page; defensive)
                if (b24 && !b16) {
                    uint32_t old;
                    memcpy(&old, sp.bytes + off0, 4);
                    v = (old & 0xFF000000u) | (v & 0xFFFFFF);
                }
                for (uint32_t k = 0; k < n; k++) {
                    uint8_t &b = sp.bytes[off0 + k];
                    const uint8_t keep = merge ? sp.written[off0 + k] : 0;
                    b = (uint8_t)((b & keep) | ((v >> (8 * k)) & ~keep & 0xFF));
                }
            }
        }
    };
    for (const DeferredPart &q : sp.deferred) // deferred_resolve, as at the snapshot
        store(q, true);
    for (const DeferredPart &q : sp.readback) // then readback_rect, as readback_pages
        store(q, false);
    sp.complete = true;
}
} // namespace

ClutSnapshot *clut_snapshot(uint32_t bp, uint32_t bw, uint32_t psm, int x, int y, int w, int h) {
    if (!g_on)
        return nullptr;
    flush(); // CPU reads observe all earlier queued GS primitives (vram_needed)
    g_rb_reason = 0;
    const PageCoverage c = page_coverage(bp, bw, psm, x, y, w, h);
    // What vram_needed would synchronize, refined for deferred pages to the
    // actual bytes read: bytes the CPU rewrote since the deferral are final.
    const SwizzleTab *T = swizzle_table(psm, bw);
    std::vector<uint32_t> def_pages, rb_pages;
    for (uint32_t i = 0; i < c.count; ++i) {
        const uint32_t p = c.pages[i];
        DeferredPage *d = g_deferred[p];
        if (d && (c.touched[p] & d->pending_blocks)) {
            bool final_bytes = true;
            for (int py = y; py < y + h && final_bytes; py++)
                for (int px = x; px < x + w && final_bytes; px++) {
                    uint32_t addr, n;
                    pixel_bytes(psm, T->addr(bp, px, py), addr, n);
                    if (addr / 8192 != p)
                        continue;
                    for (uint32_t k = 0; k < n; k++)
                        if (d->written[(addr + k) % 8192] != 0xFF)
                            final_bytes = false;
                }
            if (!final_bytes)
                def_pages.push_back(p);
        }
        Surface *o = g_owner[p];
        if (o && !g_vram_ok[p] && (c.touched[p] & ~g_cpu_valid_blocks[p]))
            rb_pages.push_back(p);
    }
    if (def_pages.empty() && rb_pages.empty())
        return nullptr;
    bool lazy = lazy_clut_enabled() && g_free_slots.size() >= 64;
    for (uint32_t p : def_pages)
        lazy = lazy && std::find(rb_pages.begin(), rb_pages.end(), p) == rb_pages.end();
    for (uint32_t p : rb_pages) {
        bool found = false;
        std::vector<PageRect> pages;
        surface_pages(g_owner[p], g_owner[p]->h, pages);
        for (const PageRect &q : pages)
            found = found || q.page == p;
        lazy = lazy && found;
    }
    if (!lazy) { // the synchronous order
        readback_pages(bp, bw, psm, x, y, w, h);
        return nullptr;
    }
    auto *snap = new ClutSnapshot;
    bool recorded = false;
    for (uint32_t i = 0; i < c.count; ++i) {
        const uint32_t p = c.pages[i];
        auto *sp = new SnapshotPage;
        sp->page = p;
        memcpy(sp->bytes, vram + (size_t)p * 8192, 8192);
        if (std::find(def_pages.begin(), def_pages.end(), p) != def_pages.end()) {
            DeferredPage *d = g_deferred[p];
            sp->deferred = d->parts;
            for (const DeferredPart &q : d->parts)
                ++g_slot_refs[q.slot];
            sp->written.assign(d->written, d->written + 8192);
            sp->serial = d->serial;
        }
        if (std::find(rb_pages.begin(), rb_pages.end(), p) != rb_pages.end()) {
            Surface *o = g_owner[p];
            std::vector<PageRect> pages;
            surface_pages(o, o->h, pages);
            for (const PageRect &q : pages) {
                if (q.page != p || q.w <= 0 || q.h <= 0)
                    continue;
                const int slot = slot_take();
                readback_record(o, q.x, q.y, q.w, q.h);
                D3D11_BOX box = {0, 0, 0, (UINT)q.w, (UINT)q.h, 1};
                g_ctx->CopySubresourceRegion(g_drb_staging, 0, 0, (UINT)(slot * kSlotPixels), 0, g_rb_tex, 0, &box);
                sp->readback.push_back({o->bp, o->bw, o->psm, q.x, q.y, q.w, q.h, slot});
                recorded = true;
            }
        }
        snap->pages.push_back(sp);
    }
    if (recorded) {
        g_ctx->host_read_barrier();
        for (SnapshotPage *sp : snap->pages)
            if (!sp->readback.empty())
                sp->serial = g_dev->serial; // (after the deferred part's serial, which is earlier)
    }
    ++g_clut_lazy;
    return snap;
}

const uint8_t *clut_snapshot_page(ClutSnapshot *s, uint32_t page) {
    for (SnapshotPage *sp : s->pages)
        if (sp->page == page) {
            snapshot_complete(*sp);
            return sp->bytes;
        }
    return nullptr;
}

void clut_snapshot_free(ClutSnapshot *s) {
    if (!s)
        return;
    for (SnapshotPage *sp : s->pages) {
        for (const DeferredPart &q : sp->deferred)
            slot_release(q.slot);
        for (const DeferredPart &q : sp->readback)
            slot_release(q.slot);
        delete sp;
    }
    delete s;
}
namespace {

// ------------------------------------------------------------ conversions
// Remap tables between two block layouts of the same pixel size: for each
// pixel of a page in layout `dst`, the pixel of a page in layout `src` at the
// same address.
std::unordered_map<uint64_t, ID3D11ShaderResourceView *> g_remaps;

ID3D11ShaderResourceView *remap_table(uint32_t dst_psm, uint32_t src_psm) {
    uint64_t key = ((uint64_t)dst_psm << 32) | src_psm;
    auto it = g_remaps.find(key);
    if (it != g_remaps.end())
        return it->second;
    int pw, ph;
    page_dims(dst_psm, pw, ph);
    const SwizzleTab *Td = swizzle_table(dst_psm, 1), *Ts = swizzle_table(src_psm, 1);
    std::unordered_map<uint32_t, uint32_t> where; // address -> src x | y << 16
    for (int y = 0; y < ph; y++)
        for (int x = 0; x < pw; x++)
            where[Ts->addr(0, x, y)] = (uint32_t)x | ((uint32_t)y << 16);
    std::vector<uint16_t> tab((size_t)pw * ph * 2);
    for (int y = 0; y < ph; y++)
        for (int x = 0; x < pw; x++) {
            auto w = where.find(Td->addr(0, x, y));
            uint32_t v = w != where.end() ? w->second : ((uint32_t)x | ((uint32_t)y << 16));
            tab[((size_t)y * pw + x) * 2] = (uint16_t)(v & 0xFFFF);
            tab[((size_t)y * pw + x) * 2 + 1] = (uint16_t)(v >> 16);
        }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)pw;
    td.Height = (UINT)ph;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16_UINT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = {tab.data(), (UINT)pw * 4, 0};
    ID3D11Texture2D *t = nullptr;
    ID3D11ShaderResourceView *srv = nullptr;
    if (SUCCEEDED(CHECK(g_dev->CreateTexture2D(&td, &init, &t)))) {
        CHECK(g_dev->CreateShaderResourceView(t, nullptr, &srv));
        t->Release();
    }
    g_remaps[key] = srv;
    return srv;
}

bool same_layout(uint32_t a, uint32_t b) {
    // CT32/CT24 share a layout, as do Z32/Z24; everything else differs
    auto norm = [](uint32_t p) { return (p & 0xF) == 0x1 ? (p & ~0xFu) : p; };
    return norm(a) == norm(b);
}

// Can a page of `src` be converted to `dst` on the GPU?
bool convertible(const Surface *src, const Surface *dst) {
    return src->bp == dst->bp && bpp_of(src->psm) == bpp_of(dst->psm);
}

// Fills rectangle (x, y, w, h) (GS pixels) of dst from the same memory in src.
// texa: produce texture colours (16/24-bit alpha from TEXA) into a colour
// target.
void convert_rect(Surface *src, ID3D11RenderTargetView *dst_rtv, ID3D11DepthStencilView *dst_dsv, uint32_t dst_psm,
                  uint32_t dst_bw, int dst_w, int dst_h, int x, int y, int w, int h, bool texa, const Env *e) {
    flush();
    unbind_all();
    if (dst_dsv)
        g_ctx->OMSetRenderTargets(0, nullptr, dst_dsv);
    else
        g_ctx->OMSetRenderTargets(1, &dst_rtv, nullptr);
    set_viewport(dst_w * g_scale, dst_h * g_scale);
    g_ctx->OMSetBlendState(g_bs_opaque, nullptr, 0xFFFFFFFF);
    g_ctx->OMSetDepthStencilState(dst_dsv ? g_ds_write_always : g_ds_off, 0);
    ID3D11ShaderResourceView *srv = src->srv;
    g_ctx->PSSetShaderResources(src->depth ? 1 : 0, 1, &srv);
    PassCB cb = {};
    cb.fmt[0] = src->depth ? FMT_DEPTH : color_fmt(src->psm);
    cb.fmt[1] = dst_dsv ? FMT_DEPTH : color_fmt(dst_psm);
    cb.misc[0] = (float)depth_range(src->psm);
    cb.misc[1] = (float)depth_range(dst_psm);
    cb.misc[2] = (float)g_scale;
    cb.misc[3] = (float)g_scale;
    if (!same_layout(src->psm, dst_psm) || src->bw != dst_bw) {
        cb.fmt[2] |= 1;
        int pw, ph;
        page_dims(dst_psm, pw, ph);
        cb.page[0] = (uint32_t)pw;
        cb.page[1] = (uint32_t)ph;
        cb.page[2] = dst_bw ? dst_bw : 1;
        cb.page[3] = src->bw ? src->bw : 1;
        ID3D11ShaderResourceView *rm = same_layout(src->psm, dst_psm) ? nullptr : remap_table(dst_psm, src->psm);
        if (!rm)
            rm = remap_table(dst_psm, dst_psm);
        g_ctx->PSSetShaderResources(3, 1, &rm);
    }
    if (texa && e) {
        cb.fmt[2] |= 2;
        cb.texa[0] = e->ta0;
        cb.texa[1] = e->ta1;
        cb.texa[2] = e->aem;
    }
    float sc = (float)g_scale;
    run_pass(dst_dsv ? g_ps_convert_depth : g_ps_convert_color, dst_w * g_scale, dst_h * g_scale, x * sc, y * sc,
             (x + w) * sc, (y + h) * sc, 0, 0, 0, 0, cb);
    unbind_all();
    g_bound_rt = g_bound_ds = nullptr;
    g_stats.converts++;
}

// Makes the surface the owner of its pages in rows [0, rows).
// Runs fn(x, y, w, h) over the pages: once over their bounding rectangle when
// they fill it, else page by page.
template <class F> void for_rects(const std::vector<PageRect> &pages, F fn) {
    if (pages.empty())
        return;
    int x0 = pages[0].x, y0 = pages[0].y, x1 = x0, y1 = y0, area = 0;
    for (const PageRect &p : pages) {
        x0 = std::min(x0, p.x);
        y0 = std::min(y0, p.y);
        x1 = std::max(x1, p.x + p.w);
        y1 = std::max(y1, p.y + p.h);
        area += p.w * p.h;
    }
    if (area == (x1 - x0) * (y1 - y0)) {
        fn(x0, y0, x1 - x0, y1 - y0);
        return;
    }
    // Sparse ownership often forms row strips. Merge adjacent pages without
    // touching holes owned by another surface; this avoids a render scope per page.
    std::vector<PageRect> sorted = pages, rects;
    std::sort(sorted.begin(), sorted.end(),
              [](const PageRect &a, const PageRect &b) { return a.y != b.y ? a.y < b.y : a.x < b.x; });
    for (const PageRect &p : sorted) {
        if (!rects.empty() && rects.back().y == p.y && rects.back().h == p.h && rects.back().x + rects.back().w == p.x)
            rects.back().w += p.w;
        else
            rects.push_back(p);
    }
    for (size_t i = 0; i < rects.size(); ++i) {
        auto &r = rects[i];
        if (!r.h)
            continue;
        for (size_t j = i + 1; j < rects.size(); ++j) {
            auto &n = rects[j];
            if (n.h && n.x == r.x && n.w == r.w && n.y == r.y + r.h) {
                r.h += n.h;
                n.h = 0;
            }
        }
        fn(r.x, r.y, r.w, r.h);
    }
}

// Makes the surface the owner of the pages under rectangle (x0, y0)-(x1, y1),
// bringing over what the memory held.
void acquire(Surface *s, int x0, int y0, int x1, int y1) {
    std::vector<PageRect> pages;
    surface_pages_rect(s, x0, y0, x1, y1, pages);
    std::vector<PageRect> up;
    std::vector<std::pair<Surface *, std::vector<PageRect>>> conv;
    for (const PageRect &p : pages) {
        Surface *o = g_owner[p.page];
        if (o != s) {
            if (!o || g_vram_ok[p.page]) {
                up.push_back(p);
            } else if (convertible(o, s)) {
                auto it = std::find_if(conv.begin(), conv.end(), [&](const auto &c) { return c.first == o; });
                if (it == conv.end()) {
                    conv.push_back({o, {}});
                    it = conv.end() - 1;
                }
                it->second.push_back(p);
            } else {
                // through the CPU copy: the owner's data at this page, then upload
                g_rb_reason = 2;
                std::vector<PageRect> op;
                surface_pages(o, o->h, op);
                for (const PageRect &q : op)
                    if (q.page == p.page)
                        readback_rect(o, q.x, q.y, q.w, q.h);
                up.push_back(p);
            }
        }
    }
    for (auto &c : conv)
        for_rects(c.second, [&](int x, int y, int w, int h) {
            convert_rect(c.first, s->rtv, s->dsv, s->psm, s->bw, s->w, s->h, x, y, w, h, false, nullptr);
        });
    for_rects(up, [&](int x, int y, int w, int h) { upload_rect(s, x, y, w, h); });
    for (const PageRect &p : pages) {
        g_owner[p.page] = s;
        g_vram_ok[p.page] = false; // about to be drawn: the CPU copy goes stale
        g_cpu_valid_blocks[p.page] = 0;
    }
    if (readback_trace_enabled())
        diagnostic_blocks(s->bp,s->bw,s->psm,x0,y0,x1-x0,y1-y0,
            [](uint32_t page,uint32_t mask){g_diag_draw_blocks[page] |= mask;});
}

Surface *get_surface(uint32_t bp, uint32_t bw, uint32_t psm, bool depth, int rows) {
    if (!bw)
        bw = 1;
    Surface *s = find_surface(bp, bw, psm, depth);
    int need_h = rows <= 512 ? 512 : rows <= 1024 ? 1024 : 2048;
    if (s && s->h < need_h) { // grow (keeps the contents)
        Surface *n = new Surface(*s);
        n->tex = nullptr;
        n->rtv = nullptr;
        n->dsv = nullptr;
        n->srv = nullptr;
        n->h = need_h;
        flush();
        if (!create_surface_texture(n)) {
            delete n;
            return s;
        }
        D3D11_BOX box = {0, 0, 0, (UINT)(s->w * g_scale), (UINT)(s->h * g_scale), 1};
        g_ctx->CopySubresourceRegion(n->tex, 0, 0, 0, 0, s->tex, 0, &box);
        for (auto &o : g_owner)
            if (o == s)
                o = n;
        std::replace(g_surfaces.begin(), g_surfaces.end(), s, n);
        if (g_bound_rt == s || g_bound_ds == s)
            g_bound_rt = g_bound_ds = nullptr;
        release(s->rtv);
        release(s->dsv);
        release(s->srv);
        release(s->tex);
        delete s;
        s = n;
    }
    if (!s) {
        s = new Surface;
        s->bp = bp;
        s->bw = bw;
        s->psm = psm;
        s->depth = depth;
        s->w = (int)std::min<uint32_t>(bw * 64, 2048);
        s->h = need_h;
        if (!create_surface_texture(s)) {
            delete s;
            return nullptr;
        }
        g_surfaces.push_back(s);
    }
    s->last_use = g_frame_no;
    return s;
}

// --------------------------------------------------------- texture cache
struct TexObj {
    ID3D11Texture2D *tex = nullptr;
    ID3D11ShaderResourceView *srv = nullptr; // a host replacement's srv holds its own reference
    uint32_t bytes = 0;
    alpha_order::Range alpha;
    uint64_t last_use = 0;
    float scale = 1;   // bound texels per GS texel (host replacement factor)
    uint64_t host = 0; // g_host_gpu key of the bound replacement, 0 = native
};
std::unordered_map<uint64_t, TexObj> g_tex;
uint64_t g_tex_bytes;

// ------------------------------------------------- host texture replacements
// docs/HOST_TEXTURE_REPLACEMENT.md. Keys are computed only on a texture-cache
// miss and only when replacements are loaded or EOD_HOST_TEXTURE_LOG is set.
constexpr uint32_t kTexHostFlag = 0x8000; // F_TEX_HOST in gs_shaders_vk.hlsl
struct HostGpu {
    ID3D11Texture2D *tex = nullptr;
    ID3D11ShaderResourceView *srv = nullptr;
    uint64_t bytes = 0, last_use = 0;
    alpha_order::Range alpha;
};
hosttex::Store g_host;
bool g_host_on;   // replacements loaded
FILE *g_host_log; // EOD_HOST_TEXTURE_LOG
uint64_t g_host_budget = 1536ull << 20;
std::unordered_map<uint64_t, HostGpu> g_host_gpu; // (entry << 1 | GS alpha domain) + 1
uint64_t g_host_gpu_bytes;
std::unordered_set<uint64_t> g_host_logged, g_host_noted;
struct HostStats {
    uint32_t hits, misses, fallbacks, uploads, evictions;
    double upload_ms;
} g_hstats;
// validation shortcut: texture description -> (content key, VRAM clock when
// checked)
struct TexCheck {
    uint64_t key;
    uint32_t clock;
};
std::unordered_map<uint64_t, TexCheck> g_tex_check;

inline uint64_t mix64(uint64_t h, uint64_t v) {
    h ^= v * 0x9E3779B97F4A7C15ull;
    h = (h << 27 | h >> 37) * 0xC2B2AE3D27D4EB4Full;
    return h;
}

void block_dims(uint32_t psm, int &bw, int &bh, int &unit_bits) {
    switch (psm) {
    case PSMCT16:
    case PSMCT16S:
    case PSMZ16:
    case PSMZ16S:
        bw = 16;
        bh = 8;
        unit_bits = 16;
        break;
    case PSMT8:
        bw = 16;
        bh = 16;
        unit_bits = 8;
        break;
    case PSMT4:
        bw = 32;
        bh = 16;
        unit_bits = 4;
        break;
    default:
        bw = 8;
        bh = 8;
        unit_bits = 32;
        break;
    }
}

// hash of the GS memory blocks holding a w x h texture level
uint64_t hash_level(uint32_t tbp, uint32_t tbw, uint32_t psm, int w, int h) {
    const SwizzleTab *T = swizzle_table(psm, tbw);
    int bw, bh, ub;
    block_dims(psm, bw, bh, ub);
    uint64_t hash = 0x84222325CBF29CE4ull;
    for (int y = 0; y < h; y += bh)
        for (int x = 0; x < w; x += bw) {
            uint32_t a = T->addr(tbp, x, y);
            uint32_t byte_addr = ub == 32 ? a * 4 : ub == 16 ? a * 2 : ub == 8 ? a : a / 2;
            const uint64_t *p = reinterpret_cast<const uint64_t *>(vram + ((byte_addr & ~255u) & (kVramSize - 1)));
            // Read exactly the same 256-byte GS block. A vectorized content
            // hash avoids the old 32-deep dependent multiply chain; ownership,
            // generations, deferred reads and texture decoding stay unchanged.
            static const bool fast_hash = [] {
                const char *v = std::getenv("EOD_TEXTURE_HASH");
                return !v || std::atoi(v) != 0;
            }();
            if (fast_hash) hash = mix64(hash, XXH3_64bits(p, 256));
            else for (int k = 0; k < 32; k++) hash = mix64(hash, p[k]);
        }
    return hash;
}

// RGBA texels of a level
void decode_level(const Env &e, int lvl, int w, int h, std::vector<uint32_t> &out) {
    out.resize((size_t)w * h);
    uint32_t bp = e.mip_tbp[lvl], bw = e.mip_tbw[lvl];
    const SwizzleTab *T = swizzle_table(e.tpsm, bw);
    for (int y = 0; y < h; y++) {
        uint32_t rb = T->row_base(bp, y), rc = T->rowC[y & 2047];
        uint32_t *d = &out[(size_t)y * w];
        switch (e.tpsm) {
        case PSMCT32:
        case PSMZ32:
            for (int x = 0; x < w; x++)
                d[x] = word(T->addr_row(rb, rc, x));
            break;
        case PSMT8:
            for (int x = 0; x < w; x++)
                d[x] = e.pal[byte(T->addr_row(rb, rc, x))];
            break;
        case PSMT4:
            for (int x = 0; x < w; x++)
                d[x] = e.pal[nibble(T->addr_row(rb, rc, x))];
            break;
        default:
            for (int x = 0; x < w; x++)
                d[x] = tex_fetch(e, x, y, lvl);
            break;
        }
    }
}

int tex_levels(const Env &e) { return (e.mmin >= 2 && e.mxl > 0) ? std::min(e.mxl, 6) + 1 : 1; }

ID3D11ShaderResourceView *host_texture(int idx, uint32_t native_alpha_high, alpha_order::Range &alpha, uint64_t &id);

// Palette indices of mip 0, row-major (PSMT8/PSMT4 and the high-bit forms).
void host_index_plane(const Env &e, std::vector<uint8_t> &out) {
    out.resize((size_t)e.tw * e.th);
    const SwizzleTab *T = swizzle_table(e.tpsm, e.mip_tbw[0]);
    uint32_t bp = e.mip_tbp[0];
    for (uint32_t y = 0; y < e.th; y++) {
        uint32_t rb = T->row_base(bp, (int)y), rc = T->rowC[y & 2047];
        uint8_t *d = &out[(size_t)y * e.tw];
        switch (e.tpsm) {
        case PSMT8:
            for (uint32_t x = 0; x < e.tw; x++) d[x] = byte(T->addr_row(rb, rc, (int)x));
            break;
        case PSMT4:
            for (uint32_t x = 0; x < e.tw; x++) d[x] = (uint8_t)nibble(T->addr_row(rb, rc, (int)x));
            break;
        case PSMT8H:
            for (uint32_t x = 0; x < e.tw; x++) d[x] = (uint8_t)(word(T->addr_row(rb, rc, (int)x)) >> 24);
            break;
        case PSMT4HL:
            for (uint32_t x = 0; x < e.tw; x++) d[x] = (uint8_t)((word(T->addr_row(rb, rc, (int)x)) >> 24) & 15);
            break;
        default: // PSMT4HH
            for (uint32_t x = 0; x < e.tw; x++) d[x] = (uint8_t)(word(T->addr_row(rb, rc, (int)x)) >> 28);
            break;
        }
    }
}

ID3D11ShaderResourceView *vram_texture(const Env &e, alpha_order::Range &alpha, float &scale, uint32_t &flags) {
    int levels = tex_levels(e);
    // description key: what identifies the texture apart from the memory contents
    uint64_t desc = mix64(0, e.tpsm | (e.tw << 8) | ((uint64_t)e.th << 20) | ((uint64_t)levels << 40));
    for (int i = 0; i < levels; i++)
        desc = mix64(desc, e.mip_tbp[i] | ((uint64_t)e.mip_tbw[i] << 32));
    bool pal = e.tpsm == PSMT8 || e.tpsm == PSMT4 || e.tpsm == PSMT8H || e.tpsm == PSMT4HL || e.tpsm == PSMT4HH;
    if (pal) {
        int n = (e.tpsm == PSMT8 || e.tpsm == PSMT8H) ? 256 : 16;
        uint64_t ph = 0;
        for (int i = 0; i < n; i += 2)
            ph = mix64(ph, e.pal[i] | ((uint64_t)e.pal[i + 1] << 32));
        desc = mix64(desc, ph);
    } else {
        desc = mix64(desc, e.ta0 | (e.ta1 << 8) | ((uint32_t)e.aem << 16));
    }
    // memory unchanged since the last check: same contents
    uint32_t newest = 0;
    for (int i = 0; i < levels; i++) {
        uint32_t first, count;
        page_range(e.mip_tbp[i], e.mip_tbw[i], e.tpsm, 0, 0, std::max(1, (int)e.tw >> i), std::max(1, (int)e.th >> i),
                   first, count);
        for (uint32_t k = 0; k < count; k++)
            newest = std::max(newest, g_page_gen[(first + k) % kPages]);
    }
    uint64_t key;
    auto ck = g_tex_check.find(desc);
    if (ck != g_tex_check.end() && ck->second.clock >= newest) {
        key = ck->second.key;
    } else {
        // The content key may hash stale bytes of deferred pages: the key only
        // has to change whenever a decoded texel does, and decode below never
        // reads a bit still awaiting deferred GPU data.
        key = desc;
        for (int i = 0; i < levels; i++)
            key = mix64(key, hash_level(e.mip_tbp[i], e.mip_tbw[i], e.tpsm, std::max(1, (int)e.tw >> i),
                                        std::max(1, (int)e.th >> i)));
        g_tex_check[desc] = {key, g_vram_clock};
    }
    auto it = g_tex.find(key);
    if (it != g_tex.end()) {
        it->second.last_use = g_frame_no;
        g_stats.tex_hit++;
        alpha=it->second.alpha;
        scale = it->second.scale;
        if (it->second.host) {
            flags |= kTexHostFlag;
            g_hstats.hits++;
            auto h = g_host_gpu.find(it->second.host);
            if (h != g_host_gpu.end())
                h->second.last_use = g_frame_no;
        }
        return it->second.srv;
    }
    // decode and upload
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = e.tw;
    td.Height = e.th;
    td.MipLevels = (UINT)levels;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<std::vector<uint32_t>> data((size_t)levels);
    D3D11_SUBRESOURCE_DATA init[7];
    uint32_t bytes = 0;
    alpha_order::Range decoded_alpha{255,0};
    for (int i = 0; i < levels; i++)
        if (!deferred_texels_final(e.mip_tbp[i], e.mip_tbw[i], e.tpsm, std::max(1, (int)e.tw >> i),
                                   std::max(1, (int)e.th >> i)))
            deferred_before_read(e.mip_tbp[i], e.mip_tbw[i], e.tpsm, 0, 0, std::max(1, (int)e.tw >> i),
                                 std::max(1, (int)e.th >> i), "texture decode");
    for (int i = 0; i < levels; i++) {
        int w = std::max(1, (int)e.tw >> i), h = std::max(1, (int)e.th >> i);
        decode_level(e, i, w, h, data[(size_t)i]);
        for(uint32_t pixel:data[(size_t)i]) {
            decoded_alpha.low=std::min(decoded_alpha.low,pixel>>24);
            decoded_alpha.high=std::max(decoded_alpha.high,pixel>>24);
        }
        init[i].pSysMem = data[(size_t)i].data();
        init[i].SysMemPitch = (UINT)w * 4;
        init[i].SysMemSlicePitch = 0;
        bytes += (uint32_t)(w * h * 4);
    }
    if (pal && (g_host_on || g_host_log)) {
        bool candidate = g_host_on && g_host.has_dims(e.tw, e.th);
        if (candidate || g_host_log) {
            static std::vector<uint8_t> plane;
            host_index_plane(e, plane);
            const uint32_t bits = (e.tpsm == PSMT8 || e.tpsm == PSMT8H) ? 8 : 4;
            const uint64_t ik = hosttex::index_key(bits, e.tw, e.th, plane.data());
            const uint64_t pk = hosttex::palette_key(e.pal, bits == 8 ? 256 : 16);
            if (g_host_log && g_host_logged.insert(mix64(mix64(ik, pk), e.mip_tbp[0] | (uint64_t)e.tpsm << 32)).second) {
                fprintf(g_host_log, "%016llx %016llx %02x %u %u %x %llu\n", (unsigned long long)ik,
                        (unsigned long long)pk, e.tpsm, e.tw, e.th, e.mip_tbp[0], (unsigned long long)g_frame_no);
                fflush(g_host_log);
            }
            if (candidate) {
                std::string why;
                int idx = g_host.find(ik, pk, e.tw, e.th, why);
                if (idx < 0 && why.empty()) {
                    g_hstats.misses++;
                } else if (idx < 0) {
                    g_hstats.fallbacks++;
                    if (g_host_noted.insert(mix64(ik, pk)).second)
                        WARN("host textures: %016llx %016llx %ux%u not bound: %s; native texture kept",
                             (unsigned long long)ik, (unsigned long long)pk, e.tw, e.th, why.c_str());
                } else {
                    alpha_order::Range ha;
                    uint64_t id = 0;
                    ID3D11ShaderResourceView *srv = host_texture(idx, decoded_alpha.high, ha, id);
                    if (srv) {
                        TexObj t;
                        srv->AddRef();
                        t.srv = srv;
                        t.alpha = alpha = ha;
                        t.last_use = g_frame_no;
                        t.scale = scale = (float)g_host.entries()[(size_t)idx].scale;
                        t.host = id;
                        flags |= kTexHostFlag;
                        g_hstats.hits++;
                        g_stats.tex_new++;
                        return g_tex.emplace(key, t).first->second.srv;
                    }
                    g_hstats.fallbacks++;
                }
            }
        }
    }
    TexObj t;
    if (FAILED(CHECK(g_dev->CreateTexture2D(&td, init, &t.tex))))
        return nullptr;
    CHECK(g_dev->CreateShaderResourceView(t.tex, nullptr, &t.srv));
    // Guard normalized filtering/conversion truncation at byte boundaries.
    t.alpha=alpha=alpha_order::vertex_range(decoded_alpha.low,decoded_alpha.high);
    t.bytes = bytes;
    t.last_use = g_frame_no;
    g_tex_bytes += bytes;
    g_stats.tex_new++;
    g_stats.tex_bytes += bytes;
    // evict textures unused for a while when the cache is large
    if (g_tex_bytes > (256u << 20)) {
        flush();
        for (auto i = g_tex.begin(); i != g_tex.end();) {
            if (i->second.last_use + 2 < g_frame_no) {
                g_tex_bytes -= i->second.bytes;
                release(i->second.srv);
                release(i->second.tex);
                i = g_tex.erase(i);
            } else {
                ++i;
            }
        }
    }
    return g_tex.emplace(key, t).first->second.srv;
}

// Drops a resident replacement and every cached texture that borrowed it.
void host_evict(uint64_t id) {
    auto h = g_host_gpu.find(id);
    if (h == g_host_gpu.end())
        return;
    flush(); // queued draws may still reference the view
    for (auto i = g_tex.begin(); i != g_tex.end();) {
        if (i->second.host == id) {
            release(i->second.srv);
            i = g_tex.erase(i);
        } else {
            ++i;
        }
    }
    g_host_gpu_bytes -= h->second.bytes;
    release(h->second.srv);
    release(h->second.tex);
    g_host_gpu.erase(h);
    g_hstats.evictions++;
}

// The GPU texture of replacement idx in the alpha domain of the native
// texture (native alpha <= 0x80: GS domain), uploading it within the budget.
ID3D11ShaderResourceView *host_texture(int idx, uint32_t native_alpha_high, alpha_order::Range &alpha, uint64_t &id) {
    const hosttex::Entry &en = g_host.entries()[(size_t)idx];
    const bool gs_alpha = hosttex::gs_alpha_domain(native_alpha_high);
    id = ((uint64_t)idx << 1 | (gs_alpha ? 1 : 0)) + 1;
    auto it = g_host_gpu.find(id);
    if (it != g_host_gpu.end()) {
        it->second.last_use = g_frame_no;
        alpha = it->second.alpha;
        return it->second.srv;
    }
    const uint64_t need = hosttex::htx_total_bytes(en.w, en.h, en.levels) - hosttex::kHtxHeaderBytes;
    while (g_host_gpu_bytes + need > g_host_budget) {
        uint64_t victim = 0, oldest = UINT64_MAX;
        for (auto &h : g_host_gpu)
            if (h.second.last_use < g_frame_no && h.second.last_use < oldest) {
                oldest = h.second.last_use;
                victim = h.first;
            }
        if (!victim) {
            if (g_host_noted.insert(id ^ 0x8000000000000000ull).second)
                WARN("host textures: %s (%s) not bound: GPU budget %llu MB full (%llu MB resident, %llu MB needed); "
                     "native texture kept",
                     en.asset_id.c_str(), en.package_id.c_str(), (unsigned long long)(g_host_budget >> 20),
                     (unsigned long long)(g_host_gpu_bytes >> 20), (unsigned long long)(need >> 20));
            return nullptr;
        }
        host_evict(victim);
    }
    auto t0 = std::chrono::steady_clock::now();
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = en.w;
    td.Height = en.h;
    td.MipLevels = en.levels;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    HostGpu g;
    if (FAILED(CHECK(g_dev->CreateTexture2D(&td, nullptr, &g.tex))))
        return nullptr;
    uint32_t lo = 255, hi = 0;
    g_dev->UploadLevels(g.tex, [&](UINT l, uint8_t *dst) {
        size_t n = (size_t)std::max(1u, en.w >> l) * std::max(1u, en.h >> l);
        hosttex::convert_texels(en.data + hosttex::htx_level_offset(en.w, en.h, l), dst, n, gs_alpha, lo, hi);
    });
    CHECK(g_dev->CreateShaderResourceView(g.tex, nullptr, &g.srv));
    g.bytes = need;
    g.last_use = g_frame_no;
    g.alpha = alpha = alpha_order::vertex_range(lo, hi);
    g_host_gpu_bytes += need;
    g_host_gpu.emplace(id, g);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    g_hstats.uploads++;
    g_hstats.upload_ms += ms;
    WARN("host textures: bound %s (%s) %ux%u x%u, native alpha max 0x%02x -> %s, replacement alpha %u..%u, "
         "%.1f MB uploaded in %.1f ms, %.1f MB resident",
         en.asset_id.c_str(), en.package_id.c_str(), en.w, en.h, en.scale, native_alpha_high,
         gs_alpha ? "GS domain (a*128+127)/255" : "straight 0..255", lo, hi, need / 1048576.0, ms,
         g_host_gpu_bytes / 1048576.0);
    return g.srv;
}

void host_textures_init() {
    auto sink = [](const std::string &m) { WARN("%s", m.c_str()); };
    const char *root = std::getenv("EOD_MOD_DIR");
    const char *off = std::getenv("EOD_HOST_TEXTURES");
    const bool disabled = off && std::strcmp(off, "0") == 0;
    if (root && *root) {
        char path[4096];
        if (platform_find_file(root, "PROFILE.TXT", path, sizeof path))
            if (FILE *f = fopen(path, "r")) {
                char line[1024];
                while (fgets(line, sizeof line, f)) {
                    size_t n = strlen(line);
                    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
                        line[--n] = 0;
                    WARN("profile: %s", line);
                }
                fclose(f);
            }
        if (disabled) {
            WARN("host textures: disabled (EOD_HOST_TEXTURES=0)");
        } else {
            auto find = [](const std::string &dir, const std::string &rel, std::string &out) {
                char p[4096];
                if (!platform_find_file(dir.c_str(), rel.c_str(), p, sizeof p))
                    return false;
                out = p;
                return true;
            };
            auto t0 = std::chrono::steady_clock::now();
            size_t n = g_host.load(root, sink, find);
            g_host_on = n > 0;
            if (const char *b = std::getenv("EOD_HOST_TEXTURE_BUDGET_MB"))
                if (std::atoll(b) > 0)
                    g_host_budget = (uint64_t)std::atoll(b) << 20;
            uint64_t cpu_budget = 2048ull << 20;
            if (const char *b = std::getenv("EOD_HOST_TEXTURE_CPU_BUDGET_MB"))
                if (std::atoll(b) >= 0)
                    cpu_budget = (uint64_t)std::atoll(b) << 20;
            if (g_host_on) {
                uint64_t total = 0;
                for (const auto &en : g_host.entries())
                    total += en.bytes;
                WARN("host textures: %zu replacements, %.1f MB on disk, manifest parsed in %.1f ms; GPU budget %llu MB",
                     n, total / 1048576.0,
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(),
                     (unsigned long long)(g_host_budget >> 20));
                g_host.prefetch(cpu_budget, sink);
            }
        }
    }
    if (const char *p = std::getenv("EOD_HOST_TEXTURE_LOG"))
        if (*p && !(g_host_log = fopen(p, "a")))
            WARN("host textures: cannot open EOD_HOST_TEXTURE_LOG %s", p);
}

// Temporary colour textures (conversions of surfaces for sampling, feedback
// copies).
struct TempTex {
    ID3D11Texture2D *tex = nullptr;
    ID3D11RenderTargetView *rtv = nullptr;
    ID3D11ShaderResourceView *srv = nullptr;
    int w = 0, h = 0;
    // what it holds
    Surface *src = nullptr;
    uint32_t src_version = 0, psm = 0, texa = 0;
    uint64_t frame = 0;
};
TempTex g_temp[8];
int g_temp_next;

TempTex *temp_texture(int w, int h) {
    TempTex &t = g_temp[g_temp_next];
    g_temp_next = (g_temp_next + 1) % 8;
    if (t.w != w || t.h != h || !t.tex) {
        release(t.srv);
        release(t.rtv);
        release(t.tex);
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = (UINT)(w * g_scale);
        td.Height = (UINT)(h * g_scale);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(CHECK(g_dev->CreateTexture2D(&td, nullptr, &t.tex))))
            return nullptr;
        CHECK(g_dev->CreateRenderTargetView(t.tex, nullptr, &t.rtv));
        CHECK(g_dev->CreateShaderResourceView(t.tex, nullptr, &t.srv));
        t.w = w;
        t.h = h;
    }
    t.src = nullptr;
    return &t;
}

// The pages a w x h rectangle of a buffer really touches (block by block;
// page_range is conservative for buffers that don't start on a page).
void exact_pages(uint32_t bp, uint32_t bw, uint32_t psm, int w, int h, std::vector<uint32_t> &out) {
    out.clear();
    const SwizzleTab *T = swizzle_table(psm, bw);
    int bwid, bhgt, ub;
    block_dims(psm, bwid, bhgt, ub);
    uint32_t last = ~0u;
    for (int y = 0; y < h; y += bhgt)
        for (int x = 0; x < w; x += bwid) {
            uint32_t a = T->addr(bp, x, y);
            uint32_t byte_addr = ub == 32 ? a * 4 : ub == 16 ? a * 2 : ub == 8 ? a : a / 2;
            uint32_t page = (byte_addr / 8192) % kPages;
            if (page == last)
                continue;
            last = page;
            if (std::find(out.begin(), out.end(), page) == out.end())
                out.push_back(page);
        }
}

// texture source for the current env: SRV plus how to sample it
struct TexSource {
    alpha_order::Range alpha;
    ID3D11ShaderResourceView *srv = nullptr;
    float w = 1,
          h = 1;        // GS texels covered by the texture (for normalised coordinates)
    float scale = 1;    // texels of the SRV per GS texel
    uint32_t flags = 0; // F_TEX_RT24 / F_TEX_RT16
};

TexSource texture_for(const Env &e, Surface *target) {
    TexSource ts;
    int levels = tex_levels(e);
    // A page may still contain unrelated GPU-owned pixels while every block
    // this texture requests was completely supplied by the CPU. Decode that
    // current CPU data directly; acquiring a GPU texture view would discard
    // the validity proof and can force a later, unnecessary palette readback.
    // Keep the alternate texture-source selection within the opt-in experiment.
    bool cpu_current = gpu_uploads_enabled();
    for (int i = 0; i < levels && cpu_current; ++i) {
        const PageCoverage requested = page_coverage(e.mip_tbp[i], e.mip_tbw[i], e.tpsm, 0, 0,
            std::max(1, (int)e.tw >> i), std::max(1, (int)e.th >> i));
        for (uint32_t n = 0; n < requested.count; ++n) {
            uint32_t p = requested.pages[n];
            if (g_owner[p] && !g_vram_ok[p] && (requested.touched[p] & ~g_cpu_valid_blocks[p])) {
                cpu_current = false;
                break;
            }
        }
    }
    if (cpu_current) {
        ts.srv = vram_texture(e, ts.alpha, ts.scale, ts.flags);
        ts.w = (float)e.tw;
        ts.h = (float)e.th;
        return ts;
    }
    // a surface drawn at the texture's address (the usual render-to-texture case)
    Surface *owner = nullptr;
    bool mixed = false, gpu = false;
    {
        uint32_t p0 = (e.tbp >> 5) % kPages;
        Surface *o = g_owner[p0];
        if (o && !g_vram_ok[p0] && o->bp == e.tbp && o->bw == (e.tbw ? e.tbw : 1) && levels == 1) {
            owner = o;
            gpu = true;
            // A colour surface in the texture's own layout at this address: gather
            // the texture area into it (parts may belong to a depth or differently
            // formatted surface at the same address).
            Surface *c = find_surface(e.tbp, e.tbw ? e.tbw : 1, e.tpsm, false);
            if (c && c != owner && bpp_of(c->psm) == bpp_of(e.tpsm))
                owner = c;
            if (!owner->depth && same_layout(owner->psm, e.tpsm)) {
                // as far down as surfaces at this address have drawn
                int rows = 0;
                std::vector<PageRect> pr;
                surface_pages_rect(owner, 0, 0, std::min((int)e.tw, owner->w), std::min((int)e.th, owner->h), pr);
                for (const PageRect &q : pr) {
                    Surface *w = g_owner[q.page];
                    if (w && w->bp == e.tbp)
                        rows = std::max(rows, q.y + q.h);
                }
                acquire(owner, 0, 0, std::min((int)e.tw, owner->w), rows);
            }
        }
    }
    if (!owner) {
        std::vector<uint32_t> pages;
        for (int i = 0; i < levels; i++) {
            exact_pages(e.mip_tbp[i], e.mip_tbw[i], e.tpsm, std::max(1, (int)e.tw >> i), std::max(1, (int)e.th >> i),
                        pages);
            for (uint32_t p : pages) {
                Surface *o = g_owner[p];
                if (!o || g_vram_ok[p])
                    continue;
                gpu = true;
                if (owner && owner != o)
                    mixed = true;
                owner = o;
            }
        }
    }
    if (!gpu) {
        ts.srv = vram_texture(e, ts.alpha, ts.scale, ts.flags);
        ts.w = (float)e.tw;
        ts.h = (float)e.th;
        return ts;
    }
    bool tex16 = bpp_of(e.tpsm) == 16 && (e.tpsm & 0x30) != 0x10;
    bool tex32 = !tex16 && (e.tpsm == PSMCT32 || e.tpsm == PSMCT24 || e.tpsm == PSMZ32 || e.tpsm == PSMZ24);
    if (!mixed && levels == 1 && owner->bp == e.tbp && owner->bw == (e.tbw ? e.tbw : 1) && (tex16 || tex32) &&
        bpp_of(owner->psm) == bpp_of(e.tpsm)) {
        g_stats.tex_rt++;
        if (!owner->depth && same_layout(owner->psm, e.tpsm)) {
            // sample the surface itself (a copy if it is the current target)
            ts.srv = owner->srv;
            if (owner == target) {
                flush();
                TempTex *t = temp_texture(owner->w, owner->h);
                if (t) {
                    g_ctx->CopyResource(t->tex, owner->tex);
                    ts.srv = t->srv;
                }
            }
            if (e.tpsm == PSMCT24 || e.tpsm == PSMZ24)
                ts.flags = 0x2000;
            if (tex16)
                ts.flags = 0x4000;
        } else {
            // same memory, another layout or a depth buffer: convert to a texture
            // (cached per surface version)
            uint32_t texa = e.ta0 | (e.ta1 << 8) | ((uint32_t)e.aem << 16);
            TempTex *hit = nullptr;
            for (TempTex &t : g_temp)
                if (t.src == owner && t.src_version == owner->version && t.psm == e.tpsm && t.texa == texa &&
                    t.frame == g_frame_no)
                    hit = &t;
            if (!hit) {
                hit = temp_texture(owner->w, owner->h);
                if (!hit)
                    return ts;
                std::vector<PageRect> pages;
                surface_pages(owner, owner->h, pages);
                int rows = 0;
                for (const PageRect &p : pages)
                    if (g_owner[p.page] == owner)
                        rows = std::max(rows, p.y + p.h);
                convert_rect(owner, hit->rtv, nullptr, e.tpsm, e.tbw ? e.tbw : 1, owner->w, owner->h, 0, 0, owner->w,
                             rows, true, &e);
                hit->src = owner;
                hit->src_version = owner->version;
                hit->psm = e.tpsm;
                hit->texa = texa;
                hit->frame = g_frame_no;
            }
            ts.srv = hit->srv;
        }
        ts.w = (float)owner->w;
        ts.h = (float)owner->h;
        ts.scale = (float)g_scale;
        return ts;
    }
    // anything else: bring the rendered pages back to the CPU copy and decode
    g_rb_reason = 1;
    for (int i = 0; i < levels; i++)
        readback_pages(e.mip_tbp[i], e.mip_tbw[i], e.tpsm, 0, 0, std::max(1, (int)e.tw >> i),
                       std::max(1, (int)e.th >> i));
    g_rb_reason = 0;
    ts.srv = vram_texture(e, ts.alpha, ts.scale, ts.flags);
    ts.w = (float)e.tw;
    ts.h = (float)e.th;
    return ts;
}

// -------------------------------------------------------------- draw state
uint32_t g_reg_gen_seen; // g_env_serial the state was built for

void build_state(const Env &e, int topology) {
    DrawState s;
    memset(&s, 0, sizeof s); // (compared bytewise below)
    s.topology = topology;
    int rows = e.scy1 + 1;
    uint32_t full = (e.fpsm & 0xF) == 0x1 ? 0xFFFFFFu : 0xFFFFFFFFu;
    bool color_write = (e.fbmsk & full) != full;
    bool zwrite = !e.zmsk && e.ztst != 0;
    bool ztest = e.ztst >= 2;
    // targets: the colour buffer always (its size defines the viewport), depth
    // when used
    s.rt = get_surface(e.fbp, e.fbw, e.fpsm_mem, false, rows);
    if (ztest || zwrite)
        s.ds = get_surface(e.zbp, e.fbw, e.zpsm, true, rows);
    if (s.ds && s.rt && s.ds->h != s.rt->h) { // views must have the same size
        int h = std::max(s.ds->h, s.rt->h);
        s.rt = get_surface(e.fbp, e.fbw, e.fpsm_mem, false, h);
        s.ds = get_surface(e.zbp, e.fbw, e.zpsm, true, h);
    }
    if (!s.rt) {
        flush();
        g_state = s;
        return;
    }
    if (s.ds && s.ds->w != s.rt->w)
        s.ds = nullptr; // different widths: can't bind together
    // texture before taking the target pages (a texture may be converted from the
    // target's memory)
    TexSource tex;
    if (e.tme)
        tex = texture_for(e, s.rt);

    s.texture_alpha=tex.alpha; // Assign after DrawState memset, including unknown 0..255.
    DrawCB &cb = s.cb;
    cb.vp_scale[0] = 2.0f / (float)s.rt->w;
    cb.vp_scale[1] = -2.0f / (float)s.rt->h;
    cb.vp_offset[0] = -1.0f;
    cb.vp_offset[1] = 1.0f;
    double zr = depth_range(e.zpsm);
    cb.zscale = (float)(1.0 / zr);
    cb.zmax = (float)e.zmax;
    uint32_t f = 0;
    if (e.tme && tex.srv) {
        f |= 0x1;
        if (e.fst)
            f |= 0x2;
        if (e.tcc)
            f |= 0x4;
        int levels = tex_levels(e);
        if (levels > 1)
            f |= 0x40;
        if (e.lcm)
            f |= 0x80;
        if (e.bilinear_mag)
            f |= 0x100;
        if (e.bilinear_min)
            f |= 0x200;
        f |= tex.flags;
        f |= (uint32_t)e.tfx << 16;
        cb.lod_k = e.k;
        cb.lod_l = (float)(1 << e.l);
        cb.tex_size[0] = tex.w;
        cb.tex_size[1] = tex.h;
        cb.stq_size[0] = (float)e.tw; // STQ: s,t are fractions of tw/th
        cb.stq_size[1] = (float)e.th;
        cb.tex_scale = tex.scale;
        cb.wrap[0] = e.wms == 0 ? 0 : 1;
        cb.wrap[1] = e.wmt == 0 ? 0 : 1;
        cb.wrap[2] = levels - 1;
        s.tex = tex.srv;
        s.sampler_mag = (e.bilinear_mag ? 1 : 0) | (e.wms ? 2 : 0) | (e.wmt ? 4 : 0);
        s.sampler_min = (e.bilinear_min ? 1 : 0) | (e.wms ? 2 : 0) | (e.wmt ? 4 : 0);
    }
    if (e.fge)
        f |= 0x8;
    if (e.abe)
        f |= 0x10;
    if (e.fba)
        f |= 0x20;
    if (e.iip)
        f |= 0x1000;
    f |= (uint32_t)e.atst << 20;
    if (color_fmt(e.fpsm_mem) == FMT_16)
        f |= 0x1000000;
    cb.flags = f;
    cb.aref = e.aref;
    cb.fix = e.bfix;
    cb.fog[0] = e.fogr;
    cb.fog[1] = e.fogg;
    cb.fog[2] = e.fogb;
    cb.texa[0] = e.ta0;
    cb.texa[1] = e.ta1;
    cb.texa[2] = e.aem;
    cb.blend[0] = (uint32_t)e.ba;
    cb.blend[1] = (uint32_t)e.bb;
    cb.blend[2] = (uint32_t)e.bc;
    cb.blend[3] = (uint32_t)e.bd;

    // colour write mask: whole channels only
    uint32_t mask = 0;
    if (color_write) {
        if ((e.fbmsk & 0xFF) != 0xFF)
            mask |= 1;
        if ((e.fbmsk & 0xFF00) != 0xFF00)
            mask |= 2;
        if ((e.fbmsk & 0xFF0000) != 0xFF0000)
            mask |= 4;
        if ((e.fpsm & 0xF) != 0x1 && (e.fbmsk & 0xFF000000u) != 0xFF000000u)
            mask |= 8;
    }
    // blending: Cs * cs +/- Cd * cd with factors from the shader (dual source)
    uint32_t bkey;
    if (e.abe) {
        auto coef = [&](int which, int &k, int &m) {
            k = (e.ba == which ? 1 : 0) - (e.bb == which ? 1 : 0);
            m = e.bd == which ? 1 : 0;
        };
        int ks, ms, kd, md;
        coef(0, ks, ms);
        coef(1, kd, md);
        if (color_fmt(e.fpsm_mem) == FMT_16) {
            // Cd +/- Cs exactly (C = FIX = 0x80 or not involved): 16-bit truncation
            // emulated in the shader
            bool unit_c = e.bc == 2 && e.bfix == 0x80;
            int cs = ks == 0 ? ms : (unit_c ? ks + ms : 99), cd = kd == 0 ? md : (unit_c ? kd + md : 99);
            if (cd == 1 && cs == 1)
                s.cb.flags |= 0x2000000;
            if (cd == 1 && cs == -1)
                s.cb.flags |= 0x4000000;
        }
        bool neg_s = ks < 0 && ms == 0, neg_d = kd < 0 && md == 0;
        D3D11_BLEND_OP op = D3D11_BLEND_OP_ADD;
        D3D11_BLEND sf = D3D11_BLEND_SRC1_ALPHA, df = D3D11_BLEND_SRC1_COLOR;
        if (neg_s && !neg_d)
            op = D3D11_BLEND_OP_REV_SUBTRACT;
        else if (!neg_s && neg_d)
            op = D3D11_BLEND_OP_SUBTRACT;
        else if (neg_s && neg_d)
            sf = df = D3D11_BLEND_ZERO;
        if (e.bc == 1) { // C = destination alpha: fixed function factors
            sf = ks == 0
                     ? (ms ? D3D11_BLEND_ONE : D3D11_BLEND_ZERO)
                     : (ks > 0 ? D3D11_BLEND_DEST_ALPHA : (ms ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_DEST_ALPHA));
            df = kd == 0
                     ? (md ? D3D11_BLEND_ONE : D3D11_BLEND_ZERO)
                     : (kd > 0 ? D3D11_BLEND_DEST_ALPHA : (md ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_DEST_ALPHA));
        }
        bkey = blend_key(true, sf, df, op, mask);
    } else {
        bkey = blend_key(false, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, mask);
    }
    D3D11_COMPARISON_FUNC zf = e.ztst == 2   ? D3D11_COMPARISON_GREATER_EQUAL
                               : e.ztst == 3 ? D3D11_COMPARISON_GREATER
                                             : D3D11_COMPARISON_ALWAYS;
    uint32_t dkey = s.ds ? depth_key(true, zf, zwrite) : depth_key(false, D3D11_COMPARISON_ALWAYS, false);
    s.blend_key = bkey;
    s.depth_key = dkey;
    // alpha test: the passing pixels as usual; failing ones per AFAIL
    if (e.ate && e.atst != 1) {
        bool fb_only = e.afail == 1, zb_only = e.afail == 2, rgb_only = e.afail == 3;
        bool fail_writes = (fb_only && color_write) || (zb_only && zwrite && s.ds) || (rgb_only && (mask & 7));
        if (fb_only && !zwrite) {
            // failing pixels write the colour, passing ones too: no test at all
        } else if (!fail_writes) {
            s.cb.flags |= 0x400; // keep passing pixels only
        } else {
            s.two_pass = true;
            s.fb_only_alpha_failure=fb_only && !e.date;
            uint32_t fmask = fb_only ? mask : zb_only ? 0 : (mask & 7);
            s.fail_blend_key = (bkey & ~(15u << 14)) | (fmask << 14);
            s.fail_depth_key = zb_only ? dkey : (dkey & ~32u);
            if (e.atst == 0) { // NEVER: only the fail pass (skip the empty pass via
                               // the flag trick)
                s.blend_key = s.fail_blend_key;
                s.depth_key = s.fail_depth_key;
                s.two_pass = false;
            }
        }
    }
    int sc = g_scale;
    s.scissor[0] = std::max(0, e.scx0) * sc;
    s.scissor[1] = std::max(0, e.scy0) * sc;
    s.scissor[2] = std::min(e.scx1 + 1, s.rt->w) * sc;
    s.scissor[3] = std::min(e.scy1 + 1, s.rt->h) * sc;
    // same GPU state as the batch being built: keep adding to it
    if (!g_state_valid || memcmp(&s, &g_state, sizeof s) != 0) {
        flush();
        g_state = s;
    }
    g_state_valid = true;
}

void ensure_state(const Env &e, int topology) {
    if (!g_state_valid || g_env_serial != g_reg_gen_seen || g_state.topology != topology) {
        build_state(e, topology);
        g_reg_gen_seen = g_env_serial;
    }
    g_stats.prims++;
}

inline uint32_t pack_rgba(const Vertex &v) {
    return (uint32_t)v.r | ((uint32_t)v.g << 8) | ((uint32_t)v.b << 16) | ((uint32_t)v.a << 24);
}

inline HwVertex hv(const Env &e, const Vertex &v, uint32_t rgba) {
    HwVertex o;
    o.x = v.x / 16.0f;
    o.y = v.y / 16.0f;
    o.z = v.z;
    o.rgba = rgba;
    if (e.fst) {
        o.s = v.u / 16.0f;
        o.t = v.v / 16.0f;
        o.q = 1.0f;
    } else {
        o.s = v.s;
        o.t = v.t;
        o.q = v.q;
    }
    o.fog = v.f / 255.0f;
    return o;
}

// Upscaled sprites cover whole native pixels at the scissor edges
// (gs_sprite_edges.h). false: no sliver is needed, draw the plain quad.
bool push_sprite_slivers(const Env &e, const HwVertex &v0, const HwVertex &v1, const Vertex &a, const Vertex &b) {
    SpriteSegment xs[3], ys[3];
    const int nx = sprite_axis_segments(a.x, b.x, v0.s, v1.s, std::max(0, e.scx0),
                                        std::min(e.scx1, g_state.rt->w - 1), g_scale, xs);
    const int ny = sprite_axis_segments(a.y, b.y, v0.t, v1.t, std::max(0, e.scy0),
                                        std::min(e.scy1, g_state.rt->h - 1), g_scale, ys);
    if (nx == 1 && ny == 1) return false;
    for (int j = 0; j < ny; j++)
        for (int i = 0; i < nx; i++) {
            HwVertex c[4];  // top left, top right, bottom left, bottom right
            for (int k = 0; k < 4; k++) {
                c[k] = v0;
                c[k].x = (k & 1) ? xs[i].p1 : xs[i].p0;
                c[k].s = (k & 1) ? xs[i].c1 : xs[i].c0;
                c[k].y = (k & 2) ? ys[j].p1 : ys[j].p0;
                c[k].t = (k & 2) ? ys[j].c1 : ys[j].c0;
            }
            for (int k : {0, 1, 2, 1, 3, 2}) g_verts.push_back(c[k]);
        }
    return true;
}

} // namespace

// ================================================================ interface
bool start(bool with_window) {
    if (g_on)
        return true;
    try {
        g_dev = g_ctx = new vkgs::Context;
        vkgs::ctx = g_dev;
        if (!g_dev->init(with_window))
            return false;
        // Auto (RenderScale=0): the scale nearest the display's pixel height
        // (e.g. 1800-pixel Retina 1440x900: 4, 2560x1792), not the next one up,
        // which would render ~1.6x the pixels only to downsample them.
        int automatic_scale = with_window ? ((int)g_dev->extent.height + 224) / 448 : 1;
        g_scale = std::clamp(g_config.render_scale > 0 ? g_config.render_scale : automatic_scale, 1, 6);
        if (!create_shaders() || !create_raw_textures())
            return false;
        host_textures_init();
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = kVBBytes;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        CHECK(g_dev->CreateBuffer(&bd, nullptr, &g_vb));
        bd.ByteWidth = (sizeof(DrawCB) + 15) & ~15u;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        CHECK(g_dev->CreateBuffer(&bd, nullptr, &g_cb_draw));
        bd.ByteWidth = (sizeof(PassCB) + 15) & ~15u;
        CHECK(g_dev->CreateBuffer(&bd, nullptr, &g_cb_pass));
        D3D11_SAMPLER_DESC smp = {};
        smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        smp.MaxLOD = D3D11_FLOAT32_MAX;
        CHECK(g_dev->CreateSamplerState(&smp, &g_point_clamp));
        smp.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        CHECK(g_dev->CreateSamplerState(&smp, &g_linear_clamp));
        ID3D11SamplerState *defaults[2] = {g_point_clamp, g_linear_clamp};
        g_ctx->PSSetSamplers(0, 2, defaults);
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = FALSE;
        rd.ScissorEnable = TRUE;
        CHECK(g_dev->CreateRasterizerState(&rd, &g_rs_scissor));
        rd.ScissorEnable = FALSE;
        CHECK(g_dev->CreateRasterizerState(&rd, &g_rs_noscissor));
        D3D11_DEPTH_STENCIL_DESC dd = {};
        dd.DepthEnable = FALSE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        CHECK(g_dev->CreateDepthStencilState(&dd, &g_ds_off));
        dd.DepthEnable = TRUE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        CHECK(g_dev->CreateDepthStencilState(&dd, &g_ds_write_always));
        D3D11_BLEND_DESC bl = {};
        bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        CHECK(g_dev->CreateBlendState(&bl, &g_bs_opaque));
        bl.RenderTarget[0].RenderTargetWriteMask = 0;
        CHECK(g_dev->CreateBlendState(&bl, &g_bs_none));
        bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        bl.RenderTarget[0].BlendEnable = TRUE;
        bl.RenderTarget[0].SrcBlend = D3D11_BLEND_BLEND_FACTOR;
        bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_BLEND_FACTOR;
        bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        CHECK(g_dev->CreateBlendState(&bl, &g_bs_alpha));
        memset(g_owner, 0, sizeof g_owner);
        memset(g_vram_ok, 0, sizeof g_vram_ok);
        g_on = true;
        overlay_enable(); // the in-game front end can be drawn (window and offscreen captures)
        LOG(LOG_GS, "vulkan: render scale %d%s", g_scale, with_window ? (g_dev->wantVsync ? " launch VSync on (FIFO)" : " launch VSync off (immediate/mailbox when available)") : " offscreen");
        return true;
    } catch (const std::exception &e) {
        WARN("vulkan: %s", e.what());
        // SDL can now recreate the window for software rendering. Detach its
        // Vulkan surface first, including failures partway through initialization.
        if (g_dev && g_dev->device)
            vkDeviceWaitIdle(g_dev->device);
        if (g_dev && g_dev->swap)
            vkDestroySwapchainKHR(g_dev->device, g_dev->swap, nullptr);
        if (g_dev && g_dev->surface) {
            vkDestroySurfaceKHR(g_dev->instance, g_dev->surface, nullptr);
            g_dev->surface = VK_NULL_HANDLE;
        }
        g_on = false;
        return false;
    }
}

void draw_triangle(const Env &e, const Vertex &a, const Vertex &b, const Vertex &c) {
    ensure_state(e, 0);
    if (!g_state.rt)
        return;
    if (e.iip) {
        g_verts.push_back(hv(e, a, pack_rgba(a)));
        g_verts.push_back(hv(e, b, pack_rgba(b)));
        g_verts.push_back(hv(e, c, pack_rgba(c)));
    } else { // flat: the last vertex's colour
        uint32_t col = pack_rgba(c);
        g_verts.push_back(hv(e, a, col));
        g_verts.push_back(hv(e, b, col));
        g_verts.push_back(hv(e, c, col));
    }
}

void draw_sprite(const Env &e, const Vertex &a, const Vertex &b) {
    ensure_state(e, 0);
    if (!g_state.rt)
        return;
    uint32_t col = pack_rgba(b);
    HwVertex v0 = hv(e, a, col), v1 = hv(e, b, col);
    v0.z = v1.z; // sprites use the second vertex's depth, fog and (STQ) Q
    v0.fog = v1.fog;
    if (!e.fst)
        v0.q = v1.q;
    if (push_sprite_slivers(e, v0, v1, a, b))
        return;
    HwVertex v2 = v0, v3 = v0;
    v2.x = v1.x; // top right
    v2.s = v1.s;
    v3.y = v1.y; // bottom left
    v3.t = v1.t;
    g_verts.push_back(v0);
    g_verts.push_back(v2);
    g_verts.push_back(v3);
    g_verts.push_back(v2);
    g_verts.push_back(v1);
    g_verts.push_back(v3);
}

void draw_line(const Env &e, const Vertex &a, const Vertex &b) {
    ensure_state(e, 1);
    if (!g_state.rt)
        return;
    uint32_t cb = pack_rgba(b);
    g_verts.push_back(hv(e, a, e.iip ? pack_rgba(a) : cb));
    g_verts.push_back(hv(e, b, cb));
}

void draw_point(const Env &e, const Vertex &a) {
    ensure_state(e, 2);
    if (!g_state.rt)
        return;
    g_verts.push_back(hv(e, a, pack_rgba(a)));
}

void vram_written(uint32_t bp, uint32_t bw, uint32_t psm, int x, int y, int w, int h) {
    if (!g_on)
        return;
    // All prior GPU primitives precede these actual CPU bytes. Do not invalidate
    // ownership while a queued batch could later redraw older pixels over them.
    flush();
    const PageCoverage written = page_coverage(bp, bw, psm, x, y, w, h);
    for (uint32_t i = 0; i < written.count; ++i) {
        uint32_t p = written.pages[i];
        // A complete physical page can be discarded only when every storage bit
        // is overwritten by the actual buffered chunk. Masked/partial writes
        // preserve its GPU contents before the CPU modifies them.
        if (g_owner[p] && !g_vram_ok[p] && written.complete[p] != 0xffffffffu) {
            // Only storage blocks proven supplied by this chunk can be patched
            // without a host read. Partial/masked blocks keep the fallback.
            std::vector<UploadBlockRect> patches;
            Surface *owner = g_owner[p];
            if (gpu_uploads_enabled() && written.complete[p] == written.touched[p] &&
                gpu_upload_blocks(owner->bp, owner->bw, owner->psm, owner->w, owner->h,
                                  p, written.complete[p], patches)) {
                for (const auto &rect : patches) g_pending_uploads.push_back({owner, p, written.complete[p], rect});
                g_vram_ok[p] = false;
                continue;
            }
            if (readback_trace_enabled() && readback_trace_line())
                fprintf(stderr, "[gs-readback] cpu-write-preserve bp=%x bw=%u psm=%x rect=%d,%d,%d,%d page=%u owner=%x/%u/%x depth=%d\n",
                        bp,bw,psm,x,y,w,h,p,g_owner[p]->bp,g_owner[p]->bw,g_owner[p]->psm,int(g_owner[p]->depth));
            g_rb_reason = 0;
            if (deferred_preserve_enabled())
                deferred_schedule(p);
            else
                readback_pages(p * 32, 1, PSMCT32, 0, 0, 64, 32);
        }
        if (readback_trace_enabled()) {
            if(g_owner[p] && !g_vram_ok[p] && readback_trace_line())
                fprintf(stderr,"[gs-readback] frame=%llu cpu-write-drops-owner request_bp=%x psm=%x rect=%d,%d,%d,%d page=%u owner_bp=%x owner_psm=%x drawn_blocks=%08x\n",
                    (unsigned long long)g_frame_no,bp,psm,x,y,w,h,p,g_owner[p]->bp,g_owner[p]->psm,g_diag_draw_blocks[p]);
            g_diag_draw_blocks[p]=0;
        }
        // Remaining old contents are now preserved or provably overwritten.
        g_owner[p] = nullptr;
        g_vram_ok[p] = false;
        g_cpu_valid_blocks[p] = 0;
    }
    if (g_deferred_count)
        deferred_cpu_written(written, bp, bw, psm, x, y, w, h);
    g_state_valid = false;
}

void vram_write_complete() {
    if (!g_on || g_pending_uploads.empty()) return;
    // A later region in this same chunk may have invalidated an earlier owner.
    // Patch only owners still retained. All actual CPU bytes are now present.
    std::vector<PendingUpload> pending;
    pending.swap(g_pending_uploads);
    std::vector<Surface *> owners;
    for (const auto &p : pending)
        if (g_owner[p.page] == p.owner && std::find(owners.begin(), owners.end(), p.owner) == owners.end())
            owners.push_back(p.owner);
    for (Surface *owner : owners) {
        std::vector<PageRect> rects;
        for (const auto &p : pending)
            if (p.owner == owner && g_owner[p.page] == owner)
                rects.push_back({p.page, p.rect.x, p.rect.y, p.rect.w, p.rect.h});
        std::sort(rects.begin(), rects.end(), [](const PageRect &a, const PageRect &b) {
            if (a.y != b.y) return a.y < b.y;
            if (a.x != b.x) return a.x < b.x;
            if (a.w != b.w) return a.w < b.w;
            return a.h < b.h;
        });
        rects.erase(std::unique(rects.begin(), rects.end(), [](const PageRect &a, const PageRect &b) {
            return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
        }), rects.end());
        for_rects(rects, [&](int x, int y, int w, int h) { upload_rect(owner, x, y, w, h); });
        ++owner->version; // rendered-texture conversions must observe the patch
        for (const auto &p : pending)
            if (p.owner == owner && g_owner[p.page] == owner) g_cpu_valid_blocks[p.page] |= p.valid_blocks;
    }
    g_state_valid = false;
}

void readback_pages(uint32_t bp, uint32_t bw, uint32_t psm, int x, int y, int w, int h) {
    if (!g_on)
        return;
    const PageCoverage requested = page_coverage(bp, bw, psm, x, y, w, h);
    for (uint32_t i = 0; i < requested.count && g_deferred_count; ++i) {
        uint32_t p = requested.pages[i];
        if (g_deferred[p] && (requested.touched[p] & g_deferred[p]->pending_blocks))
            deferred_resolve(p, "cpu read");
    }
    const bool trace = readback_trace_enabled();
    // page_range is deliberately conservative, adding an extra page for any
    // unaligned BP. Only actual physical GS blocks can require synchronization.
    // This does not consult the diagnostic drawn masks or skip any needed data.
    for (uint32_t i = 0; i < requested.count; ++i) {
        uint32_t p = requested.pages[i];
        Surface *o = g_owner[p];
        if (!o || g_vram_ok[p] || !(requested.touched[p] & ~g_cpu_valid_blocks[p]))
            continue;
        if (trace && readback_trace_line())
            fprintf(stderr,"[gs-readback] frame=%llu cpu-read reason=%d request_bp=%x psm=%x rect=%d,%d,%d,%d page=%u owner_bp=%x owner_psm=%x requested_blocks=%08x drawn_blocks=%08x overlap=%08x\n",
                (unsigned long long)g_frame_no,g_rb_reason,bp,psm,x,y,w,h,p,o->bp,o->psm,
                requested.touched[p],g_diag_draw_blocks[p],requested.touched[p]&g_diag_draw_blocks[p]);
        std::vector<PageRect> pages;
        surface_pages(o, o->h, pages);
        for (const PageRect &q : pages)
            if (q.page == p) {
                readback_rect(o, q.x, q.y, q.w, q.h);
                g_vram_ok[p] = true;
                g_cpu_valid_blocks[p] = 0xffffffffu;
                if (trace) g_diag_draw_blocks[p] = 0;
            }
        if (!g_vram_ok[p]) { // not part of the surface's rows (shouldn't happen):
                             // give up the page
            g_owner[p] = nullptr;
        }
    }
}

void vram_needed(uint32_t bp, uint32_t bw, uint32_t psm, int x, int y, int w, int h, const char *) {
    flush(); // CPU reads observe all earlier queued GS primitives.
    g_rb_reason = 0;
    readback_pages(bp, bw, psm, x, y, w, h);
}

// ----------------------------------------------------------------- display
namespace {

struct Circuit {
    bool on;
    uint32_t fbp, fbw, psm;
    int x, y, w, h, lines;
};

Circuit read_circuit_regs(int n) {
    Circuit c = {};
    if (!((st.pmode >> n) & 1))
        return c;
    uint64_t fb = st.dispfb[n], disp = st.display[n];
    c.fbp = bits(fb, 0, 9) * 32;
    c.fbw = bits(fb, 9, 6);
    c.psm = bits(fb, 15, 5);
    c.x = (int)bits(fb, 32, 11);
    c.y = (int)bits(fb, 43, 11);
    int magh = (int)bits(disp, 23, 4) + 1, magv = (int)bits(disp, 27, 2) + 1;
    c.w = ((int)bits(disp, 32, 12) + 1) / magh;
    c.h = ((int)bits(disp, 44, 11) + 1) / magv;
    if (c.w <= 0 || c.h <= 0 || c.w > 2048 || c.h > 2048)
        return c;
    bool field_mode = (st.smode2 & 1) && ((st.smode2 >> 1) & 1);
    c.lines = field_mode ? c.h / 2 : c.h;
    c.on = true;
    return c;
}

ID3D11Texture2D *g_disp_tex[2]; // CPU-side display images (movies), per circuit
ID3D11ShaderResourceView *g_disp_srv[2];
int g_disp_w, g_disp_h;
// the last composition (what read_display shows)
Circuit g_last[2];
bool g_have_last;
int g_last_aspect = -1;

// source of a display circuit: a surface or the CPU copy (uploaded)
ID3D11ShaderResourceView *circuit_source(int n, const Circuit &c, float &x0, float &y0, float &x1, float &y1, float &sw,
                                         float &sh) {
    Surface *s = find_surface(c.fbp, c.fbw ? c.fbw : 1, c.psm, false);
    uint32_t page = (c.fbp >> 5) % kPages;
    static const char *show = getenv("EOD_D3D_SHOW"); // debug: display this buffer (hex block address) instead
    if (show) {
        Surface *d = find_surface((uint32_t)strtoul(show, nullptr, 16), c.fbw ? c.fbw : 1, c.psm, false);
        if (d)
            s = d, page = (d->bp >> 5) % kPages;
    }
    { // log where the display comes from when that changes (not the buffer flips)
        static uint64_t last[2];
        uint64_t key = ((uint64_t)c.fbw << 40) ^ ((uint64_t)c.psm << 32) ^ ((uint64_t)c.w << 20) ^
                       ((uint64_t)c.lines << 8) ^ (s && g_owner[page] == s ? 1 : 0);
        if (key != last[n]) {
            last[n] = key;
            LOG(LOG_GS, "vulkan: display %d shows %x/%d/%x %dx%d at %d,%d from %s", n, c.fbp, c.fbw, c.psm, c.w,
                c.lines, c.x, c.y, s && g_owner[page] == s ? "a render target" : "GS memory");
        }
    }
    if (s && g_owner[page] == s) {
        float sc = (float)g_scale;
        x0 = c.x * sc;
        y0 = c.y * sc;
        x1 = (c.x + c.w) * sc;
        y1 = (c.y + c.lines) * sc;
        sw = s->w * sc;
        sh = s->h * sc;
        return s->srv;
    }
    // rendered by another surface (other format) or written by the CPU: read
    // gs::vram
    g_rb_reason = 3;
    readback_pages(c.fbp, c.fbw ? c.fbw : 1, c.psm, c.x, c.y, c.w, c.lines);
    if (!g_disp_tex[n]) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = 2048;
        td.Height = 2048;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(CHECK(g_dev->CreateTexture2D(&td, nullptr, &g_disp_tex[n]))))
            return nullptr;
        CHECK(g_dev->CreateShaderResourceView(g_disp_tex[n], nullptr, &g_disp_srv[n]));
        g_disp_w = g_disp_h = 2048;
    }
    std::vector<uint32_t> img((size_t)c.w * c.lines);
    const SwizzleTab *T = swizzle_table(c.psm, c.fbw);
    bool b16 = bpp_of(c.psm) == 16;
    for (int y = 0; y < c.lines; y++) {
        int py = (c.y + y) & 2047;
        uint32_t rb = T->row_base(c.fbp, py), rc = T->rowC[py];
        uint32_t *row = &img[(size_t)y * c.w];
        for (int x = 0; x < c.w; x++) {
            uint32_t a = T->addr_row(rb, rc, (c.x + x) & 2047);
            if (b16) {
                uint32_t v = half(a);
                row[x] = ((v & 0x1F) << 3) | (((v >> 5) & 0x1F) << 11) | (((v >> 10) & 0x1F) << 19) | 0xFF000000u;
            } else {
                row[x] = word(a) | 0xFF000000u;
            }
        }
    }
    D3D11_BOX box = {0, 0, 0, (UINT)c.w, (UINT)c.lines, 1};
    g_ctx->UpdateSubresource(g_disp_tex[n], 0, &box, img.data(), (UINT)c.w * 4, 0);
    x0 = 0;
    y0 = 0;
    x1 = (float)c.w;
    y1 = (float)c.lines;
    sw = (float)g_disp_w;
    sh = (float)g_disp_h;
    return g_disp_srv[n];
}

// draws the display into rtv (size tw x th) at rectangle (rx, ry, rw, rh)
void compose(ID3D11RenderTargetView *rtv, int tw, int th, int rx, int ry, int rw, int rh, bool smooth) {
    Circuit c[2] = {g_last[0], g_last[1]};
    // circuit 1 over circuit 2 with PMODE.ALP (MMOD = 1) or opaque (as the
    // software compositor)
    bool alpha_blend = c[0].on && c[1].on && ((st.pmode >> 5) & 1);
    float alp = alpha_blend ? bits(st.pmode, 8, 8) / 255.0f : 1.0f;
    if (c[0].on && c[1].on && !alpha_blend)
        c[1].on = false;
    // sources first (reading the CPU copy may run passes of its own)
    ID3D11ShaderResourceView *srv[2] = {};
    float r[2][6] = {};
    for (int n = 0; n < 2; n++)
        if (c[n].on)
            srv[n] = circuit_source(n, c[n], r[n][0], r[n][1], r[n][2], r[n][3], r[n][4], r[n][5]);
    unbind_all();
    g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    set_viewport(tw, th);
    g_ctx->OMSetDepthStencilState(g_ds_off, 0);
    for (int n = 1; n >= 0; n--) { // circuit 2 first, circuit 1 over it
        if (!srv[n])
            continue;
        g_ctx->PSSetShaderResources(0, 1, &srv[n]);
        ID3D11SamplerState *ss[2] = {g_point_clamp, g_linear_clamp};
        g_ctx->PSSetSamplers(0, 2, ss);
        PassCB cb = {};
        cb.misc[0] = r[n][4];
        cb.misc[1] = r[n][5];
        cb.fmt[2] = smooth ? 0 : 4;
        cb.color[3] = 1.0f;
        float factor[4] = {alp, alp, alp, alp};
        if (n == 0 && alpha_blend)
            g_ctx->OMSetBlendState(g_bs_alpha, factor, 0xFFFFFFFF);
        else
            g_ctx->OMSetBlendState(g_bs_opaque, nullptr, 0xFFFFFFFF);
        run_pass(g_ps_copy, tw, th, (float)rx, (float)ry, (float)(rx + rw), (float)(ry + rh), r[n][0], r[n][1], r[n][2],
                 r[n][3], cb);
    }
    unbind_all();
    g_bound_rt = g_bound_ds = nullptr;
}

// ------------------------------------------------------- front-end overlay
// The in-game front end (runtime/ui): ImGui draw lists over the composed image,
// at the target's resolution, with their own shaders, blend state and textures
// (font atlas + decoded game UI art). Indexed lists are expanded to the GS
// vertex layout so the adapter's vertex path serves (ImGui lists are small).
ID3D11VertexShader *g_vs_overlay;
ID3D11PixelShader *g_ps_overlay;
ID3D11BlendState *g_bs_overlay;
ID3D11Buffer *g_cb_overlay, *g_vb_overlay;
std::vector<ID3D11Texture2D *> g_overlay_tex;
std::vector<uint32_t> g_overlay_gen;

bool overlay_pipeline() {
    if (g_vs_overlay) return true;
    auto shader = [](const uint32_t *code, size_t bytes, const char *entry, int id) {
        auto *sh = new vkgs::Shader;
        sh->entry = entry;
        sh->id = id;
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = bytes;
        info.pCode = code;
        vkgs::require(vkCreateShaderModule(g_dev->device, &info, nullptr, &sh->module), entry);
        return sh;
    };
    g_vs_overlay = shader(k_spv_vs_overlay, sizeof k_spv_vs_overlay, "vs_overlay", vkgs::kOverlayVertexShader);
    g_ps_overlay = shader(k_spv_ps_overlay, sizeof k_spv_ps_overlay, "ps_overlay", vkgs::kOverlayVertexShader + 1);
    D3D11_BLEND_DESC bl = {};
    D3D11_RENDER_TARGET_BLEND_DESC &rt = bl.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = VK_BLEND_FACTOR_SRC_ALPHA;
    rt.DestBlend = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    CHECK(g_dev->CreateBlendState(&bl, &g_bs_overlay));
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 16;
    CHECK(g_dev->CreateBuffer(&bd, nullptr, &g_cb_overlay));
    bd.ByteWidth = 32;
    CHECK(g_dev->CreateBuffer(&bd, nullptr, &g_vb_overlay));
    return true;
}

ID3D11ShaderResourceView *overlay_srv(uint32_t id) {
    OverlayTexture t;
    if (!overlay_texture(id, t) || !t.rgba || t.w <= 0 || t.h <= 0) return nullptr;
    if (id >= g_overlay_tex.size()) {
        g_overlay_tex.resize(id + 1, nullptr);
        g_overlay_gen.resize(id + 1, 0);
    }
    if (!g_overlay_tex[id] || g_overlay_gen[id] != t.generation) {
        release(g_overlay_tex[id]);
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = (UINT)t.w;
        td.Height = (UINT)t.h;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init = {t.rgba, (UINT)t.w * 4, 0};
        if (FAILED(CHECK(g_dev->CreateTexture2D(&td, &init, &g_overlay_tex[id])))) return nullptr;
        g_overlay_gen[id] = t.generation;
    }
    return g_overlay_tex[id];
}

// Draws the published overlay into rtv (tw x th pixels at its origin).
void draw_overlay(ID3D11RenderTargetView *rtv, int tw, int th) {
    static OverlayDraw d;
    if (!overlay_latest(d) || d.w <= 0 || d.h <= 0 || d.cmds.empty()) return;
    try {
        overlay_pipeline();
    } catch (const std::exception &e) {
        WARN("vulkan overlay: %s", e.what());
        return;
    }
    // expand the indexed lists (all commands, in order)
    size_t total = 0;
    for (const OverlayCmd &c : d.cmds) total += c.index_count;
    g_vb_overlay->data.resize(std::max<size_t>(32, total * sizeof(HwVertex)));
    HwVertex *out = reinterpret_cast<HwVertex *>(g_vb_overlay->data.data());
    size_t n = 0;
    for (const OverlayCmd &c : d.cmds)
        for (uint32_t i = 0; i < c.index_count; i++) {
            const size_t vi = (size_t)c.base_vertex + d.indices[c.first_index + i];
            const OverlayVertex &v = vi < d.verts.size() ? d.verts[vi] : d.verts[0];
            out[n++] = {v.x, v.y, 0, v.rgba, v.u, v.v, 1.0f, 0.0f};
        }
    const float scale[4] = {2.0f / (float)d.w, 2.0f / (float)d.h, 0, 0};
    memcpy(g_cb_overlay->data.data(), scale, sizeof scale);
    unbind_all();
    g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    set_viewport(tw, th);
    UINT stride = sizeof(HwVertex), offset = 0;
    g_ctx->IASetVertexBuffers(0, 1, &g_vb_overlay, &stride, &offset);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_vs_overlay, nullptr, 0);
    g_ctx->VSSetConstantBuffers(1, 1, &g_cb_overlay);
    g_ctx->PSSetShader(g_ps_overlay, nullptr, 0);
    ID3D11SamplerState *ss[2] = {g_point_clamp, g_linear_clamp};
    g_ctx->PSSetSamplers(0, 2, ss);
    float factor[4] = {0, 0, 0, 0};
    g_ctx->OMSetBlendState(g_bs_overlay, factor, 0xFFFFFFFF);
    g_ctx->OMSetDepthStencilState(g_ds_off, 0);
    g_ctx->RSSetState(g_rs_scissor);
    const float sx = (float)tw / (float)d.w, sy = (float)th / (float)d.h;
    UINT first = 0;
    for (const OverlayCmd &c : d.cmds) {
        ID3D11ShaderResourceView *srv = overlay_srv(c.texture);
        if (srv) {
            D3D11_RECT r = {(int)(c.clip[0] * sx), (int)(c.clip[1] * sy), (int)(c.clip[2] * sx + 0.5f), (int)(c.clip[3] * sy + 0.5f)};
            g_ctx->RSSetScissorRects(1, &r);
            g_ctx->PSSetShaderResources(0, 1, &srv);
            g_ctx->Draw(c.index_count, first);
        }
        first += c.index_count;
    }
    unbind_all();
    g_bound_rt = g_bound_ds = nullptr;
    g_state_valid = false;
}

std::mutex g_shot_mtx;
std::condition_variable g_shot_cv;
bool g_shot_wanted, g_shot_done;
std::vector<uint32_t> g_shot;
int g_shot_w, g_shot_h;

// Reads back the native GS image, or an explicitly sized display composition.
bool capture(std::vector<uint32_t> &rgba, int &w, int &h) {
    // No game image yet (the first-run / error screen runs before the game): the front end
    // alone, at the explicit screenshot size.
    const Circuit none{};
    const bool image = g_have_last && (g_last[0].on || g_last[1].on);
    const Circuit &c = !image ? none : g_last[0].on ? g_last[0] : g_last[1];
    if (!image && !(overlay_in_screenshots() && getenv("EOD_SCREENSHOT_WIDTH") && getenv("EOD_SCREENSHOT_HEIGHT")))
        return false;
    int shot_scale = 1;
    if (const char *scale = getenv("EOD_SCREENSHOT_SCALE"))
        shot_scale = std::clamp(atoi(scale), 1, g_scale);
    const ScreenshotSize size=screenshot_size(image ? c.w : 640,image ? c.h : 448,shot_scale,getenv("EOD_SCREENSHOT_WIDTH"),getenv("EOD_SCREENSHOT_HEIGHT"));
    if (!size.valid) {
        WARN("Vulkan screenshot: set both EOD_SCREENSHOT_WIDTH/HEIGHT to pixel dimensions from 1 to 8192");
        return false;
    }
    w=size.width;h=size.height;
    flush();
    // TempTex dimensions are multiplied by internal scale. Round up only for
    // an explicit output size so the copy region fits without upscaling the
    // requested output dimensions a second time.
    TempTex *t = size.composed ? temp_texture((w+g_scale-1)/g_scale,(h+g_scale-1)/g_scale) : temp_texture(c.w,c.h);
    if (!t) return false;
    int x=0,y=0,draw_w=w,draw_h=h;
    if(size.composed) {
        const float black[4]={0,0,0,1};
        g_ctx->ClearRenderTargetView(t->rtv,black);
        const int queued_aspect=presentation::frame_aspect;
        presentation::set(g_last_aspect); // capture the last composed frame's policy
        platform_display_rect(w,h,&x,&y,&draw_w,&draw_h);
        presentation::set(queued_aspect);
    }
    if (image)
        compose(t->rtv,t->w*g_scale,t->h*g_scale,x,y,draw_w,draw_h,size.composed ? g_config.smooth_display : shot_scale>1);
    if (overlay_in_screenshots())
        draw_overlay(t->rtv, w, h); // the front end appears in screenshots as on screen
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *stg = nullptr;
    if (FAILED(CHECK(g_dev->CreateTexture2D(&td, nullptr, &stg))))
        return false;
    D3D11_BOX box = {0, 0, 0, (UINT)w, (UINT)h, 1};
    g_ctx->CopySubresourceRegion(stg, 0, 0, 0, 0, t->tex, 0, &box);
    D3D11_MAPPED_SUBRESOURCE m;
    bool ok = SUCCEEDED(CHECK(g_ctx->Map(stg, 0, D3D11_MAP_READ, 0, &m)));
    LOG(LOG_GS, "vulkan: capture %dx%d circuits %d/%d map %d pmode %llx", w, h, (int)g_last[0].on, (int)g_last[1].on,
        (int)ok, (unsigned long long)st.pmode);
    for (int n = 0; n < 2; n++) {
        const Circuit &k = g_last[n];
        Surface *sf = find_surface(k.fbp, k.fbw ? k.fbw : 1, k.psm, false);
        LOG(LOG_GS,
            "vulkan:   circuit %d on %d fb %x/%d/%x %dx%d at %d,%d surface %p "
            "owner %p",
            n, (int)k.on, k.fbp, k.fbw, k.psm, k.w, k.lines, k.x, k.y, (void *)sf,
            (void *)g_owner[(k.fbp >> 5) % kPages]);
    }
    if (ok) {
        rgba.resize((size_t)w * h);
        for (int y = 0; y < h; y++)
            memcpy(&rgba[(size_t)y * w], (const uint8_t *)m.pData + (size_t)y * m.RowPitch, (size_t)w * 4);
        g_ctx->Unmap(stg, 0);
        LOG(LOG_GS, "vulkan: capture pixel %08x %08x", rgba[(size_t)(h / 2) * w + w / 2], rgba[0]);
    }
    stg->Release();
    return ok;
}

} // namespace

void bind_thread() {
    g_gs_thread = std::this_thread::get_id();
    g_gs_thread_known = true;
}

void vblank() {
    if (!g_on)
        return;
    std::unique_lock<std::mutex> lk(g_shot_mtx);
    if (g_shot_wanted) {
        g_shot_wanted = false;
        g_shot_done = capture(g_shot, g_shot_w, g_shot_h);
        g_shot_cv.notify_all();
    }
}

namespace { void show(bool vsync, bool fresh_scanout = false); }
void present(bool vsync) {
    if (!g_on)
        return;
    flush();
    g_last_aspect = presentation::frame_aspect;
    g_last[0] = read_circuit_regs(0);
    g_last[1] = read_circuit_regs(1);
    g_have_last = g_last[0].on || g_last[1].on;
    g_frame_no++;
    g_state_valid = false;
    show(vsync, true);
}

// Presents the last composed image again (upstream's in-game menu pauses the game).
void redraw() {
    if (!g_on)
        return;
    flush();
    g_state_valid = false;
    show(true);
}

namespace {
void show(bool vsync, bool fresh_scanout) {
    if (g_dev->surface) {
        if (auto *back = g_dev->acquire(vsync)) {
            float black[4] = {0, 0, 0, 1};
            g_ctx->ClearRenderTargetView(back, black);
            int x, y, w, h;
            platform_display_rect(g_dev->extent.width, g_dev->extent.height, &x, &y, &w, &h);
            if (g_have_last) {
                compose(back, g_dev->extent.width, g_dev->extent.height, x, y, w, h, g_config.smooth_display);
                if (fresh_scanout && editor_test_startup::state().hardware_composed())
                    std::fprintf(stderr, "[editor-test] Vulkan race scanout composed: frame=%llu size=%dx%d pmode=%llx dispfb=%llx/%llx\n",
                        (unsigned long long)g_frame_no, w, h, (unsigned long long)st.pmode,
                        (unsigned long long)st.dispfb[0], (unsigned long long)st.dispfb[1]);
            }
            draw_overlay(back, g_dev->extent.width, g_dev->extent.height);
            if (g_dev->present() && g_have_last && editor_test_startup::state().hardware_presented())
                std::fprintf(stderr, "[editor-test] Vulkan race scanout present accepted: frame=%llu\n",
                    (unsigned long long)g_frame_no);
            g_stats.presents++;
        }
    } else {
        g_dev->submit(false);
        // No window exists headlessly. Freeze valid fresh circuit registers for
        // the existing screenshot compositor; never acknowledge presentation.
        if (fresh_scanout && g_have_last) editor_test_startup::state().hardware_composed();
    }
}
}  // namespace

bool read_display(std::vector<uint32_t> &rgba, int &w, int &h) {
    if (!g_on)
        return false;
    if (!g_gs_thread_known || std::this_thread::get_id() == g_gs_thread)
        return capture(rgba, w, h);
    std::unique_lock<std::mutex> lk(g_shot_mtx);
    g_shot_wanted = true;
    g_shot_done = false;
    if (!g_shot_cv.wait_for(lk, std::chrono::seconds(2), [] { return !g_shot_wanted; }) || !g_shot_done)
        return false;
    rgba = g_shot;
    w = g_shot_w;
    h = g_shot_h;
    return true;
}

void reset() {
    if (!g_on)
        return;
    flush();
    g_pending_uploads.clear();
    // GS memory was replaced: deferred preservation data is obsolete.
    for (uint32_t p = 0; p < kPages && g_deferred_count; ++p)
        if (g_deferred[p])
            deferred_free(p);
    memset(g_owner, 0, sizeof g_owner);
    memset(g_vram_ok, 0, sizeof g_vram_ok);
    memset(g_cpu_valid_blocks, 0, sizeof g_cpu_valid_blocks);
    memset(g_diag_draw_blocks, 0, sizeof g_diag_draw_blocks);
    g_state_valid = false;
    for (TempTex &t : g_temp)
        t.src = nullptr;
}

void finish() {
    if (!g_on)
        return;
    flush();
    g_dev->submit();
}

void stats_line(char *buf, size_t size) {
    snprintf(buf, size,
             "vulkan: %u presents, %u draw calls, %u batches, %u prims, %u "
             "uploads, %u readbacks, %u conversions, textures %u new "
             "(%u KB) %u cached %u rendered, %u surfaces, %u MB textures; "
             "readbacks for CPU %u textures %u targets %u "
             "display %u",
             g_stats.presents, g_stats.draws, g_stats.flushes, g_stats.prims, g_stats.uploads, g_stats.readbacks,
             g_stats.converts, g_stats.tex_new, g_stats.tex_bytes / 1024, g_stats.tex_hit, g_stats.tex_rt,
             (unsigned)g_surfaces.size(), (unsigned)(g_tex_bytes >> 20), g_stats.rb_reason[0], g_stats.rb_reason[1],
             g_stats.rb_reason[2], g_stats.rb_reason[3]);
    size_t used = strlen(buf);
    snprintf(buf + used, size > used ? size - used : 0,
             "; submit %llu (%llu mid-frame), GPU wait %.2f ms, pipeline creation %.2f ms (%zu pipelines)",
             (unsigned long long)g_dev->submit_count, (unsigned long long)g_dev->chunk_submits, g_dev->wait_us / 1000.0, g_dev->pipeline_us / 1000.0,
             g_dev->pipelines.size());
    used = strlen(buf);
    if (g_dev->specializer) {
        std::lock_guard<std::mutex> lock(g_dev->specializer->mutex);
        snprintf(buf + used, size > used ? size - used : 0, "; specialized %llu in background (%.1f ms)",
                 (unsigned long long)g_dev->specializer->compiled, g_dev->specializer->compile_us / 1000.0);
        g_dev->specializer->compiled = g_dev->specializer->compile_us = 0;
        used = strlen(buf);
    }
    const uint64_t *e = g_dev->pass_ends;
    snprintf(buf + used, size > used ? size - used : 0,
             "; render passes %llu, ended by submit %llu upload %llu copy %llu clear %llu map %llu target %llu layout %llu",
             (unsigned long long)g_dev->passes, (unsigned long long)e[0], (unsigned long long)e[1],
             (unsigned long long)e[2], (unsigned long long)e[3], (unsigned long long)e[4], (unsigned long long)e[5],
             (unsigned long long)e[6]);
    used = strlen(buf);
    snprintf(buf + used, size > used ? size - used : 0,
             "; deferred preservation %u (merged %u, waited %u, overwritten %u, kicked %u, open %u)",
             g_dstats.scheduled, g_dstats.merged, g_dstats.waited, g_dstats.overwritten, g_dstats.kicked,
             g_deferred_count);
    g_dstats = DeferredStats();
    used = strlen(buf);
    snprintf(buf + used, size > used ? size - used : 0, "; lazy CLUT %llu (waited %llu)",
             (unsigned long long)g_clut_lazy, (unsigned long long)g_clut_lazy_waits);
    g_clut_lazy = g_clut_lazy_waits = 0;
    if (g_host_on) {
        used = strlen(buf);
        snprintf(buf + used, size > used ? size - used : 0,
                 "; host textures %u hits %u misses %u fallbacks, %u uploads (%.1f ms) %u evictions, %llu MB resident",
                 g_hstats.hits, g_hstats.misses, g_hstats.fallbacks, g_hstats.uploads, g_hstats.upload_ms,
                 g_hstats.evictions, (unsigned long long)(g_host_gpu_bytes >> 20));
    }
    g_hstats = HostStats();
    g_dev->passes = 0;
    std::fill(std::begin(g_dev->pass_ends), std::end(g_dev->pass_ends), 0);
    g_dev->submit_count = g_dev->wait_us = g_dev->pipeline_us = g_dev->chunk_submits = 0;
    g_stats = Stats();
}

#include "modern_vk.inc"

#ifdef EOD_HW_SELECT
} // namespace vulkan
#endif
} // namespace hw
} // namespace gs

#endif // EOD_VULKAN
