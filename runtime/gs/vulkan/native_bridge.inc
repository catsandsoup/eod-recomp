// native_bridge.inc - hands the game's frame and depth buffer surfaces to the native renderer
// (runtime/render/native_renderer.cpp), included at the end of gs_vulkan.cpp inside gs::hw.
// The native renderer records its plume commands into the backend's command buffer at this point
// of GS command order, so its objects and the GS's own draws share surfaces, depth and order.
// Nothing here draws: it ends the backend's work so far, takes ownership of the surfaces' pages
// (uploading what the CPU copy holds newer, as a GS draw would) and afterwards forgets the
// backend's bound state.
#ifdef EOD_PLUME

bool native_begin(const Env &e, bool want_depth, NativeSurfaces &out) {
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
    g_ctx->barrier(2);
    const int cols = std::min((int)e.fbw * 64, rt->w);
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
    return true;
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

#endif
