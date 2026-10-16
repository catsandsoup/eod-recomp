// gs_d3d11.cpp - Direct3D 11 GS renderer (feature level 10_0 and later).
//
// Memory model: gs::vram (the CPU copy of the 4MB GS memory) holds everything
// the GPU hasn't drawn.  Frame and depth buffers become GPU "surfaces"; every
// 8KB page of GS memory is owned either by the CPU copy or by one surface.
// Pages change hands when the game uses memory differently than it last
// wrote it:
//   - a surface drawn to takes its pages, uploading CPU data or converting
//     another surface's data on the GPU (formats and block layouts differ
//     between colour and depth buffers; a remap table per page handles that),
//   - textures come from a content-hashed cache of CPU-decoded images, or
//     straight from the surface that drew them,
//   - CPU reads of rendered memory (CLUT loads, local->local transfers) and
//     unusual reinterpretations copy the surface back to gs::vram first.
//
// Pixel pipeline (gs_shaders.hlsl): render targets are RGBA8 holding the GS
// 8-bit channels, depth buffers D32_FLOAT holding z / 2^bits.  Blending uses
// dual-source factors so every (A - B) * C + D combination maps to one blend
// operation.  The internal resolution is a multiple (render scale) of the GS
// one; textures sampled from surfaces keep the extra detail.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../../core/config.h"
#include "../../core/prof.h"
#include "../../core/log.h"
#include "../../platform/platform.h"
#include "../../ui/overlay.h"
#include "../gs_hw.h"
#include "../gs_mem.h"
#include "../gs_state.h"
#include "../gs_swizzle.h"

