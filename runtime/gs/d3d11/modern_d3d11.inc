// modern_d3d11.inc - the EOD_MODERN pass (modern_scene.h), included at the end of
// gs_d3d11.cpp inside gs::hw so it shares the device, the surfaces and the
// texture cache. It draws the frame's sky, world and objects from the decoded
// batches at the internal resolution with 4x MSAA and 16x anisotropic filtering:
// linear-space lighting from the game's own light direction, a sun shadow map,
// glossy car paint (specular + sky reflection with Fresnel), distance haze in the
// game's fog colour and filmic tone mapping. The result is resolved into the
// game's frame buffer before the GS draws the HUD on top.

namespace {

const char k_modern_hlsl[] = R"(
cbuffer Frame : register(b0) {
    float4 clip_x, clip_y, clip_w;  // world -> clip rows (z is set below)
    float4 params;                  // x near, y shadow texel, z haze distance, w exposure
    float4 cam_pos;                 // world camera position
    float4 up;                      // world up
    float4 sh_x, sh_y, sh_z;        // world -> shadow map rows
    float4 fog;                     // fog colour (gamma)
    float4 sun;                     // world direction to the sun, w ambient
    float4 gsz;                     // x 1 / 2^bits of the GS depth format
    float4 c0_x, c0_y, c0_z;        // cascade 0 (near, sharp) world -> shadow rows
    float4 c1_x, c1_y, c1_z;        // cascade 1 (far)
    float4 cs;                      // x, y world size of a texel in cascade 0 / 1; z, w depth range of each
    float4 post;                    // x pixels per world unit at w = 1, y AO radius (world), z bloom threshold, w bloom
    float4 post2;                   // x 1 / width, y 1 / height, zw blur step (uv)
    float4 set;                     // settings: x shadows, y reflections, z ambient occlusion, w vignette
    float4 set2;                    // x bloom, y colour grade, z soft shadows (light size), w haze
};
cbuffer Batch : register(b1) {
    float4 m0, m1, m2;   // model rows
    float4 light;        // world direction, w = ambient (VU1 qword 17)
    float4 info;         // x role (0 sky, 1 world, 2 object), y layer, z textured, w gloss
    float4 info2;        // x 1: an additive/subtractive GS layer (unlit, as the game draws it), y vertex alpha,
                         // z alpha from the vertex only, w use the mesh's vertex alpha
    float4 atest;        // the GS alpha test: x method (-1 off), y reference (GS units), z discard on fail
    float4 zrow;         // the batch's GS depth row (model space)
};
Texture2D tex : register(t0);
Texture2D<float> shadow_map : register(t1);
SamplerState smp : register(s0);
SamplerComparisonState shadow_smp : register(s1);
struct VIn {
    float3 p : POSITION;
    float2 uv : TEXCOORD0;
    float4 n : NORMAL;
    float4 c : COLOR;
    float2 va : TEXCOORD1;  // the kicked vertex alpha (0x80 = 1.0) and whether the primitive blends
};
struct VOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float3 n : TEXCOORD1;
    float3 wp : TEXCOORD2;
    float4 c : COLOR;
    float2 va : TEXCOORD3;
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
    float2 uv : TEXCOORD0;
    float2 va : TEXCOORD1;
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
// only what is visible casts: a blended primitive casts by its alpha as the GS forms it (the
// car's reflection and damage layers are kicked at alpha 0 until damaged), alpha-tested texels
// that fail do not cast
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
Texture2DMS<float> scene_depth : register(t3);
Texture2D<float4> post_src : register(t4);
Texture2D<float> ao_tex : register(t5);
Texture2D<float4> bloom_tex : register(t6);
SamplerState lin_clamp : register(s2);
float view_w(int2 p) {
    float d = scene_depth.Load(p, 0);
    return d > 0 ? params.x / d : 1e9;
}
float ps_ao(float4 pos : SV_Position) : SV_Target {  // half resolution
    int2 p = int2(pos.xy * 2);
    float w = view_w(p);
    if (w > 1e8) return 1;
    // the local surface plane in screen space: a sample only occludes when it is nearer than the
    // plane predicts (slopes and car panels do not shadow themselves)
    float gx = (view_w(p + int2(2, 0)) - view_w(p - int2(2, 0))) * 0.25;
    float gy = (view_w(p + int2(0, 2)) - view_w(p - int2(0, 2))) * 0.25;
    if (abs(gx) > w || abs(gy) > w) gx = gy = 0;  // a depth edge: no plane
    float r = clamp(post.y * post.x / w, 3, 80);
    int2 cell = int2(pos.xy) & 3;  // 4x4 interleaved rotation, removed by the blur in ps_comp
    float a0 = (cell.x * 4 + cell.y) * (6.2832 / 16);
    float occ = 0;
    [unroll] for (int k = 0; k < 12; k++) {
        float a = a0 + k * 2.3999, rr = r * (k + 0.5) / 12;
        float2 o = float2(cos(a), sin(a)) * rr;
        float pred = w + gx * o.x + gy * o.y;
        float dz = pred - view_w(p + int2(o));  // > 0: nearer than the surface
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
    c = saturate(lerp(l.xxx, c, 1 + 0.12 * set2.y));    // a little more colour
    c = lerp(c, c * c * (3 - 2 * c), 0.35 * set2.y);    // gentle S-curve
    float2 v = uv - 0.5;
    c *= 1 - dot(v, v) * set.w;                         // vignette (off unless chosen)
    return float4(c, 128.0 / 255.0);
}

// the GS frame so far (the game's own sky), copied into the multisampled target
Texture2D<float4> gs_frame : register(t2);
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
// soft shadows that harden at contact (PCSS): the blocker's distance sets the filter width
// range: world depth of the cascade (z unit), texel: world size of one of its texels
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
        // the penumbra grows with the distance to the blocker (a sun about 1.5 degrees across, softened)
        r = clamp((s.z - zb) * range / texel * set2.z, 1.5, 12);
    }
    float sum = 0;
    [unroll] for (int k = 0; k < 16; k++)
        sum += shadow_map.SampleCmpLevelZero(shadow_smp, uv + mul(R, k_disk[k]) * tx * r, s.z);
    return sum / 16;
}
float shadow_raw(float3 wp, float3 n, float2 pix) {
    // a per-pixel rotation of the sample disk (interleaved gradient noise): no banding
    float ign = frac(52.9829189 * frac(dot(pix, float2(0.06711056, 0.00583715))));
    float2 rot = float2(cos(ign * 6.2832), sin(ign * 6.2832));
    // normal offset by a texel and a half of the cascade used: no acne, no floating shadows
    float4 w0 = float4(wp + n * cs.x * 1.5, 1);
    float3 s0 = float3(dot(c0_x, w0), dot(c0_y, w0), dot(c0_z, w0));
    if (all(abs(s0.xy) < 0.9) && s0.z < 1) return shadow_cascade(s0, 0, cs.z, cs.x, rot);
    float4 w1 = float4(wp + n * cs.y * 1.5, 1);
    float3 s1 = float3(dot(c1_x, w1), dot(c1_y, w1), dot(c1_z, w1));
    if (any(abs(s1.xy) > 1) || s1.z > 1) return 1;
    float k = saturate(1 - max(abs(s1.x), abs(s1.y)) * 1.1 + 0.1) * 4;  // fade out at the far edge
    return lerp(1, shadow_cascade(s1, 0.5, cs.w, cs.y, rot), saturate(k));
}
float shadow_at(float3 wp, float3 n, float2 pix) {
    if (set.x <= 0) return 1;
    return lerp(1, shadow_raw(wp, n, pix), set.x);
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
// the split-sum DFG term, analytic fit (Karis): scale and bias of f0
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
// the sky seen by a rough surface: the reflection widens toward the whole hemisphere
float3 sky_rough(float3 r, float3 n, float rough) {  // (dimmed: the ground reflects too)
    float3 d = normalize(lerp(r, n, rough * rough));
    float3 avg = lerp(lin(fog.rgb) * 0.6, float3(0.30, 0.40, 0.58), dot(n, up.xyz) * 0.5 + 0.5);
    return lerp(sky_env(d), avg, rough * rough);
}
float4 ps_modern(VOut i) : SV_Target {
    float4 t = info.z > 0 ? tex.Sample(smp, i.uv) : float4(0.75, 0.75, 0.75, 128.0 / 255.0);
    // the GS alpha test on the source alpha (GS units: 0x80 = 1.0), as the game sets it per batch
    // source alpha by the texture function (GS): MODULATE At * Av, DECAL / HIGHLIGHT2 At,
    // HIGHLIGHT At + Av; without TCC only Av
    float av = info2.y * i.va.x;
    int tfx = (int)info2.w;
    float as_gs = info2.z > 0 ? av * 128 : tfx == 0 ? t.a * 255 * av : tfx == 2 ? t.a * 255 + av * 128 : t.a * 255;
    if (atest.x >= 0 && atest.z > 0) {  // AFAIL KEEP: a failing pixel writes nothing
        float r = atest.y;
        int m = (int)atest.x;
        bool ok = m == 1 || (m == 2 && as_gs < r) || (m == 3 && as_gs <= r) || (m == 4 && abs(as_gs - r) < 0.5) ||
                    (m == 5 && as_gs >= r) || (m == 6 && as_gs > r) || (m == 7 && abs(as_gs - r) >= 0.5);
        if (!ok) discard;
    }
    // a blended primitive that adds nothing (alpha 0) writes nothing, depth included
    if (i.va.y > 0.5 && as_gs < 1) discard;
    t.a = i.va.y > 0.5 ? saturate(as_gs / 128.0) : 1;  // an unblended primitive is opaque
    if (atest.x < 0 && t.a < (info.x == 2 && info2.y >= 0.99 ? 0.5 : 0.02)) discard;
    if (info.x == 0 || info2.x > 0) return float4(t.rgb * (i.c.a > 0.5 ? 1 : i.c.rgb), t.a);  // as authored
    float3 albedo = lin(t.rgb);
    float3 V = normalize(cam_pos.xyz - i.wp);
    float3 L = normalize(sun.xyz);
    float3 sun_col = float3(1.0, 0.94, 0.84) * 1.7;
    float3 col;
    if (i.c.a > 0.5) {
        float3 n = normalize(i.n);
        if (dot(n, V) < 0) n = -n;  // two-sided
        float sh = shadow_at(i.wp, n, i.pos.xy);
        float nl = saturate(dot(n, L));
        if (fog.w > 0) return float4(sh, nl, 0, 1);  // EOD_MODERN_DEBUG: red = lit by the sun, green = facing it
        float hemi = dot(n, up.xyz) * 0.5 + 0.5;
        float3 ambient = lerp(lin(fog.rgb) * 0.3, float3(0.30, 0.38, 0.55), hemi) * (0.5 + sun.w);
        // base layer: a dielectric, rougher where the game draws a plain object
        float gloss = info.w * set.y;
        float rough = gloss > 0 ? lerp(0.85, 0.45, saturate(gloss * 2.2)) : 0.85;
        float a = rough * rough;
        float3 H = normalize(L + V);
        float NoV = max(dot(n, V), 1e-3), NoH = saturate(dot(n, H)), LoH = saturate(dot(L, H));
        float3 F = F_Schlick(0.04, LoH);
        float3 Fr = D_GGX(NoH, a) * V_SmithGGXCorrelated(NoV, nl, a) * F * PI;
        col = (albedo * (1 - F) + Fr) * sun_col * nl * sh;
        float2 ab = dfg(NoV, rough);
        float spec_occ = 0.5 + 0.5 * sh;  // the sun's shadow stands in for the sky's occlusion
        col += albedo * ambient + sky_rough(reflect(-V, n), n, rough) * (0.04 * ab.x + ab.y) * spec_occ * 0.5 * set.y;
        // clear coat (car paint): a smooth varnish over the base, as Filament's clearCoat layer
        float cc = saturate(gloss * 0.8);  // weathered paint: a thin, worn varnish
        if (cc > 0) {
            const float ca = 0.08 * 0.08;
            float Fc = F_Schlick(0.04, LoH).x * cc;
            float Frc = D_GGX(NoH, ca) * V_Kelemen(LoH) * Fc * PI;
            col = col * (1 - Fc) + Frc * sun_col * nl * sh;
            float Fcv = (0.04 + 0.96 * pow(1 - NoV, 5)) * cc;
            col = col * (1 - Fcv) + sky_env(reflect(-V, n)) * spec_occ * Fcv;
        }
    } else {
        // baked lighting (the world's vertex colours) under the sun shadow
        float sh = shadow_at(i.wp, up.xyz, i.pos.xy);
        col = albedo * lin(i.c.rgb) * 1.35 * (0.5 + 0.5 * sh);
        if (fog.w > 0) return float4(sh, 0, 1, 1);
    }
    float dist = length(i.wp - cam_pos.xyz);
    float haze = 1 - exp(-dist / params.z);
    col = lerp(col, lerp(lin(fog.rgb), float3(0.45, 0.55, 0.75), 0.4), haze * 0.35 * set2.w);
    col = aces(col * params.w);
    return float4(pow(col, 1 / 2.2), t.a);
}
)";

