// native_modern.hlsl - 2026 Remaster Linear-HDR PBR & Automotive Shading Engine.
// Compiled by tools/build_native_shaders.py into native_modern_spv.inc.
// Pairs 1:1 with native_post.hlsl (GTAO + Contact Shadows + Volumetric Atmo + 5-Mip HDR Bloom + AgX).
//
// Push-constant layout (filled by native_renderer.cpp modern_patch):
//   mode.x      GS flags + M_HUD_2D (0x1000: bypass HDR encoding for 2D HUD) + M_EMISSIVE_HDR (0x2000)
//   mode.z      bits 16..19 metalness (4b), 20..23 roughness (4b), 24 emissive, 25 normal from height,
//               26 has material record, 27..30 class, 31 authored environment
//   light.xyz   model-space light direction (lit: VU1 vf12; prelit: frame sun), diffuse.rgb colour (0..128)
//   light.w     sky upper colour (RGBA8), diffuse.w sky horizon colour (RGBA8)
//   mode.w      model-space up (octahedral 12+12 bits); bits 24..25 stock surface tag; bit 26 mapped HTX
//   ambient.w   M_SPEC draws: ITOP alpha (0..7) + game half vector (8..31 octahedral)

struct Object {
    float4 cx, cy, cz, cw;
    float4 light;
    float4 diffuse;
    float4 ambient;
    uint4 mode;
};
[[vk::push_constant]] Object obj;

#define M_TME 0x1
#define M_TCC 0x2
#define M_TFX_SHIFT 2
#define M_ATE 0x10
#define M_ATST_SHIFT 5
#define M_AFAIL_DISCARD 0x100
#define M_CFIX 0x200
#define M_SPEC 0x400
#define M_FGE 0x800
#define M_HUD_2D 0x1000u
#define M_EMISSIVE_HDR 0x2000u
#define M_STORE_ZERO_A 0x4000u
#define M_DISCARD_ZERO_A 0x8000u
#define M_SKY_FAR 0x10000u
#define M_FAILED 0x80000000u

#define C_GENERIC    0
#define C_UNLIT      1
#define C_PAINT      2
#define C_GLASS      3
#define C_WATER      4
#define C_CHROME     5
#define C_VFX_FIRE   6
#define C_VFX_SMOKE  7
#define C_VFX_SPARK  8
#define C_FOLIAGE    9
#define C_GRASS_HAY  10
#define C_MASONRY    11
#define C_WOOD       12
#define C_RUST_METAL 13
#define C_INTERIOR   14

#define VFX_VOLUME_ACTIVE 0x80000000u
#define VFX_FIRE    1
#define VFX_SMOKE   2
#define VFX_EXHAUST 3
#define VFX_EMBER   4

[[vk::binding(0, 0)]] Texture2D tex;
[[vk::binding(1, 0)]] SamplerState smp;
[[vk::binding(2, 0)]] Texture2D<float4> stock_mask;
[[vk::binding(0, 1)]] cbuffer AuthoredEnvironment {
    float4 authored_fog_color;
    float4 environment_params;
};

[[vk::binding(0, 2)]] Texture2D<float4> scene_color;
[[vk::binding(1, 2)]] Texture2D<float> scene_depth;
[[vk::binding(2, 2)]] SamplerState scene_sampler;
[[vk::binding(3, 2)]] cbuffer SceneInputs {
    float4 scene_view;
    float4 scene_grid;
};

struct VIn {
    [[vk::location(0)]] float3 p : POSITION;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] int4 n : NORMAL;
    [[vk::location(3)]] float4 c : COLOR;
};
struct VOut {
    float4 pos : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
    [[vk::location(1)]] float4 c : COLOR;
    [[vk::location(2)]] float f : FOG;
    [[vk::location(3)]] float3 mp : TEXCOORD1;
    [[vk::location(4)]] float3 mv : TEXCOORD2;
    [[vk::location(5)]] float4 mn : TEXCOORD3;
};

float3 model_eye() {
    const float3 a = obj.cx.xyz, b = obj.cy.xyz, c = obj.cw.xyz;
    const float3 r = -float3(obj.cx.w, obj.cy.w, obj.cw.w);
    const float3 bc = cross(b, c), ca = cross(c, a), ab = cross(a, b);
    const float det = dot(a, bc);
    if (abs(det) < 1e-20) return float3(0, 0, 0);
    return (r.x * bc + r.y * ca + r.z * ab) / det;
}

float3 oct_decode(uint v) {
    float2 e = float2(v & 0xFFF, (v >> 12) & 0xFFF) / 4095.0 * 2.0 - 1.0;
    float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0) n.xy = (1.0 - abs(n.yx)) * float2(n.x >= 0 ? 1 : -1, n.y >= 0 ? 1 : -1);
    return normalize(n);
}

VOut vs_modern(VIn i) {
    VOut o;
    float4 q = float4(i.p, 1);
    o.pos = float4(dot(obj.cx, q), dot(obj.cy, q), dot(obj.cz, q), dot(obj.cw, q));
    if (obj.mode.x & M_SKY_FAR) {
        // Pin sky dome right in front of reversed-Z far clear plane so it never receives CSM shadows or GTAO!
        o.pos.z = 1.0e-6 * o.pos.w;
    }
    float3 rgb;
    if (i.n.w != 0) {
        float3 n = float3(i.n.xyz) / 128.0;
        float d = max(0.0, dot(n, obj.light.xyz));
        rgb = min(obj.ambient.rgb + d * obj.diffuse.rgb, 128.0);
    } else {
        rgb = i.c.rgb * 255.0;
    }
    float va = obj.ambient.w < 0.0 ? round(i.c.a * 255.0) : obj.ambient.w;
    if (obj.mode.x & M_SPEC) {
        const uint pk = asuint(obj.ambient.w);
        va = (float)(pk & 0xFF);
        if (i.n.w != 0) va = max(0.0, 1152.0 * dot(float3(i.n.xyz) / 128.0, oct_decode(pk >> 8)) - 1056.0);
    }
    o.c = floor(float4(rgb, va));
    o.f = (float)((obj.mode.z >> 8) & 0xFF);
    o.uv = i.uv;
    o.mp = i.p;
    o.mv = model_eye() - i.p;
    const float3 mn_raw = float3(i.n.xyz) / 127.0;
    const float has_smooth_n = (dot(mn_raw, mn_raw) > 0.05) ? -1.0 : 0.0;
    o.mn = float4(mn_raw, i.n.w != 0 ? 1.0 : has_smooth_n);
    if ((obj.mode.w & VFX_VOLUME_ACTIVE) && ((obj.mode.z >> 27) & 15) == C_VFX_SPARK &&
        ((obj.mode.z >> 22) & 7) == VFX_EMBER)
        o.mn = float4(0.0, 0.0, i.n.x > 0 ? float(i.n.x - 1) / 126.0 : -1.0, 0.0);
    return o;
}

struct EffectIn {
    [[vk::location(0)]] float4 center_radius : POSITION;
    [[vk::location(1)]] float4 uv_rect : TEXCOORD0;
    [[vk::location(2)]] float2 angle_shade : TEXCOORD1;
    [[vk::location(3)]] float4 color : COLOR;
};

VOut vs_vfx_billboard(EffectIn i, uint vertex : SV_VertexID) {
    const uint corners[6] = {0, 1, 2, 1, 3, 2};
    const uint corner = corners[vertex];
    const float2 uv = float2(corner & 1, corner >> 1);
    const float2 p = uv * 2.0 - 1.0;
    const float2 axes = float2(p.x, obj.light.w != 0.0 ? p.y : -p.y);
    float s, c;
    sincos(i.angle_shade.x, s, c);
    const float3 right = obj.light.xyz, up = obj.diffuse.xyz;
    const float3 local = axes.x * (c * right - s * up) + axes.y * (s * right + c * up);
    const float shade = saturate(1.0 - i.angle_shade.y * (1.0 - clamp(local.y, -1.0, 1.0)));
    VIn v;
    v.p = i.center_radius.xyz + local * i.center_radius.w;
    v.uv = lerp(i.uv_rect.xy, i.uv_rect.zw, uv);
    v.n = 0;
    v.c = float4(min(i.color.rgb * shade, 128.0 / 255.0), i.color.a);
    VOut o = vs_modern(v);
    o.c.a = round(i.color.a * 255.0);
    o.mn = float4(abs(i.center_radius.w), p, 0.0);
    return o;
}