namespace gs {
namespace hw {

bool g_on;
void readback_pages(uint32_t bp, uint32_t bw, uint32_t psm, int x, int y, int w, int h);

namespace {

#include "gs_shaders.inc"

template <class T>
void release(T *&p) {
    if (p) p->Release();
    p = nullptr;
}

// Failed Direct3D calls are logged (the first 50) with their source line.
int g_fails_logged;
HRESULT check(HRESULT hr, int line) {
    if (FAILED(hr) && g_fails_logged < 50) {
        g_fails_logged++;
        WARN("d3d11: call at gs_d3d11.cpp:%d failed (%08lx)", line, (unsigned long)hr);
    }
    return hr;
}
#define CHECK(call) check((call), __LINE__)

// ------------------------------------------------------------------ device
ID3D11Device *g_dev;
ID3D11DeviceContext *g_ctx;
IDXGISwapChain *g_swap;
ID3D11RenderTargetView *g_back_rtv;
HWND g_hwnd;
int g_back_w, g_back_h;
D3D_FEATURE_LEVEL g_level;
int g_scale = 1;  // internal resolution multiplier

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
// the in-game menu / frame counter (ui/overlay.h)
ID3D11VertexShader *g_vs_overlay;
ID3D11PixelShader *g_ps_overlay;
ID3D11InputLayout *g_layout_overlay;
ID3D11BlendState *g_bs_overlay;
ID3D11Buffer *g_cb_overlay, *g_vb_overlay, *g_ib_overlay;
size_t g_vb_overlay_size, g_ib_overlay_size;
ID3D11Texture2D *g_font_tex;
ID3D11ShaderResourceView *g_font_srv;
uint32_t g_font_gen;

std::thread::id g_gs_thread;
bool g_gs_thread_known;

struct Stats {
    uint32_t draws, prims, states, flushes, uploads, readbacks, async_readbacks, converts, tex_new, tex_hit, tex_rt, tex_bytes, presents;
    uint32_t rb_reason[4];  // vram_needed from: 0 CPU (CLUT, transfers), 1 textures, 2 acquire, 3 display
} g_stats;
int g_rb_reason;
const char *g_rb_what = "?";  // what wanted the CPU copy (statistics)
bool g_rb_clut;  // readback_pages for a CLUT load
bool g_async_clut = getenv("EOD_D3D_SYNC_CLUT") == nullptr;

// --------------------------------------------------------------- formats
enum Fmt : uint32_t { FMT_32 = 0, FMT_24 = 1, FMT_16 = 2, FMT_DEPTH = 3 };

bool is_depth_psm(uint32_t psm) { return (psm & 0x30) == 0x30; }
uint32_t color_fmt(uint32_t psm) {
    switch (psm & 0xF) {
    case 0x1: return FMT_24;
    case 0x2: case 0xA: return FMT_16;
    default: return FMT_32;
    }
}
int bpp_of(uint32_t psm) { return color_fmt(psm) == FMT_16 ? 16 : 32; }
double depth_range(uint32_t zpsm) {  // 2^bits
    switch (zpsm & 0xF) {
    case 0x0: return 4294967296.0;
    case 0x1: return 16777216.0;
    default: return 65536.0;
    }
}
void page_dims(uint32_t psm, int &pw, int &ph) {
    pw = 64;
    ph = bpp_of(psm) == 16 ? 64 : 32;
}

// ---------------------------------------------------------------- shaders
bool create_shaders() {
    struct S {
        const unsigned char *code;
        size_t size;
        const char *name;
        bool vs;
        void **out;
    } list[] = {
        {k_vs_draw, sizeof k_vs_draw, "vs_draw", true, (void **)&g_vs_draw},
        {k_ps_draw, sizeof k_ps_draw, "ps_draw", false, (void **)&g_ps_draw},
        {k_vs_pass, sizeof k_vs_pass, "vs_pass", true, (void **)&g_vs_pass},
        {k_ps_convert_color, sizeof k_ps_convert_color, "ps_convert_color", false, (void **)&g_ps_convert_color},
        {k_ps_convert_depth, sizeof k_ps_convert_depth, "ps_convert_depth", false, (void **)&g_ps_convert_depth},
        {k_ps_upload_color, sizeof k_ps_upload_color, "ps_upload_color", false, (void **)&g_ps_upload_color},
        {k_ps_upload_depth, sizeof k_ps_upload_depth, "ps_upload_depth", false, (void **)&g_ps_upload_depth},
        {k_ps_readback_color, sizeof k_ps_readback_color, "ps_readback_color", false, (void **)&g_ps_readback_color},
        {k_ps_readback_depth, sizeof k_ps_readback_depth, "ps_readback_depth", false, (void **)&g_ps_readback_depth},
        {k_ps_copy, sizeof k_ps_copy, "ps_copy", false, (void **)&g_ps_copy},
        {k_vs_overlay, sizeof k_vs_overlay, "vs_overlay", true, (void **)&g_vs_overlay},
        {k_ps_overlay, sizeof k_ps_overlay, "ps_overlay", false, (void **)&g_ps_overlay},
    };
    // Precompiled bytecode; if a driver refuses it, compile the embedded
    // source with d3dcompiler_47/43 when the system has one.
    typedef HRESULT(WINAPI * Compile_t)(LPCVOID, SIZE_T, LPCSTR, const void *, void *, LPCSTR, LPCSTR, UINT, UINT,
                                        ID3DBlob **, ID3DBlob **);
    Compile_t compile = nullptr;
    for (S &s : list) {
        HRESULT hr = s.vs ? g_dev->CreateVertexShader(s.code, s.size, nullptr, (ID3D11VertexShader **)s.out)
                          : g_dev->CreatePixelShader(s.code, s.size, nullptr, (ID3D11PixelShader **)s.out);
        if (SUCCEEDED(hr)) continue;
        if (!compile) {
            const char *dlls[] = {"d3dcompiler_47.dll", "d3dcompiler_46.dll", "d3dcompiler_43.dll"};
            for (const char *d : dlls)
                if (HMODULE m = LoadLibraryA(d)) {
                    compile = (Compile_t)GetProcAddress(m, "D3DCompile");
                    if (compile) break;
                }
        }
        ID3DBlob *blob = nullptr, *err = nullptr;
        if (!compile ||
            FAILED(compile(k_hlsl_source, sizeof k_hlsl_source - 1, "gs_shaders.hlsl", nullptr, nullptr, s.name,
                           s.vs ? "vs_4_0" : "ps_4_0", 0, 0, &blob, &err))) {
            WARN("d3d11: shader %s rejected (%08lx)", s.name, (unsigned long)hr);
            if (err) err->Release();
            return false;
        }
        hr = s.vs ? g_dev->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                                              (ID3D11VertexShader **)s.out)
                  : g_dev->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                                             (ID3D11PixelShader **)s.out);
        blob->Release();
        if (FAILED(hr)) return false;
    }
    D3D11_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"DEPTH", 0, DXGI_FORMAT_R32_UINT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"FOG", 0, DXGI_FORMAT_R32_FLOAT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    D3D11_INPUT_ELEMENT_DESC ol[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    CHECK(g_dev->CreateInputLayout(ol, 3, k_vs_overlay, sizeof k_vs_overlay, &g_layout_overlay));
    return SUCCEEDED(CHECK(g_dev->CreateInputLayout(il, 5, k_vs_draw, sizeof k_vs_draw, &g_layout)));
}

// --------------------------------------------------------------- surfaces
struct Surface {
    uint32_t bp, bw, psm;  // psm: layout and format (a Z format for depth buffers)
    bool depth;
    int w, h;              // GS pixels
    ID3D11Texture2D *tex = nullptr;
    ID3D11RenderTargetView *rtv = nullptr;
    ID3D11DepthStencilView *dsv = nullptr;
    ID3D11ShaderResourceView *srv = nullptr;
    uint32_t version = 0;  // bumped when drawn to
    uint64_t last_use = 0;
};

std::vector<Surface *> g_surfaces;
Surface *g_owner[kPages];   // nullptr: the CPU copy is current
bool g_vram_ok[kPages];     // a surface owns the page and gs::vram also holds its contents
uint64_t g_frame_no;

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
    if (FAILED(CHECK(g_dev->CreateTexture2D(&td, nullptr, &s->tex)))) return false;
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
        if (s->bp == bp && s->bw == bw && s->psm == psm && s->depth == depth) return s;
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
    if (x0 >= x1 || y0 >= y1) return;
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

// Draws a rectangle pass into the bound target: dst in target texels, src in source texels.
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
    int sampler_mag, sampler_min;  // indices into the sampler cache
    int topology;                  // 0 triangles, 1 lines, 2 points
    uint32_t blend_key;            // see blend_state()
    uint32_t depth_key;            // see depth_state()
    int scissor[4];
    bool two_pass;                 // alpha test with a fail mode that still writes something
    uint32_t fail_blend_key, fail_depth_key;
    DrawCB cb;
};

DrawState g_state;
bool g_state_valid;
std::vector<HwVertex> g_verts;

std::unordered_map<uint32_t, ID3D11BlendState *> g_blend_cache;
std::unordered_map<uint32_t, ID3D11DepthStencilState *> g_depth_cache;
std::vector<ID3D11SamplerState *> g_samplers;  // index = linear | (wrap_u << 1) | (wrap_v << 2)

// blend key: bit 0 enable, 1-4 src factor, 5-9 dst factor, 10-12 op, 13-16 write mask
ID3D11BlendState *blend_state(uint32_t key) {
    auto it = g_blend_cache.find(key);
    if (it != g_blend_cache.end()) return it->second;
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
    if (it != g_depth_cache.end()) return it->second;
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = key & 1;
    dd.DepthFunc = (D3D11_COMPARISON_FUNC)((key >> 1) & 15);
    dd.DepthWriteMask = (key & 32) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    if (!dd.DepthFunc) dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
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
    DrawCB cb = s.cb;
    if (s.two_pass) cb.flags |= fail_pass ? 0x800u : 0x400u;
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(CHECK(g_ctx->Map(g_cb_draw, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))) {
        memcpy(m.pData, &cb, sizeof cb);
        g_ctx->Unmap(g_cb_draw, 0);
    }
    float zero[4] = {0, 0, 0, 0};
    g_ctx->OMSetBlendState(blend_state(fail_pass ? s.fail_blend_key : s.blend_key), zero, 0xFFFFFFFF);
    g_ctx->OMSetDepthStencilState(depth_state(fail_pass ? s.fail_depth_key : s.depth_key), 0);
    static const bool no_draw = getenv("EOD_D3D_NODRAW") != nullptr;  // benchmarks: CPU side only
    if (!no_draw) g_ctx->Draw(count, first);
    g_stats.draws++;
}

void flush() {
    if (g_verts.empty()) return;
    ProfScope prof(P_GS_DRAW);
    std::vector<HwVertex> verts;
    verts.swap(g_verts);  // (the passes below flush too)
    const DrawState s = g_state;
    // take the pages under the batch (not the whole scissor: buffers share memory with textures)
    float mx0 = 1e9f, my0 = 1e9f, mx1 = -1e9f, my1 = -1e9f;
    for (const HwVertex &v : verts) {
        mx0 = std::min(mx0, v.x);
        my0 = std::min(my0, v.y);
        mx1 = std::max(mx1, v.x);
        my1 = std::max(my1, v.y);
    }
    int sc = g_scale;
    int bx0 = std::max((int)floorf(mx0) - 1, s.scissor[0] / sc), by0 = std::max((int)floorf(my0) - 1, s.scissor[1] / sc);
    int bx1 = std::min((int)ceilf(mx1) + 2, s.scissor[2] / sc), by1 = std::min((int)ceilf(my1) + 2, s.scissor[3] / sc);
    if (bx0 >= bx1 || by0 >= by1) return;
    acquire(s.rt, bx0, by0, bx1, by1);
    s.rt->version++;
    if (s.ds) {
        acquire(s.ds, bx0, by0, bx1, by1);
        s.ds->version++;
    }
    UINT bytes = (UINT)(verts.size() * sizeof(HwVertex));
    if (bytes > kVBBytes) {  // split huge batches (multiples of 6 vertices)
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
    if (FAILED(CHECK(g_ctx->Map(g_vb, 0, mode, 0, &m)))) return;
    memcpy((uint8_t *)m.pData + g_vb_pos, verts.data(), bytes);
    g_ctx->Unmap(g_vb, 0);
    UINT first = g_vb_pos / sizeof(HwVertex), count = (UINT)verts.size();
    g_vb_pos += bytes;

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
    draw_batch(s, first, count, false);
    if (s.two_pass) draw_batch(s, first, count, true);
    g_stats.flushes++;
}

Surface *g_bound_rt, *g_bound_ds;

void bind_targets(Surface *rt, Surface *ds) {
    Surface *any = rt ? rt : ds;
    ID3D11RenderTargetView *rtv = rt ? rt->rtv : nullptr;
    // unbind textures first (a target may have been a texture of the previous batch)
    ID3D11ShaderResourceView *null_srv[4] = {};
    g_ctx->PSSetShaderResources(0, 4, null_srv);
    g_ctx->OMSetRenderTargets(rtv ? 1 : 0, rtv ? &rtv : nullptr, ds ? ds->dsv : nullptr);
    if (any) set_viewport(any->w * g_scale, any->h * g_scale);
    g_bound_rt = rt;
    g_bound_ds = ds;
}

// ------------------------------------------------------- VRAM <-> surfaces
// raw GS value of pixel (x, y) of buffer (bp, bw, psm) in gs::vram
inline uint32_t vram_raw(const SwizzleTab *T, uint32_t rb, uint32_t rc, int x, uint32_t psm) {
    uint32_t a = T->addr_row(rb, rc, x);
    if (bpp_of(psm) == 16) return half(a);
    uint32_t v = word(a);
    return (psm & 0xF) == 0x1 ? (v & 0xFFFFFF) : v;
}

ID3D11Texture2D *g_raw_tex;          // R32_UINT staging for uploads (DEFAULT, 2048x2048)
ID3D11ShaderResourceView *g_raw_srv;
ID3D11Texture2D *g_rb_tex, *g_rb_staging;  // readback target and staging (R32_UINT 2048x2048)
ID3D11RenderTargetView *g_rb_rtv;

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
    if (FAILED(CHECK(g_dev->CreateTexture2D(&td, nullptr, &g_raw_tex)))) return false;
    CHECK(g_dev->CreateShaderResourceView(g_raw_tex, nullptr, &g_raw_srv));
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(CHECK(g_dev->CreateTexture2D(&td, nullptr, &g_rb_tex)))) return false;
    CHECK(g_dev->CreateRenderTargetView(g_rb_tex, nullptr, &g_rb_rtv));
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    return SUCCEEDED(CHECK(g_dev->CreateTexture2D(&td, nullptr, &g_rb_staging)));
}

