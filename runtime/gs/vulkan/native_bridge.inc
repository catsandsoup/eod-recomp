// native_bridge.inc - hands the game's frame and depth buffer surfaces to the native renderer
// (runtime/render/native_renderer.cpp), included at the end of gs_vulkan.cpp inside gs::hw.
// The native renderer records its plume commands into the backend's command buffer at this point
// of GS command order, so its objects and the GS's own draws share surfaces, depth and order.
// Bridge commands flush earlier GS work, take ownership of target pages (uploading newer
// CPU contents) and invalidate cached bindings after recording.
#ifdef EOD_PLUME

#include "display_history_vk.inc"

// Same format and storage extent as the fixed CT32 scanout surface. Only the
// texture and its views alternate; GS page ownership stays on the Surface.
static Surface g_display_spare{};

bool native_display_copy(const Env &e) {
    // The scaled sprite path fills the lower edge when it reaches the scissor.
    if (g_scale > 1 && e.scy1 != 447) return false;
    if (g_skip_frame) return true;  // keep the previous scanout history
    native::gs_barrier();

    constexpr int width = 640, rows = 448;
    Surface *src = find_surface(e.tbp, 10, PSMCT32, false);
    if (!src || src->w < width || src->h < rows || !src->srv) return false;

    std::vector<PageRect> pages;
    surface_pages_rect(src, 0, 0, width, rows, pages);
    for (const auto &page : pages)
        if (g_owner[page.page] != src) return false;
    Surface *dst = get_surface(0, 10, PSMCT32, false, rows);
    if (!dst || dst->w < width || dst->h < rows || !dst->rtv) return false;
    pages.clear();
    surface_pages_rect(dst, 0, 0, width, rows, pages);
    for (const auto &page : pages)
        if (g_owner[page.page] && g_owner[page.page] != dst && !g_vram_ok[page.page])
            return false;  // do not introduce a synchronous transfer for an alias

    static ID3D11PixelShader *shader;
    if (!shader) {
        auto candidate = std::make_unique<vkgs::Shader>();
        candidate->entry = "ps_display_history";
        candidate->id = 12;
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = sizeof k_spv_ps_display_history;
        info.pCode = k_spv_ps_display_history;
        if (vkCreateShaderModule(g_dev->device, &info, nullptr, &candidate->module) != VK_SUCCESS)
            return false;
        shader = candidate.release();
    }

    flush();
    unbind_all();
    static const bool snapshot = [] {
        const char *value = std::getenv("EOD_NATIVE_DISPLAY_SNAPSHOT");
        return value && value[0] == '1';
    }();
    const bool rotate = !snapshot && dst->w == width && dst->h == 512;
    if (rotate && !g_display_spare.tex) {
        g_display_spare.psm = PSMCT32;
        g_display_spare.w = dst->w;
        g_display_spare.h = dst->h;
        if (!create_surface_texture(&g_display_spare)) return false;
    }
    TempTex *previous = rotate ? nullptr : temp_texture(width, rows);
    if (!rotate && (!previous || !previous->srv)) return false;
    acquire(dst, 0, 0, width, rows);
    const UINT render_width = width * g_scale, render_rows = rows * g_scale;
    const int target_width = dst->w * g_scale, target_rows = dst->h * g_scale;
    g_ctx->begin();
    EOD_GPU_ZONE(g_ctx, "display history");
    const uint32_t timer = g_ctx->gpu_begin(vkgs::Context::GPU_DISPLAY_HISTORY);
    if (rotate) {
        // Preserve the storage rows outside the 448-row copy rectangle.
        const D3D11_BOX tail = {0, render_rows, 0, render_width, (UINT)target_rows, 1};
        g_ctx->CopySubresourceRegion(g_display_spare.tex, 0, 0, render_rows, 0, dst->tex, 0, &tail);
    } else {
        const D3D11_BOX region = {0, 0, 0, render_width, render_rows, 1};
        g_ctx->CopySubresourceRegion(previous->tex, 0, 0, 0, 0, dst->tex, 0, &region);
    }
    auto *target = rotate ? g_display_spare.rtv : dst->rtv;
    g_ctx->OMSetRenderTargets(1, &target, nullptr);
    set_viewport(target_width, target_rows);
    g_ctx->OMSetDepthStencilState(g_ds_off, 0);
    g_ctx->OMSetBlendState(g_bs_opaque, nullptr, 0xffffffff);
    ID3D11ShaderResourceView *sources[] = {src->srv, rotate ? dst->srv : previous->srv};
    g_ctx->PSSetShaderResources(0, 2, sources);
    PassCB cb{};
    // Packet XYZ starts at -15/16 with UV 0. The draw VS adds 1/2 GS
    // pixel, and point sampling floors scaled UV at each subpixel center.
    cb.misc[0] = (float)((8 + 7 * g_scale) / 16);
    cb.fmt[0] = e.bfix;
    cb.fmt[1] = e.colclamp;
    cb.fmt[2] = e.pabe;
    cb.fmt[3] = e.fba;
    run_pass(shader, target_width, target_rows, 0, 0, (float)render_width, (float)render_rows,
             0, 0, (float)render_width, (float)render_rows, cb);
    g_ctx->gpu_end(timer);
    if (rotate) {
        std::swap(dst->tex, g_display_spare.tex);
        std::swap(dst->rtv, g_display_spare.rtv);
        std::swap(dst->srv, g_display_spare.srv);
    }
    ++dst->version;
    g_native_display_history = true;
    native_end();
    return true;
}

void native_display_copy_fallback() { g_native_display_history = false; }