struct MeshGpu {
    ID3D11Buffer *vb = nullptr, *ib = nullptr;
    UINT count = 0;
    bool normals = false;
};
std::unordered_map<uint64_t, MeshGpu> g_mmesh;
std::vector<ID3D11ShaderResourceView *> g_mtex;  // this frame's batch textures (one reference each)
ID3D11VertexShader *g_mvs, *g_mvs_shadow, *g_mvs_full, *g_mvs_gsdepth;
ID3D11PixelShader *g_mps, *g_mps_shadow, *g_mps_full, *g_mps_ao, *g_mps_bright, *g_mps_blur, *g_mps_comp;
// post targets: depth SRV of the scene, AO and two bloom buffers at half resolution, the final image
ID3D11ShaderResourceView *g_mdepth_srv, *g_mresolve_srv;
ID3D11Texture2D *g_mao, *g_mb1, *g_mb2, *g_mfinal;
ID3D11RenderTargetView *g_mao_rtv, *g_mb1_rtv, *g_mb2_rtv, *g_mfinal_rtv;
ID3D11ShaderResourceView *g_mao_srv, *g_mb1_srv, *g_mb2_srv;
ID3D11SamplerState *g_mlin_clamp;
bool g_mpost;  // the post chain is available (MSAA depth readable: feature level 10.1+)
ID3D11InputLayout *g_mlayout;
ID3D11Buffer *g_malpha_vb, *g_mones_vb;
constexpr UINT kAlphaRing = 1 << 20;
UINT g_malpha_pos;
ID3D11Buffer *g_mcb_frame, *g_mcb_batch;
ID3D11SamplerState *g_msampler, *g_mshadow_smp;
ID3D11SamplerState *g_msamplers[4];  // repeat / clamp in u (bit 0) and v (bit 1)
ID3D11BlendState *g_mblend;
ID3D11DepthStencilState *g_mds_sky, *g_mds_draw, *g_mds_shadow;
ID3D11RasterizerState *g_mrs, *g_mrs_shadow, *g_mrs_gsdepth;
ID3D11Texture2D *g_mcolor, *g_mdepth, *g_mresolve, *g_mshadow;
ID3D11RenderTargetView *g_mrtv;
ID3D11DepthStencilView *g_mdsv, *g_mshadow_dsv;
ID3D11ShaderResourceView *g_mshadow_srv;
constexpr int kShadowSize = 4096;  // atlas width: two 2048x2048 cascades side by side
int g_mw, g_mh;
UINT g_msamples = 4;
bool g_mready, g_mfailed;