// Uploads rectangle (x, y, w, h) of the surface from gs::vram.
void upload_rect(Surface *s, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    ProfScope prof(P_GS_UPLOAD);
    flush();
    const SwizzleTab *T = swizzle_table(s->psm, s->bw);
    std::vector<uint32_t> raw((size_t)w * h);
    for (int r = 0; r < h; r++) {
        int py = y + r;
        uint32_t rb = T->row_base(s->bp, py), rc = T->rowC[py & 2047];
        for (int c = 0; c < w; c++) raw[(size_t)r * w + c] = vram_raw(T, rb, rc, x + c, s->psm);
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

// Renders rectangle (x, y, w, h) of the surface as raw GS values into
// g_rb_tex and copies it to the staging texture `stg` (at 0, 0).
void readback_gpu(Surface *s, int x, int y, int w, int h, ID3D11Texture2D *stg) {
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
    // pixel i (1x) samples source texel x*S + i*S + S/2: the middle of the scaled pixel
    float sc = (float)g_scale;
    run_pass(s->depth ? g_ps_readback_depth : g_ps_readback_color, 2048, 2048, 0, 0, (float)w, (float)h, x * sc,
             y * sc, (x + w) * sc, (y + h) * sc, cb);
    unbind_all();
    g_bound_rt = g_bound_ds = nullptr;
    D3D11_BOX box = {0, 0, 0, (UINT)w, (UINT)h, 1};
    g_ctx->CopySubresourceRegion(stg, 0, 0, 0, 0, g_rb_tex, 0, &box);
}

// Writes mapped readback data (w x h raw values) to gs::vram at (x, y) of buffer (bp, bw, psm).
void readback_store(const D3D11_MAPPED_SUBRESOURCE &m, uint32_t bp, uint32_t bw, uint32_t psm, int x, int y, int w,
                    int h) {
    const SwizzleTab *T = swizzle_table(psm, bw);
    bool b16 = bpp_of(psm) == 16, b24 = (psm & 0xF) == 0x1;
    for (int r = 0; r < h; r++) {
        const uint32_t *src = (const uint32_t *)((const uint8_t *)m.pData + (size_t)r * m.RowPitch);
        int py = y + r;
        uint32_t rb = T->row_base(bp, py), rc = T->rowC[py & 2047];
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
}

// Writes rectangle (x, y, w, h) of the surface back to gs::vram.
void readback_rect(Surface *s, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    ProfScope prof(P_GS_READBACK);
    readback_gpu(s, x, y, w, h, g_rb_staging);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(CHECK(g_ctx->Map(g_rb_staging, 0, D3D11_MAP_READ, 0, &m)))) return;
    readback_store(m, s->bp, s->bw, s->psm, x, y, w, h);
    g_ctx->Unmap(g_rb_staging, 0);
    vram_mark(s->bp, s->bw, s->psm, x, y, w, h);
    g_stats.readbacks++;
    g_stats.rb_reason[g_rb_reason & 3]++;
}

// CLUT loads from pages a surface has drawn (the game loads palettes from
// its shadow buffer every frame): waiting for the GPU there stalls the whole
// pipeline, so the copy is started and the CLUT gets what the previous copy of
// the page brought back (a frame or so old).
struct AsyncReadback {
    ID3D11Texture2D *stg = nullptr;  // 64x64 R32_UINT staging
    Surface *src = nullptr;          // what it was read from (not dereferenced later)
    uint32_t bp = 0, bw = 0, psm = 0;
    int x = 0, y = 0, w = 0, h = 0;
    bool busy = false;  // copy in flight
};
AsyncReadback g_async_rb[kPages];  // per page
bool g_async_rb_seen[kPages];      // a synchronous readback was done once

// Stores the copies that have arrived (if the page still belongs to the same surface).
void async_readback_poll() {
    for (uint32_t p = 0; p < kPages; p++) {
        AsyncReadback &a = g_async_rb[p];
        if (!a.busy) continue;
        D3D11_MAPPED_SUBRESOURCE m;
        HRESULT hr = g_ctx->Map(a.stg, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
        a.busy = false;
        if (FAILED(hr)) continue;
        bool store = g_owner[p] == a.src && !g_vram_ok[p];
        if (store) readback_store(m, a.bp, a.bw, a.psm, a.x, a.y, a.w, a.h);
        g_ctx->Unmap(a.stg, 0);
        if (store) vram_mark(a.bp, a.bw, a.psm, a.x, a.y, a.w, a.h);
    }
}

// Readback for a CLUT load of page p (rectangle q of surface o).
void clut_readback(uint32_t p, Surface *o, const PageRect &q) {
    AsyncReadback &a = g_async_rb[p];
    if (!g_async_rb_seen[p] || q.w > 64 || q.h > 64) {  // first time: exact
        g_async_rb_seen[p] = true;
        readback_rect(o, q.x, q.y, q.w, q.h);
        return;
    }
    ProfScope prof(P_GS_READBACK);
    if (a.busy) return;  // the previous copy is still on its way
    if (!a.stg) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = 64;
        td.Height = 64;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32_UINT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(CHECK(g_dev->CreateTexture2D(&td, nullptr, &a.stg)))) {
            readback_rect(o, q.x, q.y, q.w, q.h);
            return;
        }
    }
    readback_gpu(o, q.x, q.y, q.w, q.h, a.stg);
    a.src = o;
    a.bp = o->bp;
    a.bw = o->bw;
    a.psm = o->psm;
    a.x = q.x;
    a.y = q.y;
    a.w = q.w;
    a.h = q.h;
    a.busy = true;
    g_stats.async_readbacks++;
}

// ------------------------------------------------------------ conversions
// Remap tables between two block layouts of the same pixel size: for each
// pixel of a page in layout `dst`, the pixel of a page in layout `src` at the
// same address.
std::unordered_map<uint64_t, ID3D11ShaderResourceView *> g_remaps;

ID3D11ShaderResourceView *remap_table(uint32_t dst_psm, uint32_t src_psm) {
    uint64_t key = ((uint64_t)dst_psm << 32) | src_psm;
    auto it = g_remaps.find(key);
    if (it != g_remaps.end()) return it->second;
    int pw, ph;
    page_dims(dst_psm, pw, ph);
    const SwizzleTab *Td = swizzle_table(dst_psm, 1), *Ts = swizzle_table(src_psm, 1);
    std::unordered_map<uint32_t, uint32_t> where;  // address -> src x | y << 16
    for (int y = 0; y < ph; y++)
        for (int x = 0; x < pw; x++) where[Ts->addr(0, x, y)] = (uint32_t)x | ((uint32_t)y << 16);
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
// texa: produce texture colours (16/24-bit alpha from TEXA) into a colour target.
void convert_rect(Surface *src, ID3D11RenderTargetView *dst_rtv, ID3D11DepthStencilView *dst_dsv, uint32_t dst_psm,
                  uint32_t dst_bw, int dst_w, int dst_h, int x, int y, int w, int h, bool texa, const Env *e) {
    flush();
    ProfScope prof(P_GS_CONVERT);
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
        if (!rm) rm = remap_table(dst_psm, dst_psm);
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
template <class F>
void for_rects(const std::vector<PageRect> &pages, F fn) {
    if (pages.empty()) return;
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
    for (const PageRect &p : pages) fn(p.x, p.y, p.w, p.h);
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
                    if (q.page == p.page) readback_rect(o, q.x, q.y, q.w, q.h);
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
        g_vram_ok[p.page] = false;  // about to be drawn: the CPU copy goes stale
    }
}

Surface *get_surface(uint32_t bp, uint32_t bw, uint32_t psm, bool depth, int rows) {
    if (!bw) bw = 1;
    Surface *s = find_surface(bp, bw, psm, depth);
    int need_h = rows <= 512 ? 512 : rows <= 1024 ? 1024 : 2048;
    if (s && s->h < need_h) {  // grow (keeps the contents)
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
            if (o == s) o = n;
        std::replace(g_surfaces.begin(), g_surfaces.end(), s, n);
        if (g_bound_rt == s || g_bound_ds == s) g_bound_rt = g_bound_ds = nullptr;
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
    ID3D11ShaderResourceView *srv = nullptr;
    uint32_t bytes = 0;
    uint64_t last_use = 0;
};
std::unordered_map<uint64_t, TexObj> g_tex;
uint64_t g_tex_bytes;
// validation shortcut: texture description -> (content key, VRAM clock when checked)
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
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: bw = 16; bh = 8; unit_bits = 16; break;
    case PSMT8: bw = 16; bh = 16; unit_bits = 8; break;
    case PSMT4: bw = 32; bh = 16; unit_bits = 4; break;
    default: bw = 8; bh = 8; unit_bits = 32; break;
    }
}

// hash of the GS memory blocks holding a w x h texture level
uint64_t hash_level(uint32_t tbp, uint32_t tbw, uint32_t psm, int w, int h) {
    const SwizzleTab *T = swizzle_table(psm, tbw);
    int bw, bh, ub;
    block_dims(psm, bw, bh, ub);
    // four independent lanes (the multiplies overlap)
    uint64_t h0 = 0x84222325CBF29CE4ull, h1 = 0x9E3779B97F4A7C15ull, h2 = 0xC2B2AE3D27D4EB4Full, h3 = 0x165667B19E3779F9ull;
    for (int y = 0; y < h; y += bh)
        for (int x = 0; x < w; x += bw) {
            uint32_t a = T->addr(tbp, x, y);
            uint32_t byte_addr = ub == 32 ? a * 4 : ub == 16 ? a * 2 : ub == 8 ? a : a / 2;
            const uint64_t *p = reinterpret_cast<const uint64_t *>(vram + ((byte_addr & ~255u) & (kVramSize - 1)));
            for (int k = 0; k < 32; k += 4) {
                h0 = mix64(h0, p[k]);
                h1 = mix64(h1, p[k + 1]);
                h2 = mix64(h2, p[k + 2]);
                h3 = mix64(h3, p[k + 3]);
            }
        }
    return mix64(mix64(mix64(h0, h1), h2), h3);
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
        case PSMCT32: case PSMZ32:
            for (int x = 0; x < w; x++) d[x] = word(T->addr_row(rb, rc, x));
            break;
        case PSMT8:
            for (int x = 0; x < w; x++) d[x] = e.pal[byte(T->addr_row(rb, rc, x))];
            break;
        case PSMT4:
            for (int x = 0; x < w; x++) d[x] = e.pal[nibble(T->addr_row(rb, rc, x))];
            break;
        default:
            for (int x = 0; x < w; x++) d[x] = tex_fetch(e, x, y, lvl);
            break;
        }
    }
}

