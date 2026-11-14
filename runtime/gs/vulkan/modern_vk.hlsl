// modern_vk.hlsl - the EOD_MODERN pass for the Vulkan GS backend (modern_vk.inc).
// tools/build_modern_vk_shaders.py compiles each entry to SPIR-V with DXC
// (modern_vk_spv.inc, committed). Descriptor set 0 holds the frame's resources,
// set 1 a batch's texture and sampler; the batch constants are push constants.
// Clip space follows Direct3D (the pass sets a negative-height viewport).

#define B0(n) [[vk::binding(n, 0)]]
#define B1(n) [[vk::binding(n, 1)]]

B0(0) cbuffer Frame {
    float4 clip_x, clip_y, clip_w;  // world -> clip rows (z is set below)
    float4 params;                  // x near, y shadow texel, z haze distance, w exposure
    float4 cam_pos;                 // world camera position
    float4 up;                      // world up
    float4 sh_x, sh_y, sh_z;        // world -> shadow map rows
    float4 fog;                     // fog colour (gamma); w debug view
    float4 sun;                     // world direction to the sun, w ambient
    float4 gsz;                     // x 1 / 2^bits of the GS depth format, y daylight (0 night, 1 day)
    float4 c0_x, c0_y, c0_z;        // cascade 0 (near, sharp) world -> shadow rows
    float4 c1_x, c1_y, c1_z;        // cascade 1 (far)
    float4 cs;                      // x, y world size of a texel in cascade 0 / 1; z, w depth range of each
    float4 post;                    // x pixels per world unit at w = 1, y AO radius (world), z bloom threshold, w bloom
    float4 post2;                   // x 1 / width, y 1 / height, zw blur step (uv)
    float4 set;                     // settings: x shadows, y reflections, z ambient occlusion, w vignette
    float4 set2;                    // x bloom, y colour grade, z soft shadows (light size), w haze
};

struct BatchConsts {
    float4 m0, m1, m2;   // model rows
    float4 light;        // world direction, w = ambient (VU1 qword 17)
    float4 info;         // x role (0 sky, 1 world, 2 object), y layer, z textured, w gloss
    float4 info2;        // x 1: an additive/subtractive GS layer (unlit), y vertex alpha,
                         // z alpha from the vertex only, w texture function
    float4 atest;        // the GS alpha test: x method (-1 off), y reference (GS units), z discard on fail
    float4 zrow;         // the batch's GS depth row (model space)
};
[[vk::push_constant]] BatchConsts pcb;
#define m0 pcb.m0
#define m1 pcb.m1
#define m2 pcb.m2
#define info pcb.info
#define info2 pcb.info2
#define atest pcb.atest
#define zrow pcb.zrow

B1(0) Texture2D tex;
B1(1) SamplerState smp;
B0(1) Texture2D<float> shadow_map;
B0(2) Texture2D<float4> gs_frame;
B0(3) Texture2DMS<float> scene_depth;
B0(4) Texture2D<float4> post_src;
B0(5) Texture2D<float> ao_tex;
B0(6) Texture2D<float4> bloom_tex;
B0(20) SamplerComparisonState shadow_smp;
B0(21) SamplerState lin_clamp;