struct ModernFrameCB {
    float clip_x[4], clip_y[4], clip_w[4], params[4], cam_pos[4], up[4], sh_x[4], sh_y[4], sh_z[4], fog[4], sun[4], gsz[4];
    float c0_x[4], c0_y[4], c0_z[4], c1_x[4], c1_y[4], c1_z[4], cs[4], post[4], post2[4], set[4], set2[4];
};
struct ModernBatchCB {
    float m[12], light[4], info[4], info2[4], atest[4], zrow[4];
};
std::vector<uint32_t> g_matest;  // per batch: ate | atst << 1 | afail << 4 | aref << 8
// per batch of this frame: the GS blend (abe | a << 1 | b << 3 | c << 5 | d << 7 | fix << 9), ~0 unknown
std::vector<uint32_t> g_mblend_key;
std::vector<uint64_t> g_malpha;  // per batch: vertex alpha sum << 24 | draws
std::unordered_map<uint32_t, ID3D11BlendState *> g_mblend_cache;

// The GS blend (A - B) * C + D (A, B, D: 0 Cs, 1 Cd, 2 zero; C: 0 As, 1 Ad, 2 FIX) as a D3D11 blend
// state. raw: the layer is drawn unlit (add / subtract layers). Ad is taken as 1.0 (the modern
// target's alpha is the constant 0x80). The alpha channel always gets the blend factor's alpha.
ID3D11BlendState *modern_blend(uint32_t key, bool &raw) {
    raw = false;
    if (key != ~0u && !(key & 1)) {  // the game draws it without blending
        static ID3D11BlendState *opaque = [] {
            D3D11_BLEND_DESC bl = {};
            bl.RenderTarget[0].BlendEnable = TRUE;
            bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
            bl.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
            bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_BLEND_FACTOR;
            bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
            bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            ID3D11BlendState *b = nullptr;
            g_dev->CreateBlendState(&bl, &b);
            return b;
        }();
        return opaque;
    }
    if (key == ~0u) key = 1 | 0 << 1 | 1 << 3 | 0 << 5 | 1 << 7;  // unknown: (Cs - Cd) * As + Cd
    const int A = (key >> 1) & 3, B = (key >> 3) & 3, C = (key >> 5) & 3, D = (key >> 7) & 3;
    const uint32_t ck = key & 0x1FF;
    auto it = g_mblend_cache.find(ck);
    D3D11_BLEND c = C == 0 ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_BLEND_FACTOR;
    D3D11_BLEND ic = C == 0 ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_BLEND_FACTOR;
    D3D11_BLEND sf = c, df = ic;
    D3D11_BLEND_OP op = D3D11_BLEND_OP_ADD;
    if (A == 0 && B == 1 && D == 1) {  // mix
    } else if (A == 0 && B == 2 && D == 1) {  // add
        df = D3D11_BLEND_ONE;
        raw = true;
    } else if (A == 2 && B == 0 && D == 1) {  // subtract
        df = D3D11_BLEND_ONE;
        op = D3D11_BLEND_OP_REV_SUBTRACT;
        raw = true;
    } else if (A == 1 && B == 0 && D == 0) {  // reverse mix
        sf = ic;
        df = c;
    } else if (A == 0 && B == 2 && D == 2) {  // scaled source
        df = D3D11_BLEND_ZERO;
    }
    if (it != g_mblend_cache.end()) return it->second;
    D3D11_BLEND_DESC bl = {};
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = sf;
    bl.RenderTarget[0].DestBlend = df;
    bl.RenderTarget[0].BlendOp = op;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_BLEND_FACTOR;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ID3D11BlendState *bs = nullptr;
    CHECK(g_dev->CreateBlendState(&bl, &bs));
    g_mblend_cache[ck] = bs;
    return bs;
}

