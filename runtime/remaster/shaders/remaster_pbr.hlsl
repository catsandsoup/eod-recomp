// runtime/remaster/shaders/remaster_pbr.hlsl
// Halo 2 Anniversary / Saber3D-style Forward+ Linear HDR PBR Shaders.
// ZERO PS2 GS register flags, ZERO baked vertex lighting, ZERO fake post-processing.

struct FrameUBO {
    float4   view_proj[4];        // explicit 4 row vectors (100% immune to DXC/glslang matrix-major quirks)
    float4   csm_view_proj[12];   // 3 cascades x 4 row vectors
    float4   csm_splits;          // x=c0_end, y=c1_end, z=c2_end, w=shadow_strength
    float4   cam_pos_debug;       // xyz=world camera pos, w=debug_mode (0..5)
    float4   sun_dir_intensity;   // xyz=unit vector toward sun, w=sun_intensity
    float4   sun_color_day;       // rgb=linear sun color, w=is_day (1 or 0)
    float4   ambient_sky_int;     // rgb=linear sky hemisphere, w=ambient_intensity
    float4   ambient_ground_pad;  // rgb=linear ground bounce, w=0
    float4   fog_color_density;   // rgb=linear fog color, w=fog_density
    float4   fog_start_end_lights;// x=fog_start, y=fog_end, z=num_lights, w=csm_texel_size
    float4   light_pos_rad[32];   // xyz=world pos, w=radius
    float4   light_col_int[32];   // rgb=linear color * intensity, w=spot_cos
    float4   light_dir_pad[32];   // xyz=world spotlight dir, w=0
};

struct DrawPC {
    float4   model[4];            // 64B: explicit 4 row vectors object-to-world (identity for static GRAF)
    float4   base_color;          // 16B: linear RGBA material tint
    float4   pbr_params;          // 16B: x=roughness, y=metallic, z=ao, w=emissive_scale
    uint4    flags;               // 16B: x=alpha_cutout, y=is_sky, z=cascade_idx, w=0
};

float4 mul_rows4(float4 r0, float4 r1, float4 r2, float4 r3, float4 v) {
    return float4(dot(r0, v), dot(r1, v), dot(r2, v), dot(r3, v));
}

float3 mul_rows3(float4 r0, float4 r1, float4 r2, float3 v) {
    return float3(dot(r0.xyz, v), dot(r1.xyz, v), dot(r2.xyz, v));
}

[[vk::push_constant]] DrawPC pc;

// Set 0: Per-frame scene bindings (UBO + 3 CSM shadow maps + reflection cubemap + samplers)
[[vk::binding(0, 0)]] ConstantBuffer<FrameUBO> frame;
[[vk::binding(1, 0)]] Texture2D<float> csm_map0;
[[vk::binding(2, 0)]] Texture2D<float> csm_map1;
[[vk::binding(3, 0)]] Texture2D<float> csm_map2;
[[vk::binding(4, 0)]] TextureCube<float4> env_cube;
[[vk::binding(5, 0)]] SamplerState samp_linear;
[[vk::binding(6, 0)]] SamplerComparisonState samp_shadow;

// Set 1: Per-material PBR textures (albedo, normal, roughness, metallic, ao)
[[vk::binding(0, 1)]] Texture2D<float4> tex_albedo;
[[vk::binding(1, 1)]] Texture2D<float4> tex_normal;
[[vk::binding(2, 1)]] Texture2D<float4> tex_roughness;
[[vk::binding(3, 1)]] Texture2D<float4> tex_metallic;
[[vk::binding(4, 1)]] Texture2D<float4> tex_ao;

struct VSIn {
    float3 pos     : POSITION;
    float3 normal  : NORMAL;
    float4 tangent : TANGENT;
    float2 uv      : TEXCOORD0;
};