int tex_levels(const Env &e) { return (e.mmin >= 2 && e.mxl > 0) ? std::min(e.mxl, 6) + 1 : 1; }

ID3D11ShaderResourceView *vram_texture(const Env &e) {
    ProfScope prof(P_GS_TEX);
    int levels = tex_levels(e);
    // what identifies the texture apart from its address and the memory contents: the game streams
    // textures into alternating areas, so the same texture is found again wherever it was loaded
    uint64_t form = mix64(0, e.tpsm | (e.tw << 8) | ((uint64_t)e.th << 20) | ((uint64_t)levels << 40));
    for (int i = 0; i < levels; i++)
        form = mix64(form, ((e.mip_tbp[i] - e.tbp) & 0x3FFF) | ((uint64_t)e.mip_tbw[i] << 32));
    bool pal = e.tpsm == PSMT8 || e.tpsm == PSMT4 || e.tpsm == PSMT8H || e.tpsm == PSMT4HL || e.tpsm == PSMT4HH;
    form = mix64(form, pal ? e.pal_hash : (e.ta0 | (e.ta1 << 8) | ((uint32_t)e.aem << 16)));
    uint64_t desc = mix64(form, e.tbp);  // ... and where it is
    // memory unchanged since the last check: same contents
    uint32_t newest = 0;
    for (int i = 0; i < levels; i++) {
        uint32_t first, count;
        page_range(e.mip_tbp[i], e.mip_tbw[i], e.tpsm, 0, 0, std::max(1, (int)e.tw >> i), std::max(1, (int)e.th >> i),
                   first, count);
        for (uint32_t k = 0; k < count; k++) newest = std::max(newest, g_page_gen[(first + k) % kPages]);
    }
    uint64_t key;
    auto ck = g_tex_check.find(desc);
    if (ck != g_tex_check.end() && ck->second.clock >= newest) {
        key = ck->second.key;
    } else {
        key = form;
        for (int i = 0; i < levels; i++)
            key = mix64(key, hash_level(e.mip_tbp[i], e.mip_tbw[i], e.tpsm, std::max(1, (int)e.tw >> i),
                                        std::max(1, (int)e.th >> i)));
        g_tex_check[desc] = {key, g_vram_clock};
    }
    auto it = g_tex.find(key);
    if (it != g_tex.end()) {
        it->second.last_use = g_frame_no;
        g_stats.tex_hit++;
        return it->second.srv;
    }
    // decode and upload
    ProfScope prof_new(P_GS_TEXNEW);
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
    for (int i = 0; i < levels; i++) {
        int w = std::max(1, (int)e.tw >> i), h = std::max(1, (int)e.th >> i);
        decode_level(e, i, w, h, data[(size_t)i]);
        init[i].pSysMem = data[(size_t)i].data();
        init[i].SysMemPitch = (UINT)w * 4;
        init[i].SysMemSlicePitch = 0;
        bytes += (uint32_t)(w * h * 4);
    }
    TexObj t;
    if (FAILED(CHECK(g_dev->CreateTexture2D(&td, init, &t.tex)))) return nullptr;
    CHECK(g_dev->CreateShaderResourceView(t.tex, nullptr, &t.srv));
    t.bytes = bytes;
    t.last_use = g_frame_no;
    g_tex_bytes += bytes;
    g_stats.tex_new++;
    g_stats.tex_bytes += bytes;
    // evict textures unused for a while when the cache is large
    if (g_tex_bytes > (256u << 20)) {
        flush();
        if (g_tex_check.size() > 65536) g_tex_check.clear();
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

// Temporary colour textures (conversions of surfaces for sampling, feedback copies).
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
        if (FAILED(CHECK(g_dev->CreateTexture2D(&td, nullptr, &t.tex)))) return nullptr;
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
            if (page == last) continue;
            last = page;
            if (std::find(out.begin(), out.end(), page) == out.end()) out.push_back(page);
        }
}

// texture source for the current env: SRV plus how to sample it
struct TexSource {
    ID3D11ShaderResourceView *srv = nullptr;
    float w = 1, h = 1;    // GS texels covered by the texture (for normalised coordinates)
    float scale = 1;       // texels of the SRV per GS texel
    uint32_t flags = 0;    // F_TEX_RT24 / F_TEX_RT16
};

