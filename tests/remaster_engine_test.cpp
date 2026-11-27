// tests/remaster_engine_test.cpp
// Unit tests for the zero-VU/GS Halo 2 Anniversary Remaster engine:
//   - Decompiled EE SceneEnvironment extraction (0x4CD2E8, 0x3DBB3C, 0x3DBB38, 0x3DBB40,
//     0x4CD478, 0x3DC064, 0x4D8B80, 0x4D8B84..8C, 0x3DBF68 + 0x3DBB48)
//   - World Editor editor_environment::Override layering (confirming shadows stay ENABLED!)
//   - EE SceneCamera extraction (0x3E57A8 / 0x3E57CC / 0x4D7C10..2C)
//   - Smooth normal welding & tangent generation
//   - Halo 2 Anniversary AssetRegistry HD .obj injection & retail fallback
//   - 3-Cascade CSM math & Khronos PBR Neutral tone mapper
#include "remaster/asset_registry.h"
#include "remaster/graf_loader.h"
#include "remaster/remaster_renderer.h"
#include "remaster/scene_bridge.h"
#include "render/editor_environment.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

// Provide g_ram symbol for standalone test linking against scene_bridge.cpp
uint8_t g_ram[32 * 1024 * 1024] = {};

namespace {

void wr_u32(std::vector<uint8_t> &ram, uint32_t addr, uint32_t v) {
    std::memcpy(ram.data() + addr, &v, 4);
}
void wr_f32(std::vector<uint8_t> &ram, uint32_t addr, float v) {
    std::memcpy(ram.data() + addr, &v, 4);
}

bool near_f(float a, float b, float eps = 1e-3f) {
    return std::fabs(a - b) <= eps;
}

void test_ee_environment_extraction() {
    std::vector<uint8_t> ram(6 * 1024 * 1024, 0);

    // Populate decompiled EE globals (day track: 0x4CD2E8 stores DOWNWARD light-travel ray with +Y up)
    wr_f32(ram, remaster::ee_addr::kSunDir + 0, -0.3f);
    wr_f32(ram, remaster::ee_addr::kSunDir + 4, -0.8f); // light travels downward (-Y)
    wr_f32(ram, remaster::ee_addr::kSunDir + 8,  0.5f);
    wr_u32(ram, remaster::ee_addr::kSunColor, 0x00FDFEE7u);
    wr_f32(ram, remaster::ee_addr::kSunIntensity, 0.9f);
    wr_u32(ram, remaster::ee_addr::kSunIsDay, 1u);
    wr_u32(ram, remaster::ee_addr::kAmbientColor, 0xFF909090u);
    wr_u32(ram, remaster::ee_addr::kShadowAlpha, 0x80u);
    wr_u32(ram, remaster::ee_addr::kFogColor, 0x00EBC9C9u); // BBGGRR
    wr_f32(ram, remaster::ee_addr::kFogRange + 0, 120.0f);
    wr_f32(ram, remaster::ee_addr::kFogRange + 4, 800.0f);
    wr_f32(ram, remaster::ee_addr::kFogRange + 8, 0.45f);

    const auto env = remaster::extract_environment_from_ram(ram.data(), (uint32_t)ram.size(), nullptr);
    assert(env.is_day);
    assert(!env.editor_override_active);
    assert(env.sun_dir[1] > 0.5f); // unit vector TOWARD sun (+Y is UP)
    assert(near_f(env.shadow_strength, 1.0f));
    assert(near_f(env.fog_start, 120.0f));
    assert(near_f(env.fog_end, 800.0f));
    assert(near_f(env.fog_density, 0.45f));
    // Neutral daylight hemisphere fill: no blue shadow cast (B/R ratio <= 1.22)
    assert(env.ambient_sky[0] > 0.35f && env.ambient_sky[2] / env.ambient_sky[0] < 1.22f);
    assert(env.ambient_ground[0] > env.ambient_ground[2]); // neutral warm-earth ground bounce

    // Apply World Editor override and verify shadows stay ENABLED (shadow_strength == 1.0f)
    editor_environment::Override ov{};
    ov.track = "ANOKA";
    ov.sun_dir = {0.4f, 0.8f, -0.4f};
    ov.sun_color = {1.0f, 0.9f, 0.7f};
    ov.fog_color = {0.5f, 0.6f, 0.8f};
    ov.ambient_intensity = 0.75f;
    ov.fog_start = 200.0f;
    ov.fog_end = 1200.0f;
    ov.fog_density = 0.25f;

    const auto env_ov = remaster::extract_environment_from_ram(ram.data(), (uint32_t)ram.size(), &ov);
    assert(env_ov.editor_override_active);
    assert(near_f(env_ov.shadow_strength, 1.0f));
    assert(near_f(env_ov.ambient_intensity, 0.75f));
    assert(near_f(env_ov.fog_start, 200.0f));

    // Verify FlareSource stadium floodlight extraction (pos at +0x04, +0x08, +0x0C; +Y is UP)
    wr_u32(ram, remaster::ee_addr::kFlareCount, 2u);
    wr_f32(ram, remaster::ee_addr::kFlareArray + 0x04, 10.0f);
    wr_f32(ram, remaster::ee_addr::kFlareArray + 0x08, 20.0f);
    wr_f32(ram, remaster::ee_addr::kFlareArray + 0x0C, 15.0f);
    wr_f32(ram, remaster::ee_addr::kFlareArray + 0x2C + 0x04, -10.0f);
    wr_f32(ram, remaster::ee_addr::kFlareArray + 0x2C + 0x08, 20.0f);
    wr_f32(ram, remaster::ee_addr::kFlareArray + 0x2C + 0x0C, -15.0f);
    std::vector<remaster::DynamicLight> lights;
    remaster::extract_flare_lights_from_ram(ram.data(), (uint32_t)ram.size(), false, lights);
    assert(lights.size() == 2);
    assert(lights[0].dir[1] < 0.0f); // aimed downward (-Y) toward track
    assert(lights[0].radius >= 150.0f);
    assert(lights[0].spot_cos < 0.0f && lights[0].spot_cos > -0.5f); // wide hemispherical downward spread
    assert(lights[0].color[2] >= lights[0].color[0]); // cool-neutral metal halide white (zero orange cast)

    // Verify night ambient & direct session event mask (0x495BE0 + 0x3C = 0x495C1C, struct directly)
    wr_u32(ram, remaster::ee_addr::kSunIsDay, 0u);
    const auto night_env = remaster::extract_environment_from_ram(ram.data(), (uint32_t)ram.size(), nullptr);
    assert(!night_env.is_day);
    assert(night_env.ambient_intensity >= 0.8f);
    assert(night_env.sun_intensity >= 0.7f);
    assert(night_env.sun_color[2] >= night_env.sun_color[0]);
}

void test_smooth_normals_and_csm_and_tonemap() {
    // Two triangles sharing an edge with duplicated vertices (simulating PS2 strip seam)
    std::vector<remaster::PbrVertex> verts(6);
    verts[0].pos[0] = 0; verts[0].pos[1] = 0; verts[0].pos[2] = 0; verts[0].uv[0] = 0; verts[0].uv[1] = 0;
    verts[1].pos[0] = 1; verts[1].pos[1] = 0; verts[1].pos[2] = 0; verts[1].uv[0] = 1; verts[1].uv[1] = 0;
    verts[2].pos[0] = 0; verts[2].pos[1] = 0; verts[2].pos[2] = 1; verts[2].uv[0] = 0; verts[2].uv[1] = 1;
    verts[3].pos[0] = 1; verts[3].pos[1] = 0; verts[3].pos[2] = 0; verts[3].uv[0] = 1; verts[3].uv[1] = 0;
    verts[4].pos[0] = 1; verts[4].pos[1] = 0.5f; verts[4].pos[2] = 1; verts[4].uv[0] = 1; verts[4].uv[1] = 1;
    verts[5].pos[0] = 0; verts[5].pos[1] = 0; verts[5].pos[2] = 1; verts[5].uv[0] = 0; verts[5].uv[1] = 1;
    std::vector<uint32_t> idx = {0, 1, 2, 3, 4, 5};
    remaster::compute_smooth_normals_and_tangents(verts, idx);
    // Coincident vertices 1 and 3 must receive the exact same welded smooth normal
    for (int k = 0; k < 3; ++k) assert(near_f(verts[1].normal[k], verts[3].normal[k], 1e-4f));

    // PBR material ranges: demolition derby paint must be 0.60..0.85, dirt 0.85..0.95, glass/chrome 0.05..0.20
    const auto paint = remaster::classify_pbr_material("car_body_paint", false, true);
    assert(paint.roughness >= 0.60f && paint.roughness <= 0.85f);
    const auto dirt = remaster::classify_pbr_material("an-grs02_dirt", false, false);
    assert(dirt.roughness >= 0.88f && dirt.roughness <= 0.95f && dirt.metallic == 0.0f);
    // Unmatched static world textures (even with 'detail' or 'car' substrings) MUST stay strictly matte (0.92)
    const auto world_unmatched = remaster::classify_pbr_material("an_infield_detail_car", false, false);
    assert(world_unmatched.roughness >= 0.88f && world_unmatched.metallic == 0.0f);
    // Static spotlight cones / bridge glow lights must classify as additive
    const auto beam_cone = remaster::classify_pbr_material("an-lite_cone", false, false);
    assert(beam_cone.alpha_additive);
    const auto chrome = remaster::classify_pbr_material("chrome_bumper", false, true);
    assert(chrome.roughness >= 0.05f && chrome.roughness <= 0.20f);

    // 3-Cascade CSM math (maximum quality covers >= 500m horizon with zero widescreen edge cutoff)
    remaster::SceneCamera cam{};
    cam.pos[0] = 10.0f; cam.pos[1] = -4.0f; cam.pos[2] = 20.0f;
    remaster::SceneEnvironment env{};
    const auto csm = remaster::compute_csm_cascades(cam, env, 2048, true);
    assert(csm.splits[0] < csm.splits[1] && csm.splits[1] < csm.splits[2]);
    assert(csm.splits[2] >= 500.0f);
    for (int c = 0; c < 3; ++c) {
        assert(std::isfinite(csm.view_proj[c][0]));
        assert(near_f(csm.view_proj[c][15], 1.0f));
    }

    // Khronos PBR Neutral tone mapper: preserves midtones, compresses HDR highlights smoothly into [0..1]
    const auto mid = remaster::khronos_pbr_neutral_cpu(0.3f, 0.3f, 0.3f);
    assert(mid[0] > 0.2f && mid[0] < 0.35f);
    const auto bright = remaster::khronos_pbr_neutral_cpu(8.0f, 4.0f, 1.0f);
    assert(bright[0] > 0.85f && bright[0] < 1.0f);
    assert(bright[0] >= bright[1] && bright[1] >= bright[2]);

    // Reversed-Z camera projection: near_z -> 1.0, far_z -> 0.0 for high D32_FLOAT precision
    remaster::SceneCamera rcam{};
    rcam.near_z = 0.35f;
    rcam.far_z = 9200.0f;
    remaster::rebuild_camera_projection(rcam, 1280, 720);
    const float z_near_ndc = (rcam.proj[10] * rcam.near_z + rcam.proj[11]) / rcam.near_z;
    const float z_far_ndc  = (rcam.proj[10] * rcam.far_z  + rcam.proj[11]) / rcam.far_z;
    assert(near_f(z_near_ndc, 1.0f, 1e-5f));
    assert(near_f(z_far_ndc, 0.0f, 1e-5f));
}

void test_asset_registry_injection() {
    auto &reg = remaster::AssetRegistry::instance();
    reg.clear();
    const std::string tmp_root = "/tmp/eod_remaster_test_assets";
    std::system(("mkdir -p " + tmp_root + "/car/derby").c_str());
    {
        std::ofstream obj(tmp_root + "/car/derby/hood.obj");
        obj << "v 0 0 0\nv 2 0 0\nv 0 0 2\nf 1 2 3\n";
    }
    reg.set_remaster_root(tmp_root);

    remaster::CpuMeshData retail_dummy{};
    retail_dummy.asset_id = "car/derby/hood";
    retail_dummy.texture_id = "tex/derby_paint";
    const uint32_t h = reg.register_mesh("car/derby/hood", retail_dummy);
    auto loaded = reg.get_mesh(h);
    assert(loaded != nullptr);
    assert(loaded->vertices.size() == 3); // Loaded from HD .obj override!
    assert(loaded->indices.size() == 3);
    assert(near_f(loaded->vertices[1].pos[0], 2.0f));

    // Verify authored material_table.inc is loaded directly into AssetRegistry (3,000+ entries)
    assert(remaster::AssetRegistry::material_table_entry_count() >= 3000u);

    // Fallback test when no HD file exists
    auto tex = reg.resolve_texture("tex/an-road1");
    assert(tex != nullptr && !tex->has_hd_override && !tex->albedo.pixels.empty());
    reg.clear();
}

} // namespace

int main() {
    test_ee_environment_extraction();
    test_smooth_normals_and_csm_and_tonemap();
    test_asset_registry_injection();
    std::printf("remaster_engine_test: ALL PASSED\n");
    return 0;
}
