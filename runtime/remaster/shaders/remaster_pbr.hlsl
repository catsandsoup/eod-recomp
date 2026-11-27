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
    float4   ambient_ground_pad;  // rgb=linear ground bounce, w=time_sec
    float4   fog_color_density;   // rgb=linear fog color, w=fog_density
    float4   fog_start_end_lights;// x=fog_start, y=fog_end, z=num_lights, w=csm_texel_size
    float4   light_pos_rad[32];   // xyz=world pos, w=radius
    float4   light_col_int[32];   // rgb=linear color * intensity, w=spot_cos
    float4   light_dir_pad[32];   // xyz=world spotlight dir, w=0
    float4   water_wakes[32];     // xy=world xz, z=birth_time_sec, w=strength
    float4   water_wake_meta;     // x=active_wake_count, y=water_y, z=0, w=0
};

struct DrawPC {
    float4   model[4];            // 64B: explicit 4 row vectors object-to-world (identity for static GRAF)
    float4   base_color;          // 16B: linear RGBA material tint
    float4   pbr_params;          // 16B: x=roughness, y=metallic, z=ao, w=emissive_scale
    uint4    flags;               // 16B: x=alpha_mode, y=0:normal 1:sky 2:water 3:water_spray, z=cascade_idx, w=0
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
    if (pc.flags.y == 1u) {
        // Sky dome: pin to Reversed-Z far plane (z/w = 0.0) so it never clips or occludes track geometry
        o.clip_pos.z = 0.0;
    }
    o.world_n = normalize(mul_rows3(pc.model[0], pc.model[1], pc.model[2], vin.normal));
    if (pc.flags.y == 3u) {
        // Water spray particle: pass per-particle metadata directly in world_t (x=alpha, y=seed, z=kind, w=1)
        o.world_t = vin.tangent;
    } else {
        o.world_t = float4(normalize(mul_rows3(pc.model[0], pc.model[1], pc.model[2], vin.tangent.xyz)), vin.tangent.w);
    }
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

// Non-repeating 2D value noise with analytic gradient (returns float3(val, dV/dx, dV/dy))
float water_hash21(float2 p) {
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float3 water_noise_grad(float2 x) {
    float2 i = floor(x);
    float2 f = frac(x);
    float2 u = f * f * (3.0 - 2.0 * f);
    float2 du = 6.0 * f * (1.0 - f);

    float a = water_hash21(i + float2(0.0, 0.0));
    float b = water_hash21(i + float2(1.0, 0.0));
    float c = water_hash21(i + float2(0.0, 1.0));
    float d = water_hash21(i + float2(1.0, 1.0));

    float k1 = b - a;
    float k2 = c - a;
    float k3 = a - b - c + d;

    float val = a + k1 * u.x + k2 * u.y + k3 * u.x * u.y;
    float2 grad = du * float2(k1 + k3 * u.y, k2 + k3 * u.x);
    return float3(val, grad);
}

// 3-octave domain-rotated ultra-calm still-pond micro-ripple gradient (ZERO stream flow, ZERO tiling)
float2 eval_organic_water_grad(float2 wp_xz, float t) {
    float2 p0 = wp_xz * 0.36 + float2(t * 0.028, t * 0.019);
    float2 g0 = water_noise_grad(p0).yz * 0.36;

    // Rotate domain by ~37 deg so octaves never align into a lattice
    float2 r1 = float2(wp_xz.x * 0.80 - wp_xz.y * 0.60, wp_xz.x * 0.60 + wp_xz.y * 0.80);
    float2 p1 = r1 * 0.88 + float2(-t * 0.042, t * 0.033);
    float2 gr1 = water_noise_grad(p1).yz * 0.88;
    float2 g1 = float2(gr1.x * 0.80 + gr1.y * 0.60, -gr1.x * 0.60 + gr1.y * 0.80);

    float2 r2 = float2(wp_xz.x * 0.52 + wp_xz.y * 0.85, -wp_xz.x * 0.85 + wp_xz.y * 0.52);
    float2 p2 = r2 * 2.10 + float2(t * 0.065, -t * 0.052);
    float2 gr2 = water_noise_grad(p2).yz * 2.10;
    float2 g2 = float2(gr2.x * 0.52 - gr2.y * 0.85, gr2.x * 0.85 + gr2.y * 0.52);

    return g0 * 0.0085 + g1 * 0.0045 + g2 * 0.0020;
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
    // Select the finest CSM cascade whose projected light-space bounds actually contain world_pos.
    // Never drop shadows to 1.0 on widescreen (16:9 / 21:9 / 32:9) edges when a near pixel falls
    // outside Cascade 0 or 1 — seamlessly fall through to the next wider cascade!
    uint ci = 3u;
    float3 uvz = float3(0.0, 0.0, 0.0);
    [unroll]
    for (uint c = 0u; c < 3u; ++c) {
        if (ci == 3u) {
            uint cb = c * 4u;
            float4 lp = mul_rows4(frame.csm_view_proj[cb + 0u], frame.csm_view_proj[cb + 1u],
                                  frame.csm_view_proj[cb + 2u], frame.csm_view_proj[cb + 3u], float4(world_pos, 1.0));
            float3 ndc = lp.xyz / max(lp.w, 1e-5);
            float3 cand = float3(ndc.x * 0.5 + 0.5, ndc.y * 0.5 + 0.5, ndc.z);
            if (cand.x >= 0.005 && cand.x <= 0.995 && cand.y >= 0.005 && cand.y <= 0.995 &&
                cand.z >= 0.0 && cand.z <= 1.0) {
                ci = c;
                uvz = cand;
            }
        }
    }
    if (ci >= 3u || view_dist > frame.csm_splits.z) return 1.0;

    float ndl = max(dot(N, L), 0.0);
    float bias = max(0.0010 * (1.0 - ndl), 0.00028) * (1.0 + float(ci) * 0.55);
    float vis = sample_csm_pcf(ci, uvz, bias);

    // Smooth fade only near the very outer edge of Cascade 2 so there is never a hard shadow line.
    if (ci == 2u) {
        float fade = smoothstep(frame.csm_splits.z * 0.88, frame.csm_splits.z, view_dist);
        vis = lerp(vis, 1.0, fade);
    }
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
    if (pc.flags.x == 3u) {
        float add_scale = (pc.pbr_params.w > 0.01) ? pc.pbr_params.w : 0.45;
        return float4(albedo * alpha * add_scale, 1.0);
    }
    if (pc.flags.y == 1u) {
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

    if (pc.flags.y == 1u) {
        // Sky dome: unlit linear radiance, unaffected by shadows or fog
        if (debug_mode == 1u) return float4(albedo, 1.0);
        if (debug_mode == 2u) return float4(1.0, 0.0, 0.0, 1.0);
        if (debug_mode == 3u) return float4(0.5, 0.5, 1.0, 1.0);
        if (debug_mode == 4u) return float4(0.0, 0.0, 0.0, 1.0);
        return float4(albedo, 1.0);
    }

    if (pc.flags.y == 2u) {
        // Pass 3B Physical Water Surface (EE live wave mesh + organic non-repeating micro-ripples
        // + reactive vehicle wakes & expanding concentric ripple rings + Fresnel cubemap reflection)
        float3 to_cam = frame.cam_pos_debug.xyz - pin.world_pos;
        float view_dist = length(to_cam);
        float3 V = to_cam / max(view_dist, 1e-4);

        float3 Ng = normalize(pin.world_n);
        if (Ng.y < 0.0) Ng = -Ng;
        // Amplify subtle EE WaterSimulate heightfield slope so live EE splashes read clearly
        Ng = normalize(float3(Ng.x * 2.4, Ng.y, Ng.z * 2.4));

        float t = frame.ambient_ground_pad.w;
        float2 p = pin.world_pos.xz;
        float ripple_fade = saturate(1.0 - view_dist / 800.0);
        float2 micro_grad = eval_organic_water_grad(p, t) * ripple_fade;

        // Reactive vehicle wakes & expanding dispersive concentric ripple rings (up to 32 stamps)
        float2 wake_grad = float2(0.0, 0.0);
        float wake_foam_raw = 0.0;
        uint n_wakes = min(uint(frame.water_wake_meta.x + 0.5), 32u);
        for (uint wi = 0u; wi < n_wakes; ++wi) {
            float4 wk = frame.water_wakes[wi];
            float age = t - wk.z;
            if (age < 0.0 || age > 4.2 || wk.w <= 0.001) continue;
            float2 d = p - wk.xy;
            float dist = length(d);
            float max_r = 2.2 + age * 4.8 + 4.0;
            if (dist > max_r) continue;
            float2 dir = d / max(dist, 0.05);

            // Expanding primary wave + fine trailing capillary ripples
            float wave_r = 0.85 + age * 4.4;
            float delta = dist - wave_r;
            float env = exp(-delta * delta * 0.45) * exp(-age * 0.85) * wk.w;
            float phase = delta * 4.2;
            float cap_env = exp(-(delta + 0.7) * (delta + 0.7) * 0.65) * exp(-age * 1.15) * wk.w;
            float slope = env * cos(phase) * 0.38 + cap_env * cos((delta + 0.7) * 8.5) * 0.14;
            wake_grad += dir * slope;

            // Turbulent wheel-churn foam near hull and wave crests
            float hull_env = exp(-dist * dist * 0.34) * exp(-age * 2.0) * wk.w;
            float crest_foam = env * saturate(sin(phase) * 0.5 + 0.5) * exp(-age * 1.05);
            wake_foam_raw += (hull_env * 0.65 + crest_foam * 0.42);
        }

        // Break up wake foam with high-frequency frothy bubble noise so it looks like real churned water foam
        float foam_n1 = water_noise_grad(p * 3.4 + float2(t * 0.12, -t * 0.09)).x;
        float foam_n2 = water_noise_grad(p * 8.2 - float2(t * 0.18, t * 0.15)).x;
        float foam_tex = foam_n1 * 0.60 + foam_n2 * 0.40;
        float wake_foam = saturate(smoothstep(0.24, 0.78, wake_foam_raw * (0.55 + foam_tex * 0.85)));

        float2 total_grad = micro_grad + wake_grad;
        float3 N = normalize(Ng + float3(-total_grad.x, 0.0, -total_grad.y));

        float ndv = max(dot(N, V), 0.0);
        float fresnel = 0.02 + 0.98 * pow(clamp(1.0 - ndv, 0.0, 1.0), 5.0);
        float water_alpha = saturate(lerp(0.66, 0.96, fresnel) + wake_foam * 0.28);

        // Blend two rotated UV samples of the retail water texture with natural pond depth tint
        // so the low-res PS2 water texture never shows visible tile repetition
        float2 uv_flow = total_grad * 0.12;
        float3 tex_a = srgb_to_linear3(tex_albedo.Sample(samp_linear, pin.uv + uv_flow).rgb);
        float2 uv_b = float2(pin.uv.x * 0.61 - pin.uv.y * 0.79, pin.uv.x * 0.79 + pin.uv.y * 0.61) * 0.53 - uv_flow;
        float3 tex_b = srgb_to_linear3(tex_albedo.Sample(samp_linear, uv_b).rgb);
        float3 pond_base = float3(0.055, 0.085, 0.092);
        float3 water_albedo = lerp(pond_base, (tex_a * 0.55 + tex_b * 0.45) * 0.55, 0.45) * pc.base_color.rgb;

        if (debug_mode == 1u) return float4(water_albedo, water_alpha);
        if (debug_mode == 2u) return float4(0.05, 0.0, 0.0, water_alpha);
        if (debug_mode == 3u) return float4(N * 0.5 + 0.5, water_alpha);

        float3 L_sun = normalize(frame.sun_dir_intensity.xyz);
        float3 sun_rad = frame.sun_color_day.rgb * frame.sun_dir_intensity.w;
        float csm_vis = compute_csm_visibility(pin.world_pos, N, L_sun, view_dist);
        float ndl = max(dot(N, L_sun), 0.0);

        // Crisp direct sun GGX specular sparkle (roughness = 0.04) modulated by CSM shadows
        float3 H = normalize(V + L_sun);
        float ndh = max(dot(N, H), 0.0);
        float hdv = max(dot(H, V), 0.0);
        float D = distribution_ggx(ndh, 0.04);
        float G = geometry_smith(max(ndv, 1e-4), max(ndl, 1e-4), 0.04);
        float3 F_sun = fresnel_schlick(hdv, float3(0.02, 0.02, 0.02));
        float3 sun_spec = ((D * G * F_sun) / max(4.0 * max(ndv, 1e-4) * max(ndl, 1e-4), 1e-4)) * sun_rad * ndl * csm_vis;
        float3 sun_diff = (water_albedo / PI) * sun_rad * ndl * csm_vis;

        if (debug_mode == 4u) return float4(sun_diff + sun_spec, water_alpha);

        // Live 256x256 environment cubemap reflection
        float3 R = reflect(-V, N);
        R.y = max(R.y, 0.05);
        R = normalize(R);
        float3 env_refl = env_cube.SampleLevel(samp_linear, R, 0.0).rgb;

        if (debug_mode == 5u) return float4(env_refl * fresnel, water_alpha);

        float up_factor = clamp(N.y * 0.5 + 0.5, 0.0, 1.0);
        float3 hemi = lerp(frame.ambient_ground_pad.rgb, frame.ambient_sky_int.rgb, up_factor) * frame.ambient_sky_int.w;
        float3 water_body = water_albedo * ((sun_rad / PI) * ndl * csm_vis + hemi);
        float3 color = lerp(water_body, env_refl, fresnel) + sun_spec;

        // Frothy sunlit whitecap/splash foam on car wakes
        float3 foam_col = float3(0.84, 0.88, 0.90) * ((sun_rad / PI) * ndl * csm_vis + hemi * 1.15);
        color = lerp(color, foam_col, wake_foam * 0.82);

        float fog_start = frame.fog_start_end_lights.x;
        float fog_end = max(fog_start + 1.0, frame.fog_start_end_lights.y);
        float fog_t = saturate((view_dist - fog_start) / (fog_end - fog_start)) * frame.fog_color_density.w;
        color = lerp(color, frame.fog_color_density.rgb, fog_t);

        return float4(max(color, 0.0), water_alpha);
    }

    if (pc.flags.y == 3u) {
        // Pass 3B 3D Wheel Water Spray / Droplet Plumes (sunlit backlit droplets & mist)
        if (debug_mode >= 2u) discard;
        float2 q = pin.uv * 2.0 - 1.0; // [-1, 1] quad space
        float r2 = dot(q, q);
        if (r2 >= 1.0) discard;

        float life_alpha = pin.world_t.x;
        float seed = pin.world_t.y;
        float is_droplet = pin.world_t.z; // 0 = spray mist puff, 1 = crisp droplet cluster

        float mask = 0.0;
        if (is_droplet > 0.5) {
            // Cluster of 4 crisp water droplets inside the billboard
            float2 c0 = float2(sin(seed * 12.7) * 0.35, cos(seed * 9.3) * 0.35);
            float2 c1 = float2(cos(seed * 17.1) * 0.45, sin(seed * 21.4) * 0.42);
            float2 c2 = float2(sin(seed * 29.3) * 0.48, -cos(seed * 14.9) * 0.38);
            float d0 = smoothstep(0.28, 0.08, length(q - c0));
            float d1 = smoothstep(0.22, 0.06, length(q - c1));
            float d2 = smoothstep(0.20, 0.05, length(q - c2));
            float d3 = smoothstep(0.32, 0.10, length(q));
            mask = saturate(max(max(d0, d1), max(d2, d3 * 0.85)));
        } else {
            // Soft wispy water mist / roostertail foam cloud
            float n = water_noise_grad(q * 2.6 + float2(seed * 13.7, seed * 7.1)).x;
            float radial = pow(saturate(1.0 - r2), 1.65);
            mask = saturate(radial * (0.45 + 0.85 * n));
        }

        float spray_alpha = saturate(mask * life_alpha);
        if (spray_alpha < 0.01) discard;

        float3 to_cam = frame.cam_pos_debug.xyz - pin.world_pos;
        float view_dist = length(to_cam);
        float3 V = to_cam / max(view_dist, 1e-4);
        float3 L_sun = normalize(frame.sun_dir_intensity.xyz);
        float3 sun_rad = frame.sun_color_day.rgb * frame.sun_dir_intensity.w;

        // Forward-scattering backlit sunlight glow + soft sky/ground fill
        float backscatter = pow(saturate(dot(-V, L_sun)), 3.0) * 0.55;
        float3 hemi = lerp(frame.ambient_ground_pad.rgb, frame.ambient_sky_int.rgb, 0.72) * frame.ambient_sky_int.w;
        float3 spray_albedo = lerp(float3(0.76, 0.82, 0.85), float3(0.92, 0.96, 1.0), is_droplet);
        float3 color = spray_albedo * ((sun_rad / PI) * (0.65 + backscatter) + hemi * 1.25);

        float fog_start = frame.fog_start_end_lights.x;
        float fog_end = max(fog_start + 1.0, frame.fog_start_end_lights.y);
        float fog_t = saturate((view_dist - fog_start) / (fog_end - fog_start)) * frame.fog_color_density.w;
        color = lerp(color, frame.fog_color_density.rgb, fog_t);

        return float4(max(color, 0.0), spray_alpha);
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