TexSource texture_for(const Env &e, Surface *target) {
    TexSource ts;
    int levels = tex_levels(e);
    // a surface drawn at the texture's address (the usual render-to-texture case)
    Surface *owner = nullptr;
    bool mixed = false, gpu = false;
    {
        uint32_t p0 = (e.tbp >> 5) % kPages;
        Surface *o = g_owner[p0];
        if (o && !g_vram_ok[p0] && o->bp == e.tbp && o->bw == (e.tbw ? e.tbw : 1) && levels == 1) {
            owner = o;
            gpu = true;
            // A colour surface in the texture's own layout at this address: gather the texture area into it
            // (parts may belong to a depth or differently formatted surface at the same address).
            Surface *c = find_surface(e.tbp, e.tbw ? e.tbw : 1, e.tpsm, false);
            if (c && c != owner && bpp_of(c->psm) == bpp_of(e.tpsm)) owner = c;
            if (!owner->depth && same_layout(owner->psm, e.tpsm)) {
                // as far down as surfaces at this address have drawn
                int rows = 0;
                std::vector<PageRect> pr;
                surface_pages_rect(owner, 0, 0, std::min((int)e.tw, owner->w), std::min((int)e.th, owner->h), pr);
                for (const PageRect &q : pr) {
                    Surface *w = g_owner[q.page];
                    if (w && w->bp == e.tbp) rows = std::max(rows, q.y + q.h);
                }
                acquire(owner, 0, 0, std::min((int)e.tw, owner->w), rows);
            }
        }
    }
    if (!owner) {
    for (int i = 0; i < levels; i++) {
        // (the pages of a texture only depend on where it is and its size: remembered)
        static std::unordered_map<uint64_t, std::vector<uint32_t>> known;
        uint32_t lw = std::max(1, (int)e.tw >> i), lh = std::max(1, (int)e.th >> i);
        uint64_t k = (uint64_t)e.mip_tbp[i] | (uint64_t)e.mip_tbw[i] << 14 | (uint64_t)e.tpsm << 20 |
                     (uint64_t)lw << 26 | (uint64_t)lh << 38;
        auto kp = known.find(k);
        if (kp == known.end()) {
            if (known.size() > 4096) known.clear();
            kp = known.emplace(k, std::vector<uint32_t>()).first;
            exact_pages(e.mip_tbp[i], e.mip_tbw[i], e.tpsm, (int)lw, (int)lh, kp->second);
        }
        for (uint32_t p : kp->second) {
            Surface *o = g_owner[p];
            if (!o || g_vram_ok[p]) continue;
            gpu = true;
            if (owner && owner != o) mixed = true;
            owner = o;
        }
    }
    }
    if (!gpu) {
        ts.srv = vram_texture(e);
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
            if (e.tpsm == PSMCT24 || e.tpsm == PSMZ24) ts.flags = 0x2000;
            if (tex16) ts.flags = 0x4000;
        } else {
            // same memory, another layout or a depth buffer: convert to a texture (cached per surface version)
            uint32_t texa = e.ta0 | (e.ta1 << 8) | ((uint32_t)e.aem << 16);
            TempTex *hit = nullptr;
            for (TempTex &t : g_temp)
                if (t.src == owner && t.src_version == owner->version && t.psm == e.tpsm && t.texa == texa &&
                    t.frame == g_frame_no)
                    hit = &t;
            if (!hit) {
                hit = temp_texture(owner->w, owner->h);
                if (!hit) return ts;
                std::vector<PageRect> pages;
                surface_pages(owner, owner->h, pages);
                int rows = 0;
                for (const PageRect &p : pages)
                    if (g_owner[p.page] == owner) rows = std::max(rows, p.y + p.h);
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
    for (int i = 0; i < levels; i++) readback_pages(e.mip_tbp[i], e.mip_tbw[i], e.tpsm, 0, 0, std::max(1, (int)e.tw >> i),
                                                std::max(1, (int)e.th >> i));
    g_rb_reason = 0;
    ts.srv = vram_texture(e);
    ts.w = (float)e.tw;
    ts.h = (float)e.th;
    return ts;
}

// -------------------------------------------------------------- draw state
uint32_t g_reg_gen_seen;  // g_env_serial the state was built for

void build_state(const Env &e, int topology) {
    ProfScope prof(P_GS_STATE);
    g_stats.states++;
    DrawState s;
    memset(&s, 0, sizeof s);  // (compared bytewise below)
    s.topology = topology;
    int rows = e.scy1 + 1;
    uint32_t full = (e.fpsm & 0xF) == 0x1 ? 0xFFFFFFu : 0xFFFFFFFFu;
    bool color_write = (e.fbmsk & full) != full;
    bool zwrite = !e.zmsk && e.ztst != 0;
    bool ztest = e.ztst >= 2;
    // targets: the colour buffer always (its size defines the viewport), depth when used
    s.rt = get_surface(e.fbp, e.fbw, e.fpsm_mem, false, rows);
    if (ztest || zwrite) s.ds = get_surface(e.zbp, e.fbw, e.zpsm, true, rows);
    if (s.ds && s.rt && s.ds->h != s.rt->h) {  // views must have the same size
        int h = std::max(s.ds->h, s.rt->h);
        s.rt = get_surface(e.fbp, e.fbw, e.fpsm_mem, false, h);
        s.ds = get_surface(e.zbp, e.fbw, e.zpsm, true, h);
    }
    if (!s.rt) {
        flush();
        g_state = s;
        return;
    }
    if (s.ds && s.ds->w != s.rt->w) s.ds = nullptr;  // different widths: can't bind together
    // texture before taking the target pages (a texture may be converted from the target's memory)
    TexSource tex;
    if (e.tme) tex = texture_for(e, s.rt);

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
        if (e.fst) f |= 0x2;
        if (e.tcc) f |= 0x4;
        int levels = tex_levels(e);
        if (levels > 1) f |= 0x40;
        if (e.lcm) f |= 0x80;
        if (e.bilinear_mag) f |= 0x100;
        if (e.bilinear_min) f |= 0x200;
        f |= tex.flags;
        f |= (uint32_t)e.tfx << 16;
        cb.lod_k = e.k;
        cb.lod_l = (float)(1 << e.l);
        cb.tex_size[0] = tex.w;
        cb.tex_size[1] = tex.h;
        cb.stq_size[0] = (float)e.tw;  // STQ: s,t are fractions of tw/th
        cb.stq_size[1] = (float)e.th;
        cb.tex_scale = tex.scale;
        cb.wrap[0] = e.wms == 0 ? 0 : 1;
        cb.wrap[1] = e.wmt == 0 ? 0 : 1;
        cb.wrap[2] = levels - 1;
        s.tex = tex.srv;
        s.sampler_mag = (e.bilinear_mag ? 1 : 0) | (e.wms ? 2 : 0) | (e.wmt ? 4 : 0);
        s.sampler_min = (e.bilinear_min ? 1 : 0) | (e.wms ? 2 : 0) | (e.wmt ? 4 : 0);
    }
    if (e.fge) f |= 0x8;
    if (e.abe) f |= 0x10;
    if (e.fba) f |= 0x20;
    if (e.iip) f |= 0x1000;
    f |= (uint32_t)e.atst << 20;
    if (color_fmt(e.fpsm_mem) == FMT_16) f |= 0x1000000;
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
        if ((e.fbmsk & 0xFF) != 0xFF) mask |= 1;
        if ((e.fbmsk & 0xFF00) != 0xFF00) mask |= 2;
        if ((e.fbmsk & 0xFF0000) != 0xFF0000) mask |= 4;
        if ((e.fpsm & 0xF) != 0x1 && (e.fbmsk & 0xFF000000u) != 0xFF000000u) mask |= 8;
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
            // Cd +/- Cs exactly (C = FIX = 0x80 or not involved): 16-bit truncation emulated in the shader
            bool unit_c = e.bc == 2 && e.bfix == 0x80;
            int cs = ks == 0 ? ms : (unit_c ? ks + ms : 99), cd = kd == 0 ? md : (unit_c ? kd + md : 99);
            if (cd == 1 && cs == 1) s.cb.flags |= 0x2000000;
            if (cd == 1 && cs == -1) s.cb.flags |= 0x4000000;
        }
        bool neg_s = ks < 0 && ms == 0, neg_d = kd < 0 && md == 0;
        D3D11_BLEND_OP op = D3D11_BLEND_OP_ADD;
        D3D11_BLEND sf = D3D11_BLEND_SRC1_ALPHA, df = D3D11_BLEND_SRC1_COLOR;
        if (neg_s && !neg_d) op = D3D11_BLEND_OP_REV_SUBTRACT;
        else if (!neg_s && neg_d) op = D3D11_BLEND_OP_SUBTRACT;
        else if (neg_s && neg_d) sf = df = D3D11_BLEND_ZERO;
        if (e.bc == 1) {  // C = destination alpha: fixed function factors
            sf = ks == 0 ? (ms ? D3D11_BLEND_ONE : D3D11_BLEND_ZERO)
                         : (ks > 0 ? D3D11_BLEND_DEST_ALPHA : (ms ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_DEST_ALPHA));
            df = kd == 0 ? (md ? D3D11_BLEND_ONE : D3D11_BLEND_ZERO)
                         : (kd > 0 ? D3D11_BLEND_DEST_ALPHA : (md ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_DEST_ALPHA));
        }
        bkey = blend_key(true, sf, df, op, mask);
    } else {
        bkey = blend_key(false, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, mask);
    }
    D3D11_COMPARISON_FUNC zf = e.ztst == 2 ? D3D11_COMPARISON_GREATER_EQUAL
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
            s.cb.flags |= 0x400;  // keep passing pixels only
        } else {
            s.two_pass = true;
            uint32_t fmask = fb_only ? mask : zb_only ? 0 : (mask & 7);
            s.fail_blend_key = (bkey & ~(15u << 14)) | (fmask << 14);
            s.fail_depth_key = zb_only ? dkey : (dkey & ~32u);
            if (e.atst == 0) {  // NEVER: only the fail pass (skip the empty pass via the flag trick)
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

}  // namespace

// ================================================================ interface
bool start(bool with_window) {
    if (g_on) return true;
    IDXGIFactory *factory = nullptr;
    IDXGIAdapter *adapter = nullptr;
    if (SUCCEEDED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void **)&factory))) {
        if (FAILED(factory->EnumAdapters((UINT)g_config.adapter, &adapter))) factory->EnumAdapters(0, &adapter);
    }
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    UINT flags = D3D11_CREATE_DEVICE_SINGLETHREADED;
    if (getenv("EOD_D3D_DEBUG")) flags |= D3D11_CREATE_DEVICE_DEBUG;
    // --renderer warp: Microsoft's software Direct3D 11 device (tells driver bugs from renderer bugs)
    bool warp = g_config.renderer == "warp";
    if (warp) release(adapter);
    if (adapter) {
        DXGI_ADAPTER_DESC ad;
        if (SUCCEEDED(adapter->GetDesc(&ad))) {
            char name[128];
            WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof name, nullptr, nullptr);
            LOG(LOG_GS, "d3d11: adapter %s (%04x:%04x, %u MB)", name, ad.VendorId, ad.DeviceId,
                (unsigned)(ad.DedicatedVideoMemory >> 20));
        }
    }
    HRESULT hr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN
                                            : warp  ? D3D_DRIVER_TYPE_WARP
                                                    : D3D_DRIVER_TYPE_HARDWARE,
                                   nullptr, flags, levels, 3, D3D11_SDK_VERSION, &g_dev, &g_level, &g_ctx);
    if (FAILED(hr)) {
        WARN("d3d11: no device (%08lx)", (unsigned long)hr);
        release(adapter);
        release(factory);
        return false;
    }
    g_scale = std::max(1, std::min(6, g_config.render_scale));
    if (with_window) {
        g_hwnd = (HWND)platform_native_window();
        RECT rc;
        GetClientRect(g_hwnd, &rc);
        g_back_w = std::max(1L, rc.right - rc.left);
        g_back_h = std::max(1L, rc.bottom - rc.top);
        DXGI_SWAP_CHAIN_DESC sd = {};
        sd.BufferDesc.Width = (UINT)g_back_w;
        sd.BufferDesc.Height = (UINT)g_back_h;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.OutputWindow = g_hwnd;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        IDXGIDevice *dxdev = nullptr;
        IDXGIAdapter *ad = nullptr;
        IDXGIFactory *fac = nullptr;
        g_dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxdev);
        if (dxdev) dxdev->GetAdapter(&ad);
        if (ad) ad->GetParent(__uuidof(IDXGIFactory), (void **)&fac);
        if (fac) {
            hr = fac->CreateSwapChain(g_dev, &sd, &g_swap);
            fac->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        }
        release(fac);
        release(ad);
        release(dxdev);
        if (!g_swap) {
            WARN("d3d11: no swap chain (%08lx)", (unsigned long)hr);
            release(g_ctx);
            release(g_dev);
            release(adapter);
            release(factory);
            return false;
        }
        ID3D11Texture2D *bb = nullptr;
        CHECK(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb));
        if (bb) {
            CHECK(g_dev->CreateRenderTargetView(bb, nullptr, &g_back_rtv));
            bb->Release();
        }
    }
    release(adapter);
    release(factory);
    if (!create_shaders() || !create_raw_textures()) {
        WARN("d3d11: initialisation failed");
        return false;
    }
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
    if (g_swap) overlay_enable();  // the in-game menu can be drawn
    LOG(LOG_GS, "d3d11: feature level %x, render scale %d%s", (unsigned)g_level, g_scale, warp ? " (WARP)" : "");
    return true;
}

