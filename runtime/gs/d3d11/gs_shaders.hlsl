// gs_shaders.hlsl - Direct3D 11 shaders of the hardware GS renderer.
// Shader model 4.0 (feature level 10_0 and later).  Compiled ahead of time
// to gs_shaders.inc by tools/build_shaders.py (needs Wine).
//
// The Vulkan renderer (runtime/gs/vulkan) compiles the same source to SPIR-V
// with -DVK and -DVK_DRAW / -DVK_PASS (tools/build_vk_shaders.py): the macros
// below then give the cbuffers as push constants and the resources their
// descriptor bindings (t0-t3 -> 0-3, s0/s1 -> 8/9); for Direct3D they are empty.
//
// Colour render targets are R8G8B8A8_UNORM holding the GS 8-bit channels
// (alpha as stored by the GS: 0x80 = 1.0); depth buffers are D32_FLOAT
// holding z / 2^bits of the depth format.

#ifdef VK
#define VK_PUSH [[vk::push_constant]]
#define CB_REG(r)
#define VK_BIND(n) [[vk::binding(n, 0)]]
#define VK_LOC(n) [[vk::location(n)]]
// dual-source output: location 0 index 1 (glslang has no vk::index; the build
// script moves this location-1 output there in the SPIR-V)
#define VK_DUAL [[vk::location(1)]]
#define VK_POINT_SIZE , [[vk::builtin("PointSize")]] out float psize : PSIZE
#define VK_SET_POINT_SIZE psize = 1.0;
#else
#define VK_PUSH
#define CB_REG(r) : register(r)
#define VK_BIND(n)
#define VK_LOC(n)
#define VK_DUAL
#define VK_POINT_SIZE
#define VK_SET_POINT_SIZE
#endif

#if !defined(VK) || defined(VK_DRAW)
// ===================================================================== draw
VK_PUSH cbuffer Draw CB_REG(b0) {
    float2 c_vp_scale;   // GS pixel -> clip space: ndc = (p + 0.5) * scale + offset
    float2 c_vp_offset;
    float c_zscale;      // 1 / 2^(depth bits)
    float c_zmax;        // largest depth value of the format
    float c_lod_k;       // TEX1.K
    float c_lod_l;       // 2^TEX1.L
    float2 c_tex_size;   // GS texels covered by the bound texture (normalised coordinates)
    float2 c_stq_size;   // tw, th (STQ coordinates are fractions of them)
    float c_tex_scale;   // texels of the bound texture per GS texel (render scale of surfaces)
    uint c_flags;
    uint c_aref;
    uint c_fix;          // ALPHA.FIX
    uint4 c_fog;         // fog colour (rgb)
    uint4 c_texa;        // ta0, ta1, aem
    uint4 c_blend;       // a, b, c, d of the GS blend equation
    int4 c_wrap;         // wms, wmt, mxl, unused
};

// c_flags
#define F_TME        0x1
#define F_FST        0x2
#define F_TCC        0x4
#define F_FGE        0x8
#define F_ABE        0x10
#define F_FBA        0x20
#define F_MIP        0x40
#define F_LCM        0x80
#define F_MAG_LIN    0x100
#define F_MIN_LIN    0x200
#define F_ATEST_PASS 0x400    // keep the pixels that pass the alpha test
#define F_ATEST_FAIL 0x800    // keep the pixels that fail it
#define F_IIP        0x1000
#define F_TEX_RT24   0x2000   // texture is a render target read as a 24-bit format
#define F_TEX_RT16   0x4000   // texture is a render target read as a 16-bit format
#define F_DST16      0x1000000  // 16-bit frame buffer: 5 bits per channel are stored
#define F_UNIT_ADD   0x2000000  // blend is Cd + Cs (16-bit buffer: Cs counts in steps of 8, rounded down)
#define F_UNIT_SUB   0x4000000  // blend is Cd - Cs (rounded up)
// bits 16-17: tfx, 20-22: atst
#define TFX(f) (((f) >> 16) & 3)
#define ATST(f) (((f) >> 20) & 7)

VK_BIND(0) Texture2D<float4> t_tex : register(t0);
VK_BIND(8) SamplerState s_mag : register(s0);
VK_BIND(9) SamplerState s_min : register(s1);