struct VSOut {
    float4 clip_pos   : SV_Position;
    float3 world_pos  : TEXCOORD0;
    float3 world_n    : TEXCOORD1;
    float4 world_t    : TEXCOORD2;
    float2 uv         : TEXCOORD3;
};

// ============================================================================
// PASS 1: 3-Cascade CSM Shadow Maps
// ============================================================================
struct CsmVSOut {
    float4 clip_pos : SV_Position;
    float2 uv       : TEXCOORD0;
};

CsmVSOut vs_csm_depth(VSIn vin) {
    CsmVSOut o;
    float4 wp = mul_rows4(pc.model[0], pc.model[1], pc.model[2], pc.model[3], float4(vin.pos, 1.0));
    uint ci = min(pc.flags.z, 2u) * 4u;
    o.clip_pos = mul_rows4(frame.csm_view_proj[ci + 0u], frame.csm_view_proj[ci + 1u],
                           frame.csm_view_proj[ci + 2u], frame.csm_view_proj[ci + 3u], wp);
    o.uv = vin.uv;
    return o;
}

void ps_csm_alpha(CsmVSOut pin) {
    if (pc.flags.x == 1u) {
        float a = tex_albedo.Sample(samp_linear, pin.uv).a * pc.base_color.a;
        if (a < 0.45) discard;
    }
}

// ============================================================================
// Common Geometry Vertex Shader (Pass 2 Cubemap + Pass 3 Forward HDR PBR)
// ============================================================================
VSOut vs_pbr_main(VSIn vin) {
    VSOut o;
    float4 wp = mul_rows4(pc.model[0], pc.model[1], pc.model[2], pc.model[3], float4(vin.pos, 1.0));
    o.world_pos = wp.xyz;
    o.clip_pos = mul_rows4(frame.view_proj[0], frame.view_proj[1], frame.view_proj[2], frame.view_proj[3], wp);
    if (pc.flags.y != 0u) {
        // Sky dome: pin to far plane (z/w = 1.0) so it never clips or occludes track geometry
        o.clip_pos.z = o.clip_pos.w;
    }
    o.world_n = normalize(mul_rows3(pc.model[0], pc.model[1], pc.model[2], vin.normal));
    o.world_t = float4(normalize(mul_rows3(pc.model[0], pc.model[1], pc.model[2], vin.tangent.xyz)), vin.tangent.w);
    o.uv = vin.uv;
    return o;
}

// ============================================================================
// PBR Math Helpers (Cook-Torrance GGX + Schlick Fresnel + 3-Cascade CSM PCF)
// ============================================================================
static const float PI = 3.14159265359;

float3 srgb_to_linear3(float3 c) {
    c = saturate(c);
    float3 lo = c / 12.92;
    float3 hi = pow((c + 0.055) / 1.055, 2.4);
    return float3(c.r <= 0.04045 ? lo.r : hi.r,
                  c.g <= 0.04045 ? lo.g : hi.g,
                  c.b <= 0.04045 ? lo.b : hi.b);
}

float distribution_ggx(float ndh, float rough) {
    float a = rough * rough;
    float a2 = a * a;
    float d = ndh * ndh * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-5);
}

float geometry_schlick(float ndx, float rough) {
    float r = rough + 1.0;
    float k = (r * r) * 0.125;
    return ndx / max(ndx * (1.0 - k) + k, 1e-5);
}

float geometry_smith(float ndv, float ndl, float rough) {
    return geometry_schlick(ndv, rough) * geometry_schlick(ndl, rough);
}

float3 fresnel_schlick(float cos_theta, float3 f0) {
    return f0 + (1.0 - f0) * pow(clamp(1.0 - cos_theta, 0.0, 1.0), 5.0);
}

float3 fresnel_schlick_roughness(float cos_theta, float3 f0, float rough) {
    return f0 + (max(float3(1.0 - rough, 1.0 - rough, 1.0 - rough), f0) - f0) *
                pow(clamp(1.0 - cos_theta, 0.0, 1.0), 5.0);
}

