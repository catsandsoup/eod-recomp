// native_modern.hlsl - the native renderer's Modern look (docs/NATIVE_RENDERER.md "Stage R6").
// tools/build_native_shaders.py compiles the Modern entries to native_modern_spv.inc; the
// Classic shaders (native_shaders.hlsl) are a separate file and stay as they are.
//
// Same push constants as Classic, with optional scene inputs for selected materials. Everything the game decides stays
// the game's: blending (pipeline key), alpha, alpha test and fog are computed exactly as in Classic.
// Only the colour of lit surfaces changes: Filament's standard model (Filament "Physically Based
// Rendering in Filament", sections 4.4-4.9 and 5.3: D_GGX, V_SmithGGXCorrelated, F_Schlick,
// Lambert, clear coat with V_Kelemen), with the material record's metalness and roughness, and
// image-based light from the track's sky texture. An authored track may opt into editor lighting and fog.
//
// Modern-only fields (filled by the renderer at flush, native_renderer.cpp modern_patch):
//   mode.z bits 16..19 metalness (4 bits), 20..23 roughness (4 bits), 24 emissive (Classic colour
//   kept), 25 normal from height, 26 the texture has a material record, 27..30 class,
//   31 authored environment applies to this gameplay surface
//   light.xyz   model-space light direction times the light scale (lit draws: the VU1 vf12; prelit
//               draws: this frame's sun), diffuse.rgb its colour (0..128)
//   light.w     sky upper colour (RGBA8 bits), diffuse.w sky horizon colour (RGBA8 bits)
//   mode.w      model-space up, octahedral 12 + 12 bits
//   ambient.w   M_SPEC draws: ITOP alpha in bits 0..7, the game's q17 half vector in 8..31 (octahedral)

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
#define M_STORE_ZERO_A 0x4000u
#define M_DISCARD_ZERO_A 0x8000u
#define M_FAILED 0x80000000u

#define C_GENERIC   0
#define C_UNLIT     1
#define C_PAINT     2
#define C_GLASS     3
#define C_WATER     4
#define C_CHROME    5
#define C_VFX_FIRE  6
#define C_VFX_SMOKE 7
#define C_VFX_SPARK 8

// VFX-only mode.z bits 22..24 when the nonnegative clock's sign bit is set.
// Bits 20..21 retain the retail blend fade mode.
#define VFX_VOLUME_ACTIVE 0x80000000u
#define VFX_FIRE 1
#define VFX_SMOKE 2
#define VFX_EXHAUST 3
#define VFX_EMBER 4

[[vk::binding(0, 0)]] Texture2D tex;
[[vk::binding(1, 0)]] SamplerState smp;
[[vk::binding(0, 1)]] cbuffer AuthoredEnvironment {
    float4 authored_fog_color;
    float4 environment_params; // fog start, fog end, fog density, ambient intensity
};

[[vk::binding(0, 2)]] Texture2D<float4> scene_color;
[[vk::binding(1, 2)]] Texture2D<float> scene_depth;
[[vk::binding(2, 2)]] SamplerState scene_sampler;
[[vk::binding(3, 2)]] cbuffer SceneInputs {
    float4 scene_view; // scaled origin and size of the copied view
    float4 scene_grid; // snapshot dimensions, source framebuffer dimensions
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
    [[vk::location(1)]] float4 c : COLOR;      // GS vertex colour 0..128 (as Classic), a: ITOP alpha
    [[vk::location(2)]] float f : FOG;
    [[vk::location(3)]] float3 mp : TEXCOORD1; // model-space position
    [[vk::location(4)]] float3 mv : TEXCOORD2; // model-space eye - position
    [[vk::location(5)]] float4 mn : TEXCOORD3; // model-space normal, w = 1 when the vertex has one
};