struct POut {
    [[vk::location(0)]] float4 color : SV_Target0;
    [[vk::location(0), vk::index(1)]] float4 factor : SV_Target1;
};

bool alpha_pass(float a, uint atst, float aref) {
    switch (atst) {
    case 0: return false;
    case 1: return true;
    case 2: return a < aref;
    case 3: return a <= aref;
    case 4: return a == aref;
    case 5: return a >= aref;
    case 6: return a > aref;
    default: return a != aref;
    }
}

static const float PI = 3.14159265359;

// Color space & reversible HDR perceptual encoding (paired with native_post.hlsl hdr_decode)
float3 srgb_to_linear(float3 c) {
    return pow(max(c, 0.0), 2.2);
}
float3 hdr_encode(float3 lin) {
    // Khronos PBR Neutral + linear-to-sRGB directly (zero dark post-process dependency).
    float3 c = max(lin, 0.0);
    const float start_compression = 0.76;
    const float desaturation = 0.15;
    const float x = min(c.r, min(c.g, c.b));
    const float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    c -= offset;
    const float peak = max(c.r, max(c.g, c.b));
    if (peak >= start_compression) {
        const float d = 1.0 - start_compression;
        const float new_peak = 1.0 - d * d / (peak + d - start_compression);
        c *= new_peak / peak;
        const float g = 1.0 - 1.0 / (desaturation * (peak - new_peak) + 1.0);
        c = lerp(c, float3(new_peak, new_peak, new_peak), g);
    }
    c = saturate(c);
    const float3 lo = c * 12.92;
    const float3 hi = 1.055 * pow(c, 1.0 / 2.4) - 0.055;
    return float3(c.r <= 0.0031308 ? lo.r : hi.r,
                  c.g <= 0.0031308 ? lo.g : hi.g,
                  c.b <= 0.0031308 ? lo.b : hi.b);
}
float3 hdr_decode(float3 s) {
    const float3 c = saturate(s);
    const float3 y = c * c;
    return 0.85 * y / max(1.0 - y, 0.022);
}

// Physically-Based BRDF (Filament / Unreal Engine 5 standard model)
float D_GGX(float NoH, float r) {
    const float a = NoH * r;
    const float k = r / max(1.0 - NoH * NoH + a * a, 1e-7);
    return k * k * (1.0 / PI);
}
float V_SmithGGXCorrelated(float NoV, float NoL, float r) {
    const float gv = NoL * (NoV * (1.0 - r) + r);
    const float gl = NoV * (NoL * (1.0 - r) + r);
    return 0.5 / max(gv + gl, 1e-7);
}
float pow5(float x) {
    const float x2 = x * x;
    return x2 * x2 * x;
}
float3 F_Schlick(float3 f0, float VoH) { return f0 + (1.0 - f0) * pow5(saturate(1.0 - VoH)); }
float F_Schlick1(float f0, float VoH) { return f0 + (1.0 - f0) * pow5(saturate(1.0 - VoH)); }
float V_Kelemen(float LoH) { return 0.25 / max(LoH * LoH, 1e-4); }

float2 env_dfg(float NoV, float r) {
    const float4 c0 = float4(-1, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1, 0.0425, 1.04, -0.04);
    const float4 t = r * c0 + c1;
    const float a004 = min(t.x * t.x, exp2(-9.28 * NoV)) * t.x + t.y;
    return float2(a004 * -1.04 + t.z, a004 * 1.04 + t.w);
}

float3 unpack_rgb(uint v) { return float3(v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF) / 255.0; }

