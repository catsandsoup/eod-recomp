// native_shaders.hlsl - the native renderer's PS2 look (docs/NATIVE_RENDERER.md, stage R1).
// tools/build_native_shaders.py compiles each entry with DXC to SPIR-V (native_shaders_spv.inc,
// committed). Descriptor set 0: the object's texture and sampler; per-object values are push
// constants.
//
// Values are kept in GS units, as the GS stores them: vertex colours 0..128 (0x80 = 1.0),
// texels and frame buffer bytes 0..255, alpha 0x80 = 1.0. The colour target is the game's
// own frame buffer surface (R8G8B8A8_UNORM holding the GS bytes) and the depth target its
// depth buffer (D32 holding z / 2^bits), so native objects and GS draws test against each other.

struct Object {
    float4 cx, cy, cz, cw;  // model -> clip rows (camera packet maths folded with the surface mapping)
    float4 light;           // model-space light direction x the light scale (VU1 vf12), w: half vector x
    float4 diffuse;         // TOPS+4 rgb (0..128), w: half vector y
    float4 ambient;         // TOPS+5 rgb (0..128), w = vertex alpha (ITOP low byte)
    uint4 mode;             // x flags (below), y alpha reference | FOGCOL << 8, z FIX | F << 8,
                            // w: half vector z (float bits)
};
[[vk::push_constant]] Object obj;

// mode.x
#define M_TME 0x1        // textured
#define M_TCC 0x2        // texture alpha used
#define M_TFX_SHIFT 2    // 2 bits: modulate, decal, highlight, highlight2
#define M_ATE 0x10       // alpha test on
#define M_ATST_SHIFT 5   // 3 bits: never, always, less, lequal, equal, gequal, greater, notequal
#define M_AFAIL_DISCARD 0x100  // a failing pixel writes nothing
#define M_CFIX 0x200     // blend factor C = FIX (else As)
#define M_SPEC 0x400     // vertex alpha is the car body highlight (VU1 0x2E0), not ITOP
#define M_FGE 0x800      // GS PRIM.FGE (render flags 0x4D8BC8 bit 3, src/native_fog.cpp)
// stage R5 A2 (docs/NATIVE_RENDERER.md "Stage R5"): the GS texture unit, from the EE state
#define M_TW_SHIFT 12    // 4 bits: log2 TEX0 width
#define M_TH_SHIFT 16    // 4 bits: log2 TEX0 height
#define M_MXL_SHIFT 20   // 3 bits: TEX1.MXL (levels the texture holds beyond the base)
#define M_MMAG 0x800000  // TEX1.MMAG: bilinear at level 0
#define M_MMIN_SHIFT 24  // 3 bits: TEX1.MMIN
#define M_WMS_SHIFT 27   // 2 bits: CLAMP.WMS
#define M_WMT_SHIFT 29   // 2 bits: CLAMP.WMT
#define M_FAILED 0x80000000u  // draw only the pixels that fail the alpha test (AFAIL second pass)
// mode.z bits 16..27 TEX1.K (signed, 1/16), 28..29 TEX1.L, bit 30 TEX1.LCM

[[vk::binding(0, 0)]] Texture2D tex;
[[vk::binding(1, 0)]] SamplerState smp;

struct VIn {
    [[vk::location(0)]] float3 p : POSITION;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] int4 n : NORMAL;      // (byte - 128), w = 1 when the vertex has a normal
    [[vk::location(3)]] float4 c : COLOR;     // vertex colour bytes (0x80 = 1.0) when it has none
};
struct VOut {
    float4 pos : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
    [[vk::location(1)]] float4 c : COLOR;     // GS vertex colour 0..128 / alpha
    [[vk::location(2)]] float f : FOG;        // GS fog coefficient F (0..255), Gouraud like RGBA
    [[vk::location(3)]] float w : TEXCOORD1;  // clip w = 1 / GS Q (VU1 div Q, vf0w, w)
};

VOut vs_main(VIn i) {
    VOut o;
    float4 q = float4(i.p, 1);
    o.pos = float4(dot(obj.cx, q), dot(obj.cy, q), dot(obj.cz, q), dot(obj.cw, q));
    float3 rgb;
    if (i.n.w != 0) {
        // VU1 body loop: n = (b - 128) / 128, colour = min(TOPS+5 + max(0, n . vf12) * TOPS+4, 128)
        float3 n = float3(i.n.xyz) / 128.0;
        float d = max(0.0, dot(n, obj.light.xyz));
        rgb = min(obj.ambient.rgb + d * obj.diffuse.rgb, 128.0);
    } else {
        rgb = i.c.rgb * 255.0;  // prelit
    }
    float va = obj.ambient.w;
    if ((obj.mode.x & M_SPEC) && i.n.w != 0) {
        // VU1 0x948: a = max(0, 1152 (n . h) - 1056), h = q17 the model-space half vector
        float3 h = float3(obj.light.w, obj.diffuse.w, asfloat(obj.mode.w));
        va = max(0.0, 1152.0 * dot(float3(i.n.xyz) / 128.0, h) - 1056.0);
    }
    o.c = floor(float4(rgb, va));  // ftoi0
    o.f = (float)((obj.mode.z >> 8) & 0xFF);
    o.uv = i.uv;
    o.w = o.pos.w;
    return o;
}

