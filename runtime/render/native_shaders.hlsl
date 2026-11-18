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
    uint4 mode;             // x flags (below), y alpha reference, z FIX, w: half vector z (float bits)
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
    o.uv = i.uv;
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

POut ps_main(VOut i) {
    const uint m = obj.mode.x;
    float3 v = i.c.rgb;
    float va = i.c.a;
    float3 rgb;
    float a;
    if (m & M_TME) {
        float4 t = round(tex.Sample(smp, i.uv) * 255.0);
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
    if (m & M_ATE) {
        if (!alpha_pass(a, (m >> M_ATST_SHIFT) & 7, (float)obj.mode.y) && (m & M_AFAIL_DISCARD)) discard;
    }
    POut o;
    o.color = float4(rgb / 255.0, a / 255.0);
    const float c = (m & M_CFIX) ? (float)obj.mode.z : a;
    o.factor = float4(0, 0, 0, saturate(c / 128.0));
    return o;
}