// The eye in model space: the point the model -> clip rows map to x = y = w = 0.
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
    float3 rgb;
    if (i.n.w != 0) {
        float3 n = float3(i.n.xyz) / 128.0;
        float d = max(0.0, dot(n, obj.light.xyz));
        rgb = min(obj.ambient.rgb + d * obj.diffuse.rgb, 128.0);
    } else {
        rgb = i.c.rgb * 255.0;
    }
    // The water immediate feed preserves VU1's per-vertex alpha in COLOR.w.
    float va = obj.ambient.w < 0.0 ? round(i.c.a * 255.0) : obj.ambient.w;
    if (obj.mode.x & M_SPEC) {
        // exactly Classic's alpha: ITOP alpha, or for lit vertices the VU1 0x948 highlight
        // a = max(0, 1152 (n . h) - 1056) with the game's q17 half vector h (modern_patch packs it)
        const uint pk = asuint(obj.ambient.w);
        va = (float)(pk & 0xFF);
        if (i.n.w != 0) va = max(0.0, 1152.0 * dot(float3(i.n.xyz) / 128.0, oct_decode(pk >> 8)) - 1056.0);
    }
    o.c = floor(float4(rgb, va));
    o.f = (float)((obj.mode.z >> 8) & 0xFF);
    o.uv = i.uv;
    o.mp = i.p;
    o.mv = model_eye() - i.p;
    o.mn = float4(float3(i.n.xyz) / 128.0, i.n.w != 0 ? 1.0 : 0.0);
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
    // The packet's view supplies the same right/up basis as the Classic feed.
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
    // Prelit VFX has no normal. Carry the unflipped local footprint and world radius here.
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

static const float PI = 3.14159265;

// Filament: D_GGX (4.4.1), V_SmithGGXCorrelated (4.4.2), F_Schlick (4.4.3), V_Kelemen (4.9.1)
float D_GGX(float NoH, float r) {
    const float a = NoH * r;
    const float k = r / max(1.0 - NoH * NoH + a * a, 1e-7);
    return k * k * (1.0 / PI);
}
// Filament 4.4.2 fast Smith-GGX correlated visibility (zero sqrts, visually matching exact)
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

// Environment BRDF (Filament's DFG term, here the analytic fit of Karis 2014 in place of the LUT)
float2 env_dfg(float NoV, float r) {
    const float4 c0 = float4(-1, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1, 0.0425, 1.04, -0.04);
    const float4 t = r * c0 + c1;
    const float a004 = min(t.x * t.x, exp2(-9.28 * NoV)) * t.x + t.y;
    return float2(a004 * -1.04 + t.z, a004 * 1.04 + t.w);
}

float3 unpack_rgb(uint v) { return float3(v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF) / 255.0; }

// Image-based light from the track sky: the sky texture's upper and horizon colours over the
// hemisphere; rougher lobes see more of the horizon (a pre-filtered radiance in two bands).
float3 sky_radiance(float3 dir, float3 up, float r) {
    const float3 top = unpack_rgb(asuint(obj.light.w)), hor = unpack_rgb(asuint(obj.diffuse.w));
    const float h = dot(dir, up);
    const float k = saturate(h * (1.0 - 0.6 * r) * 1.5 + 0.15);
    const float3 sky = lerp(hor, top, k);
    // below the horizon: the ground reflects the horizon colour, darker
    return h >= 0 ? sky : lerp(hor * 0.5, hor, saturate(1.0 + h * 2.0) * r);
}

// The copied rectangle is the only valid sample domain, including for
// a viewport that shares its game color/depth buffers with another player.
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
    return saturate(min(edge.x, edge.y) * 20.0);
}

// GS reversed depth is b + k / clip.w (screen_matrix). Recover a camera
// distance in model units, so the ray thickness does not grow with d-buffer quantization.
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