struct VIn {
    [[vk::location(0)]] float3 p : POSITION;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] float4 n : NORMAL;
    [[vk::location(3)]] float4 c : COLOR;
    [[vk::location(4)]] float2 va : TEXCOORD1;  // the kicked vertex alpha (0x80 = 1.0) and whether the primitive blends
};
struct VOut {
    float4 pos : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
    [[vk::location(1)]] float3 n : TEXCOORD1;
    [[vk::location(2)]] float3 wp : TEXCOORD2;
    [[vk::location(3)]] float4 c : COLOR;
    [[vk::location(4)]] float2 va : TEXCOORD3;
};
float4 world(float3 p) {
    float4 q = float4(p, 1);
    return float4(dot(m0, q), dot(m1, q), dot(m2, q), 1);
}
VOut vs_modern(VIn i) {
    VOut o;
    float4 w = world(i.p);
    float4 c;
    c.x = dot(clip_x, w);
    c.y = dot(clip_y, w);
    c.w = dot(clip_w, w);
    // reverse depth: depth = near / w (later world layers a little nearer); the sky is farthest
    c.z = info.x == 0 ? 0 : params.x * (1 + info.y * 2e-5);
    o.pos = c;
    o.uv = i.uv;
    o.n = float3(dot(m0.xyz, i.n.xyz), dot(m1.xyz, i.n.xyz), dot(m2.xyz, i.n.xyz));
    o.wp = w.xyz;
    o.c = float4(i.c.rgb * (255.0 / 128.0), i.n.w);  // a: normal present
    o.va = float2(i.va.x * (255.0 / 128.0), i.va.y);
    return o;
}
struct SOut {
    float4 pos : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
    [[vk::location(1)]] float2 va : TEXCOORD1;
};
SOut vs_shadow(VIn i) {
    SOut o;
    float4 w = world(i.p);
    o.pos = float4(dot(sh_x, w), dot(sh_y, w), dot(sh_z, w), 1);
    o.uv = i.uv;
    o.va = float2(i.va.x * (255.0 / 128.0), i.va.y);
    return o;
}
// the depth the GS would have written for this batch, into the game's own depth buffer
SOut vs_gsdepth(VIn i) {
    SOut o;
    float4 w = world(i.p);
    float4 q = float4(i.p, 1);
    o.pos = float4(dot(clip_x, w), dot(clip_y, w), dot(zrow, q) * gsz.x, dot(clip_w, w));
    o.uv = i.uv;
    o.va = i.va;
    return o;
}
// only what is visible casts: a blended primitive casts by its alpha as the GS forms it, alpha-tested
// texels that fail do not cast
void ps_shadow(SOut i) {
    float ta = info.z > 0 ? tex.Sample(smp, i.uv).a * (255.0 / 128.0) : 1;
    int tfx = (int)info2.w;
    float av = info2.y * i.va.x;
    float a = info2.z > 0 ? av : tfx == 0 ? ta * av : tfx == 2 ? ta + av : ta;
    if (i.va.y > 0.5 && a < 0.5) discard;
    if (ta < 0.5) discard;
}
float3 lin(float3 c) { return pow(saturate(c), 2.2); }