void draw_triangle(const Env &e, const Vertex &a, const Vertex &b, const Vertex &c) {
    ensure_state(e, 0);
    if (!g_state.rt) return;
    if (e.iip) {
        g_verts.push_back(hv(e, a, pack_rgba(a)));
        g_verts.push_back(hv(e, b, pack_rgba(b)));
        g_verts.push_back(hv(e, c, pack_rgba(c)));
    } else {  // flat: the last vertex's colour
        uint32_t col = pack_rgba(c);
        g_verts.push_back(hv(e, a, col));
        g_verts.push_back(hv(e, b, col));
        g_verts.push_back(hv(e, c, col));
    }
}

void draw_sprite(const Env &e, const Vertex &a, const Vertex &b) {
    ensure_state(e, 0);
    if (!g_state.rt) return;
    uint32_t col = pack_rgba(b);
    HwVertex v0 = hv(e, a, col), v1 = hv(e, b, col);
    v0.z = v1.z;  // sprites use the second vertex's depth, fog and (STQ) Q
    v0.fog = v1.fog;
    if (!e.fst) v0.q = v1.q;
    HwVertex v2 = v0, v3 = v0;
    v2.x = v1.x;  // top right
    v2.s = v1.s;
    v3.y = v1.y;  // bottom left
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
    if (!g_state.rt) return;
    uint32_t cb = pack_rgba(b);
    g_verts.push_back(hv(e, a, e.iip ? pack_rgba(a) : cb));
    g_verts.push_back(hv(e, b, cb));
}

void draw_point(const Env &e, const Vertex &a) {
    ensure_state(e, 2);
    if (!g_state.rt) return;
    g_verts.push_back(hv(e, a, pack_rgba(a)));
}

void vram_written(uint32_t bp, uint32_t bw, uint32_t psm, int x, int y, int w, int h) {
    if (!g_on) return;
    uint32_t first, count;
    page_range(bp, bw, psm, x, y, w, h, first, count);
    for (uint32_t k = 0; k < count; k++) {
        uint32_t p = (first + k) % kPages;
        // partial writes into rendered pages drop the rest of their GPU contents
        // (the game only does that with buffers it redraws anyway)
        g_owner[p] = nullptr;
        g_vram_ok[p] = false;
    }
    g_state_valid = false;
}

void readback_pages(uint32_t bp, uint32_t bw, uint32_t psm, int x, int y, int w, int h) {
    if (!g_on) return;
    uint32_t first, count;
    page_range(bp, bw, psm, x, y, w, h, first, count);
    for (uint32_t k = 0; k < count; k++) {
        uint32_t p = (first + k) % kPages;
        Surface *o = g_owner[p];
        if (!o || g_vram_ok[p]) continue;
        std::vector<PageRect> pages;
        surface_pages(o, o->h, pages);
        bool found = false;
        for (const PageRect &q : pages)
            if (q.page == p) {
                found = true;
                if (g_rb_clut) {
                    clut_readback(p, o, q);
                } else {
                    readback_rect(o, q.x, q.y, q.w, q.h);
                    g_vram_ok[p] = true;
                }
            }
        if (!found) {  // not part of the surface's rows (shouldn't happen): give up the page
            g_owner[p] = nullptr;
        }
    }
}