struct VSIn {
    VK_LOC(0) float2 pos : POSITION;
    VK_LOC(1) uint z : DEPTH;
    VK_LOC(2) float4 color : COLOR;
    VK_LOC(3) float3 stq : TEXCOORD0;
    VK_LOC(4) float fog : FOG;
};

struct VSOut {
    float4 pos : SV_Position;
    float4 color : COLOR;
    noperspective float3 stq : TEXCOORD0;
    float fog : FOG;
};

VSOut vs_draw(VSIn v VK_POINT_SIZE) {
    VSOut o;
    VK_SET_POINT_SIZE
    float z = min((float)v.z, c_zmax) * c_zscale;
    o.pos = float4((v.pos + 0.5) * c_vp_scale + c_vp_offset, z, 1.0);
    o.color = v.color;
    o.stq = v.stq;
    o.fog = v.fog;
    return o;
}

struct PSOut {
    VK_LOC(0) float4 color : SV_Target0;
    VK_DUAL float4 factors : SV_Target1;  // dual-source blend factors: rgb = destination, a = source
};

int wrap_coord(int c, int size, int mode) {
    if (mode == 0) return ((c % size) + size) % size;  // REPEAT
    return clamp(c, 0, size - 1);                      // CLAMP (region modes approximated)
}

float4 sample_tex(float2 tc, int lvl, bool linear_filter) {
    if (linear_filter) {
        float2 uv = tc / c_tex_size;
        return lvl > 0 ? t_tex.SampleLevel(s_min, uv, lvl) : t_tex.SampleLevel(s_mag, uv, 0);
    }
    // nearest texel: GS texel = floor(u); sampled at the bound texture's resolution
    uint w, h, n;
    t_tex.GetDimensions(lvl, w, h, n);
    float2 p = floor(tc * c_tex_scale / (float)(1 << lvl) + 1.0 / 1024.0);
    int2 ip = int2(wrap_coord((int)p.x, (int)w, c_wrap.x), wrap_coord((int)p.y, (int)h, c_wrap.y));
    return t_tex.Load(int3(ip, lvl));
}

PSOut ps_draw(VSOut i) {
    uint f = c_flags;
    int4 c = (int4)round(i.color * 255.0);
    if (f & F_TME) {
        float2 tc;
        int lvl = 0;
        if (f & F_FST) {
            tc = i.stq.xy;
        } else {
            float q = i.stq.z == 0.0 ? 1e-30 : i.stq.z;
            tc = i.stq.xy / q * c_stq_size;
            if (f & F_MIP) {
                float lod = (f & F_LCM) ? c_lod_k : (-log2(abs(q)) * c_lod_l + c_lod_k);
                lvl = lod <= 0.0 ? 0 : min((int)(lod + 0.5), c_wrap.z);
            }
        }
        bool lin = lvl > 0 ? (f & F_MIN_LIN) != 0 : (f & F_MAG_LIN) != 0;
        int4 t = (int4)round(sample_tex(tc, lvl, lin) * 255.0);
        if (f & F_TEX_RT16) {  // 16-bit texel: 5 bits per channel, alpha from TEXA
            t.rgb &= 0xF8;
            t.a = t.a >= 0x80 ? (int)c_texa.y : ((c_texa.z != 0 && all(t.rgb == 0)) ? 0 : (int)c_texa.x);
        } else if (f & F_TEX_RT24) {
            t.a = (c_texa.z != 0 && all(t.rgb == 0)) ? 0 : (int)c_texa.x;
        }
        uint tfx = TFX(f);
        bool tcc = (f & F_TCC) != 0;
        if (tfx == 0) {         // MODULATE
            c.rgb = min((t.rgb * c.rgb) >> 7, 255);
            if (tcc) c.a = (t.a * c.a) >> 7;
        } else if (tfx == 1) {  // DECAL
            c.rgb = t.rgb;
            if (tcc) c.a = t.a;
        } else {                // HIGHLIGHT, HIGHLIGHT2
            c.rgb = min(((t.rgb * c.rgb) >> 7) + c.a, 255);
            if (tcc) c.a = tfx == 2 ? t.a + c.a : t.a;
        }
        c.a = min(c.a, 255);
    }
    if (f & F_FGE) {
        int fo = (int)round(i.fog * 255.0);
        c.rgb = (fo * c.rgb + (255 - fo) * (int3)c_fog.rgb) >> 8;
    }
    if (f & (F_ATEST_PASS | F_ATEST_FAIL)) {
        uint a = (uint)c.a, r = c_aref;
        bool ok;
        switch (ATST(f)) {
        case 0: ok = false; break;
        case 1: ok = true; break;
        case 2: ok = a < r; break;
        case 3: ok = a <= r; break;
        case 4: ok = a == r; break;
        case 5: ok = a >= r; break;
        case 6: ok = a > r; break;
        default: ok = a != r; break;
        }
        if ((f & F_ATEST_PASS) && !ok) discard;
        if ((f & F_ATEST_FAIL) && ok) discard;
    }
    PSOut o;
    int aout = (f & F_FBA) ? (c.a | 0x80) : c.a;
    if (f & F_DST16) {
        // The GS stores c >> 3 after blending.  Destination values are kept as multiples of 8, so for
        // Cd +/- Cs the stored result is exact when Cs is rounded to a multiple of 8 the same way.
        if (f & F_UNIT_ADD) c.rgb &= 0xF8;
        else if (f & F_UNIT_SUB) c.rgb = min((c.rgb + 7) & 0x1F8, 255);
        else if (!(f & F_ABE)) c.rgb &= 0xF8;
        if (!(f & F_ABE)) aout = aout >= 0x80 ? 0x80 : 0;
    }
    o.color = float4(c.rgb, aout) / 255.0;
    // (A - B) * C / 128 + D with A, B, D in {Cs, Cd, 0} and C in {As, Ad, FIX}:
    // result = Cs * cs + Cd * cd; the blend op takes care of the signs.
    float C = c_blend.z == 0 ? c.a / 128.0 : (float)c_fix / 128.0;
    float cs = ((c_blend.x == 0 ? 1.0 : 0.0) - (c_blend.y == 0 ? 1.0 : 0.0)) * C + (c_blend.w == 0 ? 1.0 : 0.0);
    float cd = ((c_blend.x == 1 ? 1.0 : 0.0) - (c_blend.y == 1 ? 1.0 : 0.0)) * C + (c_blend.w == 1 ? 1.0 : 0.0);
    o.factors = float4(abs(cd), abs(cd), abs(cd), abs(cs));
    return o;
}
#endif  // draw