// ---- post: ambient occlusion from the scene depth, bloom, composite with a filmic grade
float view_w(int2 p) {
    float d = scene_depth.Load(p, 0);
    return d > 0 ? params.x / d : 1e9;
}
float ps_ao(float4 pos : SV_Position) : SV_Target {  // half resolution
    int2 p = int2(pos.xy * 2);
    float w = view_w(p);
    if (w > 1e8) return 1;
    float gx = (view_w(p + int2(2, 0)) - view_w(p - int2(2, 0))) * 0.25;
    float gy = (view_w(p + int2(0, 2)) - view_w(p - int2(0, 2))) * 0.25;
    if (abs(gx) > w || abs(gy) > w) gx = gy = 0;  // a depth edge: no plane
    float r = clamp(post.y * post.x / w, 3, 80);
    int2 cell = int2(pos.xy) & 3;
    float a0 = (cell.x * 4 + cell.y) * (6.2832 / 16);
    float occ = 0;
    [unroll] for (int k = 0; k < 12; k++) {
        float a = a0 + k * 2.3999, rr = r * (k + 0.5) / 12;
        float2 o = float2(cos(a), sin(a)) * rr;
        float pred = w + gx * o.x + gy * o.y;
        float dz = pred - view_w(p + int2(o));
        occ += dz > post.y * 0.08 && dz < post.y * 3 ? 1 - dz / (post.y * 3) : 0;
    }
    return saturate(1 - occ / 12 * 1.2);
}
float4 ps_bright(float4 pos : SV_Position) : SV_Target {  // half resolution
    float3 c = post_src.Sample(lin_clamp, pos.xy * post2.xy * 2).rgb;
    float l = max(c.r, max(c.g, c.b));
    return float4(c * saturate((l - post.z) / (1 - post.z)), 1);
}
float4 ps_blur(float4 pos : SV_Position) : SV_Target {  // half resolution, one direction
    float2 uv = pos.xy * post2.xy * 2;
    const float wt[5] = {0.227, 0.195, 0.122, 0.054, 0.016};
    float4 c = post_src.Sample(lin_clamp, uv) * wt[0];
    [unroll] for (int k = 1; k < 5; k++)
        c += (post_src.Sample(lin_clamp, uv + post2.zw * k) + post_src.Sample(lin_clamp, uv - post2.zw * k)) * wt[k];
    return c;
}
float4 ps_comp(float4 pos : SV_Position) : SV_Target {
    float2 uv = pos.xy * post2.xy, h = post2.xy;
    float3 c = post_src.Load(int3(pos.xy, 0)).rgb;
    float ao = 0;
    [unroll] for (int y = -1; y <= 1; y++)
        [unroll] for (int x = -1; x <= 1; x++) ao += ao_tex.Sample(lin_clamp, uv + h * float2(x, y) * 2.5);
    c *= lerp(1, ao / 9, set.z);
    c += bloom_tex.Sample(lin_clamp, uv).rgb * post.w;
    float l = dot(c, float3(0.299, 0.587, 0.114));
    c = saturate(lerp(l.xxx, c, 1 + 0.12 * set2.y));
    c = lerp(c, c * c * (3 - 2 * c), 0.35 * set2.y);
    float2 v = uv - 0.5;
    c *= 1 - dot(v, v) * set.w;
    return float4(c, 128.0 / 255.0);
}

// the GS frame so far (the game's own sky), copied into the multisampled target
float4 vs_full(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps_full(float4 pos : SV_Position) : SV_Target {
    return float4(gs_frame.Load(int3(pos.xy, 0)).rgb, 128.0 / 255.0);
}
float3 aces(float3 x) { return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14)); }
static const float2 k_disk[16] = {
    float2(-0.94, -0.40), float2(0.95, -0.77), float2(-0.09, -0.93), float2(0.34, 0.29),
    float2(-0.92, 0.46), float2(-0.82, -0.88), float2(-0.38, 0.28), float2(-0.11, 0.65),
    float2(0.54, -0.41), float2(0.45, -0.89), float2(0.24, 0.92), float2(-0.81, 0.15),
    float2(0.97, 0.76), float2(0.44, 0.13), float2(-0.62, 0.96), float2(0.79, 0.19)};
