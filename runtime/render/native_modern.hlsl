// native_modern.hlsl - the native renderer's Modern look (docs/NATIVE_RENDERER.md "Stage R6").
// tools/build_native_shaders.py compiles vs_modern / ps_modern to native_modern_spv.inc; the
// Classic shaders (native_shaders.hlsl) are a separate file and stay as they are.
//
// Same push constants and descriptor set as the Classic look. Everything the game decides stays
// the game's: blending (pipeline key), alpha, alpha test and fog are computed exactly as in Classic.
// Only the colour of lit surfaces changes: Filament's standard model (Filament "Physically Based
// Rendering in Filament", sections 4.4-4.9 and 5.3: D_GGX, V_SmithGGXCorrelated, F_Schlick,
// Lambert, clear coat with V_Kelemen), with the material record's metalness and roughness, and
// image-based light from the track's sky texture. No tone curve, grade, vignette or haze.
//
// Modern-only fields (filled by the renderer at flush, native_renderer.cpp modern_patch):
//   mode.z bits 16..19 metalness (4 bits), 20..23 roughness (4 bits), 24 emissive (Classic colour
//   kept), 25 normal from height, 26 the texture has a material record, 27..30 class
//   light.xyz   model-space light direction times the light scale (lit draws: the VU1 vf12; prelit
//               draws: this frame's sun), diffuse.rgb its colour (0..128)
//   light.w     sky upper colour (RGBA8 bits), diffuse.w sky horizon colour (RGBA8 bits)
//   mode.w      model-space up, octahedral 12 + 12 bits

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

#define C_GENERIC 0
#define C_UNLIT 1
#define C_PAINT 2
#define C_GLASS 3
#define C_WATER 4
#define C_CHROME 5

[[vk::binding(0, 0)]] Texture2D tex;
[[vk::binding(1, 0)]] SamplerState smp;

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
    o.c = floor(float4(rgb, obj.ambient.w));
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

float3 oct_decode(uint v) {
    float2 e = float2(v & 0xFFF, (v >> 12) & 0xFFF) / 4095.0 * 2.0 - 1.0;
    float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0) n.xy = (1.0 - abs(n.yx)) * float2(n.x >= 0 ? 1 : -1, n.y >= 0 ? 1 : -1);
    return normalize(n);
}

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

POut ps_modern(VOut i) {
    const uint m = obj.mode.x;
    const uint mz = obj.mode.z;
    const float4 ts = round(tex.Sample(smp, i.uv) * 255.0);
    const float3 dpx = ddx(i.mp), dpy = ddy(i.mp);
    const float hgt = dot(ts.rgb, float3(0.2126, 0.7152, 0.0722)) / 255.0;
    const float dhx = ddx(hgt), dhy = ddy(hgt);

    float3 v = i.c.rgb;
    float va = i.c.a;
    const uint cls = (mz >> 27) & 15;
    const bool unlit = cls == C_UNLIT || (mz & (1u << 24)) != 0;
    float3 V = normalize(i.mv);
    float3 N;
    if (i.mn.w > 0.5) {
        N = normalize(i.mn.xyz);
    } else {
        N = normalize(cross(dpx, dpy));
    }
    if (dot(N, V) < 0) N = -N;
    const float3 L = normalize(obj.light.xyz + float3(0, 1e-9, 0));
    if ((m & M_SPEC) && i.mn.w > 0.5) {
        // the VU1 0x948 highlight alpha with the half vector of this light and eye
        const float3 h = normalize(L + V);
        va = max(0.0, 1152.0 * dot(i.mn.xyz, h) - 1056.0);
    }
    float3 rgb;
    float a;
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
        const float3 E = (tfx == 1) ? float3(1, 1, 1) : v / 128.0;
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
        spec += sky_radiance(R, up, rough) * (f0 * dfg.x + dfg.y) * occl;
        float3 col = diffuse + spec;
        if (cls == C_PAINT) {
            // clear coat (Filament 4.9): roughness 0.1, f0 0.04
            const float cr = 0.01;
            const float Fc = F_Schlick1(0.04, LoH);
            const float Frc = D_GGX(NoH, cr) * V_Kelemen(LoH) * Fc;
            const float Fcv = F_Schlick1(0.04, NoV);
            col = col * (1.0 - Fcv) + (PI * Frc * NoL * sun + sky_radiance(R, up, 0.1) * Fcv) * occl;
        }
        rgb = min(col * 255.0, 255.0);
    }
    rgb = min(rgb, 255.0);
    a = min(a, 255.0);
    if (m & M_FGE) {
        const float3 fog = float3(obj.mode.y >> 8 & 0xFF, obj.mode.y >> 16 & 0xFF, obj.mode.y >> 24 & 0xFF);
        const float F = clamp(floor(i.f), 0.0, 255.0);
        rgb = fog + floor(((rgb - fog) * F + 128.0) / 256.0);
    }
    if (m & M_ATE) {
        const bool pass = alpha_pass(a, (m >> M_ATST_SHIFT) & 7, (float)(obj.mode.y & 0xFF));
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