#if !defined(VK) || defined(VK_PASS)
// =================================================================== passes
// Full-rectangle passes over a destination rectangle (6 vertices, no buffer).
VK_PUSH cbuffer PassConsts CB_REG(b1) {
    float4 p_dst;        // destination rectangle in clip space: x0, y0, x1, y1
    float4 p_src;        // source rectangle in source texels: x0, y0, x1, y1
    float4 p_misc;       // x: depth scale (2^bits) of the source, y: of the destination, z: dst scale, w: src scale
    uint4 p_fmt;         // x: source format, y: destination format, z: flags, w: unused
    uint4 p_page;        // x: page width, y: page height, z: dst buffer width (pages), w: src buffer width (pages)
    uint4 p_texa;        // ta0, ta1, aem
    float4 p_color;      // constant (blend factor of display circuits)
};

// p_fmt.z
#define P_REMAP     0x1   // layouts differ: go through the page remap table
#define P_TEXA      0x2   // produce texture colours (16/24-bit alpha from TEXA)
#define P_POINT     0x4   // copy: point sampling

VK_BIND(0) Texture2D<float4> t_src : register(t0);
VK_BIND(1) Texture2D<float> t_depth : register(t1);
VK_BIND(2) Texture2D<uint> t_raw : register(t2);
VK_BIND(3) Texture2D<uint2> t_remap : register(t3);  // within a page: destination pixel -> source pixel
VK_BIND(8) SamplerState s_point : register(s0);
VK_BIND(9) SamplerState s_linear : register(s1);

struct PassOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;  // source texel coordinates
};

PassOut vs_pass(uint id : SV_VertexID) {
    // two triangles: (0,0) (1,0) (0,1) / (1,0) (1,1) (0,1)
    PassOut o;
    float2 t = float2((id == 1 || id == 3 || id == 4) ? 1.0 : 0.0, (id == 2 || id == 4 || id == 5) ? 1.0 : 0.0);
    o.pos = float4(lerp(p_dst.xy, p_dst.zw, t), 0.0, 1.0);
    o.uv = lerp(p_src.xy, p_src.zw, t);
    return o;
}