float shadow_cascade(float3 s, float off, float range, float texel, float2 rot) {
    float2 uv = float2((s.x * 0.5 + 0.5) * 0.5 + off, 0.5 - s.y * 0.5);
    float2 tx = float2(0.5, 1) * params.y;
    float2x2 R = float2x2(rot.x, -rot.y, rot.y, rot.x);
    float r = 1.5;
    if (set2.z > 0) {
        float zb = 0, nb = 0;
        [unroll] for (int k = 0; k < 16; k += 2) {
            float d = shadow_map.SampleLevel(lin_clamp, uv + mul(R, k_disk[k]) * tx * 10, 0);
            if (d < s.z) { zb += d; nb++; }
        }
        if (nb == 0) return 1;
        zb /= nb;
        r = clamp((s.z - zb) * range / texel * set2.z, 1.5, 12);
    }
    float sum = 0;
    [unroll] for (int k = 0; k < 16; k++)
        sum += shadow_map.SampleCmpLevelZero(shadow_smp, uv + mul(R, k_disk[k]) * tx * r, s.z);
    return sum / 16;
}
float shadow_raw(float3 wp, float3 n, float2 pix) {
    float ign = frac(52.9829189 * frac(dot(pix, float2(0.06711056, 0.00583715))));
    float2 rot = float2(cos(ign * 6.2832), sin(ign * 6.2832));
    float4 w0 = float4(wp + n * cs.x * 1.5, 1);
    float3 s0 = float3(dot(c0_x, w0), dot(c0_y, w0), dot(c0_z, w0));
    if (all(abs(s0.xy) < 0.9) && s0.z < 1) return shadow_cascade(s0, 0, cs.z, cs.x, rot);
    float4 w1 = float4(wp + n * cs.y * 1.5, 1);
    float3 s1 = float3(dot(c1_x, w1), dot(c1_y, w1), dot(c1_z, w1));
    if (any(abs(s1.xy) > 1) || s1.z > 1) return 1;
    float k = saturate(1 - max(abs(s1.x), abs(s1.y)) * 1.1 + 0.1) * 4;
    return lerp(1, shadow_cascade(s1, 0.5, cs.w, cs.y, rot), saturate(k));
}
float shadow_at(float3 wp, float3 n, float2 pix) {
    if (set.x <= 0) return 1;
    // at night the moon casts only a faint shadow (the floodlights are not shadow casters)
    return lerp(1, shadow_raw(wp, n, pix), set.x * lerp(0.2, 1.0, gsz.y));
}
static const float PI = 3.14159265;
float D_GGX(float NoH, float a) {
    float a2 = a * a, f = (NoH * a2 - NoH) * NoH + 1;
    return a2 / (PI * f * f);
}
float V_SmithGGXCorrelated(float NoV, float NoL, float a) {
    float a2 = a * a;
    float gv = NoL * sqrt(NoV * NoV * (1 - a2) + a2), gl = NoV * sqrt(NoL * NoL * (1 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}
float V_Kelemen(float LoH) { return 0.25 / max(LoH * LoH, 1e-3); }
float3 F_Schlick(float3 f0, float VoH) { return f0 + (1 - f0) * pow(1 - VoH, 5); }
float2 dfg(float NoV, float rough) {
    const float4 c0 = float4(-1, -0.0275, -0.572, 0.022), c1 = float4(1, 0.0425, 1.04, -0.04);
    float4 r = rough * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
    return float2(-1.04, 1.04) * a004 + r.zw;
}
float3 sky_env(float3 r) {
    float h = saturate(dot(r, up.xyz));
    float3 horizon = lin(fog.rgb), zenith = float3(0.16, 0.32, 0.62);
    return lerp(horizon, zenith, sqrt(h)) * (dot(r, up.xyz) < 0 ? 0.35 : 1.0);
}
float3 sky_rough(float3 r, float3 n, float rough) {
    float3 d = normalize(lerp(r, n, rough * rough));
    float3 avg = lerp(lin(fog.rgb) * 0.6, float3(0.30, 0.40, 0.58), dot(n, up.xyz) * 0.5 + 0.5);
    return lerp(sky_env(d), avg, rough * rough);
}
float4 ps_modern(VOut i) : SV_Target {
    float4 t = info.z > 0 ? tex.Sample(smp, i.uv) : float4(0.75, 0.75, 0.75, 128.0 / 255.0);
    float av = info2.y * i.va.x;
    int tfx = (int)info2.w;
    float as_gs = info2.z > 0 ? av * 128 : tfx == 0 ? t.a * 255 * av : tfx == 2 ? t.a * 255 + av * 128 : t.a * 255;
    if (atest.x >= 0 && atest.z > 0) {
        float r = atest.y;
        int m = (int)atest.x;
        bool ok = m == 1 || (m == 2 && as_gs < r) || (m == 3 && as_gs <= r) || (m == 4 && abs(as_gs - r) < 0.5) ||
                  (m == 5 && as_gs >= r) || (m == 6 && as_gs > r) || (m == 7 && abs(as_gs - r) >= 0.5);
        if (!ok) discard;
    }
    if (i.va.y > 0.5 && as_gs < 1) discard;
    t.a = i.va.y > 0.5 ? saturate(as_gs / 128.0) : 1;
    if (atest.x < 0 && t.a < (info.x == 2 && info2.y >= 0.99 ? 0.5 : 0.02)) discard;
    if (info.x == 0 || info2.x > 0) return float4(t.rgb * (i.c.a > 0.5 ? 1 : i.c.rgb), t.a);
    float3 albedo = lin(t.rgb);
    float3 V = normalize(cam_pos.xyz - i.wp);
    float3 L = normalize(sun.xyz);
    // night: a dim cool moonlight and ambient (the floodlights are painted into the world's vertex light)
    float day = gsz.y;
    float3 sun_col = lerp(float3(0.30, 0.36, 0.50) * 0.35, float3(1.0, 0.94, 0.84) * 1.7, day);
    float3 col;
    if (i.c.a > 0.5) {
        float3 n = normalize(i.n);
        if (dot(n, V) < 0) n = -n;
        float sh = shadow_at(i.wp, n, i.pos.xy);
        float nl = saturate(dot(n, L));
        if (fog.w > 0) return float4(sh, nl, 0, 1);
        float hemi = dot(n, up.xyz) * 0.5 + 0.5;
        float3 ambient = lerp(lin(fog.rgb) * 0.3, float3(0.30, 0.38, 0.55), hemi) * (0.5 + sun.w) * lerp(0.28, 1.0, day);
        float gloss = info.w * set.y;
        float rough = gloss > 0 ? lerp(0.85, 0.45, saturate(gloss * 2.2)) : 0.85;
        float a = rough * rough;
        float3 H = normalize(L + V);
        float NoV = max(dot(n, V), 1e-3), NoH = saturate(dot(n, H)), LoH = saturate(dot(L, H));
        float3 F = F_Schlick(0.04, LoH);
        float3 Fr = D_GGX(NoH, a) * V_SmithGGXCorrelated(NoV, nl, a) * F * PI;
        col = (albedo * (1 - F) + Fr) * sun_col * nl * sh;
        float2 ab = dfg(NoV, rough);
        float spec_occ = 0.5 + 0.5 * sh;
        col += albedo * ambient + sky_rough(reflect(-V, n), n, rough) * (0.04 * ab.x + ab.y) * spec_occ * 0.5 * set.y * lerp(0.15, 1.0, day);
        float cc = saturate(gloss * 0.8);
        if (cc > 0) {
            const float ca = 0.08 * 0.08;
            float Fc = F_Schlick(0.04, LoH).x * cc;
            float Frc = D_GGX(NoH, ca) * V_Kelemen(LoH) * Fc * PI;
            col = col * (1 - Fc) + Frc * sun_col * nl * sh;
            float Fcv = (0.04 + 0.96 * pow(1 - NoV, 5)) * cc;
            col = col * (1 - Fcv) + sky_env(reflect(-V, n)) * spec_occ * Fcv * lerp(0.15, 1.0, day);
        }
    } else {
        float sh = shadow_at(i.wp, up.xyz, i.pos.xy);
        // baked vertex light: ACES lifts low values ~1.45x, so 0.75 sunlit matches the original;
        // shadowed ground keeps 43% (skylight)
        col = albedo * lin(i.c.rgb) * (0.32 + 0.43 * sh);
        if (fog.w > 0) return float4(sh, 0, 1, 1);
    }
    float dist = length(i.wp - cam_pos.xyz);
    float haze = 1 - exp(-dist / params.z);
    col = lerp(col, lerp(lin(fog.rgb), float3(0.45, 0.55, 0.75), 0.4), haze * 0.35 * set2.w);
    col = aces(col * params.w);
    return float4(pow(col, 1 / 2.2), t.a);
}