struct POut {
    [[vk::location(0)]] float4 color : SV_Target0;
    [[vk::location(0), vk::index(1)]] float4 factor : SV_Target1;  // dual source: a = blend factor C (GS units / 128)
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

// GS CLAMP addressing of one texel coordinate (runtime/gs/gs_draw.cpp wrap). The EE's CLAMP_1
// (0x3620A8) has MINU..MAXV = 0, so the region modes address texel 0.
int wrap_coord(int c, int size, uint mode) {
    if (mode == 0) return c & (size - 1);           // REPEAT
    if (mode == 1) return clamp(c, 0, size - 1);    // CLAMP
    return 0;                                       // REGION_CLAMP / REGION_REPEAT with bounds 0
}

uint4 texel(int x, int y, uint lvl) { return (uint4)round(tex.Load(int3(x, y, lvl)) * 255.0); }

// The GS texture unit (runtime/gs/gs_draw.cpp tri_spans / sample): u, v in 1/16 texel, level
// LOD = log2(1 / |Q|) << L + K with the exponent-and-mantissa log2, rounded, at most MXL; MMIN for
// level > 0, MMAG for level 0; bilinear with 4-bit weights, truncated.
float4 gs_sample(float2 uv, float w) {
    const uint m = obj.mode.x;
    const int tw = 1 << ((m >> M_TW_SHIFT) & 15), th = 1 << ((m >> M_TH_SHIFT) & 15);
    int u = (int)floor(uv.x * (float)(tw * 16)), v = (int)floor(uv.y * (float)(th * 16));
    const uint mxl = (m >> M_MXL_SHIFT) & 7, mmin = (m >> M_MMIN_SHIFT) & 7;
    int lvl = 0;
    if (mmin >= 2 && mxl > 0) {
        const float K = (float)((int)(obj.mode.z << 4) >> 20) / 16.0;  // bits 16..27, signed
        float lod;
        if (obj.mode.z & 0x40000000u) {
            lod = K;
        } else {
            float q = 1.0 / w;
            if (q == 0.0) q = 1e-30;
            const float l2 = (float)((int)(asuint(q) & 0x7FFFFFFFu) - (127 << 23)) * (1.0 / 8388608.0);
            lod = -l2 * (float)(1u << ((obj.mode.z >> 28) & 3)) + K;
        }
        lvl = lod <= 0.0 ? 0 : (int)(lod + 0.5);
        lvl = min(lvl, (int)mxl);
    }
    const bool bil = lvl > 0 ? (mmin == 1 || mmin == 4 || mmin == 5) : (m & M_MMAG) != 0;
    const int lw = max(1, tw >> lvl), lh = max(1, th >> lvl);
    const uint wms = (m >> M_WMS_SHIFT) & 3, wmt = (m >> M_WMT_SHIFT) & 3;
    u >>= lvl;
    v >>= lvl;
    if (!bil) return (float4)texel(wrap_coord(u >> 4, lw, wms), wrap_coord(v >> 4, lh, wmt), lvl);
    u -= 8;
    v -= 8;
    const int x0 = u >> 4, y0 = v >> 4;
    const uint fu = (uint)(u & 15), fv = (uint)(v & 15), gu = 16 - fu, gv = 16 - fv;
    const int xa = wrap_coord(x0, lw, wms), xb = wrap_coord(x0 + 1, lw, wms);
    const int ya = wrap_coord(y0, lh, wmt), yb = wrap_coord(y0 + 1, lh, wmt);
    const uint4 c00 = texel(xa, ya, lvl), c10 = texel(xb, ya, lvl), c01 = texel(xa, yb, lvl), c11 = texel(xb, yb, lvl);
    return (float4)(((c00 * gu + c10 * fu) * gv + (c01 * gu + c11 * fu) * fv) >> 8);
}

POut ps_main(VOut i) {
    const uint m = obj.mode.x;
    float3 v = i.c.rgb;
    float va = i.c.a;
    float3 rgb;
    float a;
    if (m & M_TME) {
        float4 t = (obj.mode.z & 0x80000000u) ? round(tex.Sample(smp, i.uv) * 255.0) : gs_sample(i.uv, i.w);
        const uint tfx = (m >> M_TFX_SHIFT) & 3;
        const bool tcc = (m & M_TCC) != 0;
        if (tfx == 0) {         // modulate
            rgb = floor(t.rgb * v / 128.0);
            a = tcc ? floor(t.a * va / 128.0) : va;
        } else if (tfx == 1) {  // decal
            rgb = t.rgb;
            a = tcc ? t.a : va;
        } else {                // highlight, highlight2
            rgb = floor(t.rgb * v / 128.0) + va;
            a = tcc ? (tfx == 2 ? t.a + va : t.a) : va;
        }
    } else {
        rgb = v;
        a = va;
    }
    rgb = min(rgb, 255.0);
    a = min(a, 255.0);
    if (m & M_FGE) {
        // GS fogging after the texture function: C = FOGCOL + (((C - FOGCOL) * F + 128) >> 8)
        // (runtime/gs/gs_pixel_math.h fog_channel, checked against paraLLEl-GS); alpha is unchanged.
        const float3 fog = float3(obj.mode.y >> 8 & 0xFF, obj.mode.y >> 16 & 0xFF, obj.mode.y >> 24 & 0xFF);
        const float F = clamp(floor(i.f), 0.0, 255.0);
        rgb = fog + floor(((rgb - fog) * F + 128.0) / 256.0);
    }
    if (m & M_ATE) {
        const bool pass = alpha_pass(a, (m >> M_ATST_SHIFT) & 7, (float)(obj.mode.y & 0xFF));
        if (m & M_FAILED) {
            if (pass) discard;  // AFAIL second pass: only the failing pixels
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