bool native_begin(const Env &e, bool want_depth, NativeSurfaces &out, bool require_no_readback) {
    if (g_skip_frame) return false;
    const int rows = e.scy1 + 1;
    Surface *rt = get_surface(e.fbp, e.fbw, e.fpsm_mem, false, rows);
    Surface *ds = want_depth ? get_surface(e.zbp, e.fbw, e.zpsm, true, rows) : nullptr;
    if (rt && ds && ds->h != rt->h) {  // views must have the same size (as build_state)
        const int h = std::max(ds->h, rt->h);
        rt = get_surface(e.fbp, e.fbw, e.fpsm_mem, false, h);
        ds = get_surface(e.zbp, e.fbw, e.zpsm, true, h);
    }
    if (!rt || !rt->tex) return false;
    if (ds && (!ds->tex || ds->w != rt->w)) ds = nullptr;
    flush();
    const int cols = std::min((int)e.fbw * 64, rt->w);
    if (require_no_readback) {
        if (want_depth && !ds) return false;
        std::vector<PageRect> pages;
        for (Surface *surface : {rt, ds}) {
            if (!surface) continue;
            surface_pages_rect(surface, 0, 0, cols, rows, pages);
            for (const auto &page : pages) {
                const Surface *owner = g_owner[page.page];
                if (owner != surface && ((owner && !g_vram_ok[page.page]) || g_deferred[page.page]))
                    return false;
            }
        }
    }
    g_ctx->barrier(2);
    acquire(rt, 0, 0, cols, std::min(rows, rt->h));
    rt->version++;
    if (ds) {
        acquire(ds, 0, 0, cols, std::min(rows, ds->h));
        ds->version++;
    }
    g_ctx->barrier(2);
    out.cmd = (void *)g_ctx->cmd;
    out.color = (uint64_t)rt->tex->image;
    out.depth = ds ? (uint64_t)ds->tex->image : 0;
    out.gs_width = (uint32_t)rt->w;
    out.gs_height = (uint32_t)rt->h;
    out.scale = g_scale;
    out.width = (uint32_t)(rt->w * g_scale);
    out.height = (uint32_t)(rt->h * g_scale);
    out.color_ownership = rt->ownership;
    out.depth_ownership = ds ? ds->ownership : 0;
    return true;
}

bool native_begin_depth_clear(const Env &e, NativeSurfaces &out) {
    constexpr int cols = 640, rows = 448;
    if (g_skip_frame || (g_scale > 1 && e.scy1 != rows - 1)) return false;
    Surface *ds = find_surface(e.zbp, e.fbw, e.zpsm, true);
    if (!ds || !ds->dsv || ds->w != cols || ds->h < rows) return false;

    // The covered depth pages must already be resident in their exact owner.
    // In particular, don't acquire or invalidate any scene color pages.
    std::vector<PageRect> pages;
    surface_pages_rect(ds, 0, 0, cols, rows, pages);
    for (const auto &page : pages)
        if (g_owner[page.page] != ds) return false;

    flush();
    g_ctx->barrier(2);
    acquire(ds, 0, 0, cols, rows);
    ++ds->version;
    ds->last_use = g_frame_no;
    out = {(void *)g_ctx->cmd, 0, (uint64_t)ds->tex->image, (uint32_t)(ds->w * g_scale), (uint32_t)(ds->h * g_scale),
           (uint32_t)ds->w, (uint32_t)ds->h, g_scale, 0, ds->ownership};
    return true;
}

void native_mark_bootstrap_depth_clear(const Env &e) {
    // The first Z16S page lies wholly inside the completed zero clear.
    // 0x35BE08 aliases it as CT32 before the icon upload partially updates
    // it. Keep the known zero CPU bytes current for both operations.
    const uint32_t page = e.zbp >> 5;
    const Surface *owner = g_owner[page];
    if (!owner || !owner->depth || owner->psm != PSMZ16S || owner->bp != e.zbp) return;
    constexpr uint32_t page_bytes = kVramSize / kPages;
    memset(vram + page * page_bytes, 0, page_bytes);
    g_vram_ok[page] = true;
    g_cpu_valid_blocks[page] = ~0u;
}

void native_end() {
    g_ctx->barrier(2);
    g_ctx->boundPipeline = VK_NULL_HANDLE;
    g_ctx->dynamicValid = g_ctx->imagesValid = g_ctx->constantsValid = g_ctx->vertexValid = false;
    unbind_all();
    g_bound_rt = g_bound_ds = nullptr;
    g_state_valid = false;
}

// true: GS memory pages [first, first + count) are current in the CPU copy (no render target
// holds newer contents), so a texture there can be decoded from gs::vram.
bool native_texture_on_cpu(uint32_t first, uint32_t count) {
    for (uint32_t i = 0; i < count && i < kPages; i++) {
        const uint32_t p = (first + i) % kPages;
        if (g_owner[p] && !g_vram_ok[p]) return false;
    }
    return true;
}

bool native_texture_region_on_cpu(const Env &e) {
    const auto coverage = page_coverage(e.tbp, e.tbw, e.tpsm, 0, 0, e.tw, e.th);
    for (uint32_t i = 0; i < coverage.count; ++i) {
        const uint32_t page = coverage.pages[i];
        const uint32_t touched = coverage.touched[page];
        const auto *deferred = g_deferred[page];
        if ((deferred && (touched & deferred->pending_blocks)) ||
            (g_owner[page] && !g_vram_ok[page] && (touched & ~g_cpu_valid_blocks[page])))
            return false;
    }
    return true;
}

const hosttex::Store *native_host_textures() {
    return g_host_on ? &g_host : nullptr;
}

#endif
