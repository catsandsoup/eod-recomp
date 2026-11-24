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

// A full physical page has no surviving texels after a clear. Discard its
// previous owner and any pending readback before recording the overwrite.
void claim_full_clear_pages(Surface *surface, const PageCoverage &cov) {
    bool claimed = false;
    for (uint32_t i = 0; i < cov.count; ++i) {
        const uint32_t page = cov.pages[i];
        if (cov.complete[page] != 0xffffffffu) continue;
        if (g_deferred[page]) {
            deferred_free(page);
            g_dstats.overwritten++;
        }
        if (g_owner[page] != surface) {
            g_owner[page] = surface;
            claimed = true;
        }
        g_vram_ok[page] = false;
        g_cpu_valid_blocks[page] = 0;
    }
    if (claimed) surface->ownership = ++g_surface_ownership;
}

bool native_begin(const Env &e, bool want_depth, NativeSurfaces &out, bool require_no_readback, const int *bounds) {
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
    const int x0 = bounds ? bounds[0] : 0, y0 = bounds ? bounds[1] : 0;
    const int x1 = bounds ? bounds[2] : cols, y1 = bounds ? bounds[3] : rows;
    if (bounds && (x1 > cols || y1 > rt->h || (ds && y1 > ds->h))) return false;
    if (require_no_readback) {
        if (want_depth && !ds) return false;
        std::vector<PageRect> pages;
        for (Surface *surface : {rt, ds}) {
            if (!surface) continue;
            surface_pages_rect(surface, x0, y0, x1, y1, pages);
            for (const auto &page : pages) {
                const Surface *owner = g_owner[page.page];
                if (owner != surface && ((owner && !g_vram_ok[page.page]) || g_deferred[page.page]))
                    return false;
            }
        }
    }
    g_ctx->barrier(2);
    const uint64_t rt_own0 = rt->ownership;
    const uint64_t ds_own0 = ds ? ds->ownership : 0;
    if (g_native_scene_clear_active && !require_no_readback && cols >= 640 && rows >= 448) {
        // gs_clear_scene overwrites every pixel in [0,640)x[0,448) of rt and ds via
        // clearColor/clearDepth. Any physical page 100% covered by this clear does not
        // need prior CPU bytes uploaded or deferred GPU readbacks resolved.
        claim_full_clear_pages(rt, page_coverage(e.fbp, e.fbw, e.fpsm_mem, 0, 0, 640, 448));
        if (ds) claim_full_clear_pages(ds, page_coverage(e.zbp, e.fbw, e.zpsm, 0, 0, 640, 448));
    }
    acquire(rt, x0, y0, x1, std::min(y1, rt->h));
    rt->version++;
    if (ds) {
        acquire(ds, x0, y0, x1, std::min(y1, ds->h));
        ds->version++;
    }
    if (rt->ownership != rt_own0 || (ds && ds->ownership != ds_own0))
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

bool native_cached_targets(const Env &e, uint64_t &color, uint64_t &depth, bool check_ownership) {
    if (!e.fbw || e.scy1 < 0) return false;
    const int rows = e.scy1 + 1;
    const int allocation_rows = rows <= 512 ? 512 : rows <= 1024 ? 1024 : 2048;
    const Surface *rt = find_surface(e.fbp, e.fbw, e.fpsm_mem, false);
    const Surface *ds = find_surface(e.zbp, e.fbw, e.zpsm, true);
    if (!rt || !ds || !rt->tex || !ds->tex || rt->w != ds->w || rt->h != ds->h ||
        rt->h < allocation_rows || e.scx1 >= rt->w || e.scy1 >= rt->h)
        return false;
    if (check_ownership) {
#ifndef EOD_R7_RELEASE
        if (!g_verts.empty() || !g_ctx->cmd) return false;
#else
        if (!g_ctx->cmd) return false;
#endif
        std::vector<PageRect> pages;
        for (const Surface *target : {rt, ds}) {
            surface_pages_rect(target, 0, 0, std::min(int(e.fbw * 64), target->w), rows, pages);
            for (const auto &page : pages)
                if (g_owner[page.page] != target || g_deferred[page.page]) return false;
        }
    }
    color = (uint64_t)rt->tex->image;
    depth = (uint64_t)ds->tex->image;
    return color && depth;
}

bool native_begin_depth_clear(const Env &e, NativeSurfaces &out, const int *bounds) {
    const int x0 = bounds ? bounds[0] : 0, y0 = bounds ? bounds[1] : 0;
    const int x1 = bounds ? bounds[2] : 640, y1 = bounds ? bounds[3] : 448;
    if (g_skip_frame || (!bounds && g_scale > 1 && e.scy1 != 447)) return false;
    Surface *ds = get_surface(e.zbp, e.fbw, e.zpsm, true, y1);
    if (!ds || !ds->dsv || x1 > ds->w || y1 > ds->h) return false;

    // Discard an old owner only where this clear overwrites the entire physical page.
    // For each boundary page, acquire its surviving texels from CPU or GPU residency.
    const auto cov = page_coverage(e.zbp, e.fbw, e.zpsm, x0, y0, x1 - x0, y1 - y0);
    for (uint32_t i = 0; i < cov.count; ++i) {
        const auto page = cov.pages[i];
        const auto *owner = g_owner[page];
        if (cov.complete[page] == 0xffffffffu || owner == ds) continue;
        if (!bounds || g_deferred[page] || (owner && !g_vram_ok[page] && !convertible(owner, ds)))
            return false;
    }
    flush();
    g_ctx->barrier(2);
    claim_full_clear_pages(ds, cov);
    acquire(ds, x0, y0, x1, y1);
    ++ds->version;
    ds->last_use = g_frame_no;
    out = {(void *)g_ctx->cmd, 0, (uint64_t)ds->tex->image, (uint32_t)(ds->w * g_scale), (uint32_t)(ds->h * g_scale),
           (uint32_t)ds->w, (uint32_t)ds->h, g_scale, 0, ds->ownership};
    return true;
}

void native_mark_bootstrap_depth_clear(const Env &e) {
    // Every full Z16S page inside the completed 640x418 zero clear is known
    // zero on both GPU and CPU. Early boot uploads (e.g. 0x35BE08 icon and
    // font/UI textures) alias these pages before any scene draw touches them;
    // marking all fully cleared pages current avoids spurious deferred readbacks.
    const uint32_t first_page = e.zbp >> 5;
    const Surface *owner = g_owner[first_page];
    if (!owner || !owner->depth || owner->psm != PSMZ16S || owner->bp != e.zbp) return;
    const PageCoverage cov = page_coverage(e.zbp, e.fbw, e.zpsm, 0, 0, 640, 418);
    constexpr uint32_t page_bytes = kVramSize / kPages;
    for (uint32_t i = 0; i < cov.count; ++i) {
        const uint32_t page = cov.pages[i];
        if (cov.complete[page] != 0xffffffffu || g_owner[page] != owner) continue;
        memset(vram + page * page_bytes, 0, page_bytes);
        g_vram_ok[page] = true;
        g_cpu_valid_blocks[page] = ~0u;
    }
}

NativeSubmission native_submission() {
    return g_ctx ? NativeSubmission{g_ctx->serial, g_ctx->completed} : NativeSubmission{};
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