float3 eval_direct_brdf(float3 N, float3 V, float3 L, float3 albedo, float rough, float metallic, float3 radiance) {
    float ndl = max(dot(N, L), 0.0);
    if (ndl <= 0.0) return float3(0.0, 0.0, 0.0);
    float ndv = max(dot(N, V), 1e-4);
    float3 H = normalize(V + L);
    float ndh = max(dot(N, H), 0.0);
    float hdv = max(dot(H, V), 0.0);

    float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float D = distribution_ggx(ndh, rough);
    float G = geometry_smith(ndv, ndl, rough);
    float3 F = fresnel_schlick(hdv, f0);

    float3 spec = (D * G * F) / max(4.0 * ndv * ndl, 1e-4);
    float3 kd = (1.0 - F) * (1.0 - metallic);
    return (kd * albedo / PI + spec) * radiance * ndl;
}

float sample_csm_pcf(uint ci, float3 proj_uv, float bias) {
    float texel = frame.fog_start_end_lights.w;
    float sum = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y) {
        [unroll]
        for (int x = -1; x <= 1; ++x) {
            float2 uv = proj_uv.xy + float2(x, y) * texel;
            float d = proj_uv.z - bias;
            if (ci == 0u)      sum += csm_map0.SampleCmpLevelZero(samp_shadow, uv, d);
            else if (ci == 1u) sum += csm_map1.SampleCmpLevelZero(samp_shadow, uv, d);
            else               sum += csm_map2.SampleCmpLevelZero(samp_shadow, uv, d);
        }
    }
    return sum / 9.0;
}

float compute_csm_visibility(float3 world_pos, float3 N, float3 L, float view_dist) {
    uint ci = 2u;
    if (view_dist < frame.csm_splits.x)      ci = 0u;
    else if (view_dist < frame.csm_splits.y) ci = 1u;
    else if (view_dist > frame.csm_splits.z) return 1.0;

    uint cb = ci * 4u;
    float4 lp = mul_rows4(frame.csm_view_proj[cb + 0u], frame.csm_view_proj[cb + 1u],
                          frame.csm_view_proj[cb + 2u], frame.csm_view_proj[cb + 3u], float4(world_pos, 1.0));
    float3 ndc = lp.xyz / max(lp.w, 1e-5);
    float3 uvz = float3(ndc.x * 0.5 + 0.5, ndc.y * 0.5 + 0.5, ndc.z);
    if (uvz.x < 0.0 || uvz.x > 1.0 || uvz.y < 0.0 || uvz.y > 1.0 || uvz.z < 0.0 || uvz.z > 1.0)
        return 1.0;

    float ndl = max(dot(N, L), 0.0);
    float bias = max(0.0010 * (1.0 - ndl), 0.00028) * (1.0 + float(ci) * 0.55);
    float vis = sample_csm_pcf(ci, uvz, bias);
    return lerp(1.0, vis, frame.csm_splits.w);
}

// ============================================================================
// PASS 2: Dynamic Environment Reflection Cubemap Face Shader (Linear HDR)
// ============================================================================
float4 ps_pbr_cubemap(VSOut pin) : SV_Target0 {
    float4 samp_alb = tex_albedo.Sample(samp_linear, pin.uv);
    float alpha = samp_alb.a * pc.base_color.a;
    if (pc.flags.x == 1u && alpha < 0.45) discard;
    if (pc.flags.x == 2u && alpha < 0.02) discard;
    float3 albedo = srgb_to_linear3(samp_alb.rgb) * pc.base_color.rgb;
    if (pc.flags.y != 0u) {
        return float4(albedo, 1.0);
    }
    float3 N = normalize(pin.world_n);
    float3 L = normalize(frame.sun_dir_intensity.xyz);
    float ndl = max(dot(N, L), 0.0);
    float3 sun_rad = frame.sun_color_day.rgb * frame.sun_dir_intensity.w;
    float up_factor = clamp(N.y * 0.5 + 0.5, 0.0, 1.0); // EOD world: +Y is UP
    float3 hemi = lerp(frame.ambient_ground_pad.rgb, frame.ambient_sky_int.rgb, up_factor) * frame.ambient_sky_int.w;
    float3 color = albedo * (hemi + (sun_rad / PI) * ndl) + albedo * pc.pbr_params.w;
    return float4(color, 1.0);
}