// formats (p_fmt.x / .y): 0 = 32-bit colour, 1 = 24-bit colour, 2 = 16-bit colour, 3 = depth
uint pack_color(float4 c, uint fmt) {
    uint4 u = (uint4)round(saturate(c) * 255.0);
    if (fmt == 2) return (u.r >> 3) | ((u.g >> 3) << 5) | ((u.b >> 3) << 10) | (u.a >= 0x80 ? 0x8000 : 0);
    if (fmt == 1) return u.r | (u.g << 8) | (u.b << 16);
    return u.r | (u.g << 8) | (u.b << 16) | (u.a << 24);
}

float4 unpack_color(uint v, uint fmt, bool texa) {
    uint4 u;
    if (fmt == 2) {
        u = uint4((v & 0x1F) << 3, ((v >> 5) & 0x1F) << 3, ((v >> 10) & 0x1F) << 3, (v & 0x8000) ? 0x80 : 0);
        if (texa) u.a = (v & 0x8000) ? p_texa.y : ((p_texa.z != 0 && (v & 0x7FFF) == 0) ? 0 : p_texa.x);
    } else {
        u = uint4(v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, v >> 24);
        if (fmt == 1 && texa) u.a = (p_texa.z != 0 && (v & 0xFFFFFF) == 0) ? 0 : p_texa.x;
    }
    return float4(u) / 255.0;
}

// The raw GS value (in the source's format) at destination GS pixel p.
uint fetch_raw(float2 hp) {
    // hp: destination position in destination texels (scaled); the GS pixel and the sub-pixel part
    float ds = p_misc.z, ss = p_misc.w;
    float2 gp = floor(hp / ds);
    float2 sub = hp - gp * ds;
    int2 g = (int2)gp;
    if (p_fmt.z & P_REMAP) {
        int2 pg = int2(g.x / (int)p_page.x, g.y / (int)p_page.y);
        int2 in_page = g - pg * int2(p_page.xy);
        uint n = (uint)pg.y * p_page.z + (uint)pg.x;
        int2 spg = int2(n % p_page.w, n / p_page.w);
        g = spg * int2(p_page.xy) + (int2)t_remap.Load(int3(in_page, 0));
    }
    float2 sp = ((float2)g + sub / ds) * ss;
    int2 ip = (int2)floor(sp);
    if (p_fmt.x == 3) return (uint)round(t_depth.Load(int3(ip, 0)) * p_misc.x);
    return pack_color(t_src.Load(int3(ip, 0)), p_fmt.x);
}

// any source -> colour (render target or texture)
float4 ps_convert_color(PassOut i) : SV_Target {
    uint v = fetch_raw(i.pos.xy);
    if (p_fmt.y == 3) return float4(0, 0, 0, 0);
    return unpack_color(v, p_fmt.y, (p_fmt.z & P_TEXA) != 0);
}

// any source -> depth
float ps_convert_depth(PassOut i) : SV_Depth {
    uint v = fetch_raw(i.pos.xy);
    if (p_fmt.x == 2) v &= 0xFFFF;
    return (float)v / p_misc.y;
}

// raw GS values uploaded from VRAM (R32_UINT at 1x) -> colour / depth
float4 ps_upload_color(PassOut i) : SV_Target {
    uint v = t_raw.Load(int3((int2)floor(i.uv), 0));
    return unpack_color(v, p_fmt.y, false);
}

float ps_upload_depth(PassOut i) : SV_Depth {
    uint v = t_raw.Load(int3((int2)floor(i.uv), 0));
    return (float)v / p_misc.y;
}

// colour / depth -> raw GS values (R32_UINT render target at 1x)
uint ps_readback_color(PassOut i) : SV_Target {
    return pack_color(t_src.Load(int3((int2)floor(i.uv), 0)), p_fmt.x);
}

uint ps_readback_depth(PassOut i) : SV_Target {
    return (uint)round(t_depth.Load(int3((int2)floor(i.uv), 0)) * p_misc.x);
}

// display: source texture (normalised by p_src in texels / size in p_misc.xy)
float4 ps_copy(PassOut i) : SV_Target {
    float2 uv = i.uv / p_misc.xy;
    float4 c = (p_fmt.z & P_POINT) ? t_src.SampleLevel(s_point, uv, 0) : t_src.SampleLevel(s_linear, uv, 0);
    return float4(c.rgb, p_color.a);
}
#endif  // passes