// Fast 3D procedural hash & value noise for micro-detail normals, metallic flake & track breakup
float hash31(float3 p) {
    p = frac(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return frac((p.x + p.y) * p.z);
}
float noise3(float3 p) {
    const float3 i = floor(p);
    const float3 f = frac(p);
    const float3 u = f * f * (3.0 - 2.0 * f);
    return lerp(
        lerp(lerp(hash31(i + float3(0, 0, 0)), hash31(i + float3(1, 0, 0)), u.x),
             lerp(hash31(i + float3(0, 1, 0)), hash31(i + float3(1, 1, 0)), u.x), u.y),
        lerp(lerp(hash31(i + float3(0, 0, 1)), hash31(i + float3(1, 0, 1)), u.x),
             lerp(hash31(i + float3(0, 1, 1)), hash31(i + float3(1, 1, 1)), u.x), u.y),
        u.z);
}

// ---------------------------------------------------------------------------
// 2026 UE5 / Wreckfest 2 Procedural HDR Environment Reflection Probe (IBL + Grazing Fresnel)
// Crisp sky dome, cumulus cloud bands, golden low-sun Mie horizon, warm dirt-track bounce,
// and elevatedhorizon break so curved roofs/hoods/fenders gleam like real sheet metal while
// vertical rear trunks reflect dark track/walls (ZERO plastic rear-trunk glare!).
// ---------------------------------------------------------------------------
float3 env_radiance(float3 dir, float3 up, float3 L, float rough, float3 zen_lin, float3 hor_lin, float daylight) {
    const float h = dot(dir, up);
    const float r2 = rough * rough;

    // --- DAYLIGHT: Clean Crisp Cobalt-Azure Sky + Cumulus Clouds + Neutral-Natural Horizon (ZERO yellow piss filter!) ---
    const float k = saturate(pow(saturate((h - 0.04) / 0.96), lerp(0.52, 1.05, rough)));
    const float3 day_zen = float3(0.14, 0.32, 0.68);
    const float3 day_hor = lerp(float3(0.68, 0.76, 0.88), hor_lin * 1.15 + float3(0.18, 0.20, 0.24), 0.35);
    float3 day_sky = lerp(day_hor, day_zen, k);

    // Cumulus cloud bands in the upper sky (gives curved car roofs & windshields unmistakable depth!)
    const float2 cloud_uv = dir.xz / max(h + 0.18, 0.18);
    const float c1 = noise3(float3(cloud_uv * 1.4, 0.5));
    const float c2 = noise3(float3(cloud_uv * 3.1 + 4.2, 1.2));
    const float cloud = smoothstep(0.46, 0.78, c1 * 0.65 + c2 * 0.35) * saturate((h - 0.08) * 3.2) * (1.0 - rough * 0.72);
    day_sky = lerp(day_sky, float3(1.35, 1.38, 1.42), cloud * 0.65);

    // Crisp Solar Disk in upper sky reflections (only sharp on smooth clearcoat/chrome/glass!)
    const float cos_sun = saturate(dot(dir, L));
    const float sun_exp = lerp(240.0, 14.0, r2);
    const float sun_disk = pow(cos_sun, sun_exp) * lerp(5.5, 0.15, rough);
    const float sun_halo = pow(cos_sun, 10.0) * lerp(0.55, 0.10, rough);
    const float3 day_env_up = day_sky + float3(1.25, 1.18, 1.05) * (sun_disk + sun_halo) * smoothstep(0.04, 0.25, h);

    // Neutral earthy ground bounce + dark track/barrier occlusion up to waist-level horizon (h ~ 0.10):
    // Prevents vertical rear trunks (h ~ 0) from reflecting bright sky whitewash while roofs/hoods gleam!
    const float3 day_ground = lerp(float3(0.18, 0.15, 0.13), float3(0.04, 0.04, 0.04), saturate(-h * 3.0 + 0.25));
    const float hor_break = smoothstep(lerp(0.06, -0.08, rough), lerp(0.24, 0.44, rough), h);
    const float3 day_env = lerp(day_ground, day_env_up, hor_break);

    // --- NIGHT (ANOKA / HASTINGS): Deep Inky Sky + High-Intensity Stadium Floodlight Banks ---
    const float3 night_zen = float3(0.005, 0.010, 0.026);
    const float3 night_hor = float3(0.040, 0.052, 0.082);
    float3 night_up = lerp(night_hor, night_zen, saturate(h * 2.2));
    const float elev = saturate(1.0 - abs(h - 0.42) * lerp(7.0, 2.2, rough));
    const float azim = atan2(dir.z, dir.x);
    const float banks = pow(0.5 + 0.5 * cos(azim * 8.0), lerp(10.0, 2.0, rough));
    const float flood_lobe = pow(elev, 2.0) * banks;
    const float3 flood_col = lerp(float3(1.85, 1.42, 0.95), float3(1.25, 1.45, 1.85), 0.5 + 0.5 * sin(azim * 4.0));
    night_up += flood_col * flood_lobe * lerp(6.2, 0.55, rough);
    const float3 night_ground = lerp(float3(0.032, 0.022, 0.015), float3(0.008, 0.008, 0.010), saturate(-h * 3.0 + 0.2));
    const float3 night_env = lerp(night_ground, night_up, hor_break);

    return lerp(night_env, day_env, daylight);
}

// ---------------------------------------------------------------------------
// Screen-Space Reflections & Water Refraction (Upgraded 10-step + 3-step refine)
// ---------------------------------------------------------------------------
bool scene_inside(float2 pixel) {
    const float2 p = pixel - scene_view.xy;
    return all(p >= 0.0) && all(p < scene_view.zw);
}
float2 scene_uv(float2 pixel) { return (pixel - scene_view.xy) / scene_view.zw; }
float scene_depth_at(float2 pixel) {
    if (!scene_inside(pixel)) return 0.0;
    const int2 texel = min(int2(scene_uv(pixel) * scene_grid.xy), int2(scene_grid.xy) - 1);
    return scene_depth.Load(int3(texel, 0));
}
float scene_edge_fade(float2 pixel) {
    const float2 uv = scene_uv(pixel);
    const float2 edge = min(uv, 1.0 - uv);
    return saturate(min(edge.x, edge.y) * 18.0);
}
float2 scene_depth_mapping() {
    const float norm_w = max(length(obj.cw.xyz), 1e-8);
    const float b = dot(obj.cz.xyz, obj.cw.xyz) / (norm_w * norm_w);
    return float2(b, (obj.cz.w - b * obj.cw.w) / norm_w);
}
float scene_camera_distance(float raw_depth, float2 mapping) {
    return mapping.y / max(raw_depth - mapping.x, 1e-7);
}
float scene_camera_distance(float raw_depth) {
    return scene_camera_distance(raw_depth, scene_depth_mapping());
}
float4 scene_clip(float4 model) {
    return float4(dot(obj.cx, model), dot(obj.cy, model), dot(obj.cz, model), dot(obj.cw, model));
}
float4 scene_project(float4 clip) {
    if (clip.w <= 1e-6) return 0.0;
    const float3 ndc = clip.xyz / clip.w;
    return float4((ndc.xy * 0.5 + 0.5) * scene_grid.zw, ndc.z, clip.w);
}
bool scene_projected(float4 projected) {
    return projected.w > 0.0 && projected.z > 0.0 && projected.z < 1.0 && scene_inside(projected.xy);
}

float4 scene_reflection(VOut i, float3 ray, float roughness, float visibility) {
    if (roughness >= 0.85 || visibility <= 0.01) return 0.0;
    const float2 inv_view = 1.0 / scene_view.zw;
    const float2 origin_uv = (i.pos.xy - scene_view.xy) * inv_view;
    const float2 origin_edge = min(origin_uv, 1.0 - origin_uv);
    const float edge = saturate(min(origin_edge.x, origin_edge.y) * 18.0);
    const float strength = (1.0 - smoothstep(0.45, 0.85, roughness)) * visibility * edge;
    if (strength <= 0.01) return 0.0;
    const float2 mapping = scene_depth_mapping();
    const float surface_z = scene_camera_distance(i.pos.z, mapping);
    if (!(surface_z > 0.0 && surface_z < 60.0)) return 0.0;
    const float4 origin = scene_clip(float4(i.mp, 1.0));
    const float4 direction = scene_clip(float4(ray, 0.0));
    if (origin.w + direction.w * 0.85 <= 1e-6) return 0.0;
    float last_t = 0.0;
    const int2 grid_max = int2(scene_grid.xy) - 1;
    const float surface_scene_depth = scene_depth.Load(int3(clamp(int2(origin_uv * scene_grid.xy), 0, grid_max), 0));
    bool visible = !(surface_scene_depth > i.pos.z);
    [loop]
    for (int step = 1; step <= 6; ++step) {
        const float t = 0.28 + 0.52 * (step * step);
        float4 p = scene_project(origin + direction * t);
        if (!scene_projected(p)) break;
        const float2 p_uv = (p.xy - scene_view.xy) * inv_view;
        const float z = scene_depth.Load(int3(min(int2(p_uv * scene_grid.xy), grid_max), 0));
        if (z <= p.z) {
            visible = true;
            last_t = t;
            continue;
        }
        if (!visible) break;

        float lo = last_t, hi = t;
        float4 hit = p;
        float hit_depth = z;
        [unroll]
        for (int refine = 0; refine < 2; ++refine) {
            const float mid = (lo + hi) * 0.5;
            p = scene_project(origin + direction * mid);
            const float2 m_uv = (p.xy - scene_view.xy) * inv_view;
            const float mid_depth = scene_depth.Load(int3(clamp(int2(m_uv * scene_grid.xy), 0, grid_max), 0));
            if (mid_depth > p.z) {
                hi = mid;
                hit = p;
                hit_depth = mid_depth;
            } else lo = mid;
        }
        p = hit;
        if (!scene_projected(p)) break;
        const float2 hit_uv = (p.xy - scene_view.xy) * inv_view;
        const float hit_z = scene_camera_distance(hit_depth, mapping);
        const float error = scene_camera_distance(p.z, mapping) - hit_z;
        const float thickness = min(0.85, 0.14 + 0.022 * hit_z);
        const float pixels = length((hit_uv - origin_uv) * scene_grid.xy);
        if (!(hit_depth > 0.0 && error >= 0.0 && error < thickness && hi > 0.35 && pixels > 1.2)) break;
        const float2 hit_edge = min(hit_uv, 1.0 - hit_uv);
        const float confidence = strength * saturate(min(hit_edge.x, hit_edge.y) * 18.0) *
            (1.0 - smoothstep(thickness * 0.45, thickness, error)) * (1.0 - smoothstep(18.0, 34.0, hi));
        // Scene snapshot is HDR-encoded; decode back to linear HDR radiance for physically accurate reflection!
        const float3 hit_lin = hdr_decode(scene_color.SampleLevel(scene_sampler, hit_uv, 0).rgb);
        return float4(hit_lin, confidence);
    }
    return 0.0;
}

float4 scene_transmission(VOut i, float3 direction, float2 water_slope) {
    const float2 mapping = scene_depth_mapping();
    const float behind = scene_depth_at(i.pos.xy);
    const float bottom = scene_camera_distance(behind, mapping);
    const float surface = scene_camera_distance(i.pos.z, mapping);
    const float thickness = bottom - surface;
    const float to_camera_z = dot(normalize(obj.cw.xyz), direction);
    if (!(behind > 0.0 && surface > 0.0 && thickness > 0.07 && thickness < 16.0 && to_camera_z > 0.08))
        return 0.0;
    const float3 next = i.mp + direction * min(thickness / to_camera_z, 12.0);
    const float4 p = scene_project(scene_clip(float4(next, 1.0)));
    if (!scene_projected(p)) return 0.0;
    const float2 offset = p.xy - i.pos.xy;
    const float cap = 0.025 * min(scene_view.z, scene_view.w);
    const float2 bend = offset * min(1.0, cap / max(length(offset), 1e-6));
    const float2 pixel = i.pos.xy + bend;
    const float z = scene_depth_at(pixel);
    const float water_z = i.pos.z + dot(bend, water_slope);
    const float depth = scene_camera_distance(z, mapping) - scene_camera_distance(water_z, mapping);
    if (!(z > 0.0 && water_z > z && depth > 0.07 && depth < 16.0)) return 0.0;
    const float confidence = scene_edge_fade(pixel) * smoothstep(0.07, 0.45, depth) *
                             (1.0 - smoothstep(8.0, 16.0, depth));
    const float3 absorption = exp(-float3(0.22, 0.095, 0.055) * depth);
    const float3 refr_lin = hdr_decode(scene_color.SampleLevel(scene_sampler, scene_uv(pixel), 0).rgb);
    return float4(refr_lin * absorption, confidence);
}

// Procedural 2D value noise & fBM for Volumetric VFX
float vfx_hash(float2 x) {
    return frac(sin(dot(x, float2(127.1, 311.7))) * 43758.5453);
}
float vfx_noise(float2 x) {
    const float2 fl = floor(x), fr = frac(x);
    const float2 u = fr * fr * (3.0 - 2.0 * fr);
    return lerp(lerp(vfx_hash(fl), vfx_hash(fl + float2(1.0, 0.0)), u.x),
                lerp(vfx_hash(fl + float2(0.0, 1.0)), vfx_hash(fl + float2(1.0, 1.0)), u.x), u.y);
}
float vfx_fbm(float2 x) {
    return 0.65 * vfx_noise(x) + 0.35 * vfx_noise(x * 2.07 + float2(13.1, 7.7));
}

float2 vfx_volume(float2 p, float t, float seed, float gap, float thickness,
                  bool billboard, uint profile, float age) {
    const float footprint = billboard ? saturate(1.0 - dot(p, p)) : 1.0;
    const float intersection = smoothstep(0.0, thickness, gap);
    if (footprint <= 0.0 || intersection <= 0.0) return 0.0.xx;
    if (profile == VFX_EMBER) {
        const float heat = age < 0.0 ? 1.0 : exp2(-age * 4.5);
        const float edge = 1.0 - smoothstep(lerp(0.14, 0.44, heat), 0.98, abs(p.x));
        return float2(intersection * edge * lerp(0.35, 1.0, heat), heat);
    }
    const float chord = sqrt(footprint);
    const float speed = profile == VFX_SMOKE ? 0.36 : profile == VFX_EXHAUST ? 2.9 : 1.3;
    const float2 drift = float2(seed, t * speed);
    float transmittance = 1.0;
    float hot = 0.0;
    [unroll]
    for (int slice = -1; slice <= 1; slice++) {
        const float z = float(slice) * 0.62;
        const float lobes = vfx_noise(p * 2.4 + drift + float2(z * 2.8, z * 1.9));
        const float turb = vfx_noise(p * 5.0 + drift * 1.4 + float2(z * 5.1, z * 3.4));
        const float radius = saturate(footprint - z * z + (lobes - 0.5) * 0.20);
        const float body = billboard ? smoothstep(0.0, 0.78, radius) * chord : 0.85;
        const float density = body * (0.20 + 0.31 * lobes + 0.20 * turb);
        const float visible = saturate((gap - z * thickness) / thickness);
        transmittance *= 1.0 - density * visible;
        hot += density * visible * saturate(0.85 - abs(z)) * (0.55 + 0.45 * turb);
    }
    return saturate(float2((1.0 - transmittance) * intersection, hot));
}

[[vk::constant_id(0)]] const uint lit_material = 0xFFFFFFFFu;

POut shade_modern_impl(VOut i, bool has_scene_inputs, bool allow_discard, bool allow_pbr,
                       bool allow_vfx, bool allow_water, bool allow_authored) {
    const uint m = obj.mode.x;
    const uint mz = obj.mode.z;
    const bool is_hud = (m & M_HUD_2D) != 0;
    const bool is_emissive_hdr = (m & M_EMISSIVE_HDR) != 0;
    const bool authored = allow_authored && ((mz & (1u << 31)) != 0);
    bool known_lit = lit_material != 0xFFFFFFFFu;
    const uint cls = known_lit ? (lit_material & 15u) : ((mz >> 27) & 15);
    const bool is_vfx = allow_vfx && (cls >= C_VFX_FIRE && cls <= C_VFX_SPARK);
    const uint profile = is_vfx && (obj.mode.w & VFX_VOLUME_ACTIVE) ? (mz >> 22) & 7 : 0;
    const bool unlit = !allow_pbr || is_hud || (!known_lit && (cls == C_UNLIT || is_vfx || (mz & (1u << 24)) != 0));
    const bool textured = known_lit || ((m & M_TME) != 0);
    const bool lit_textured = allow_pbr && !unlit && textured;
    const bool need_water = allow_water && has_scene_inputs && lit_textured && cls == C_WATER;
    const bool quad_mask = ((mz >> 25) & 1) != 0;
    const bool need_strip_uv = allow_vfx && (profile != 0) && !quad_mask;

    float3 dpx = float3(0, 0, 0), dpy = float3(0, 0, 0);
    if (lit_textured || need_strip_uv) {
        dpx = ddx(i.mp);
        dpy = ddy(i.mp);
    }
    const uint stock = lit_textured && !is_vfx ? ((obj.mode.w >> 24) & 3) : 0;
    const float3 luma_w = float3(0.2126, 0.7152, 0.0722);
    const float2 sample_uv = i.uv;
    const float4 sampled = textured ? tex.Sample(smp, sample_uv) : float4(1.0, 1.0, 1.0, 1.0);
    const float4 ts = round(sampled * 255.0);
    const float tex_lum = dot(sampled.rgb, luma_w);
    const bool mapped = stock != 0 && (obj.mode.w & (1u << 26)) != 0;
    const float4 surface_map = mapped ? stock_mask.Sample(smp, sample_uv) : 0.0.xxxx;

    // Always compute luminance surface gradient on lit surfaces so ALL materials get crisp micro-relief!
    float dhx = 0.0, dhy = 0.0;
    if (lit_textured) {
        const float hgt = mapped ? surface_map.r : tex_lum;
        dhx = ddx(hgt);
        dhy = ddy(hgt);
    }
    float2 wave_slope = float2(0, 0);
    float2 water_slope = float2(0, 0);
    if (need_water) {
        wave_slope = float2(ddx(tex_lum), ddy(tex_lum));
        water_slope = float2(ddx(i.pos.z), ddy(i.pos.z));
    }
    float2 duvx = float2(0, 0), duvy = float2(0, 0);
    if (need_strip_uv) {
        duvx = ddx(i.uv);
        duvy = ddy(i.uv);
    }

    float3 v = i.c.rgb;
    float va = i.c.a;
    float3 rgb;
    float a;
    float vfx_post_fog = 1.0;
    float3 albedo_srgb = float3(1, 1, 1);
    if (textured) {
        const float4 t = ts;
        const uint tfx = (m >> M_TFX_SHIFT) & 3;
        const bool tcc = (m & M_TCC) != 0;
        albedo_srgb = sampled.rgb;
        if (tfx == 0) {
            rgb = floor(t.rgb * v * (1.0 / 128.0));
            a = tcc ? floor(t.a * va * (1.0 / 128.0)) : va;
        } else if (tfx == 1) {
            rgb = t.rgb;
            a = tcc ? t.a : va;
        } else {
            rgb = floor(t.rgb * v * (1.0 / 128.0)) + ((m & M_SPEC) && !unlit ? 0.0 : va);
            a = tcc ? (tfx == 2 ? t.a + va : t.a) : va;
        }
    } else {
        rgb = v;
        a = va;
    }

    const float a_game = min(a, 255.0);
    if (allow_discard) {
        // Discard transparent air corners on foliage & alpha-tested catch-fences so they never write depth or catch rectangular shadows!
        if ((cls == C_FOLIAGE && a_game < 24.0) || (!is_vfx && !is_hud && (m & M_ATE) && !(m & M_FAILED) && a_game < 16.0)) discard;
        if ((m & M_DISCARD_ZERO_A) && a_game == 0.0) discard;
        if (m & M_ATE) {
            const bool pass = alpha_pass(a_game, (m >> M_ATST_SHIFT) & 7, (float)(obj.mode.y & 0xFF));
            if (m & M_FAILED) {
                if (pass) discard;
            } else if (!pass && (m & M_AFAIL_DISCARD)) {
                discard;
            }
        }
        if ((m & M_STORE_ZERO_A) && a_game == 0.0) {
            POut transparent;
            transparent.color = 0.0;
            transparent.factor = 0.0;
            return transparent;
        }
    }

    // 2D HUD & Cockpit overlay bypass: preserve exact multi-pass retail sRGB & subtractive shadow blending!
    if (is_hud) {
        rgb = min(rgb, 255.0);
        a = min(a, 255.0);
        POut o;
        o.color = float4(rgb * (1.0 / 255.0), a * (1.0 / 255.0));
        const float c = (m & M_CFIX) ? (float)(obj.mode.z & 0xFF) : a;
        o.factor = float4(0, 0, 0, saturate(c * (1.0 / 128.0)));
        return o;
    }

    if (lit_textured) {
        const float3 V = normalize(i.mv);
        float3 N_geo = (abs(i.mn.w) > 0.5) ? normalize(i.mn.xyz) : normalize(cross(dpx, dpy));
        if (dot(N_geo, V) < 0.0) N_geo = -N_geo;
        float3 N = N_geo;
        const float3 L = normalize(obj.light.xyz + float3(0, 1e-9, 0));
        const float3 up = oct_decode(obj.mode.w);
        const uint tfx = (m >> M_TFX_SHIFT) & 3;
        const bool is_car_mesh = (i.mn.w > 0.5);

        // Curvature / crease detection from normal derivatives (catches dented car creases!)
        const float crease = saturate(length(fwidth(N_geo)) * 3.5);
        const bool is_foliage = (cls == C_FOLIAGE);
        const bool is_grass = (cls == C_GRASS_HAY) || (!is_car_mesh && stock != 2 && !is_foliage && (albedo_srgb.g > albedo_srgb.r * 1.08) && (albedo_srgb.g > albedo_srgb.b * 1.15));
        const bool is_vertical_wall = !is_car_mesh && stock == 0 && !is_foliage && abs(dot(N_geo, up)) < 0.38;
        const bool is_billboard = is_vertical_wall && (max(albedo_srgb.r, max(albedo_srgb.g, albedo_srgb.b)) - min(albedo_srgb.r, min(albedo_srgb.g, albedo_srgb.b)) > 0.14);
        const bool is_cloth_flag = is_car_mesh && (albedo_srgb.g > 0.45) && (albedo_srgb.g > albedo_srgb.r * 1.45) && (albedo_srgb.g > albedo_srgb.b * 1.45);

        // Multi-scale physically-accurate surface relief (replaces flat PS2 textures with crisp micro-detail!):
        // NEVER apply luminance-height embossing to ANY vehicle body mesh (eliminates lumpy paint on
        // ALL tagged & untagged Career/Action cars!).
        const float3 r1 = cross(dpy, N), r2 = cross(N, dpx);
        const float det = dot(dpx, r1);
        const float adet = abs(det);
        const float macro_noise = noise3(i.mp * 0.22);
        const float med_noise   = noise3(i.mp * 3.4);
        const float fine_noise  = noise3(i.mp * 14.0);
        const float max_c = max(albedo_srgb.r, max(albedo_srgb.g, albedo_srgb.b));
        const float min_c = min(albedo_srgb.r, min(albedo_srgb.g, albedo_srgb.b));
        const float chroma = max_c - min_c;
        // Detect vulcanized rubber tires (explicit stock == 3 OR untagged dark desaturated rubber on car meshes)
        const bool is_rubber = (stock == 3) || (is_car_mesh && cls == C_GENERIC && tex_lum < 0.24 && chroma < 0.075);
        // ANY vehicle body panel (tagged C_PAINT/STOCK_PAINT OR untagged Career car like Hunter) stays SILKY SMOOTH!
        const bool is_auto_body = (cls == C_PAINT || stock == 1) ||
                                  (is_car_mesh && cls == C_GENERIC && !is_rubber && !is_cloth_flag);
        const bool allow_bump = mapped || (!is_car_mesh && cls != C_WATER) || is_rubber || is_cloth_flag;
        // Sky & daylight detection early so dirt/mud ruts & materials adapt physically to day vs stadium night!
        const float3 zen_srgb = unpack_rgb(asuint(obj.light.w));
        const float3 hor_srgb = unpack_rgb(asuint(obj.diffuse.w));
        const float3 zen_lin = srgb_to_linear(zen_srgb);
        const float3 hor_lin = srgb_to_linear(hor_srgb);
        const float daylight = smoothstep(0.06, 0.34, dot(hor_srgb, luma_w));

        // 3D Tire-Rut & Soil Compaction synthesis on dirt/mud/gravel tracks (stock == 2)
        float rut_wave = 0.0;
        float rut_compaction = 0.0;
        if (stock == 2) {
            // Anisotropic UV-aligned tire grooves + world-space churned mud breakup
            const float groove1 = sin(i.uv.x * 37.699 + med_noise * 2.2);
            const float groove2 = sin(i.uv.x * 75.398 - med_noise * 1.6) * 0.5;
            rut_wave = (groove1 + groove2) * (0.45 + 0.55 * macro_noise);
            // Darker packed tire lines & rut troughs are compacted/damp; pale loose sand stays dry!
            const float is_dry_sand = smoothstep(0.38, 0.58, tex_lum) * smoothstep(0.04, 0.14, albedo_srgb.r - albedo_srgb.b);
            rut_compaction = saturate((1.0 - smoothstep(0.14, 0.44, tex_lum)) * 0.72 + saturate(-rut_wave) * 0.38) * (1.0 - is_dry_sand);
        }

        if (adet > 1e-10 && allow_bump) {
            float bump_scale = 0.045;
            if (mapped) bump_scale = 0.14;
            else if (stock == 2) bump_scale = 0.125;         // Deep 3D dirt & mud ruts catching low sun, floodlights & sparks!
            else if (cls == C_MASONRY) bump_scale = 0.075;   // Concrete/asphalt/brick aggregate
            else if (cls == C_WOOD) bump_scale = 0.070;      // Weathered wood plank grain
            else if (cls == C_RUST_METAL) bump_scale = 0.055;// Corrugated/rusted trackside metal
            else if (is_foliage) bump_scale = 0.065;         // Botanical leaf breakup
            else if (is_rubber) bump_scale = 0.075;          // Tire tread & sidewall grain
            else if (is_grass) bump_scale = 0.068;           // Grass blade & dry hay straw breakup
            else if (is_vertical_wall) bump_scale = 0.045;

            float proc_h = 0.0;
            if (stock == 2) {
                proc_h = rut_wave * 0.036 + med_noise * 0.040 + fine_noise * 0.020;
            } else if (cls == C_MASONRY) {
                proc_h = fine_noise * 0.030;
            } else if (cls == C_WOOD) {
                proc_h = noise3(float3(i.mp.x * 16.0, i.mp.y * 2.5, i.mp.z * 16.0)) * 0.032;
            } else if (is_grass || is_foliage) {
                proc_h = fine_noise * 0.040;
            } else if (is_billboard) {
                proc_h = sin((i.mp.x + i.mp.z) * 28.0) * 0.015;
            } else if (is_cloth_flag) {
                proc_h = sin(i.uv.x * 160.0) * sin(i.uv.y * 160.0) * 0.02;
            }
            const float pdhx = dhx * bump_scale + ddx(proc_h);
            const float pdhy = dhy * bump_scale + ddy(proc_h);
            const float3 grad = (sign(det) / adet) * (pdhx * r1 + pdhy * r2);
            N = normalize(N - grad * min(1.0, 0.28 / max(length(grad), 1e-6)));
        }

        if (need_water) {
            const float3 r1w = cross(dpy, N), r2w = cross(N, dpx);
            const float detw = dot(dpx, r1w);
            const float3 gradient = sign(detw) * 0.22 * (wave_slope.x * r1w + wave_slope.y * r2w) /
                                    max(abs(detw), 1e-8);
            N = normalize(N - gradient * min(1.0, 0.32 / max(length(gradient), 1e-8)));
        }

        // True sRGB -> Linear albedo conversion (preserves all texture detail on bright yellow/white cars!)
        float3 albedo = srgb_to_linear(albedo_srgb);

        // Automatic rust / oxidation / grime detection on car bodies
        const float oxidation = saturate((albedo_srgb.r - albedo_srgb.b) * 4.5) *
            (1.0 - smoothstep(0.08, 0.26, albedo_srgb.r - albedo_srgb.g)) *
            (1.0 - smoothstep(0.32, 0.56, tex_lum));
        const float grime = (1.0 - smoothstep(0.05, 0.22, tex_lum)) * 0.45;
        const float wear = is_auto_body
            ? (mapped ? surface_map.a : saturate(oxidation + grime * 0.5))
            : 0.0;
        const float low_dust = is_auto_body ? saturate((0.32 - dot(i.mp, up)) * 1.45) * (0.40 + 0.60 * med_noise) : 0.0;

        if (stock == 2) {
            const float al = dot(albedo, luma_w);
            albedo = max(al + (albedo - al) * 0.96, 0.0) * lerp(0.94, 1.05, macro_noise);
            albedo *= lerp(1.0, lerp(0.72, 0.86, daylight), rut_compaction * 0.55);
        } else if (is_grass) {
            albedo *= saturate(1.0 + (fine_noise - 0.5) * 0.12);
        } else if (is_rubber) {
            const float al = dot(albedo, luma_w);
            albedo = max((al + (albedo - al) * 0.90) * 0.68, float3(0.035, 0.035, 0.035));
        } else if (is_auto_body) {
            // Balanced automotive enamel saturation (0.88x clean, 0.80x rust/primer) + subtle lower-rocker dust
            const float al = dot(albedo, luma_w);
            const float sat_scale = lerp(0.88, 0.80, wear);
            albedo = max(al + (albedo - al) * sat_scale, 0.0);
            albedo = lerp(albedo, float3(0.16, 0.13, 0.10), low_dust * 0.18);
        } else if (cls == C_CHROME) {
            const float al = dot(albedo, luma_w);
            albedo = max(al + (albedo - al) * 0.95, 0.0);
        } else if (is_foliage) {
            // Preserve deep lush botanical green on foliage (never washed-out or glowing!)
            albedo *= lerp(0.88, 1.04, fine_noise);
        }

        // UE5 / Wreckfest 2 Dual-Lobe Sheet-Metal & Specular-Rut PBR Calibration
        float metal = known_lit && !(lit_material & 32u) ? 0.0 : (float)((mz >> 16) & 15) * (1.0 / 15.0);
        float rough = max((float)((mz >> 20) & 15) * (1.0 / 15.0), 0.045);

        if (cls == C_PAINT || stock == 1) {
            // Pigmented derby sheet-metal base coat (rough 0.36-0.48 clean -> 0.88 over rust/mud)
            rough = lerp(lerp(0.38, 0.88, wear), 0.92, low_dust * 0.65);
            metal = lerp(0.20, 0.0, saturate(wear + low_dust));
            const float scraped_edge = saturate(crease * 0.65 * (1.0 - wear));
            rough = lerp(rough, 0.28, scraped_edge * 0.55);
            metal = saturate(metal + scraped_edge * 0.45);
        } else if (is_auto_body) {
            rough = lerp(lerp(0.42, 0.88, wear), 0.92, low_dust * 0.65);
            metal = lerp(0.14, 0.0, saturate(wear + low_dust));
        } else if (cls == C_CHROME) {
            // Bare welded steel bumpers & chrome trim: sharp metallic edge highlights
            rough = clamp(rough, 0.16, 0.30);
            metal = max(metal, 0.90);
        } else if (cls == C_GLASS) {
            rough = min(rough, 0.05);
            metal = 0.0;
            albedo *= 0.28;
        } else if (stock == 2) {
            // Micro-facet ground specular: loose soil/sand is matte (0.90), while compacted clay grooves (day)
            // and churned stadium mud ruts (night) catch grazing sun, floodlight & spark glints!
            const float groove_rough = lerp(0.36, 0.56, daylight);
            rough = lerp(0.91, groove_rough, rut_compaction);
            metal = 0.0;
        } else if (is_rubber) {
            rough = 0.84;
            metal = 0.0;
        } else if (cls == C_FOLIAGE) {
            rough = 0.72;
            metal = 0.0;
        } else if (cls == C_GRASS_HAY) {
            rough = 0.90;
            metal = 0.0;
        } else if (cls == C_MASONRY) {
            rough = lerp(0.92, 0.80, tex_lum * 0.4);
            metal = 0.0;
        } else if (cls == C_WOOD) {
            rough = 0.82;
            metal = 0.0;
        } else if (cls == C_RUST_METAL) {
            rough = lerp(0.55, 0.90, oxidation);
            metal = lerp(0.32, 0.0, oxidation);
        } else if (cls == C_INTERIOR) {
            rough = 0.80;
            metal = 0.0;
        } else if (!is_car_mesh) {
            rough = clamp(lerp(rough, 0.84, 0.45), 0.45, 0.95);
        }
        if (mapped) {
            rough = max(surface_map.g, 0.045);
            metal = surface_map.b;
        }
        const float r = rough * rough;

        // Game's vertex irradiance E (lifted in daylight so PS2 baked shadows never double-crush dynamic CSM!)
        const float3 E_srgb = (tfx == 1) ? float3(1.0, 1.0, 1.0) : max(v * (1.0 / 128.0), 0.0);
        const float3 E_lin = srgb_to_linear(min(E_srgb, 1.45));
        const float baked_lum = dot(E_srgb, luma_w);

        const float NoV = max(dot(N, V), 1e-4);
        // Subtle two-sided foliage shading that preserves shadow side depth (never flat-lit glowing white trees!)
        const float NoL = is_foliage ? (saturate(dot(N, L)) * 0.72 + 0.22) : saturate(dot(N, L));
        const float N_up = dot(N, up);
        const float hemi = saturate(N_up * 0.5 + 0.5);
        const float3 H = normalize(L + V);
        const float NoH = saturate(dot(N, H));
        const float LoH = saturate(dot(L, H));

        // -------------------------------------------------------------------
        // Physically Energy-Conserving Linear HDR Rig (True 1/PI Lambertian Diffuse!)
        // Clean natural-white sunlight (ZERO orange/yellow piss filter!) + Cool Sky / Warm Ground Bounce.
        // Diffuse surfaces stay <= 1.8 linear HDR (NEVER halo-blooming!), while specular glints,
        // chrome bumpers, sparks & floodlights punch up to 4.0-12.0 HDR for crisp optical bloom!
        // -------------------------------------------------------------------
        const float3 sun_base = max(srgb_to_linear(obj.diffuse.rgb * (1.0 / 128.0)), float3(0.32, 0.30, 0.28));
        // Clean neutral-warm daylight sun (respects track's authored sun color without adding yellow tint!)
        const float3 sun_day = lerp(sun_base, float3(1.08, 1.04, 0.98), 0.65) * 6.8;
        const float3 sun_night = float3(1.05, 1.05, 1.12) * 2.2;
        const float3 key_radiance = lerp(sun_night, sun_day, daylight);

        // Colored Indirect HemisphereIrradiance (divided by PI in diffuse_col below!):
        const float sky_w    = saturate(N_up * 0.70 + 0.30);
        const float bounce_w = saturate(-N_up * 0.75 + 0.25);
        const float3 sky_dome_day    = float3(0.72, 0.95, 1.38);  // Clean crisp cool-blue sky dome
        const float3 dirt_bounce_day = float3(0.82, 0.68, 0.54);  // Subtle neutral-earthy ground bounce
        const float3 sky_irr_day     = sky_dome_day * sky_w + dirt_bounce_day * bounce_w;

        const float3 sky_irr_night = lerp(float3(0.04, 0.04, 0.05), float3(0.12, 0.18, 0.34), hemi);
        const float3 ambient_irr = lerp(sky_irr_night, sky_irr_day, daylight);

        const float cavity = is_car_mesh
            ? lerp(0.68, 1.02, saturate(baked_lum * 1.20))
            : lerp(0.52, 1.05, smoothstep(0.02, 0.72, baked_lum));
        const float rut_ao = (stock == 2) ? lerp(1.0, 0.82, saturate(-rut_wave) * 0.65) : 1.0;
        const float micro_ao = lerp(0.82, 1.0, smoothstep(0.02, 0.32, tex_lum)) * rut_ao;

        float3 diffuse_irr;
        if (authored) {
            const float ndl = abs(dot(N, L));
            diffuse_irr = (ambient_irr * environment_params.w + key_radiance * ndl) * cavity;
        } else if (daylight > 0.5) {
            const float3 vtint = is_car_mesh ? 1.0.xxx : clamp(E_srgb / max(baked_lum, 0.10), 0.78, 1.25);
            diffuse_irr = vtint * cavity * (ambient_irr + key_radiance * NoL);
        } else {
            // Night (Anoka / Hastings): scaled by PI because diffuse_col divides by PI below!
            const float3 flood_pools = is_car_mesh
                ? (max(srgb_to_linear(obj.ambient.rgb * (1.0 / 128.0)), float3(0.04, 0.04, 0.06)) * 4.2 +
                   key_radiance * (NoL * 0.90 + 0.12 * hemi) + ambient_irr * 0.85)
                : (E_lin * 5.8 * lerp(0.80, 1.20, hemi) + ambient_irr * 0.75 + key_radiance * NoL * 0.35);
            const float3 day_irr = cavity * (ambient_irr + key_radiance * NoL);
            diffuse_irr = lerp(flood_pools, day_irr, daylight);
        }

        float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metal);
        if (cls == C_WATER) f0 = float3(0.02, 0.02, 0.02);
        else if (stock == 2) f0 = lerp(float3(0.026, 0.026, 0.026), float3(0.042, 0.042, 0.042), rut_compaction);
        else if (is_rubber) f0 = float3(0.028, 0.028, 0.028);

        // TRUE ENERGY-CONSERVING 1/PI LAMBERTIAN DIFFUSE (prevents diffuse yellow cars & trees from blooming!)
        const float3 diffuse_col = (albedo * (1.0 / PI)) * (1.0 - metal) * diffuse_irr * micro_ao;

        // Direct Key Specular (GGX): uses full un-divided key_radiance so true specular pinpoints sparkle & bloom!
        const float3 Fr = D_GGX(NoH, r) * V_SmithGGXCorrelated(NoV, NoL, r) * F_Schlick(f0, LoH);
        const float spec_occ = saturate(cavity * micro_ao * 1.12);
        float3 spec_col = Fr * NoL * key_radiance * spec_occ;

        // Indirect IBL Reflection (Sky + Cumulus Clouds + Stadium Banks + Warm Dirt Bounce + Grazing Fresnel!)
        const float3 R = reflect(-V, N);
        const float2 dfg = env_dfg(NoV, rough);
        float3 env_col = env_radiance(R, up, L, rough, zen_lin, hor_lin, daylight);

        const float env_scale = lerp(lerp(0.55, 1.45, saturate(baked_lum * 1.5)), 1.0, daylight);
        float3 indirect_spec = env_col * env_scale * spec_occ;

        float4 reflected = 0.0;
        float visibility = (m & M_FGE) ? saturate(i.f * (1.0 / 255.0)) : 1.0;
        if (has_scene_inputs && (cls == C_CHROME || cls == C_GLASS || cls == C_WATER)) {
            reflected = scene_reflection(i, R, rough, visibility);
            const float ssr_w = cls == C_CHROME ? 0.80 : cls == C_GLASS ? 0.88 : 0.84;
            indirect_spec = lerp(indirect_spec, reflected.rgb * spec_occ, ssr_w * reflected.a);
        }

        spec_col += indirect_spec * (f0 * dfg.x + dfg.y);
        float3 col = diffuse_col + spec_col;

        // Dual-Lobe Sheet-Metal Clearcoat + Grazing Schlick Fresnel (UE5 / Wreckfest 2):
        // Roofs, hoods, C-pillars & curved fenders reflect sky, clouds & stadium lights at grazing angles,
        // while rust/mud patches & vertical rear trunks (R.y <= 0.05) stay free of plastic whitewash glare!
        if (is_auto_body) {
            const float base_coat = (cls == C_PAINT || stock == 1) ? 0.72 : 0.45;
            // Suppress mirror clearcoat on vertical downward/backward-reflecting tail panels (Bacon's fix!)
            const float rear_guard = smoothstep(-0.05, 0.25, dot(R, up)) * 0.78 + 0.22 * saturate(N_up);
            const float coat_mask = saturate(1.0 - wear * 1.15) * (1.0 - low_dust * 0.85) * base_coat * rear_guard;
            if (coat_mask > 0.01) {
                const float Fc = F_Schlick1(0.04, LoH);
                const float D_coat = D_GGX(NoH, 0.025) * 0.75 + D_GGX(NoH, 0.09) * 0.30;
                const float Frc = D_coat * V_Kelemen(LoH) * Fc;
                const float Fcv = F_Schlick1(0.04, NoV);
                const float3 coat_env = env_radiance(R, up, L, 0.08, zen_lin, hor_lin, daylight) * env_scale;
                const float3 coat_direct = Frc * NoL * key_radiance;
                col = col * (1.0 - Fcv * coat_mask * 0.50) + (coat_direct + coat_env * Fcv) * spec_occ * coat_mask;
            }
        }

        // Restrained, energy-conserving subsurface scattering (divided by PI so foliage NEVER glows white!)
        if (is_foliage || is_grass || is_cloth_flag) {
            const float sss_back = pow(saturate(dot(V, -L)), 3.0) * 0.45;
            const float sss_str = is_cloth_flag ? 0.55 : is_foliage ? 0.18 : 0.12;
            col += (albedo * (1.0 / PI)) * key_radiance * sss_back * sss_str * micro_ao;
        }

        // Water refraction with Beer-Lambert depth absorption
        if (need_water) {
            const float fresnel = F_Schlick1(0.02, NoV);
            const float3 snell = refract(-V, N, 1.0 / 1.333);
            const float4 transmitted = scene_transmission(i, snell, water_slope);
            col = lerp(col, transmitted.rgb, transmitted.a * (1.0 - fresnel) * visibility * 0.72);
        }

        // Encode Linear HDR -> Reversible Perceptual HDR for post-process decode + Bloom + AgX!
        rgb = hdr_encode(col) * 255.0;
    } else if (is_vfx) {
        // Volumetric HDR Fire, Exhaust, Sparks & Lit Dust/Smoke
        const float gdim = max((float)((mz >> 16) & 15), 1.0);
        const float2 luv = gdim > 1.5 ? frac(i.uv * gdim * 0.9999 + 0.00005) : saturate(i.uv);
        const float2 p = luv * 2.0 - 1.0;
        const float r_rad = length(p);
        const float t_sec = asfloat(obj.mode.w & ~VFX_VOLUME_ACTIVE);
        const uint fade = (mz >> 20) & 3;
        float3 tint = float3(1, 1, 1);
        float cov = 1.0;
        float hdr_boost = 1.0;
        if (profile != 0) {
            const float strip_width = need_strip_uv
                ? length(dpx * duvy.x - dpy * duvx.x)
                    / max(abs(duvx.y * duvy.x - duvy.y * duvx.x), 1.0e-5)
                : 0.0;
            const float thickness = quad_mask ? max(0.025, i.mn.x * 0.52) : clamp(strip_width * 0.32, 0.035, 0.40);
            float gap = 1.0e6;
            if (has_scene_inputs) {
                const float z = scene_depth_at(i.pos.xy);
                if (z > 0.0) gap = scene_camera_distance(z) - scene_camera_distance(i.pos.z);
            }
            const float2 footprint = quad_mask ? i.mn.yz : p;
            const float seed = dot(i.mp, float3(0.037, 0.023, 0.053));
            const float2 volume = vfx_volume(footprint, t_sec, seed, gap, thickness, quad_mask, profile, i.mn.z);
            cov = volume.x;
            if (profile != VFX_SMOKE) {
                const float3 low = profile == VFX_EMBER ? float3(1.0, 0.36, 0.06) : float3(1.0, 0.52, 0.18);
                tint = lerp(low, float3(1.0, 0.96, 0.78), volume.y);
                hdr_boost = profile == VFX_EMBER ? lerp(1.6, 3.2, volume.y) : lerp(1.2, 2.2, volume.y);
            }
        } else if (cls == C_VFX_FIRE) {
            const float rad = quad_mask ? (1.0 - smoothstep(0.45, 0.96, r_rad)) : 1.0;
            const float turb = vfx_fbm(float2(p.x * 2.6, p.y * 2.2 + t_sec * 4.2));
            const float core = saturate(1.0 - r_rad * 0.95 + (turb - 0.45) * 0.75);
            cov = quad_mask ? rad * saturate(0.45 + 0.85 * core) : saturate(0.68 + 0.50 * turb);
            const float base_lum = dot(rgb, luma_w) * (1.0 / 255.0);
            const float heat = saturate(base_lum * (0.65 + 0.70 * core));
            tint = lerp(float3(1.0, 0.24, 0.03),
                        lerp(float3(1.0, 0.65, 0.12), float3(1.0, 0.95, 0.78), smoothstep(0.55, 0.95, heat)),
                        smoothstep(0.15, 0.65, heat));
            hdr_boost = lerp(1.15, 2.15, heat);
        } else if (cls == C_VFX_SMOKE) {
            // Soft depth-faded, directional-lit tire dust & stadium haze plumes (never hard-clips fenders!)
            const float rad = quad_mask ? (1.0 - smoothstep(0.22, 0.96, r_rad)) : 1.0;
            const float wisp = 0.76 + 0.44 * vfx_fbm(p * 2.1 + float2(t_sec * 0.35, t_sec * 0.85));
            float soft_depth = 1.0;
            if (has_scene_inputs) {
                const float z = scene_depth_at(i.pos.xy);
                if (z > 0.0) {
                    const float gap = scene_camera_distance(z) - scene_camera_distance(i.pos.z);
                    soft_depth = smoothstep(0.0, 0.85, max(gap, 0.0));
                }
            }
            cov = (quad_mask ? saturate(rad * wisp * 1.25) : saturate(0.85 + 0.25 * wisp)) * soft_depth;
            const float3 hor_srgb_vfx = unpack_rgb(asuint(obj.diffuse.w));
            const float day_vfx = smoothstep(0.06, 0.34, dot(hor_srgb_vfx, luma_w));
            tint = lerp(float3(0.95, 0.96, 1.04), float3(1.15, 1.07, 0.94), day_vfx);
            hdr_boost = lerp(1.12, 1.25, day_vfx);
        } else {
            const float rad = quad_mask ? (1.0 - smoothstep(0.55, 0.96, r_rad)) : 1.0;
            const float core = exp(-r_rad * r_rad * 5.5);
            const float halo = (1.0 - smoothstep(0.12, 0.95, r_rad)) * 0.45;
            const float flicker = 0.88 + 0.12 * sin(t_sec * 37.0 + dot(i.mp, float3(11.3, 17.1, 7.9)));
            cov = quad_mask ? rad * (0.45 + 0.85 * saturate((core + halo) * flicker)) : flicker;
            tint = lerp(float3(1.0, 0.50, 0.10), float3(1.0, 0.96, 0.82), core);
            hdr_boost = lerp(2.0, 6.5, core);
        }
        cov = saturate(cov);
        float3 vfx_lin = srgb_to_linear(min(rgb * (1.0 / 255.0), 1.0)) * tint * hdr_boost;
        if (fade == 1) {
            rgb = hdr_encode(vfx_lin) * 255.0;
            a *= cov;
        } else if (fade == 2) {
            rgb = hdr_encode(vfx_lin * (profile == 0 ? cov : 1.0)) * 255.0;
            if (profile != 0) vfx_post_fog = cov;
        } else if (fade == 3) {
            rgb = hdr_encode(vfx_lin) * 255.0;
            a = 128.0 - (128.0 - min(a, 128.0)) * cov;
        } else {
            rgb = hdr_encode(vfx_lin) * 255.0;
        }
    } else {
        // Unlit 3D geometry: Sky Dome, Stadium Floodlight Bulbs, Flares, Headlights, Emissive Signs!
        float3 lin = srgb_to_linear(min(rgb * (1.0 / 255.0), 1.0));
        const float lum = dot(lin, luma_w);
        if (is_emissive_hdr) {
            // Flares & emissive light sources push cleanly into HDR for physical Bloom without whiteout!
            lin *= 2.15;
        } else {
            // Gently lift bright unlit peaks (stadium floodlight bulbs, bright clouds) into HDR
            const float bright_boost = 1.0 + smoothstep(0.55, 0.98, lum) * 0.85;
            lin *= bright_boost;
        }
        rgb = hdr_encode(lin) * 255.0;
    }

    rgb = min(rgb, 255.0);
    a = min(a, 255.0);
    if (authored) {
        const float t = saturate((length(i.mv) - environment_params.x) / max(1.0, environment_params.y - environment_params.x));
        const float fog = t * t * (3.0 - 2.0 * t) * environment_params.z;
        rgb = lerp(rgb, hdr_encode(srgb_to_linear(authored_fog_color.rgb)) * 255.0, fog);
    }
    rgb *= vfx_post_fog;
    POut o;
    o.color = float4(rgb * (1.0 / 255.0), (profile != 0 ? a_game : a) * (1.0 / 255.0));
    const float c = (m & M_CFIX) ? (float)(obj.mode.z & 0xFF) : a;
    o.factor = float4(0, 0, 0, saturate(c * (1.0 / 128.0)));
    return o;
}

POut ps_modern(VOut i) {
    return shade_modern_impl(i, false, true, true, true, false, true);
}
POut ps_modern_scene(VOut i) {
    return shade_modern_impl(i, true, true, true, true, true, true);
}
POut ps_modern_opaque(VOut i) {
    return shade_modern_impl(i, false, false, true, false, false, false);
}
POut ps_modern_scene_opaque(VOut i) {
    return shade_modern_impl(i, true, false, true, false, false, false);
}
POut ps_modern_water_opaque(VOut i) {
    return shade_modern_impl(i, true, false, true, false, true, false);
}
POut ps_modern_unlit_opaque(VOut i) {
    return shade_modern_impl(i, false, false, false, false, false, false);
}
POut ps_modern_pbr_ate(VOut i) {
    return shade_modern_impl(i, false, true, true, false, false, false);
}
POut ps_modern_scene_ate(VOut i) {
    return shade_modern_impl(i, true, true, true, false, false, false);
}
POut ps_modern_unlit_ate(VOut i) {
    return shade_modern_impl(i, false, true, false, false, false, false);
}
POut ps_modern_depth_only(VOut i) {
    return shade_modern_impl(i, false, true, false, false, false, false);
}