// ============================================================================
// PASS 3: Forward+ Linear HDR PBR Geometry Pass + Debug Buffer Views (0..5)
// ============================================================================
float4 ps_pbr_forward(VSOut pin, bool is_front_face : SV_IsFrontFace) : SV_Target0 {
    float4 samp_alb = tex_albedo.Sample(samp_linear, pin.uv);
    float alpha = samp_alb.a * pc.base_color.a;
    if (pc.flags.x == 1u && alpha < 0.45) discard;
    if (pc.flags.x == 2u && alpha < 0.02) discard;
    float out_alpha = (pc.flags.x == 2u) ? alpha : 1.0;

    float3 albedo = srgb_to_linear3(samp_alb.rgb) * pc.base_color.rgb;
    uint debug_mode = uint(frame.cam_pos_debug.w + 0.5);

    if (pc.flags.x == 3u) {
        // Pass 3B Additive (bridge string lights, flares, stadium spotlight beam cones):
        // Unlit linear emissive; black borders (rgb=0) and transparent edges (a=0) add 0!
        if (debug_mode == 1u) return float4(albedo * alpha, 1.0);
        if (debug_mode >= 2u) discard;
        float add_scale = (pc.pbr_params.w > 0.01) ? pc.pbr_params.w : 0.45;
        return float4(albedo * alpha * add_scale, 1.0);
    }

    if (pc.flags.y != 0u) {
        // Sky dome: unlit linear radiance, unaffected by shadows or fog
        if (debug_mode == 1u) return float4(albedo, 1.0);
        if (debug_mode == 2u) return float4(1.0, 0.0, 0.0, 1.0);
        if (debug_mode == 3u) return float4(0.5, 0.5, 1.0, 1.0);
        if (debug_mode == 4u) return float4(0.0, 0.0, 0.0, 1.0);
        return float4(albedo, 1.0);
    }

    float rough = clamp(tex_roughness.Sample(samp_linear, pin.uv).r * pc.pbr_params.x, 0.05, 0.98);
    float metallic = clamp(tex_metallic.Sample(samp_linear, pin.uv).r * pc.pbr_params.y, 0.0, 1.0);
    float ao = clamp(tex_ao.Sample(samp_linear, pin.uv).r * pc.pbr_params.z, 0.0, 1.0);

    // World-space normal with tangent-space normal map perturbation.
    // Geometry winding is already oriented outward (+Y up on ground); only flip double-sided foliage.
    float3 to_cam = frame.cam_pos_debug.xyz - pin.world_pos;
    float view_dist = length(to_cam);
    float3 V = to_cam / max(view_dist, 1e-4);

    float3 Ng = normalize(pin.world_n);
    if (pc.flags.x != 0u && dot(Ng, V) < 0.0) Ng = -Ng;
    float3 Tg = normalize(pin.world_t.xyz - Ng * dot(Ng, pin.world_t.xyz));
    float3 Bg = cross(Ng, Tg) * pin.world_t.w;
    float3 tn = tex_normal.Sample(samp_linear, pin.uv).xyz * 2.0 - 1.0;
    float3 N = normalize(Tg * tn.x + Bg * tn.y + Ng * max(tn.z, 0.001));

    // Debug 1: Raw Albedo (linear, zero lighting)
    if (debug_mode == 1u) return float4(albedo, out_alpha);
    // Debug 2: Roughness (R) & Metallic (G)
    if (debug_mode == 2u) return float4(rough, metallic, 0.0, out_alpha);
    // Debug 3: World-Space Normals
    if (debug_mode == 3u) return float4(N * 0.5 + 0.5, out_alpha);

    // 1. Direct Sun Light + 3-Cascade CSM (shadows attenuate DIRECT light ONLY!)
    // Strictly matte dielectrics (rough >= 0.78, metallic < 0.1) skip GGX specular completely
    // so dirt, grass, and matte track NEVER show wet specular streaks under sun or floodlights.
    bool is_matte_dielectric = (rough >= 0.78 && metallic < 0.1);
    float3 L_sun = normalize(frame.sun_dir_intensity.xyz);
    float3 sun_rad = frame.sun_color_day.rgb * frame.sun_dir_intensity.w;
    float csm_vis = compute_csm_visibility(pin.world_pos, N, L_sun, view_dist);
    float3 direct_sun;
    if (is_matte_dielectric) {
        float ndl_sun = max(dot(N, L_sun), 0.0);
        direct_sun = (albedo / PI) * sun_rad * ndl_sun * csm_vis;
    } else {
        direct_sun = eval_direct_brdf(N, V, L_sun, albedo, rough, metallic, sun_rad) * csm_vis;
    }

    // Debug 4: Direct Sun + CSM Shadow Mask only
    if (debug_mode == 4u) return float4(direct_sun, out_alpha);

    // 2. Real 3D Dynamic Lights (stadium floodlights, fires, sparks, vehicle lights)
    float3 direct_local = float3(0.0, 0.0, 0.0);
    uint n_lights = min(uint(frame.fog_start_end_lights.z + 0.5), 32u);
    for (uint i = 0u; i < n_lights; ++i) {
        float3 to_l = frame.light_pos_rad[i].xyz - pin.world_pos;
        float dist = length(to_l);
        float rad = frame.light_pos_rad[i].w;
        if (dist >= rad || dist < 1e-4) continue;
        float3 L = to_l / dist;
        float falloff = saturate(1.0 - (dist / rad));
        // Smooth stadium floodlight falloff: even arena illumination without blowing out close billboards
        float atten = (falloff * falloff) / (1.0 + 0.0015 * dist * dist);
        float spot_cos = frame.light_col_int[i].w;
        if (spot_cos > -0.99) {
            float cos_a = dot(-L, normalize(frame.light_dir_pad[i].xyz));
            atten *= smoothstep(spot_cos, min(spot_cos + 0.45, 1.0), cos_a);
        }
        if (atten > 1e-4) {
            // Suppress grazing GGX specular streaks on matte stadium dirt/grass (rough >= 0.78)
            // while keeping crisp metal-halide highlights on car paint, glass, and chrome.
            float3 rad_l = frame.light_col_int[i].rgb * atten;
            if (is_matte_dielectric) {
                float ndl_l = max(dot(N, L), 0.0);
                direct_local += (albedo / PI) * rad_l * ndl_l;
            } else {
                direct_local += eval_direct_brdf(N, V, L, albedo, rough, metallic, rad_l);
            }
        }
    }

    // 3. Sky-Ground Hemisphere Ambient + Dynamic Reflection Cubemap (NEVER shadowed by CSM!)
    float up_factor = clamp(N.y * 0.5 + 0.5, 0.0, 1.0); // EOD world: +Y is UP
    float3 hemi = lerp(frame.ambient_ground_pad.rgb, frame.ambient_sky_int.rgb, up_factor) * frame.ambient_sky_int.w;
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float ndv = max(dot(N, V), 0.0);
    float3 F_env = fresnel_schlick_roughness(ndv, f0, rough);
    float3 kd_env = (1.0 - F_env) * (1.0 - metallic);
    float3 ambient_diff = kd_env * albedo * hemi * ao;

    float3 reflection = float3(0.0, 0.0, 0.0);
    if (!is_matte_dielectric) {
        float3 R = reflect(-V, N);
        float3 env_sample = env_cube.SampleLevel(samp_linear, R, 0.0).rgb;
        float matte_mask = smoothstep(0.78, 0.58, rough);
        float spec_energy = pow(saturate(1.0 - rough), 1.45) * (metallic > 0.15 ? 1.0 : matte_mask);
        reflection = env_sample * F_env * spec_energy * ao;
    }

    // Debug 5: Sky/Cubemap Reflections only
    if (debug_mode == 5u) return float4(reflection, out_alpha);

    float3 emissive = albedo * pc.pbr_params.w;
    float3 color = direct_sun + direct_local + ambient_diff + reflection + emissive;

    // 4. Clean linear-space distance fog ONLY
    float fog_start = frame.fog_start_end_lights.x;
    float fog_end = max(fog_start + 1.0, frame.fog_start_end_lights.y);
    float fog_t = saturate((view_dist - fog_start) / (fog_end - fog_start)) * frame.fog_color_density.w;
    color = lerp(color, frame.fog_color_density.rgb, fog_t);

    return float4(max(color, 0.0), out_alpha);
}