bool modern_init() {
    if (g_mready || g_mfailed) return g_mready;
    g_mfailed = true;
    typedef HRESULT(WINAPI * Compile_t)(LPCVOID, SIZE_T, LPCSTR, const void *, void *, LPCSTR, LPCSTR, UINT, UINT,
                                        ID3DBlob **, ID3DBlob **);
    Compile_t compile = nullptr;
    for (const char *d : {"d3dcompiler_47.dll", "d3dcompiler_46.dll", "d3dcompiler_43.dll"})
        if (HMODULE m = LoadLibraryA(d))
            if ((compile = (Compile_t)GetProcAddress(m, "D3DCompile"))) break;
    if (!compile) {
        WARN("modern renderer: no shader compiler (d3dcompiler_47.dll)");
        return false;
    }
    struct Entry {
        const char *name, *target;
        ID3DBlob *blob;
    } e[11] = {{"vs_modern", "vs_4_0", nullptr}, {"ps_modern", "ps_4_0", nullptr}, {"vs_shadow", "vs_4_0", nullptr},
              {"ps_shadow", "ps_4_0", nullptr},   {"vs_full", "vs_4_0", nullptr},   {"ps_full", "ps_4_0", nullptr},
              {"vs_gsdepth", "vs_4_0", nullptr}, {"ps_ao", "ps_4_1", nullptr},  {"ps_bright", "ps_4_0", nullptr},
              {"ps_blur", "ps_4_0", nullptr},    {"ps_comp", "ps_4_0", nullptr}};
    for (Entry &x : e) {
        ID3DBlob *err = nullptr;
        if (FAILED(compile(k_modern_hlsl, sizeof k_modern_hlsl - 1, "modern.hlsl", nullptr, nullptr, x.name, x.target,
                           1 << 15 /* OPTIMIZATION_LEVEL3 */, 0, &x.blob, &err))) {
            fprintf(stderr, "[modern] shader %s: %s\n", x.name, err ? (const char *)err->GetBufferPointer() : "?");
            return false;
        }
    }
    CHECK(g_dev->CreateVertexShader(e[0].blob->GetBufferPointer(), e[0].blob->GetBufferSize(), nullptr, &g_mvs));
    CHECK(g_dev->CreatePixelShader(e[1].blob->GetBufferPointer(), e[1].blob->GetBufferSize(), nullptr, &g_mps));
    CHECK(g_dev->CreateVertexShader(e[2].blob->GetBufferPointer(), e[2].blob->GetBufferSize(), nullptr, &g_mvs_shadow));
    CHECK(g_dev->CreatePixelShader(e[3].blob->GetBufferPointer(), e[3].blob->GetBufferSize(), nullptr, &g_mps_shadow));
    CHECK(g_dev->CreateVertexShader(e[4].blob->GetBufferPointer(), e[4].blob->GetBufferSize(), nullptr, &g_mvs_full));
    CHECK(g_dev->CreatePixelShader(e[5].blob->GetBufferPointer(), e[5].blob->GetBufferSize(), nullptr, &g_mps_full));
    CHECK(g_dev->CreateVertexShader(e[6].blob->GetBufferPointer(), e[6].blob->GetBufferSize(), nullptr, &g_mvs_gsdepth));
    if (g_level >= D3D_FEATURE_LEVEL_10_1)
        CHECK(g_dev->CreatePixelShader(e[7].blob->GetBufferPointer(), e[7].blob->GetBufferSize(), nullptr, &g_mps_ao));
    CHECK(g_dev->CreatePixelShader(e[8].blob->GetBufferPointer(), e[8].blob->GetBufferSize(), nullptr, &g_mps_bright));
    CHECK(g_dev->CreatePixelShader(e[9].blob->GetBufferPointer(), e[9].blob->GetBufferSize(), nullptr, &g_mps_blur));
    CHECK(g_dev->CreatePixelShader(e[10].blob->GetBufferPointer(), e[10].blob->GetBufferSize(), nullptr, &g_mps_comp));
    D3D11_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R8G8B8A8_SNORM, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R8G8_UNORM, 1, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    {  // per-vertex alpha: a ring for the car passes, and 0x80 everywhere else
        D3D11_BUFFER_DESC vb = {};
        vb.Usage = D3D11_USAGE_DYNAMIC;
        vb.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        vb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        vb.ByteWidth = kAlphaRing;
        CHECK(g_dev->CreateBuffer(&vb, nullptr, &g_malpha_vb));
        static uint8_t ones[65536];  // alpha 0x80, blending as the batch's state says
        for (size_t k = 0; k < sizeof ones; k += 2) ones[k] = 0x80, ones[k + 1] = 0xFF;
        vb.Usage = D3D11_USAGE_IMMUTABLE;
        vb.CPUAccessFlags = 0;
        vb.ByteWidth = sizeof ones;
        D3D11_SUBRESOURCE_DATA od = {ones, 0, 0};
        CHECK(g_dev->CreateBuffer(&vb, &od, &g_mones_vb));
    }
    CHECK(g_dev->CreateInputLayout(il, 5, e[0].blob->GetBufferPointer(), e[0].blob->GetBufferSize(), &g_mlayout));
    for (Entry &x : e) x.blob->Release();
    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.ByteWidth = sizeof(ModernFrameCB);
    CHECK(g_dev->CreateBuffer(&bd, nullptr, &g_mcb_frame));
    bd.ByteWidth = sizeof(ModernBatchCB);
    CHECK(g_dev->CreateBuffer(&bd, nullptr, &g_mcb_batch));
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_ANISOTROPIC;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxAnisotropy = 16;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    CHECK(g_dev->CreateSamplerState(&sd, &g_msampler));
    {
        D3D11_SAMPLER_DESC lc = {};
        lc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        lc.AddressU = lc.AddressV = lc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        lc.MaxLOD = D3D11_FLOAT32_MAX;
        CHECK(g_dev->CreateSamplerState(&lc, &g_mlin_clamp));
    }
    for (int k = 0; k < 4; k++) {
        D3D11_SAMPLER_DESC s2 = sd;
        s2.AddressU = (k & 1) ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_WRAP;
        s2.AddressV = (k & 2) ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_WRAP;
        CHECK(g_dev->CreateSamplerState(&s2, &g_msamplers[k]));
    }
    sd.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    sd.BorderColor[0] = sd.BorderColor[1] = sd.BorderColor[2] = sd.BorderColor[3] = 1.0f;
    sd.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    sd.MaxAnisotropy = 1;
    CHECK(g_dev->CreateSamplerState(&sd, &g_mshadow_smp));
    D3D11_BLEND_DESC bl = {};
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    // the frame buffer's alpha is the GS's 0x80 (1.0): the blend factor's alpha
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_BLEND_FACTOR;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    CHECK(g_dev->CreateBlendState(&bl, &g_mblend));
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    CHECK(g_dev->CreateDepthStencilState(&dd, &g_mds_sky));
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    CHECK(g_dev->CreateDepthStencilState(&dd, &g_mds_draw));
    dd.DepthFunc = D3D11_COMPARISON_LESS;
    CHECK(g_dev->CreateDepthStencilState(&dd, &g_mds_shadow));
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.MultisampleEnable = TRUE;
    CHECK(g_dev->CreateRasterizerState(&rd, &g_mrs));
    rd.MultisampleEnable = FALSE;
    CHECK(g_dev->CreateRasterizerState(&rd, &g_mrs_gsdepth));
    rd.DepthBias = 100;
    rd.SlopeScaledDepthBias = 1.5f;
    rd.DepthBiasClamp = 0.01f;
    CHECK(g_dev->CreateRasterizerState(&rd, &g_mrs_shadow));
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = kShadowSize;
    td.Height = kShadowSize / 2;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    CHECK(g_dev->CreateTexture2D(&td, nullptr, &g_mshadow));
    if (g_mshadow) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dv = {};
        dv.Format = DXGI_FORMAT_D32_FLOAT;
        dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        CHECK(g_dev->CreateDepthStencilView(g_mshadow, &dv, &g_mshadow_dsv));
        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = DXGI_FORMAT_R32_FLOAT;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        CHECK(g_dev->CreateShaderResourceView(g_mshadow, &sv, &g_mshadow_srv));
    }
    UINT q = 0;
    while (g_msamples > 1 && (FAILED(g_dev->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, g_msamples, &q)) || !q))
        g_msamples /= 2;
    g_mready = g_mvs && g_mps && g_mvs_shadow && g_mps_shadow && g_mvs_full && g_mps_full && g_mvs_gsdepth && g_mlayout && g_mcb_frame && g_mcb_batch && g_msampler &&
               g_mshadow_smp && g_mblend && g_mds_sky && g_mds_draw && g_mds_shadow && g_mrs && g_mrs_shadow &&
               g_mshadow_dsv && g_mshadow_srv;
    g_mfailed = !g_mready;
    if (g_mready)
        fprintf(stderr, "[modern] renderer ready: %ux MSAA, 16x anisotropic, %d sun shadow atlas, scale %d, post %s\n",
                g_msamples, kShadowSize, g_scale, g_mps_ao ? "AO + bloom" : "off (feature level 10.0)");
    return g_mready;
}

bool modern_targets(int w, int h) {
    if (g_mcolor && g_mw == w && g_mh == h) return true;
    release(g_mrtv);
    release(g_mdsv);
    release(g_mcolor);
    release(g_mdepth);
    release(g_mresolve);
    for (ID3D11ShaderResourceView **v : {&g_mdepth_srv, &g_mresolve_srv, &g_mao_srv, &g_mb1_srv, &g_mb2_srv}) release(*v);
    for (ID3D11RenderTargetView **v : {&g_mao_rtv, &g_mb1_rtv, &g_mb2_rtv, &g_mfinal_rtv}) release(*v);
    for (ID3D11Texture2D **v : {&g_mao, &g_mb1, &g_mb2, &g_mfinal}) release(*v);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = g_msamples;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(CHECK(g_dev->CreateTexture2D(&td, nullptr, &g_mcolor)))) return false;
    CHECK(g_dev->CreateRenderTargetView(g_mcolor, nullptr, &g_mrtv));
    const bool readable_depth = g_level >= D3D_FEATURE_LEVEL_10_1 && g_mps_ao;
    td.Format = readable_depth ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_D32_FLOAT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | (readable_depth ? D3D11_BIND_SHADER_RESOURCE : 0);
    if (FAILED(CHECK(g_dev->CreateTexture2D(&td, nullptr, &g_mdepth)))) return false;
    {
        D3D11_DEPTH_STENCIL_VIEW_DESC dv = {};
        dv.Format = DXGI_FORMAT_D32_FLOAT;
        dv.ViewDimension = g_msamples > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
        CHECK(g_dev->CreateDepthStencilView(g_mdepth, &dv, &g_mdsv));
        if (readable_depth) {
            D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
            sv.Format = DXGI_FORMAT_R32_FLOAT;
            sv.ViewDimension = g_msamples > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MipLevels = 1;
            CHECK(g_dev->CreateShaderResourceView(g_mdepth, &sv, &g_mdepth_srv));
        }
    }
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(CHECK(g_dev->CreateTexture2D(&td, nullptr, &g_mresolve)))) return false;
    CHECK(g_dev->CreateShaderResourceView(g_mresolve, nullptr, &g_mresolve_srv));
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    CHECK(g_dev->CreateTexture2D(&td, nullptr, &g_mfinal));
    if (g_mfinal) CHECK(g_dev->CreateRenderTargetView(g_mfinal, nullptr, &g_mfinal_rtv));
    D3D11_TEXTURE2D_DESC hd = td;
    hd.Width = (UINT)std::max(1, w / 2);
    hd.Height = (UINT)std::max(1, h / 2);
    hd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    auto half_target = [&](DXGI_FORMAT f, ID3D11Texture2D **t, ID3D11RenderTargetView **r, ID3D11ShaderResourceView **v) {
        hd.Format = f;
        if (SUCCEEDED(CHECK(g_dev->CreateTexture2D(&hd, nullptr, t)))) {
            CHECK(g_dev->CreateRenderTargetView(*t, nullptr, r));
            CHECK(g_dev->CreateShaderResourceView(*t, nullptr, v));
        }
    };
    half_target(DXGI_FORMAT_R8_UNORM, &g_mao, &g_mao_rtv, &g_mao_srv);
    half_target(DXGI_FORMAT_R8G8B8A8_UNORM, &g_mb1, &g_mb1_rtv, &g_mb1_srv);
    half_target(DXGI_FORMAT_R8G8B8A8_UNORM, &g_mb2, &g_mb2_rtv, &g_mb2_srv);
    g_mpost = readable_depth && g_mdepth_srv && g_mresolve_srv && g_mfinal_rtv && g_mao_rtv && g_mb1_rtv && g_mb2_rtv &&
              g_mps_bright && g_mps_blur && g_mps_comp && g_mlin_clamp;
    g_mw = w;
    g_mh = h;
    return g_mrtv && g_mdsv;
}