void vram_needed(uint32_t bp, uint32_t bw, uint32_t psm, int x, int y, int w, int h, const char *why) {
    g_rb_reason = 0;
    g_rb_what = why;
    g_rb_clut = strcmp(why, "clut") == 0 && g_async_clut;
    if (g_rb_clut) async_readback_poll();
    readback_pages(bp, bw, psm, x, y, w, h);
    g_rb_clut = false;
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
    if (!((st.pmode >> n) & 1)) return c;
    uint64_t fb = st.dispfb[n], disp = st.display[n];
    c.fbp = bits(fb, 0, 9) * 32;
    c.fbw = bits(fb, 9, 6);
    c.psm = bits(fb, 15, 5);
    c.x = (int)bits(fb, 32, 11);
    c.y = (int)bits(fb, 43, 11);
    int magh = (int)bits(disp, 23, 4) + 1, magv = (int)bits(disp, 27, 2) + 1;
    c.w = ((int)bits(disp, 32, 12) + 1) / magh;
    c.h = ((int)bits(disp, 44, 11) + 1) / magv;
    if (c.w <= 0 || c.h <= 0 || c.w > 2048 || c.h > 2048) return c;
    bool field_mode = (st.smode2 & 1) && ((st.smode2 >> 1) & 1);
    c.lines = field_mode ? c.h / 2 : c.h;
    c.on = true;
    return c;
}

ID3D11Texture2D *g_disp_tex[2];  // CPU-side display images (movies), per circuit
ID3D11ShaderResourceView *g_disp_srv[2];
int g_disp_w, g_disp_h;
// the last composition (what read_display shows)
Circuit g_last[2];
bool g_have_last;

// source of a display circuit: a surface or the CPU copy (uploaded)
ID3D11ShaderResourceView *circuit_source(int n, const Circuit &c, float &x0, float &y0, float &x1, float &y1, float &sw,
                                         float &sh) {
    Surface *s = find_surface(c.fbp, c.fbw ? c.fbw : 1, c.psm, false);
    uint32_t page = (c.fbp >> 5) % kPages;
    static const char *show = getenv("EOD_D3D_SHOW");  // debug: display this buffer (hex block address) instead
    if (show) {
        Surface *d = find_surface((uint32_t)strtoul(show, nullptr, 16), c.fbw ? c.fbw : 1, c.psm, false);
        if (d) s = d, page = (d->bp >> 5) % kPages;
    }
    {  // log where the display comes from when that changes (not the buffer flips)
        static uint64_t last[2];
        uint64_t key = ((uint64_t)c.fbw << 40) ^ ((uint64_t)c.psm << 32) ^ ((uint64_t)c.w << 20) ^
                       ((uint64_t)c.lines << 8) ^ (s && g_owner[page] == s ? 1 : 0);
        if (key != last[n]) {
            last[n] = key;
            LOG(LOG_GS, "d3d11: display %d shows %x/%d/%x %dx%d at %d,%d from %s", n, c.fbp, c.fbw, c.psm, c.w, c.lines,
                c.x, c.y, s && g_owner[page] == s ? "a render target" : "GS memory");
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
    // rendered by another surface (other format) or written by the CPU: read gs::vram
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
        if (FAILED(CHECK(g_dev->CreateTexture2D(&td, nullptr, &g_disp_tex[n])))) return nullptr;
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
    // circuit 1 over circuit 2 with PMODE.ALP (MMOD = 1) or opaque (as the software compositor)
    bool alpha_blend = c[0].on && c[1].on && ((st.pmode >> 5) & 1);
    float alp = alpha_blend ? bits(st.pmode, 8, 8) / 255.0f : 1.0f;
    if (c[0].on && c[1].on && !alpha_blend) c[1].on = false;
    // sources first (reading the CPU copy may run passes of its own)
    ID3D11ShaderResourceView *srv[2] = {};
    float r[2][6] = {};
    for (int n = 0; n < 2; n++)
        if (c[n].on) srv[n] = circuit_source(n, c[n], r[n][0], r[n][1], r[n][2], r[n][3], r[n][4], r[n][5]);
    unbind_all();
    g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    set_viewport(tw, th);
    g_ctx->OMSetDepthStencilState(g_ds_off, 0);
    for (int n = 1; n >= 0; n--) {  // circuit 2 first, circuit 1 over it
        if (!srv[n]) continue;
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
        run_pass(g_ps_copy, tw, th, (float)rx, (float)ry, (float)(rx + rw), (float)(ry + rh), r[n][0], r[n][1],
                 r[n][2], r[n][3], cb);
    }
    unbind_all();
    g_bound_rt = g_bound_ds = nullptr;
}

std::mutex g_shot_mtx;
std::condition_variable g_shot_cv;
bool g_shot_wanted, g_shot_done;
std::vector<uint32_t> g_shot;
int g_shot_w, g_shot_h;

// renders the last composition at the GS display size and reads it back
bool capture(std::vector<uint32_t> &rgba, int &w, int &h) {
    if (!g_have_last) return false;
    const Circuit &c = g_last[0].on ? g_last[0] : g_last[1];
    if (!c.on) return false;
    w = c.w;
    h = c.h;
    flush();
    TempTex *t = temp_texture(w, h);  // (scaled; we render at 1x into its corner)
    if (!t) return false;
    compose(t->rtv, t->w * g_scale, t->h * g_scale, 0, 0, w, h, false);
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
    if (FAILED(CHECK(g_dev->CreateTexture2D(&td, nullptr, &stg)))) return false;
    D3D11_BOX box = {0, 0, 0, (UINT)w, (UINT)h, 1};
    g_ctx->CopySubresourceRegion(stg, 0, 0, 0, 0, t->tex, 0, &box);
    D3D11_MAPPED_SUBRESOURCE m;
    bool ok = SUCCEEDED(CHECK(g_ctx->Map(stg, 0, D3D11_MAP_READ, 0, &m)));
    LOG(LOG_GS, "d3d11: capture %dx%d circuits %d/%d map %d pmode %llx", w, h, (int)g_last[0].on, (int)g_last[1].on,
        (int)ok, (unsigned long long)st.pmode);
    for (int n = 0; n < 2; n++) {
        const Circuit &k = g_last[n];
        Surface *sf = find_surface(k.fbp, k.fbw ? k.fbw : 1, k.psm, false);
        LOG(LOG_GS, "d3d11:   circuit %d on %d fb %x/%d/%x %dx%d at %d,%d surface %p owner %p", n, (int)k.on, k.fbp, k.fbw,
            k.psm, k.w, k.lines, k.x, k.y, (void *)sf, (void *)g_owner[(k.fbp >> 5) % kPages]);
    }
    if (ok) {
        rgba.resize((size_t)w * h);
        for (int y = 0; y < h; y++)
            memcpy(&rgba[(size_t)y * w], (const uint8_t *)m.pData + (size_t)y * m.RowPitch, (size_t)w * 4);
        g_ctx->Unmap(stg, 0);
        LOG(LOG_GS, "d3d11: capture pixel %08x %08x", rgba[(size_t)(h / 2) * w + w / 2], rgba[0]);

    }
    stg->Release();
    return ok;
}

}  // namespace

void bind_thread() {
    g_gs_thread = std::this_thread::get_id();
    g_gs_thread_known = true;
}

void vblank() {
    if (!g_on) return;
    std::unique_lock<std::mutex> lk(g_shot_mtx);
    if (g_shot_wanted) {
        g_shot_wanted = false;
        g_shot_done = capture(g_shot, g_shot_w, g_shot_h);
        g_shot_cv.notify_all();
    }
}

namespace {
// Draws the in-game menu / frame counter (ui/overlay.h) into the back buffer.
void draw_overlay(ID3D11RenderTargetView *rtv, int tw, int th) {
    static OverlayDraw d;
    if (!g_layout_overlay || !overlay_latest(d) || d.w <= 0 || d.h <= 0) return;
    int fw, fh;
    uint32_t gen;
    const uint32_t *font = overlay_font(fw, fh, gen);
    if (!font) return;
    if (!g_font_srv || gen != g_font_gen) {
        release(g_font_srv);
        release(g_font_tex);
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = (UINT)fw;
        td.Height = (UINT)fh;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init = {font, (UINT)fw * 4, 0};
        if (FAILED(CHECK(g_dev->CreateTexture2D(&td, &init, &g_font_tex)))) return;
        CHECK(g_dev->CreateShaderResourceView(g_font_tex, nullptr, &g_font_srv));
        g_font_gen = gen;
    }
    if (!g_bs_overlay) {
        D3D11_BLEND_DESC bl = {};
        D3D11_RENDER_TARGET_BLEND_DESC &rt = bl.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D11_BLEND_ONE;
        rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        CHECK(g_dev->CreateBlendState(&bl, &g_bs_overlay));
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 16;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        CHECK(g_dev->CreateBuffer(&bd, nullptr, &g_cb_overlay));
    }
    // vertex / index buffers (grown as needed)
    auto fill = [](ID3D11Buffer *&b, size_t &cap, UINT bind, const void *data, size_t bytes) {
        if (bytes > cap) {
            release(b);
            cap = std::max<size_t>(bytes * 2, 64 * 1024);
            D3D11_BUFFER_DESC bd = {};
            bd.ByteWidth = (UINT)cap;
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.BindFlags = bind;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(CHECK(g_dev->CreateBuffer(&bd, nullptr, &b)))) {
                cap = 0;
                return false;
            }
        }
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(CHECK(g_ctx->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))) return false;
        memcpy(m.pData, data, bytes);
        g_ctx->Unmap(b, 0);
        return true;
    };
    if (!fill(g_vb_overlay, g_vb_overlay_size, D3D11_BIND_VERTEX_BUFFER, d.verts.data(),
              d.verts.size() * sizeof(OverlayVertex)) ||
        !fill(g_ib_overlay, g_ib_overlay_size, D3D11_BIND_INDEX_BUFFER, d.indices.data(), d.indices.size() * 2))
        return;
    float scale[4] = {2.0f / (float)d.w, 2.0f / (float)d.h, 0, 0};
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(CHECK(g_ctx->Map(g_cb_overlay, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))) return;
    memcpy(m.pData, scale, sizeof scale);
    g_ctx->Unmap(g_cb_overlay, 0);

    unbind_all();
    g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    set_viewport(tw, th);
    UINT stride = sizeof(OverlayVertex), offset = 0;
    g_ctx->IASetInputLayout(g_layout_overlay);
    g_ctx->IASetVertexBuffers(0, 1, &g_vb_overlay, &stride, &offset);
    g_ctx->IASetIndexBuffer(g_ib_overlay, DXGI_FORMAT_R16_UINT, 0);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_vs_overlay, nullptr, 0);
    g_ctx->VSSetConstantBuffers(2, 1, &g_cb_overlay);
    g_ctx->PSSetShader(g_ps_overlay, nullptr, 0);
    g_ctx->PSSetShaderResources(0, 1, &g_font_srv);
    ID3D11SamplerState *ss[2] = {g_point_clamp, g_linear_clamp};
    g_ctx->PSSetSamplers(0, 2, ss);
    float factor[4] = {0, 0, 0, 0};
    g_ctx->OMSetBlendState(g_bs_overlay, factor, 0xFFFFFFFF);
    g_ctx->OMSetDepthStencilState(g_ds_off, 0);
    g_ctx->RSSetState(g_rs_scissor);
    float sx = (float)tw / (float)d.w, sy = (float)th / (float)d.h;
    for (const OverlayCmd &c : d.cmds) {
        D3D11_RECT r = {(LONG)(c.clip[0] * sx), (LONG)(c.clip[1] * sy), (LONG)(c.clip[2] * sx + 0.5f),
                        (LONG)(c.clip[3] * sy + 0.5f)};
        g_ctx->RSSetScissorRects(1, &r);
        g_ctx->DrawIndexed(c.index_count, c.first_index, (INT)c.base_vertex);
    }
    unbind_all();
    g_bound_rt = g_bound_ds = nullptr;
}