// Return alpha as confidence. A ray must cross the opaque depth from the visible
// side, then survive a depth-thickness test after refinement. Off-screen, sky,
// silhouettes and unresolvable rays retain the existing sky/ambient radiance.
float4 scene_reflection(VOut i, float3 ray, float roughness, float visibility) {
    if (roughness >= 0.85 || visibility <= 0.01) return 0.0;
    const float2 inv_view = 1.0 / scene_view.zw;
    const float2 origin_uv = (i.pos.xy - scene_view.xy) * inv_view;
    const float2 origin_edge = min(origin_uv, 1.0 - origin_uv);
    const float edge = saturate(min(origin_edge.x, origin_edge.y) * 20.0);
    const float strength = (1.0 - smoothstep(0.42, 0.85, roughness)) * visibility * edge;
    if (strength <= 0.01) return 0.0;
    const float2 mapping = scene_depth_mapping();
    const float surface_z = scene_camera_distance(i.pos.z, mapping);
    if (!(surface_z > 0.0 && surface_z < 75.0)) return 0.0;
    const float4 origin = scene_clip(float4(i.mp, 1.0));
    const float4 direction = scene_clip(float4(ray, 0.0));
    if (origin.w + direction.w * 0.85 <= 1e-6) return 0.0;
    float last_t = 0.0;
    const int2 grid_max = int2(scene_grid.xy) - 1;
    const float surface_scene_depth = scene_depth.Load(int3(clamp(int2(origin_uv * scene_grid.xy), 0, grid_max), 0));
    bool visible = !(surface_scene_depth > i.pos.z);
    [loop]
    for (int step = 1; step <= 6; ++step) {
        const float t = 0.30 + 0.55 * (step * step);
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

        // Keep the already sampled crossing. Refinement only changes the hit
        // when a nearer sample also crosses the scene depth.
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
        const float thickness = min(0.65, 0.12 + 0.018 * hit_z);
        const float pixels = length((hit_uv - origin_uv) * scene_grid.xy);
        if (!(hit_depth > 0.0 && error >= 0.0 && error < thickness && hi > 0.45 && pixels > 1.5)) break;
        const float2 hit_edge = min(hit_uv, 1.0 - hit_uv);
        const float confidence = strength * saturate(min(hit_edge.x, hit_edge.y) * 20.0) *
            (1.0 - smoothstep(thickness * 0.45, thickness, error)) * (1.0 - smoothstep(11.0, 21.0, hi));
        return float4(scene_color.SampleLevel(scene_sampler, hit_uv, 0).rgb, confidence);
    }
    return 0.0;
}

// Transmission projects a Snell ray into the shallow depth behind this
// surface. Retest the distorted sample against the water plane to avoid
// pulling foreground objects or clear sky into the shoreline.
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
    const float3 absorption = exp(-float3(0.16, 0.075, 0.048) * depth);
    return float4(scene_color.SampleLevel(scene_sampler, scene_uv(pixel), 0).rgb * absorption, confidence);
}

// Procedural 2D value noise & 2-octave fBM for Modern GPU VFX (fire / smoke / sparks, stage R6 A2)
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