void modern_upload(const modern::Mesh &m) {
    if (m.idx.empty() || g_mmesh.count(m.id)) return;
    MeshGpu g;
    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.ByteWidth = (UINT)(m.v.size() * sizeof(modern::Vertex));
    D3D11_SUBRESOURCE_DATA init = {m.v.data(), 0, 0};
    if (FAILED(CHECK(g_dev->CreateBuffer(&bd, &init, &g.vb)))) return;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    bd.ByteWidth = (UINT)(m.idx.size() * 4);
    init.pSysMem = m.idx.data();
    if (FAILED(CHECK(g_dev->CreateBuffer(&bd, &init, &g.ib)))) {
        release(g.vb);
        return;
    }
    g.count = (UINT)m.idx.size();
    g.normals = !m.v.empty() && m.v[0].n[3] != 0;
    g_mmesh.emplace(m.id, g);
}

void modern_release_textures() {
    for (ID3D11ShaderResourceView *&s : g_mtex) release(s);
    g_mtex.clear();
    g_mblend_key.clear();
    g_malpha.clear();
    g_matest.clear();
}

// gloss of the car body passes (paint, detail, damage) and other objects
float modern_gloss(const modern::Batch &b) {
    switch (b.pc) {
    case 0x2e0: return 0.45f;
    case 0xd48: return 0.2f;
    case 0x15a8: return 0.1f;
    }
    return b.role == modern::OBJECT ? 0.08f : 0.0f;
}

void norm3(float *v) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0)
        for (int i = 0; i < 3; i++) v[i] /= l;
}
float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
void cross3(const float *a, const float *b, float *o) {
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}

void modern_batches(const modern::Frame &f, ModernBatchCB &bc, bool shadow_pass, std::initializer_list<uint32_t> roles,
                    uint32_t &drawn) {
    D3D11_MAPPED_SUBRESOURCE mp;
    for (uint32_t role : roles) {
        if (!shadow_pass) g_ctx->OMSetDepthStencilState(role == modern::SKY ? g_mds_sky : g_mds_draw, 0);
        for (size_t i = 0; i < f.batches.size(); i++) {
            const modern::Batch &b = f.batches[i];
            if (b.role != role || !b.drawn) continue;
            static const uint32_t skip_pc = getenv("EOD_MODERN_SKIP") ? (uint32_t)strtoul(getenv("EOD_MODERN_SKIP"), nullptr, 16) : 0;
            if (b.pc == skip_pc) continue;
            // the car body passes: the game drives their alpha per vertex (damage); a per-batch value
            // cannot stand in for it, so the damage overlay waits for per-vertex alpha
            // car passes without the per-vertex alpha (clipped): their alpha cannot be formed, so the
            // damage overlay is left out and the others are drawn plainly
            const bool car = (b.pc == 0x2e0 || b.pc == 0xd48 || b.pc == 0x15a8) && b.valpha.empty();
            if (car && b.pc == 0x15a8) continue;
            const bool per_vertex = !b.valpha.empty();
            auto it = g_mmesh.find(b.mesh);
            if (it == g_mmesh.end()) continue;
            ID3D11ShaderResourceView *srv = i < g_mtex.size() ? g_mtex[i] : nullptr;
            memcpy(bc.m, b.model, 48);
            memcpy(bc.light, b.light, 16);
            memcpy(bc.zrow, b.zrow, 16);
            bc.info[0] = (float)b.role;
            bc.info[1] = (float)b.layer;
            bc.info[2] = srv ? 1.0f : 0.0f;
            bc.info[3] = modern_gloss(b);
            const uint32_t key = i < g_mblend_key.size() ? g_mblend_key[i] : ~0u;
            bool raw = false;
            if (!shadow_pass && role != modern::SKY) {
                // per-vertex data: the primitive decides whether it blends, so the batch's equation is
                // used with blending on (unblended primitives output alpha 1)
                const uint32_t bkey = per_vertex && key != ~0u ? (key | 1u) : key;
                ID3D11BlendState *bs = modern_blend(bkey, raw);
                const float fix = key == ~0u ? 1.0f : (float)((key >> 9) & 0xFF) / 128.0f;
                const float bf[4] = {fix, fix, fix, 128.0f / 255.0f};
                g_ctx->OMSetBlendState(bs ? bs : g_mblend, bf, 0xFFFFFFFF);
            }
            bc.info2[0] = raw ? 1.0f : 0.0f;
            const uint64_t va = i < g_malpha.size() ? g_malpha[i] : 0;
            // source alpha as the GS forms it: MODULATE / HIGHLIGHT multiply the texture alpha by the vertex
            // alpha, DECAL keeps the texture's, without TCC only the vertex alpha counts. It only matters
            // where the game blends by source alpha (the car damage layer fades in with it).
            const bool by_as = key != ~0u && (key & 1) && ((key >> 5) & 3) == 0;
            const int tfx = (int)((key >> 17) & 3);
            const float av = (va & 0xFFFFFF) ? std::min(2.0f, (float)(va >> 24) / (float)(va & 0xFFFFFF) / 128.0f) : 1.0f;
            bc.info2[1] = by_as && tfx != 1 ? av : 1.0f;
            bc.info2[2] = key != ~0u && !((key >> 19) & 1) ? 1.0f : 0.0f;  // alpha from the vertex only
            if (key != ~0u && tfx != 1) bc.info2[1] = b.valpha.empty() ? av : 1.0f;  // per vertex: in the stream
            bc.info2[3] = key == ~0u ? 0.0f : (float)tfx;
            // the game's CLAMP per texture: repeat or clamp in u and v
            {
                const uint32_t wm = key == ~0u ? 0 : (key >> 21) & 15;
                const int su = (wm & 3) == 1 || (wm & 3) == 2 ? 1 : 0, sv = ((wm >> 2) & 3) == 1 || ((wm >> 2) & 3) == 2 ? 1 : 0;
                ID3D11SamplerState *smp = g_msamplers[su | sv << 1];
                g_ctx->PSSetSamplers(0, 1, &smp);
            }
            const uint32_t at = i < g_matest.size() ? g_matest[i] : 0;
            bc.atest[0] = (at & 1) ? (float)((at >> 1) & 7) : -1.0f;
            bc.atest[1] = (float)((at >> 8) & 0xFF);
            bc.atest[2] = ((at >> 4) & 3) == 0 ? 1.0f : 0.0f;  // AFAIL KEEP: nothing is written
            static const bool no_atest = getenv("EOD_MODERN_NOATEST") != nullptr;
            if (no_atest || car) bc.atest[0] = -1.0f;
            if (car) bc.info2[1] = 1.0f, bc.info2[2] = 0.0f;
            if (raw) bc.info[3] = 0;
            if (SUCCEEDED(g_ctx->Map(g_mcb_batch, 0, D3D11_MAP_WRITE_DISCARD, 0, &mp))) {
                memcpy(mp.pData, &bc, sizeof bc);
                g_ctx->Unmap(g_mcb_batch, 0);
            }
            g_ctx->PSSetShaderResources(0, 1, &srv);
            UINT stride = sizeof(modern::Vertex), offset = 0;
            g_ctx->IASetVertexBuffers(0, 1, &it->second.vb, &stride, &offset);
            {
                ID3D11Buffer *avb = g_mones_vb;
                UINT astride = 2, aoff = 0;
                const UINT nv = (UINT)b.valpha.size();
                if (nv && g_malpha_vb) {
                    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
                    if (g_malpha_pos + nv > kAlphaRing) {
                        g_malpha_pos = 0;
                        mode = D3D11_MAP_WRITE_DISCARD;
                    }
                    D3D11_MAPPED_SUBRESOURCE am;
                    if (SUCCEEDED(g_ctx->Map(g_malpha_vb, 0, mode, 0, &am))) {
                        memcpy((uint8_t *)am.pData + g_malpha_pos, b.valpha.data(), nv);
                        g_ctx->Unmap(g_malpha_vb, 0);
                        avb = g_malpha_vb;
                        aoff = g_malpha_pos;
                        g_malpha_pos += (nv + 15) & ~15u;
                    }
                }
                g_ctx->IASetVertexBuffers(1, 1, &avb, &astride, &aoff);
            }
            g_ctx->IASetIndexBuffer(it->second.ib, DXGI_FORMAT_R32_UINT, 0);
            g_ctx->DrawIndexed(it->second.count, 0, 0);
            drawn++;
        }
    }
}

}  // namespace