// Composes the last display image (and the overlay) into the window and presents it.
void show(bool vsync) {
    if (g_swap) {
        RECT rc;
        GetClientRect(g_hwnd, &rc);
        int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
        if (cw > 0 && ch > 0) {
            if (cw != g_back_w || ch != g_back_h) {
                unbind_all();
                release(g_back_rtv);
                g_ctx->Flush();
                if (SUCCEEDED(CHECK(g_swap->ResizeBuffers(0, (UINT)cw, (UINT)ch, DXGI_FORMAT_UNKNOWN, 0)))) {
                    g_back_w = cw;
                    g_back_h = ch;
                }
                ID3D11Texture2D *bb = nullptr;
                CHECK(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb));
                if (bb) {
                    CHECK(g_dev->CreateRenderTargetView(bb, nullptr, &g_back_rtv));
                    bb->Release();
                }
            }
            float black[4] = {0, 0, 0, 1};
            g_ctx->ClearRenderTargetView(g_back_rtv, black);
            int x, y, w, h;
            platform_display_rect(g_back_w, g_back_h, &x, &y, &w, &h);
            if (g_have_last) compose(g_back_rtv, g_back_w, g_back_h, x, y, w, h, g_config.smooth_display);
            draw_overlay(g_back_rtv, g_back_w, g_back_h);
            HRESULT hr = CHECK(g_swap->Present(vsync ? 1 : 0, 0));
            g_stats.presents++;
            if (FAILED(hr) && g_fails_logged < 50)
                WARN("d3d11: device removed reason %08lx", (unsigned long)g_dev->GetDeviceRemovedReason());
        }
    }
}
}  // namespace

void present(bool vsync) {
    ProfScope prof(P_GS_PRESENT);
    if (!g_on) return;
    flush();
    async_readback_poll();
    g_last[0] = read_circuit_regs(0);
    g_last[1] = read_circuit_regs(1);
    g_have_last = g_last[0].on || g_last[1].on;
    g_frame_no++;
    g_state_valid = false;
    show(vsync);
}

void redraw() {
    if (!g_on) return;
    flush();
    g_state_valid = false;
    show(true);
}

bool read_display(std::vector<uint32_t> &rgba, int &w, int &h) {
    if (!g_on) return false;
    if (!g_gs_thread_known || std::this_thread::get_id() == g_gs_thread) return capture(rgba, w, h);
    std::unique_lock<std::mutex> lk(g_shot_mtx);
    g_shot_wanted = true;
    g_shot_done = false;
    if (!g_shot_cv.wait_for(lk, std::chrono::seconds(2), [] { return !g_shot_wanted; }) || !g_shot_done) return false;
    rgba = g_shot;
    w = g_shot_w;
    h = g_shot_h;
    return true;
}

void reset() {
    if (!g_on) return;
    flush();
    memset(g_owner, 0, sizeof g_owner);
    memset(g_vram_ok, 0, sizeof g_vram_ok);
    g_state_valid = false;
    for (TempTex &t : g_temp) t.src = nullptr;
}

void finish() {
    if (!g_on) return;
    flush();
    D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
    ID3D11Query *q = nullptr;
    if (FAILED(CHECK(g_dev->CreateQuery(&qd, &q)))) return;
    g_ctx->End(q);
    BOOL done = FALSE;
    while (g_ctx->GetData(q, &done, sizeof done, 0) != S_OK || !done) Sleep(0);
    q->Release();
}

void stats_line(char *buf, size_t size) {
    snprintf(buf, size,
             "d3d11: %u presents, %u draw calls, %u batches, %u prims, %u states, %u uploads, %u readbacks, %u conversions, textures %u new "
             "(%u KB) %u cached %u rendered, %u surfaces, %u MB textures; readbacks for CPU %u textures %u targets %u "
             "display %u, CLUT readbacks without waiting %u",
             g_stats.presents, g_stats.draws, g_stats.flushes, g_stats.prims, g_stats.states, g_stats.uploads, g_stats.readbacks, g_stats.converts,
             g_stats.tex_new, g_stats.tex_bytes / 1024, g_stats.tex_hit, g_stats.tex_rt, (unsigned)g_surfaces.size(),
             (unsigned)(g_tex_bytes >> 20), g_stats.rb_reason[0], g_stats.rb_reason[1], g_stats.rb_reason[2],
             g_stats.rb_reason[3], g_stats.async_readbacks);
    g_stats = Stats();
}

}  // namespace hw
}  // namespace gs
#endif  // _WIN32