// Bounded optical depth through three slices, from the far lobe to the near lobe.
// A projected billboard uses a sphere chord. Exhaust meshes retain the retail
// nozzle and ribbon geometry; their density follows the texture's axial coordinate.
float2 vfx_volume(float2 p, float t, float seed, float gap, float thickness,
                  bool billboard, uint profile, float age) {
    const float footprint = billboard ? saturate(1.0 - dot(p, p)) : 1.0;
    const float intersection = smoothstep(0.0, thickness, gap);
    if (footprint <= 0.0 || intersection <= 0.0) return 0.0.xx;
    if (profile == VFX_EMBER) {
        // The spark's motion, endpoints and expiry are retail. Cool the hot
        // center of the live trail against its actual age in game seconds.
        const float heat = age < 0.0 ? 1.0 : exp2(-age * 5.0);
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

// A lit pipeline specializes only the material choices that change shader control
// flow. All other entry points read the original per-draw flags.
[[vk::constant_id(0)]] const uint lit_material = 0xFFFFFFFFu;

POut shade_modern_impl(VOut i, bool has_scene_inputs, bool allow_discard, bool allow_pbr,
                       bool allow_vfx, bool allow_water, bool allow_authored) {
    const uint m = obj.mode.x;
    const uint mz = obj.mode.z;
    const bool authored = allow_authored && ((mz & (1u << 31)) != 0);
    bool known_lit = lit_material != 0xFFFFFFFFu;
    const uint cls = known_lit ? (lit_material & 15u) : ((mz >> 27) & 15);
    const bool is_vfx = allow_vfx && (cls >= C_VFX_FIRE && cls <= C_VFX_SPARK);
    const uint profile = is_vfx && (obj.mode.w & VFX_VOLUME_ACTIVE) ? (mz >> 22) & 7 : 0;
    const bool unlit = !allow_pbr || (!known_lit && (cls == C_UNLIT || is_vfx || (mz & (1u << 24)) != 0));
    const bool textured = known_lit || ((m & M_TME) != 0);
    const bool lit_textured = allow_pbr && !unlit && textured;
    const bool need_height = lit_textured && (known_lit ? (lit_material & 16u) != 0 : ((mz >> 25) & 1) != 0);
    const bool need_water = allow_water && has_scene_inputs && lit_textured && cls == C_WATER;
    const bool quad_mask = ((mz >> 25) & 1) != 0;
    const bool need_strip_uv = allow_vfx && (profile != 0) && !quad_mask;

    // Sample texture and compute quad derivatives BEFORE any per-pixel discard,
    // gated only by dynamically uniform push-constant flags so quad lanes stay intact.
    const float4 sampled = textured ? tex.Sample(smp, i.uv) : float4(1.0, 1.0, 1.0, 1.0);
    const float4 ts = round(sampled * 255.0);
    float3 dpx = float3(0, 0, 0), dpy = float3(0, 0, 0);
    if (lit_textured || need_strip_uv) {
        dpx = ddx(i.mp);
        dpy = ddy(i.mp);
    }
    float dhx = 0.0, dhy = 0.0;
    if (need_height) {
        const float hgt = dot(ts.rgb, float3(0.2126, 0.7152, 0.0722)) * (1.0 / 255.0);
        dhx = ddx(hgt);
        dhy = ddy(hgt);
    }
    float2 wave_slope = float2(0, 0);
    float2 water_slope = float2(0, 0);
    if (need_water) {
        const float wave_height = dot(sampled.rgb, float3(0.2126, 0.7152, 0.0722));
        wave_slope = float2(ddx(wave_height), ddy(wave_height));
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
    float3 albedo = float3(1, 1, 1);
    if (textured) {
        const float4 t = ts;
        const uint tfx = (m >> M_TFX_SHIFT) & 3;
        const bool tcc = (m & M_TCC) != 0;
        albedo = t.rgb * (1.0 / 255.0);
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

    // Early alpha-test & zero-alpha cutout discard BEFORE PBR, SSR, water refraction, and VFX:
    // on alpha-fail second passes (M_FAILED), both passing pixels (>= aref) and completely
    // transparent cutout pixels (a == 0 with no depth write and SRC1_ALPHA blend) exit immediately.
    // Opaque entry points pass allow_discard = false so no OpKill is emitted, unlocking
    // hardware Early-Z and Apple Silicon TBDR Hidden Surface Removal (HSR).
    const float a_game = min(a, 255.0);
    if (allow_discard) {
        if ((m & M_DISCARD_ZERO_A) && a_game == 0.0) discard;
        if (m & M_ATE) {
            const bool pass = alpha_pass(a_game, (m >> M_ATST_SHIFT) & 7, (float)(obj.mode.y & 0xFF));
            if (m & M_FAILED) {
                if (pass) discard;
            } else if (!pass && (m & M_AFAIL_DISCARD)) {
                discard;
            }
        }
        // The blend unit retains destination RGB at zero coverage, but GS can
        // still replace its alpha. Run the retail alpha test before this store.
        if ((m & M_STORE_ZERO_A) && a_game == 0.0) {
            POut transparent;
            transparent.color = 0.0;
            transparent.factor = 0.0;
            return transparent;
        }
    }

    if (lit_textured) {
        const float3 V = normalize(i.mv);
        float3 N = (i.mn.w > 0.5) ? normalize(i.mn.xyz) : normalize(cross(dpx, dpy));
        if (dot(N, V) < 0) N = -N;
        const float3 L = normalize(obj.light.xyz + float3(0, 1e-9, 0));
        const uint tfx = (m >> M_TFX_SHIFT) & 3;
        if (need_height) {
            // normal from height: surface gradient (Mikkelsen 2010) of the texture luminance
            // Clamped slope prevents specular aliasing on oblique 4K surfaces.
            const float scale = 0.075;
            const float3 r1 = cross(dpy, N), r2 = cross(N, dpx);
            const float det = dot(dpx, r1);
            const float adet = abs(det);
            if (adet > 1e-10) {
                const float3 grad = (sign(det) / adet) * (dhx * scale * r1 + dhy * scale * r2);
                N = normalize(N - grad * min(1.0, 0.42 / max(length(grad), 1e-6)));
            }
        }
        const float metal = known_lit && !(lit_material & 32u) ? 0.0 : (float)((mz >> 16) & 15) * (1.0 / 15.0);
        float rough = max((float)((mz >> 20) & 15) * (1.0 / 15.0), 0.045);
        if (cls == C_CHROME) rough = min(rough, 0.22);
        const float r = rough * rough;  // Filament: perceptual roughness squared
        const float3 up = oct_decode(obj.mode.w);
        // irradiance the game gives this pixel, 1.0 = 0x80: the VU1 light of lit draws, the baked
        // vertex light of prelit draws (multiplies albedo once, never relit)
        float3 E = (tfx == 1) ? float3(1, 1, 1) : v * (1.0 / 128.0);
        if (authored) {
            // Match the editor's two-sided fill for mixed strip winding, applied over baked light.
            const float ndl = abs(dot(N, L));
            const float hemi = saturate(abs(dot(N, up)) * 0.5 + 0.5);
            const float3 fill = lerp(float3(0.38, 0.38, 0.40), float3(0.58, 0.65, 0.76), hemi) * environment_params.w;
            const float3 direct = (obj.diffuse.rgb * (1.0 / 128.0)) * (0.26 * (ndl * 0.65 + 0.35) + 0.42 * ndl);
            E *= clamp(fill + direct, 0.30, 1.35);
        }
        const float occl = saturate(dot(E, float3(0.2126, 0.7152, 0.0722)) * 1.5);
        const float3 sun = obj.diffuse.rgb * (1.0 / 128.0);
        if (need_water) {
            // Animated 0x325728 geometry supplies the wave normal. Its texture
            // provides the small ripples between vertices via a surface gradient.
            const float3 r1 = cross(dpy, N), r2 = cross(N, dpx);
            const float det = dot(dpx, r1);
            const float3 gradient = sign(det) * 0.18 * (wave_slope.x * r1 + wave_slope.y * r2) /
                                    max(abs(det), 1e-8);
            N = normalize(N - gradient * min(1.0, 0.28 / max(length(gradient), 1e-8)));
        }
        const float NoV = max(dot(N, V), 1e-4);
        const float NoL = saturate(dot(N, L));
        const float3 H = normalize(L + V);
        const float NoH = saturate(dot(N, H)), LoH = saturate(dot(L, H));
        float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metal);
        if (cls == C_WATER) f0 = float3(0.02, 0.02, 0.02);
        const float3 diffuse = albedo * (1.0 - metal) * E;
        // sun: Filament Fr * NoL * illuminance, in the game's convention (diffuse = albedo E, no 1/pi)
        const float3 Fr = D_GGX(NoH, r) * V_SmithGGXCorrelated(NoV, NoL, r) * F_Schlick(f0, LoH);
        float3 spec = PI * Fr * NoL * sun * occl;
        const float3 R = reflect(-V, N);
        const float2 dfg = env_dfg(NoV, rough);
        // a metal mirrors its surroundings, whose light is the game's light here (E), not only the sky:
        // at night the sky is dark but the baked / VU1 light is not (guardrails 4/5)
        const float3 sky = sky_radiance(R, up, rough);
        float3 indirect = lerp(sky * occl, E, metal);
        float4 reflected = 0.0;
        float visibility = (m & M_FGE) ? saturate(i.f * (1.0 / 255.0)) : 1.0;
        if (authored) {
            const float t = saturate((length(i.mv) - environment_params.x) /
                                     max(1.0, environment_params.y - environment_params.x));
            visibility *= 1.0 - t * t * (3.0 - 2.0 * t) * environment_params.z;
        }
        if (has_scene_inputs && (cls == C_PAINT || cls == C_CHROME || cls == C_GLASS || cls == C_WATER)) {
            const bool sky_only_ray = (cls == C_PAINT) && (dot(R, up) > 0.78);
            if (!sky_only_ray) {
                reflected = scene_reflection(i, R, cls == C_PAINT ? 0.12 : rough, visibility);
                const float weight = cls == C_CHROME ? 0.90 : cls == C_GLASS ? 0.80 : cls == C_WATER ? 0.78 : 0.65;
                indirect = lerp(indirect, reflected.rgb * occl, weight * reflected.a);
            }
        }
        spec += indirect * (f0 * dfg.x + dfg.y);
        float3 col = diffuse + spec;
        if (cls == C_PAINT) {
            // Dual-lobe automotive clear coat (Filament 4.9): crisp lacquer glint + smooth curve sheen,
            // plus crisp grazing Fresnel sky/SSR reflection.
            const float Fc = F_Schlick1(0.04, LoH);
            const float D_coat = D_GGX(NoH, 0.022) * 0.75 + D_GGX(NoH, 0.11) * 0.30;
            const float Frc = D_coat * V_Kelemen(LoH) * Fc;
            const float Fcv = F_Schlick1(0.04, NoV);
            const float3 coat_sky = sky_radiance(R, up, 0.12);
            const float3 coat = lerp(coat_sky * Fcv, reflected.rgb * Fcv, reflected.a * 0.85);
            col = col * (1.0 - Fcv) + (PI * Frc * NoL * sun + coat) * occl;
        }
        // Guardrails 4/5/6: never darker than Classic (albedo * E), and soft-rolloff high
        // specular peaks so non-emissive surfaces keep vivid colour without clipping to 1.0.
        const float3 base_floor = albedo * E;
        const float3 extra = max(col - base_floor, 0.0);
        col = max(min(base_floor + extra / (1.0 + extra * 0.40), 0.985), base_floor);
        if (need_water) {
            const float fresnel = F_Schlick1(0.02, NoV);
            const float3 snell = refract(-V, N, 1.0 / 1.333);
            const float4 transmitted = scene_transmission(i, snell, water_slope);
            col = lerp(col, transmitted.rgb,
                       transmitted.a * (1.0 - fresnel) * visibility * 0.64);
        }
        rgb = min(col * 255.0, 255.0);
    } else if (is_vfx) {
        // Modern-only GPU procedural fire, smoke & sparks (stage R6 A2, A5): a tint (<= 1 per
        // channel) and a coverage mask (radial falloff x noise, <= 1) on the game's own result, so a
        // VFX card keeps the texture's alpha and detail and is never brighter than Classic. The
        // coverage is applied the way the game's GS blend fades a card out (modern_patch, bits
        // 20..21): 1 src alpha (C = As, Cs C + Cd (1 - C)), 2 src colour (Cd kept: Cs F + Cd),
        // 3 inverse (Cs (1 - C) + Cd C: C -> 1), 0 no fade (opaque / FIX / destination-alpha blends
        // draw exactly as Classic: masking them would make or keep square blocks)
        const float gdim = max((float)((mz >> 16) & 15), 1.0);
        const float2 luv = gdim > 1.5 ? frac(i.uv * gdim * 0.9999 + 0.00005) : saturate(i.uv);
        const float2 p = luv * 2.0 - 1.0;
        const float r = length(p);
        const float t_sec = asfloat(obj.mode.w & ~VFX_VOLUME_ACTIVE);
        const uint fade = (mz >> 20) & 3;
        float3 tint = float3(1, 1, 1);
        float cov = 1.0;
        if (profile != 0) {
            // For a strip, derive width from the UV-to-world footprint. Billboard
            // streams have a real particle radius directly from the retail call.
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
                const float3 low = profile == VFX_EMBER ? float3(0.84, 0.34, 0.08) : float3(1.0, 0.57, 0.28);
                tint = lerp(low, float3(1.0, 0.98, 0.85), volume.y);
            }
        } else if (cls == C_VFX_FIRE) {
            const float rad = quad_mask ? (1.0 - smoothstep(0.45, 0.96, r)) : 1.0;
            const float turb = vfx_fbm(float2(p.x * 2.6, p.y * 2.2 + t_sec * 4.2));
            const float core = saturate(1.0 - r * 0.95 + (turb - 0.45) * 0.75);
            cov = quad_mask ? rad * saturate(0.45 + 0.85 * core) : saturate(0.68 + 0.50 * turb);
            const float base_lum = dot(rgb, float3(0.299, 0.587, 0.114)) * (1.0 / 255.0);
            const float heat = saturate(base_lum * (0.65 + 0.70 * core));
            const float3 bb = lerp(float3(1.0, 0.26, 0.04),
                                   lerp(float3(1.0, 0.68, 0.14), float3(1.0, 0.94, 0.76), smoothstep(0.55, 0.95, heat)),
                                   smoothstep(0.15, 0.65, heat));
            tint = lerp(float3(1.0, 1.0, 1.0), bb, 0.35);
        } else if (cls == C_VFX_SMOKE) {
            const float rad = quad_mask ? (1.0 - smoothstep(0.35, 0.96, r)) : 1.0;
            const float wisp = 0.74 + 0.42 * vfx_fbm(p * 2.0 + float2(t_sec * 0.35, t_sec * 0.85));
            cov = quad_mask ? rad * wisp : 0.82 + 0.28 * wisp;
            tint = (quad_mask ? min(0.94 + 0.10 * saturate(-p.y), 1.0) : 1.0).xxx;
        } else {
            // C_VFX_SPARK: crisp radial incandescent core + soft ember halo with subtle flicker
            const float rad = quad_mask ? (1.0 - smoothstep(0.55, 0.96, r)) : 1.0;
            const float core = exp(-r * r * 5.5);
            const float halo = (1.0 - smoothstep(0.12, 0.95, r)) * 0.45;
            const float flicker = 0.88 + 0.12 * sin(t_sec * 37.0 + dot(i.mp, float3(11.3, 17.1, 7.9)));
            cov = quad_mask ? rad * (0.45 + 0.85 * saturate((core + halo) * flicker)) : flicker;
            tint = lerp(float3(1.0, 1.0, 1.0), lerp(float3(1.0, 0.55, 0.12), float3(1.0, 0.95, 0.78), core), 0.30);
        }
        cov = saturate(cov);
        if (fade == 1) {
            rgb *= tint, a *= cov;
        } else if (fade == 2) {
            // A fully soft add/subtract draw must contribute zero even with fog.
            rgb *= tint * (profile == 0 ? cov : 1.0);
            if (profile != 0) vfx_post_fog = cov;
        } else if (fade == 3) {
            rgb *= tint, a = 128.0 - (128.0 - min(a, 128.0)) * cov;
        }
    }
    rgb = min(rgb, 255.0);
    a = min(a, 255.0);
    if (m & M_FGE) {
        const float3 fog = float3(obj.mode.y >> 8 & 0xFF, obj.mode.y >> 16 & 0xFF, obj.mode.y >> 24 & 0xFF);
        const float F = clamp(floor(i.f), 0.0, 255.0);
        rgb = fog + floor(((rgb - fog) * F + 128.0) * (1.0 / 256.0));
    }
    if (authored) {
        const float t = saturate((length(i.mv) - environment_params.x) / max(1.0, environment_params.y - environment_params.x));
        const float fog = t * t * (3.0 - 2.0 * t) * environment_params.z;
        rgb = lerp(rgb, authored_fog_color.rgb * 255.0, fog);
    }
    rgb *= vfx_post_fog;
    POut o;
    // The dual-source factor carries coverage. Keep the retail framebuffer
    // alpha visible to later draws that use destination alpha.
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

// Specialized zero-discard / low-register entry points for Early-Z + TBDR HSR:
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