void modern_alpha(int32_t batch, uint32_t alpha) {
    if (batch < 0) return;
    if ((size_t)batch >= g_malpha.size()) g_malpha.resize((size_t)batch + 1, 0);
    g_malpha[(size_t)batch] += (uint64_t)alpha << 24 | 1;
}

void modern_texture(int32_t batch, const Env &e) {
    if (!g_on || batch < 0) return;
    if ((size_t)batch >= g_mblend_key.size()) g_mblend_key.resize((size_t)batch + 1, ~0u);
    if ((size_t)batch >= g_matest.size()) g_matest.resize((size_t)batch + 1, 0);
    g_matest[(size_t)batch] = (e.ate ? 1u : 0u) | (uint32_t)(e.atst & 7) << 1 | (uint32_t)(e.afail & 3) << 4 | (e.aref & 0xFF) << 8;
    g_mblend_key[(size_t)batch] = (e.abe ? 1u : 0u) | (uint32_t)(e.ba & 3) << 1 | (uint32_t)(e.bb & 3) << 3 |
                                  (uint32_t)(e.bc & 3) << 5 | (uint32_t)(e.bd & 3) << 7 | (e.bfix & 0xFF) << 9 |
                                  (uint32_t)(e.tfx & 3) << 17 | (e.tcc ? 1u : 0u) << 19 | (e.tme ? 1u : 0u) << 20 |
                                  (uint32_t)(e.wms & 3) << 21 | (uint32_t)(e.wmt & 3) << 23;
    if (!e.tme) return;
    // render-to-texture sources (a surface owns the texture's memory) are left untextured here
    const uint32_t p0 = (e.tbp >> 5) % kPages;
    if (g_owner[p0] && !g_vram_ok[p0]) return;
    ID3D11ShaderResourceView *srv = vram_texture(e);
    if (!srv) return;
    if ((size_t)batch >= g_mtex.size()) g_mtex.resize((size_t)batch + 1, nullptr);
    if (g_mtex[(size_t)batch]) return;
    srv->AddRef();
    g_mtex[(size_t)batch] = srv;
}