// ============================================================================
// PASS 4: Single Neutral Output Resolve (Khronos PBR Neutral -> sRGB)
// ZERO bloom, ZERO AgX, ZERO auto-exposure, ZERO haze, ZERO color grading, ZERO vignette!
// ============================================================================
[[vk::binding(0, 0)]] Texture2D<float4> hdr_input;
[[vk::binding(1, 0)]] SamplerState samp_resolve;

struct QuadOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

QuadOut vs_resolve_quad(uint vid : SV_VertexID) {
    QuadOut o;
    float2 uv = float2((vid << 1) & 2, vid & 2);
    o.uv = uv;
    o.pos = float4(uv.x * 2.0 - 1.0, uv.y * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

// Official Khronos PBR Neutral Tone Mapper (linear HDR -> linear [0..1])
float3 khronos_pbr_neutral(float3 color) {
    const float start_compression = 0.8 - 0.04;
    const float desaturation = 0.15;

    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;

    float peak = max(color.r, max(color.g, color.b));
    if (peak < start_compression) return color;

    const float d = 1.0 - start_compression;
    float new_peak = 1.0 - d * d / (peak + d - start_compression);
    color *= new_peak / peak;

    float g = 1.0 - 1.0 / (desaturation * (peak - new_peak) + 1.0);
    return lerp(color, float3(new_peak, new_peak, new_peak), g);
}

float3 linear_to_srgb3(float3 c) {
    c = saturate(c);
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(c, 1.0 / 2.4) - 0.055;
    return float3(c.r <= 0.0031308 ? lo.r : hi.r,
                  c.g <= 0.0031308 ? lo.g : hi.g,
                  c.b <= 0.0031308 ? lo.b : hi.b);
}

float4 ps_neutral_resolve(QuadOut pin) : SV_Target0 {
    float3 hdr = hdr_input.Sample(samp_resolve, pin.uv).rgb;
    float3 mapped = khronos_pbr_neutral(max(hdr, 0.0));
    return float4(linear_to_srgb3(mapped), 1.0);
}

// ============================================================================
// PASS 4B: Crisp 2D HUD Composite (runs AFTER neutral resolve, never tonemapped)
// ============================================================================
struct HudVSIn {
    float2 pos   : POSITION;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
};

struct HudVSOut {
    float4 pos   : SV_Position;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
};

HudVSOut vs_hud_2d(HudVSIn vin) {
    HudVSOut o;
    o.pos = float4(vin.pos.x * 2.0 - 1.0, vin.pos.y * 2.0 - 1.0, 0.0, 1.0);
    o.uv = vin.uv;
    o.color = vin.color;
    return o;
}

float4 ps_hud_2d(HudVSOut pin) : SV_Target0 {
    float4 tex = hdr_input.Sample(samp_resolve, pin.uv);
    return tex * pin.color;
}
