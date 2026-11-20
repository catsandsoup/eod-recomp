// native_modern.hlsl - the native renderer's Modern look (docs/NATIVE_RENDERER.md "Stage R6").
// tools/build_native_shaders.py compiles vs_modern / ps_modern to native_modern_spv.inc; the
// Classic shaders (native_shaders.hlsl) are a separate file and stay as they are.
//
// Same push constants and descriptor set as the Classic look. Everything the game decides stays
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

[[vk::binding(0, 0)]] Texture2D tex;
[[vk::binding(1, 0)]] SamplerState smp;
[[vk::binding(0, 1)]] cbuffer AuthoredEnvironment {
    float4 authored_fog_color;
    float4 environment_params; // fog start, fog end, fog density, ambient intensity
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
    float va = obj.ambient.w;
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
float V_SmithGGXCorrelated(float NoV, float NoL, float r) {
    const float a2 = r * r;
    const float gv = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
    const float gl = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-7);
}
float3 F_Schlick(float3 f0, float VoH) { return f0 + (1.0 - f0) * pow(1.0 - VoH, 5.0); }
float F_Schlick1(float f0, float VoH) { return f0 + (1.0 - f0) * pow(1.0 - VoH, 5.0); }
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

POut ps_modern(VOut i) {
    const uint m = obj.mode.x;
    const uint mz = obj.mode.z;
    const bool authored = (mz & (1u << 31)) != 0;
    const float4 ts = round(tex.Sample(smp, i.uv) * 255.0);
    const float3 dpx = ddx(i.mp), dpy = ddy(i.mp);
    const float hgt = dot(ts.rgb, float3(0.2126, 0.7152, 0.0722)) / 255.0;
    const float dhx = ddx(hgt), dhy = ddy(hgt);

    float3 v = i.c.rgb;
    float va = i.c.a;
    const uint cls = (mz >> 27) & 15;
    const bool is_vfx = cls >= C_VFX_FIRE && cls <= C_VFX_SPARK;
    const bool unlit = cls == C_UNLIT || is_vfx || (mz & (1u << 24)) != 0;
    float3 V = normalize(i.mv);
    float3 N;
    if (i.mn.w > 0.5) {
        N = normalize(i.mn.xyz);
    } else {
        N = normalize(cross(dpx, dpy));
    }
    if (dot(N, V) < 0) N = -N;
    const float3 L = normalize(obj.light.xyz + float3(0, 1e-9, 0));
    float3 rgb;
    float a;
    float a_game = 0.0;  // the alpha before the Modern VFX coverage: the alpha test is the game's
    float3 albedo = float3(1, 1, 1);
    if (m & M_TME) {
        const float4 t = ts;
        const uint tfx = (m >> M_TFX_SHIFT) & 3;
        const bool tcc = (m & M_TCC) != 0;
        albedo = t.rgb / 255.0;
        if (tfx == 0) {
            rgb = floor(t.rgb * v / 128.0);
            a = tcc ? floor(t.a * va / 128.0) : va;
        } else if (tfx == 1) {
            rgb = t.rgb;
            a = tcc ? t.a : va;
        } else {
            rgb = floor(t.rgb * v / 128.0) + ((m & M_SPEC) && !unlit ? 0.0 : va);
            a = tcc ? (tfx == 2 ? t.a + va : t.a) : va;
        }
    } else {
        rgb = v;
        a = va;
    }
    if (!unlit && (m & M_TME)) {
        const uint tfx = (m >> M_TFX_SHIFT) & 3;
        if ((mz >> 25) & 1) {
            // normal from height: surface gradient (Mikkelsen 2010) of the texture luminance
            const float scale = 0.04;
            const float3 r1 = cross(dpy, N), r2 = cross(N, dpx);
            const float det = dot(dpx, r1);
            const float3 grad = sign(det) * (dhx * scale * r1 + dhy * scale * r2);
            N = normalize(abs(det) * N - grad);
        }
        const float metal = (float)((mz >> 16) & 15) / 15.0;
        const float rough = max((float)((mz >> 20) & 15) / 15.0, 0.045);
        const float r = rough * rough;  // Filament: perceptual roughness squared
        const float3 up = oct_decode(obj.mode.w);
        // irradiance the game gives this pixel, 1.0 = 0x80: the VU1 light of lit draws, the baked
        // vertex light of prelit draws (multiplies albedo once, never relit)
        float3 E = (tfx == 1) ? float3(1, 1, 1) : v / 128.0;
        if (authored) {
            // Match the editor's two-sided fill for mixed strip winding, applied over baked light.
            const float ndl = abs(dot(N, L));
            const float hemi = saturate(abs(dot(N, up)) * 0.5 + 0.5);
            const float3 fill = lerp(float3(0.38, 0.38, 0.40), float3(0.58, 0.65, 0.76), hemi) * environment_params.w;
            const float3 direct = (obj.diffuse.rgb / 128.0) * (0.26 * (ndl * 0.65 + 0.35) + 0.42 * ndl);
            E *= clamp(fill + direct, 0.30, 1.35);
        }
        const float occl = saturate(dot(E, float3(0.2126, 0.7152, 0.0722)) * 1.5);
        const float3 sun = obj.diffuse.rgb / 128.0;
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
        spec += lerp(sky_radiance(R, up, rough) * occl, E, metal) * (f0 * dfg.x + dfg.y);
        float3 col = diffuse + spec;
        if (cls == C_PAINT) {
            // clear coat (Filament 4.9): roughness 0.1, f0 0.04
            const float cr = 0.01;
            const float Fc = F_Schlick1(0.04, LoH);
            const float Frc = D_GGX(NoH, cr) * V_Kelemen(LoH) * Fc;
            const float Fcv = F_Schlick1(0.04, NoV);
            col = col * (1.0 - Fcv) + (PI * Frc * NoL * sun + sky_radiance(R, up, 0.1) * Fcv) * occl;
        }
        // guardrails 4/5: never darker than the game's result. Baked (or VU1) light x albedo is the
        // floor; the PBR terms only add on top of it (a metal's lost diffuse is not taken away)
        col = max(col, albedo * E);
        rgb = min(col * 255.0, 255.0);
    } else if (is_vfx) {
        a_game = a;
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
        const float t_sec = asfloat(obj.mode.w);
        const bool quad_mask = ((mz >> 25) & 1) != 0;
        const uint fade = (mz >> 20) & 3;
        float3 tint = float3(1, 1, 1);
        float cov = 1.0;
        if (cls == C_VFX_FIRE) {
            const float rad = quad_mask ? (1.0 - smoothstep(0.45, 0.96, r)) : 1.0;
            const float turb = vfx_fbm(float2(p.x * 2.6, p.y * 2.2 + t_sec * 4.2));
            const float core = saturate(1.0 - r * 0.95 + (turb - 0.45) * 0.75);
            cov = quad_mask ? rad * saturate(0.45 + 0.85 * core) : saturate(0.68 + 0.50 * turb);
            const float base_lum = dot(rgb, float3(0.299, 0.587, 0.114)) / 255.0;
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
            rgb *= tint * cov;
        } else if (fade == 3) {
            rgb *= tint, a = 128.0 - (128.0 - min(a, 128.0)) * cov;
        }
    }
    rgb = min(rgb, 255.0);
    a = min(a, 255.0);
    if (m & M_FGE) {
        const float3 fog = float3(obj.mode.y >> 8 & 0xFF, obj.mode.y >> 16 & 0xFF, obj.mode.y >> 24 & 0xFF);
        const float F = clamp(floor(i.f), 0.0, 255.0);
        rgb = fog + floor(((rgb - fog) * F + 128.0) / 256.0);
    }
    if (authored) {
        const float t = saturate((length(i.mv) - environment_params.x) / max(1.0, environment_params.y - environment_params.x));
        const float fog = t * t * (3.0 - 2.0 * t) * environment_params.z;
        rgb = lerp(rgb, authored_fog_color.rgb * 255.0, fog);
    }
    if (m & M_ATE) {
        const bool pass = alpha_pass(is_vfx ? min(a_game, 255.0) : a, (m >> M_ATST_SHIFT) & 7, (float)(obj.mode.y & 0xFF));
        if (m & M_FAILED) {
            if (pass) discard;
        } else if (!pass && (m & M_AFAIL_DISCARD)) {
            discard;
        }
    }
    POut o;
    o.color = float4(rgb / 255.0, a / 255.0);
    const float c = (m & M_CFIX) ? (float)(obj.mode.z & 0xFF) : a;
    o.factor = float4(0, 0, 0, saturate(c / 128.0));
    return o;
}