void modern_draw(modern::Frame *frame) {
    std::unique_ptr<modern::Frame> f(frame);
    if (!modern_init()) {  // the GS draws everything again
        modern_release_textures();
        modern::disable();
        return;
    }
    for (const modern::Mesh &m : f->meshes) modern_upload(m);
    if (f->batches.empty() || g_skip_frame || !modern::g_target.valid) {
        modern_release_textures();
        return;
    }
    ProfScope prof(P_GS_DRAW);
    flush();
    const modern::Target &t = modern::g_target;
    const int cols = (int)((t.scissor >> 16) & 0x7FF) + 1, rows = (int)((t.scissor >> 48) & 0x7FF) + 1;
    Surface *s = get_surface(t.fbp, t.fbw, t.fpsm, false, rows);
    if (!s || !s->rtv || cols <= 0 || rows <= 0) {
        modern_release_textures();
        return;
    }
    const int W = s->w * g_scale, H = s->h * g_scale;
    if (!modern_targets(W, H)) {
        modern_release_textures();
        return;
    }
    acquire(s, 0, 0, std::min(cols, s->w), std::min(rows, s->h));
    s->version++;

    // world -> GS screen (rows x, y, w of the camera) -> clip for this surface
    const float *cm = f->cam;  // column-major: element (r, c) = cm[c * 4 + r]
    const float ox = (float)(t.xyoffset & 0xFFFF) / 16.0f, oy = (float)((t.xyoffset >> 32) & 0xFFFF) / 16.0f;
    const float sw = (float)s->w, sh = (float)s->h;
    ModernFrameCB fc = {};
    for (int c = 0; c < 4; c++) {
        const float x = cm[c * 4 + 0], y = cm[c * 4 + 1], w = cm[c * 4 + 3];
        fc.clip_x[c] = 2.0f / sw * x - (1.0f + 2.0f * ox / sw) * w;
        fc.clip_y[c] = -2.0f / sh * y + (1.0f + 2.0f * oy / sh) * w;
        fc.clip_w[c] = w;
    }
    // camera position: where the x, y and w rows all vanish
    double A[3][3], b3[3];
    const int rr[3] = {0, 1, 3};
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) A[r][c] = cm[c * 4 + rr[r]];
        b3[r] = -cm[12 + rr[r]];
    }
    auto det3 = [](double m[3][3]) {
        return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
               m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    };
    const double D = det3(A);
    float cam[3] = {0, 0, 0};
    if (std::fabs(D) > 1e-12)
        for (int k = 0; k < 3; k++) {
            double M2[3][3];
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 3; c++) M2[r][c] = c == k ? b3[r] : A[r][c];
            cam[k] = (float)(det3(M2) / D);
        }
    float fwd[3] = {cm[0 * 4 + 3], cm[1 * 4 + 3], cm[2 * 4 + 3]};
    norm3(fwd);
    // world up: against the screen's y direction (GS y grows downwards) at the screen centre
    const float cy = oy + sh * 0.5f;
    float up[3] = {-(cm[0 * 4 + 1] - cy * cm[0 * 4 + 3]), -(cm[1 * 4 + 1] - cy * cm[1 * 4 + 3]),
                   -(cm[2 * 4 + 1] - cy * cm[2 * 4 + 3])};
    {  // keep only the part across the view direction
        const float d = dot3(up, fwd);
        for (int i = 0; i < 3; i++) up[i] -= d * fwd[i];
        norm3(up);
    }
    // scene scale: the median camera distance of the objects (cars, props)
    std::vector<float> dists;
    float L[3] = {0, 1, 0};
    bool have_l = false;
    for (const modern::Batch &b : f->batches) {
        if (!b.drawn) continue;
        if (!have_l && b.role != modern::SKY) {
            memcpy(L, b.light, 12);
            have_l = true;
        }
        if (b.role != modern::OBJECT) continue;
        const float d[3] = {b.model[3] - cam[0], b.model[7] - cam[1], b.model[11] - cam[2]};
        dists.push_back(std::sqrt(dot3(d, d)));
    }
    norm3(L);
    float med = 40.0f;
    if (!dists.empty()) {
        std::nth_element(dists.begin(), dists.begin() + (long)dists.size() / 2, dists.end());
        med = std::max(dists[dists.size() / 2], 1.0f);
    }
    static const float k_shadow = getenv("EOD_MODERN_SHADOW") ? (float)atof(getenv("EOD_MODERN_SHADOW")) : 1.2f;
    const float k_haze = 40.0f;
    const Config &gc = g_config;
    auto pct = [](int v) { return std::clamp(v, 0, 200) / 100.0f; };
    const float k_exposure = pct(gc.modern_exposure);
    // scene scale for the shadow cascades: smoothed over many frames, so the cascades keep their
    // size (a size that changes each frame makes shadows swim and change softness)
    static float s_scale = 0;
    s_scale = s_scale == 0 ? med : s_scale + (med - s_scale) * 0.002f;
    float u[3], v[3];
    float ref[3] = {0, 0, 1};
    if (std::fabs(dot3(ref, L)) > 0.9f) ref[0] = 1, ref[2] = 0;
    cross3(L, ref, u);
    norm3(u);
    cross3(L, u, v);
    float casc[2][3][4];
    for (int c = 0; c < 2; c++) {
        const float half = s_scale * k_shadow * (c == 0 ? 0.35f : 2.0f);
        float ctr[3] = {cam[0] + fwd[0] * half * 0.6f, cam[1] + fwd[1] * half * 0.6f, cam[2] + fwd[2] * half * 0.6f};
        // snap the centre to whole texels in the light's plane: edges stay put while the camera moves
        const float texel = 2 * half / (kShadowSize / 2);
        float cu = std::floor(dot3(u, ctr) / texel) * texel, cv = std::floor(dot3(v, ctr) / texel) * texel;
        const float cl = dot3(L, ctr);
        for (int i = 0; i < 3; i++) {
            casc[c][0][i] = u[i] / half;
            casc[c][1][i] = v[i] / half;
            casc[c][2][i] = -L[i] / (6 * half);
        }
        casc[c][0][3] = -cu / half;
        casc[c][1][3] = -cv / half;
        casc[c][2][3] = 0.5f + cl / (6 * half);
        fc.cs[c] = texel;
        fc.cs[2 + c] = 6 * half;
    }
    memcpy(fc.c0_x, casc[0][0], 16);
    memcpy(fc.c0_y, casc[0][1], 16);
    memcpy(fc.c0_z, casc[0][2], 16);
    memcpy(fc.c1_x, casc[1][0], 16);
    memcpy(fc.c1_y, casc[1][1], 16);
    memcpy(fc.c1_z, casc[1][2], 16);
    fc.params[0] = 0.05f;  // near
    fc.params[1] = 1.0f / (kShadowSize / 2);  // one texel of a cascade (the atlas is 2:1)
    const float k_ao = 0.05f;
    const float k_bloom = 0.35f * pct(gc.modern_bloom);
    fc.set[0] = pct(gc.modern_shadows);
    fc.set[1] = pct(gc.modern_reflections);
    fc.set[2] = 0.75f * pct(gc.modern_ao);
    fc.set[3] = 0.6f * pct(gc.modern_vignette);
    fc.set2[0] = pct(gc.modern_bloom);
    fc.set2[1] = pct(gc.modern_grade);
    fc.set2[2] = gc.modern_soft_shadows ? 0.03f : 0.0f;
    fc.set2[3] = pct(gc.modern_haze);
    fc.post[0] = std::sqrt(fc.clip_x[0] * fc.clip_x[0] + fc.clip_x[1] * fc.clip_x[1] + fc.clip_x[2] * fc.clip_x[2]) * W * 0.5f;
    fc.post[1] = s_scale * k_ao;
    fc.post[2] = 0.72f;
    fc.post[3] = k_bloom;
    fc.post2[0] = 1.0f / W;
    fc.post2[1] = 1.0f / H;
    fc.params[2] = med * k_haze;
    fc.params[3] = k_exposure;
    memcpy(fc.cam_pos, cam, 12);
    memcpy(fc.up, up, 12);
    const uint64_t fog = st.fogcol;
    fc.fog[0] = (fog & 0xFF) / 255.0f;
    fc.fog[1] = ((fog >> 8) & 0xFF) / 255.0f;
    fc.fog[2] = ((fog >> 16) & 0xFF) / 255.0f;
    memcpy(fc.sun, L, 12);
    static const bool dbg = getenv("EOD_MODERN_DEBUG") != nullptr;
    fc.fog[3] = dbg ? 1.0f : 0.0f;
    fc.sun[3] = f->batches.empty() ? 0.19f : f->batches[0].light[3];
    static bool logged;
    if (!logged) {
        logged = true;
        fprintf(stderr, "[modern] camera (%.1f %.1f %.1f) up (%.2f %.2f %.2f) sun (%.2f %.2f %.2f) object median %.1f\n",
                cam[0], cam[1], cam[2], up[0], up[1], up[2], L[0], L[1], L[2], med);
    }
    D3D11_MAPPED_SUBRESOURCE mp;
    if (SUCCEEDED(CHECK(g_ctx->Map(g_mcb_frame, 0, D3D11_MAP_WRITE_DISCARD, 0, &mp)))) {
        memcpy(mp.pData, &fc, sizeof fc);
        g_ctx->Unmap(g_mcb_frame, 0);
    }

    ID3D11ShaderResourceView *null_srv[4] = {};
    g_ctx->PSSetShaderResources(0, 4, null_srv);
    g_ctx->IASetInputLayout(g_mlayout);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11Buffer *cbs[2] = {g_mcb_frame, g_mcb_batch};
    g_ctx->VSSetConstantBuffers(0, 2, cbs);
    g_ctx->PSSetConstantBuffers(0, 2, cbs);
    ID3D11SamplerState *ss[3] = {g_msampler, g_mshadow_smp, g_mlin_clamp};
    g_ctx->PSSetSamplers(0, 3, ss);
    ModernBatchCB bc = {};
    uint32_t drawn = 0;

    // sun shadow cascades: the atlas' left half (near) and right half (far)
    g_ctx->OMSetRenderTargets(0, nullptr, g_mshadow_dsv);
    g_ctx->ClearDepthStencilView(g_mshadow_dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    g_ctx->RSSetState(g_mrs_shadow);
    g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    g_ctx->OMSetDepthStencilState(g_mds_shadow, 0);
    g_ctx->VSSetShader(g_mvs_shadow, nullptr, 0);
    g_ctx->PSSetShader(g_mps_shadow, nullptr, 0);
    for (int c = 0; c < (fc.set[0] > 0 ? 2 : 0); c++) {
        memcpy(fc.sh_x, casc[c][0], 16);
        memcpy(fc.sh_y, casc[c][1], 16);
        memcpy(fc.sh_z, casc[c][2], 16);
        if (SUCCEEDED(CHECK(g_ctx->Map(g_mcb_frame, 0, D3D11_MAP_WRITE_DISCARD, 0, &mp)))) {
            memcpy(mp.pData, &fc, sizeof fc);
            g_ctx->Unmap(g_mcb_frame, 0);
        }
        D3D11_VIEWPORT vp = {(float)(c * kShadowSize / 2), 0, (float)(kShadowSize / 2), (float)(kShadowSize / 2), 0, 1};
        g_ctx->RSSetViewports(1, &vp);
        modern_batches(*f, bc, true, {(uint32_t)modern::WORLD, (uint32_t)modern::OBJECT}, drawn);
    }

    // the scene, over the GS frame so far (the game's sky)
    g_ctx->OMSetRenderTargets(1, &g_mrtv, g_mdsv);
    g_ctx->ClearDepthStencilView(g_mdsv, D3D11_CLEAR_DEPTH, 0.0f, 0);
    set_viewport(W, H);
    g_ctx->RSSetState(g_mrs);
    g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    g_ctx->OMSetDepthStencilState(g_mds_sky, 0);
    g_ctx->VSSetShader(g_mvs_full, nullptr, 0);
    g_ctx->PSSetShader(g_mps_full, nullptr, 0);
    g_ctx->PSSetShaderResources(2, 1, &s->srv);
    g_ctx->Draw(3, 0);
    g_ctx->PSSetShaderResources(2, 1, null_srv);
    const float bf[4] = {0, 0, 0, 128.0f / 255.0f};
    g_ctx->OMSetBlendState(g_mblend, bf, 0xFFFFFFFF);
    g_ctx->VSSetShader(g_mvs, nullptr, 0);
    g_ctx->PSSetShader(g_mps, nullptr, 0);
    g_ctx->PSSetShaderResources(1, 1, &g_mshadow_srv);
    // sky first (no depth), then the world in draw order (layers), then objects
    modern_batches(*f, bc, false, {(uint32_t)modern::SKY, (uint32_t)modern::WORLD, (uint32_t)modern::OBJECT}, drawn);

    // resolve, then copy the frame's rows into the game's frame buffer
    g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    g_ctx->PSSetShaderResources(0, 4, null_srv);
    g_ctx->ResolveSubresource(g_mresolve, 0, g_mcolor, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
    static const bool no_post = getenv("EOD_MODERN_NOPOST") != nullptr;
    ID3D11Texture2D *out = g_mresolve;
    if (g_mpost && !no_post) {
        auto post_pass = [&](ID3D11RenderTargetView *rt, int w, int h, ID3D11PixelShader *ps,
                             std::initializer_list<std::pair<UINT, ID3D11ShaderResourceView *>> srvs) {
            g_ctx->PSSetShaderResources(0, 4, null_srv);
            ID3D11ShaderResourceView *n4[4] = {};
            g_ctx->PSSetShaderResources(3, 4, n4);
            g_ctx->OMSetRenderTargets(1, &rt, nullptr);
            set_viewport(w, h);
            for (auto &sv : srvs) g_ctx->PSSetShaderResources(sv.first, 1, &sv.second);
            g_ctx->PSSetShader(ps, nullptr, 0);
            g_ctx->Draw(3, 0);
            g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
        };
        auto set_blur = [&](float dx, float dy) {
            fc.post2[2] = dx;
            fc.post2[3] = dy;
            if (SUCCEEDED(CHECK(g_ctx->Map(g_mcb_frame, 0, D3D11_MAP_WRITE_DISCARD, 0, &mp)))) {
                memcpy(mp.pData, &fc, sizeof fc);
                g_ctx->Unmap(g_mcb_frame, 0);
            }
        };
        set_blur(0, 0);
        g_ctx->RSSetState(g_mrs_gsdepth);
        g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        g_ctx->OMSetDepthStencilState(g_mds_sky, 0);
        g_ctx->IASetInputLayout(nullptr);
        g_ctx->VSSetShader(g_mvs_full, nullptr, 0);
        ID3D11SamplerState *ls = g_mlin_clamp;
        g_ctx->PSSetSamplers(2, 1, &ls);
        const int hw = std::max(1, W / 2), hh = std::max(1, H / 2);
        post_pass(g_mao_rtv, hw, hh, g_mps_ao, {{3u, g_mdepth_srv}});
        post_pass(g_mb1_rtv, hw, hh, g_mps_bright, {{4u, g_mresolve_srv}});
        set_blur(2.0f / W, 0);
        post_pass(g_mb2_rtv, hw, hh, g_mps_blur, {{4u, g_mb1_srv}});
        set_blur(0, 2.0f / H);
        post_pass(g_mb1_rtv, hw, hh, g_mps_blur, {{4u, g_mb2_srv}});
        post_pass(g_mfinal_rtv, W, H, g_mps_comp, {{4u, g_mresolve_srv}, {5u, g_mao_srv}, {6u, g_mb1_srv}});
        ID3D11ShaderResourceView *n4[4] = {};
        g_ctx->PSSetShaderResources(3, 4, n4);
        set_blur(0, 0);
        out = g_mfinal;
    }
    D3D11_BOX box = {0, 0, 0, (UINT)(std::min(cols, s->w) * g_scale), (UINT)(std::min(rows, s->h) * g_scale), 1};
    g_ctx->CopySubresourceRegion(s->tex, 0, 0, 0, 0, out, 0, &box);

    // the GS depth the replaced draws would have written, so what the GS draws next (particles,
    // the flag, the five camera-space batches before the HUD) is hidden behind the world correctly
    if (Surface *zs = get_surface(t.zbp, t.fbw, t.zpsm, true, rows)) {
        if (zs->dsv) {
            acquire(zs, 0, 0, std::min(cols, zs->w), std::min(rows, zs->h));
            zs->version++;
            const int bits = t.zpsm == 0x30 ? 32 : t.zpsm == 0x31 ? 24 : 16;
            fc.gsz[0] = (float)(1.0 / (double)(1ull << bits));
            if (SUCCEEDED(CHECK(g_ctx->Map(g_mcb_frame, 0, D3D11_MAP_WRITE_DISCARD, 0, &mp)))) {
                memcpy(mp.pData, &fc, sizeof fc);
                g_ctx->Unmap(g_mcb_frame, 0);
            }
            g_ctx->OMSetRenderTargets(0, nullptr, zs->dsv);
            set_viewport(zs->w * g_scale, zs->h * g_scale);
            g_ctx->RSSetState(g_mrs_gsdepth);
            g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
            g_ctx->OMSetDepthStencilState(g_mds_draw, 0);  // GEQUAL + write, as the GS draws the world
            g_ctx->VSSetShader(g_mvs_gsdepth, nullptr, 0);
            g_ctx->PSSetShader(g_mps_shadow, nullptr, 0);
            uint32_t dz = 0;
            modern_batches(*f, bc, true, {(uint32_t)modern::WORLD, (uint32_t)modern::OBJECT}, dz);
            g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
        }
    }
    g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    g_stats.draws += drawn;
    {  // diagnosis: what one frame (the 600th drawn) contained
        static int n;
        if (++n == 600) {
            int cnt[3][4] = {};  // role x (drawn, not kicked, no mesh, alpha-faded)
            for (size_t i = 0; i < f->batches.size(); i++) {
                const modern::Batch &b = f->batches[i];
                const int r = b.role < 3 ? (int)b.role : 2;
                const uint64_t va = i < g_malpha.size() ? g_malpha[i] : 0;
                if (!b.drawn) cnt[r][1]++;
                else if (!g_mmesh.count(b.mesh)) cnt[r][2]++;
                else cnt[r][0]++;
                if ((va & 0xFFFFFF) && (va >> 24) / (va & 0xFFFFFF) < 32) cnt[r][3]++;
            }
            std::map<uint32_t, std::array<int, 6>> pcs;  // pc -> batches, abe, alpha <32, <96, <160, more
            for (size_t i = 0; i < f->batches.size(); i++) {
                const modern::Batch &b = f->batches[i];
                if (b.role != modern::OBJECT || !b.drawn) continue;
                auto &c = pcs[b.pc | (i < g_mblend_key.size() && g_mblend_key[i] != ~0u ? (g_mblend_key[i] >> 17 & 7) << 16 : 7u << 16)];
                const uint64_t va = i < g_malpha.size() ? g_malpha[i] : 0;
                const uint32_t k = i < g_mblend_key.size() ? g_mblend_key[i] : 0;
                const uint64_t av = (va & 0xFFFFFF) ? (va >> 24) / (va & 0xFFFFFF) : 999;
                c[0]++;
                c[1] += (k != ~0u && (k & 1)) ? 1 : 0;
                c[av < 32 ? 2 : av < 96 ? 3 : av < 160 ? 4 : 5]++;
            }
            for (auto &kv : pcs)
                fprintf(stderr, "[modern] object pc %05x (tfx|tcc<<2 above 0xffff): %d batches, %d blended, vertex alpha <32 %d, <96 %d, <160 %d, more %d\n",
                        kv.first, kv.second[0], kv.second[1], kv.second[2], kv.second[3], kv.second[4], kv.second[5]);
            fprintf(stderr, "[modern] frame: sky %d/%d/%d/%d world %d/%d/%d/%d object %d/%d/%d/%d (drawn/unkicked/no mesh/low alpha)\n",
                    cnt[0][0], cnt[0][1], cnt[0][2], cnt[0][3], cnt[1][0], cnt[1][1], cnt[1][2], cnt[1][3], cnt[2][0],
                    cnt[2][1], cnt[2][2], cnt[2][3]);
        }
    }
    modern_release_textures();
    // the GS's next batch rebinds its own state (bind_targets, shaders, buffers, samplers)
    g_state_valid = false;
}
