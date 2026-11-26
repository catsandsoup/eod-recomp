// native_post.hlsl - 2026 UE5-Grade Multi-Pass Linear-HDR Post-Process:
//   1. Horizon GTAO + SSGI Bounce + 8-Step Zero-Gap Contact Shadows + Deep Chassis Contact AO
//      + Deferred Dynamic HDR Lights (Stadium Floodlights, Sparks, Exhaust, Fires) + Volumetric Cones
//   2. Depth-Aware Cross-Bilateral Blur
//   3. True Linear-HDR CSM Shadow Resolve + Exponential Height/Distance Fog w/ Henyey-Greenstein (g=0.72) Mie Scattering
//   3b. YCoCg Variance-Clipped Temporal Anti-Aliasing (TAA)
//   4..6. 5-Mip Karis HDR Bloom (threshold = 1.25)
//   6b. GPU-Resident 1x1 Temporal Log-Luminance Auto-Exposure (Eye Adaptation)
//   7. Final Composite (CAS + Bloom + Night Anamorphic + Auto-Exposure + Extended AgX Filmic Tonemap)

struct PostConstants {
    float4 inv_size;     // xy: 1/dst_w, 1/dst_h; zw: 1/src_w, 1/src_h
    float4 view_rect;    // xy: viewport uv min, zw: viewport uv size
    float4 proj_params;  // x: depth_offset (b), y: depth_scale (k), z: tan_half_fov_x, w: tan_half_fov_y
    float4 sun_screen;   // xy: sun screen uv, z: sun_in_front (>0), w: daylight (0=night, 1=day)
    float4 sun_view_dir; // xyz: sun direction in camera view space, w: cam_world_y
    float4 sky_horizon;  // rgb: linear horizon color, w: bloom_intensity
    float4 sky_zenith;   // rgb: linear zenith color, w: ao_intensity
    float4 grade_params; // x: exposure, y: haze_intensity, z: grade_intensity, w: vignette_intensity
};
[[vk::push_constant]] PostConstants pc;

[[vk::binding(0, 0)]] Texture2D<float4> src_color;
[[vk::binding(1, 0)]] Texture2D<float>  src_depth;
[[vk::binding(2, 0)]] Texture2D<float4> src_aux;   // GTAO+SSGI or Bloom texture
[[vk::binding(3, 0)]] SamplerState      lin_smp;

#define MAX_DYNAMIC_LIGHTS 32
#define MAX_CAR_OCCLUDERS 16
[[vk::binding(4, 0)]] cbuffer DynamicLights {
    uint4  light_meta;                        // x: light count, y: bit0=has_prev, bit1=has_csm_mask, bit2=has_prev_exp, z: car count, w: high_res (>2560)
    float4 cam_right;                         // xyz: camera right in world space
    float4 cam_down;                          // xyz: camera down in world space
    float4 cam_fwd;                           // xyz: camera forward in world space
    float4 prev_reproj[4];                    // current view-space (P,1) -> previous clip space
    float4 light_pos_rad[MAX_DYNAMIC_LIGHTS]; // xyz: view-space pos, w: radius
    float4 light_col_cut[MAX_DYNAMIC_LIGHTS]; // rgb: linear HDR color, w: spot cos_cutoff (<=0: point)
    float4 light_dir_vol[MAX_DYNAMIC_LIGHTS]; // xyz: view-space spot dir, w: volumetric beam strength
    float4 car_center_rad[MAX_CAR_OCCLUDERS]; // xyz: view-space chassis bottom center, w: bounding radius
    float4 car_right_hx[MAX_CAR_OCCLUDERS];   // xyz: view-space right unit axis, w: half width
    float4 car_fwd_hz[MAX_CAR_OCCLUDERS];     // xyz: view-space forward unit axis, w: half length
    float4 car_down_pad[MAX_CAR_OCCLUDERS];   // xyz: view-space down unit axis, w: chassis height
};

[[vk::binding(5, 0)]] Texture2D<float4> src_aux2; // CSM R8 shadow mask (ps_hdr_atmo) OR 1x1 Auto-Exposure (ps_auto_exposure / ps_remaster_composite)

static const float PI = 3.14159265359;

struct VOut {
    float4 pos : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
};

VOut vs_post_quad(uint vertex : SV_VertexID) {
    VOut o;
    const float2 p = float2((vertex == 1) ? 3.0 : -1.0, (vertex == 2) ? 3.0 : -1.0);
    o.pos = float4(p, 0.0, 1.0);
    o.uv = p * 0.5 + 0.5;
    return o;
}

float ign(float2 p) {
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

float luma(float3 c) {
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// Paired 1:1 with native_modern.hlsl hdr_encode (extended headroom up to ~38.0 linear HDR)
float3 hdr_decode(float3 s) {
    const float3 c = saturate(s);
    const float3 y = c * c;
    return 0.85 * y / max(1.0 - y, 0.022);
}

float linear_depth(float raw) {
    if (raw <= 1e-6) return 1e5;
    return pc.proj_params.y / max(raw - pc.proj_params.x, 1e-6);
}

float3 view_pos_from_uv(float2 uv, float z) {
    const float2 local_uv = (uv - pc.view_rect.xy) / max(pc.view_rect.zw, 1e-5);
    const float2 ndc = local_uv * 2.0 - 1.0;
    return float3(ndc.x * pc.proj_params.z * z, ndc.y * pc.proj_params.w * z, z);
}

bool inside_view(float2 uv) {
    const float2 local_uv = (uv - pc.view_rect.xy) / max(pc.view_rect.zw, 1e-5);
    return all(local_uv >= 0.001) && all(local_uv <= 0.999);
}

float3 reconstruct_view_normal(float2 uv, float z, float3 P, float2 inv_res) {
    const float2 du = float2(inv_res.x, 0.0);
    const float2 dv = float2(0.0, inv_res.y);
    const float zl = linear_depth(src_depth.SampleLevel(lin_smp, uv - du, 0));
    const float zr = linear_depth(src_depth.SampleLevel(lin_smp, uv + du, 0));
    const float zu = linear_depth(src_depth.SampleLevel(lin_smp, uv - dv, 0));
    const float zd = linear_depth(src_depth.SampleLevel(lin_smp, uv + dv, 0));

    const float3 Pl = view_pos_from_uv(uv - du, zl);
    const float3 Pr = view_pos_from_uv(uv + du, zr);
    const float3 Pu = view_pos_from_uv(uv - dv, zu);
    const float3 Pd = view_pos_from_uv(uv + dv, zd);

    const float3 dPdx = (abs(zr - z) < abs(zl - z)) ? (Pr - P) : (P - Pl);
    const float3 dPdy = (abs(zd - z) < abs(zu - z)) ? (Pd - P) : (P - Pu);
    float3 N = normalize(cross(dPdx, dPdy));
    const float3 V = normalize(-P);
    if (dot(N, V) < 0.0) N = -N;
    return N;
}

// Henyey-Greenstein volumetric phase function (UE5 atmospheric & dust Mie scattering)
float hg_phase(float cos_theta, float g) {
    const float g2 = g * g;
    const float denom = max(1.0 + g2 - 2.0 * g * cos_theta, 1e-3);
    return (1.0 - g2) / (4.0 * PI * pow(denom, 1.5));
}

// ---------------------------------------------------------------------------
// Pass 1: Capped 960x540 Horizon GTAO + Colored SSGI Bounce
//         + 8-Step Zero-Gap Screen-Space Contact Shadows + Deep Chassis Contact AO
//         + Deferred HDR Local Lights + Analytic Volumetric Stadium Cones + Sun Shafts
// ---------------------------------------------------------------------------
float4 ps_gtao_contact(VOut i) : SV_Target0 {
    const float2 uv = i.uv;
    if (!inside_view(uv)) return float4(0.0, 0.0, 0.0, 1.0);

    const float raw_z = src_depth.SampleLevel(lin_smp, uv, 0);
    const float z = linear_depth(raw_z);
    const bool is_sky = (raw_z <= 1e-5) || (z > 350.0);
    const float3 P = is_sky ? view_pos_from_uv(uv, 400.0) : view_pos_from_uv(uv, z);
    const float3 V_dir = normalize(P);
    const float dist = length(P);

    float3 add_light = 0.0;
    float occ_out = 1.0;

    if (!is_sky) {
        const float3 N = reconstruct_view_normal(uv, z, P, pc.inv_size.zw);

        // Edge-safe Horizon GTAO + Colored SSGI Indirect Bounce
        const float world_radius = clamp(0.32 + z * 0.012, 0.32, 1.25);
        const float screen_radius = clamp(world_radius / max(z * pc.proj_params.z, 0.1) * 0.5 * pc.view_rect.z,
                                          pc.inv_size.z * 2.0, 0.036);
        const float noise = ign(i.pos.xy);
        const float base_angle = noise * PI * 2.0;

        float ao_sum = 0.0;
        float3 gi_sum = 0.0;
        const int NUM_DIRS = 6;
        const int NUM_STEPS = 3;
        [unroll]
        for (int d = 0; d < NUM_DIRS; ++d) {
            const float angle = base_angle + (float(d) * (PI * 2.0 / float(NUM_DIRS)));
            const float2 dir = float2(cos(angle), sin(angle));
            float max_sin = 0.06;
            float3 best_bounce = 0.0;
            [unroll]
            for (int s = 1; s <= NUM_STEPS; ++s) {
                const float frac_step = (float(s) - 0.35 + 0.35 * frac(noise * 7.31 + float(d) * 0.618)) / float(NUM_STEPS);
                const float2 suv = uv + dir * (screen_radius * frac_step);
                if (!inside_view(suv)) break;
                const float sz = linear_depth(src_depth.SampleLevel(lin_smp, suv, 0));
                const float3 S = view_pos_from_uv(suv, sz);
                const float3 diff = S - P;
                const float d_s = sqrt(max(dot(diff, diff), 1e-6));
                const float horizon_sin = dot(diff, N) / d_s;
                const float falloff = saturate(1.0 - (d_s / (world_radius * 1.25)));
                const float thickness_fade = 1.0 - smoothstep(world_radius * 0.55, world_radius * 1.15, d_s);
                const float contrib = horizon_sin * falloff * thickness_fade;
                if (contrib > max_sin) {
                    max_sin = contrib;
                    const float3 s_rad = hdr_decode(src_color.SampleLevel(lin_smp, suv, 0).rgb);
                    best_bounce = min(s_rad, 4.0) * contrib;
                }
            }
            ao_sum += saturate(1.0 - max_sin * 0.94);
            gi_sum += best_bounce;
        }

        const float ao_dist_fade = 1.0 - smoothstep(55.0, 180.0, z);
        const float ao_str = saturate(pc.sky_zenith.w);
        float ao = ao_sum / float(NUM_DIRS);
        // Clamp GTAO alone >= 0.54 so foreground hay bales & barriers never get dark silhouette halos,
        // while allowing tight Contact Shadows & Under-Chassis AO below to go deep pitch-dark!
        ao = lerp(1.0, max(pow(saturate(ao), 1.15), 0.54), ao_dist_fade * ao_str);
        add_light += (gi_sum / float(NUM_DIRS)) * (0.36 * ao_dist_fade * ao_str);

        // 8-Step Zero-Gap Screen-Space Contact Shadows (day & night: anchors tires, bumpers, posts & hay bales!)
        float contact = 1.0;
        if (z < 60.0) {
            const float3 L = normalize(pc.sun_view_dir.xyz);
            if (dot(N, L) > 0.02) {
                const float ray_len = clamp(0.28 + z * 0.010, 0.28, 0.95);
                const float step_len = ray_len / 8.0;
                const float jitter = frac(noise * 13.7 + 0.25);
                float occ = 0.0;
                [unroll]
                for (int k = 1; k <= 8; ++k) {
                    const float3 Q = P + L * (step_len * (float(k) - 0.85 + jitter));
                    if (Q.z <= 0.05) break;
                    const float2 q_ndc = float2(Q.x / (Q.z * pc.proj_params.z), Q.y / (Q.z * pc.proj_params.w));
                    const float2 q_uv = pc.view_rect.xy + (q_ndc * 0.5 + 0.5) * pc.view_rect.zw;
                    if (!inside_view(q_uv)) break;
                    const float q_scene_z = linear_depth(src_depth.SampleLevel(lin_smp, q_uv, 0));
                    const float delta = Q.z - q_scene_z;
                    const float thickness = 0.022 + 0.008 * Q.z;
                    if (delta > 0.008 && delta < thickness) {
                        const float fade = 1.0 - float(k) / 9.0;
                        occ = max(occ, fade * smoothstep(thickness, 0.008, delta));
                    }
                }
                const float cs_fade = (1.0 - smoothstep(28.0, 60.0, z)) * lerp(0.55, 1.0, pc.sun_screen.w);
                contact = 1.0 - occ * 0.58 * cs_fade;
            }
        }

        // Deep Projected Under-Chassis Contact AO (dark shadow pool directly beneath every vehicle!)
        float chassis_ao = 1.0;
        const uint ncars = min(light_meta.z, (uint)MAX_CAR_OCCLUDERS);
        if (ncars > 0 && z < 115.0) {
            [loop]
            for (uint ci = 0; ci < ncars; ++ci) {
                const float3 cc = car_center_rad[ci].xyz;
                const float crad = car_center_rad[ci].w;
                const float3 dp = P - cc;
                const float r_lim = crad + 1.65;
                if (dot(dp, dp) > r_lim * r_lim) continue;

                const float3 c_down = car_down_pad[ci].xyz;
                const float ground_facing = smoothstep(0.20, 0.60, dot(N, -c_down));
                if (ground_facing <= 0.01) continue;

                const float dy = dot(dp, c_down);
                if (dy < -0.24 || dy > 1.90) continue;

                const float dx = dot(dp, car_right_hx[ci].xyz);
                const float dz = dot(dp, car_fwd_hz[ci].xyz);
                const float hx = car_right_hx[ci].w;
                const float hz = car_fwd_hz[ci].w;

                const float2 q = abs(float2(dx, dz)) - float2(hx * 0.88, hz * 0.90);
                const float edge_d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0);
                const float penumbra = 0.36 + max(dy, 0.0) * 0.42;
                const float footprint = 1.0 - smoothstep(-0.14, penumbra, edge_d);
                const float h_fade = (1.0 - smoothstep(0.16, 1.80, max(dy, 0.0))) * smoothstep(-0.24, -0.01, dy);
                const float shadow_amt = footprint * h_fade * ground_facing * 0.68 * ao_str;
                chassis_ao = min(chassis_ao, 1.0 - shadow_amt);
            }
        }
        occ_out = saturate(ao * contact * chassis_ao);
    }

    // Deferred HDR Dynamic Lights (Stadium Floodlights, Sparks, Exhaust, Fires) + Analytic Volumetric Cones
    // Note: Volumetric cones also scatter across sky/air pixels (is_sky == true) so overhead floodlight beams shine!
    const uint nlights = min(light_meta.x, (uint)MAX_DYNAMIC_LIGHTS);
    if (nlights > 0) {
        const float3 V = -V_dir;
        const float3 N_surf = is_sky ? float3(0, 0, -1) : reconstruct_view_normal(uv, z, P, pc.inv_size.zw);
        const float3 base_hdr = is_sky ? 0.0.xxx : hdr_decode(src_color.SampleLevel(lin_smp, uv, 0).rgb);
        const float3 surf_albedo = clamp(base_hdr * 0.55 + 0.08, 0.08, 0.95);
        const float haze_scale = saturate(pc.grade_params.y);
        [loop]
        for (uint li = 0; li < nlights; ++li) {
            const float3 lpos = light_pos_rad[li].xyz;
            const float lrad  = light_pos_rad[li].w;
            const float3 lcol = light_col_cut[li].rgb;
            const float lcut  = light_col_cut[li].w;
            const float3 ldir = light_dir_vol[li].xyz;
            const float lvol  = light_dir_vol[li].w;

            if (!is_sky) {
                const float3 to_l = lpos - P;
                const float d2 = dot(to_l, to_l);
                if (d2 < lrad * lrad) {
                    const float d = sqrt(max(d2, 1e-4));
                    const float3 L = to_l / d;
                    const float atten_edge = saturate(1.0 - d / lrad);
                    float atten = (atten_edge * atten_edge) / (1.0 + d2 * 0.048);
                    if (lcut > 0.0) {
                        const float spot_cos = dot(-L, ldir);
                        atten *= smoothstep(lcut, min(lcut + 0.24, 0.99), spot_cos);
                    }
                    if (atten > 1e-4) {
                        const float NoL = saturate(dot(N_surf, L));
                        const float3 H = normalize(L + V);
                        // Crisp specular glint on wet mud ruts & sheet metal under sparks and floodlights!
                        const float spec = pow(saturate(dot(N_surf, H)), 24.0) * 0.28;
                        add_light += lcol * atten * (surf_albedo * NoL + spec * (0.20 + 0.80 * NoL));
                    }
                }
            }

            // Analytic 3D Volumetric Stadium Floodlight Cones through dusty night air!
            const float cam_light_dist = length(lpos);
            if (lvol > 0.001 && haze_scale > 0.01 && cam_light_dist > 5.0) {
                const float max_t = is_sky ? 140.0 : min(dist, 140.0);
                const float t_closest = clamp(dot(lpos, V_dir), 1.5, max_t);
                const float3 closest_pt = V_dir * t_closest;
                const float3 diff = closest_pt - lpos;
                const float d2_ray = dot(diff, diff);
                const float vol_r = min(lrad * 0.34, 8.5);
                if (d2_ray < vol_r * vol_r) {
                    float beam = exp(-d2_ray / max(vol_r * vol_r * 0.18, 0.25));
                    if (lcut > 0.0) {
                        const float3 from_l = normalize(diff + 1e-5);
                        beam *= smoothstep(lcut * 0.82, min(lcut + 0.22, 0.99), dot(from_l, ldir));
                    }
                    beam *= smoothstep(5.0, 12.0, cam_light_dist) * smoothstep(1.5, 7.5, t_closest);
                    add_light += lcol * beam * lvol * haze_scale * 0.68;
                }
            }
        }
    }

    // Screen-Space Volumetric Sun Shafts (daylight & golden hour)
    const float haze_strength = saturate(pc.grade_params.y);
    const float daylight = pc.sun_screen.w;
    if (haze_strength > 0.01 && daylight > 0.20 && pc.sun_screen.z > 0.0) {
        const float2 sun_uv = pc.sun_screen.xy;
        const float2 delta = (sun_uv - uv);
        const float sun_dist = length(delta);
        if (sun_dist < 1.45) {
            const float2 step_uv = delta * (0.45 / 10.0);
            float2 cur = uv + step_uv * ign(i.pos.xy);
            float shaft = 0.0;
            float decay = 1.0;
            [unroll]
            for (int s = 0; s < 10; ++s) {
                cur += step_uv;
                if (inside_view(cur)) {
                    const float sz = src_depth.SampleLevel(lin_smp, cur, 0);
                    const float sky_hit = (sz <= 1e-5 || linear_depth(sz) > 450.0) ? 1.0 : 0.0;
                    shaft += sky_hit * decay;
                }
                decay *= 0.88;
            }
            const float radial = exp(-sun_dist * 2.2) * daylight * pc.sun_screen.z * haze_strength;
            add_light += float3(1.45, 1.02, 0.62) * (shaft / 10.0) * radial * 0.34;
        }
    }

    return float4(max(add_light, 0.0), occ_out);
}

// ---------------------------------------------------------------------------
// Pass 2: Depth-Aware Cross-Bilateral Blur for Lighting (rgb) + AO/Contact (a)
// ---------------------------------------------------------------------------
float4 ps_ao_blur(VOut i) : SV_Target0 {
    const float2 uv = i.uv;
    const float4 center = src_color.SampleLevel(lin_smp, uv, 0);
    const float cz = linear_depth(src_depth.SampleLevel(lin_smp, uv, 0));
    if (cz > 350.0) return center;

    float4 sum = center * 0.30;
    float wsum = 0.30;
    const float2 texel = pc.inv_size.xy;
    const float depth_sigma = max(0.08, cz * 0.018);

    static const float2 offsets[8] = {
        float2(-1.25, -0.5), float2( 0.5, -1.25), float2( 1.25,  0.5), float2(-0.5,  1.25),
        float2(-0.85,  0.85), float2( 0.85,  0.85), float2( 0.85, -0.85), float2(-0.85, -0.85)
    };
    [unroll]
    for (int k = 0; k < 8; ++k) {
        const float2 suv = uv + offsets[k] * texel;
        if (!inside_view(suv)) continue;
        const float sz = linear_depth(src_depth.SampleLevel(lin_smp, suv, 0));
        const float dz = abs(sz - cz) / depth_sigma;
        const float w = exp(-dz * dz * 2.2);
        sum += src_color.SampleLevel(lin_smp, suv, 0) * w;
        wsum += w;
    }
    return sum / max(wsum, 1e-4);
}

// ---------------------------------------------------------------------------
// Pass 3: True Linear-HDR Decode + Physical CSM Shadow Resolve + GTAO/ChassisAO
//         + Exponential Height/Distance Fog w/ Henyey-Greenstein (g=0.72) Mie Scattering!
// ---------------------------------------------------------------------------
float4 ps_hdr_atmo(VOut i) : SV_Target0 {
    const float2 uv = i.uv;
    const float4 raw_col = src_color.SampleLevel(lin_smp, uv, 0);
    if (!inside_view(uv)) return raw_col;

    float3 hdr = hdr_decode(raw_col.rgb);
    const float raw_z = src_depth.SampleLevel(lin_smp, uv, 0);
    const float z = linear_depth(raw_z);
    const bool is_sky = (raw_z <= 1e-5) || (z > 900.0);
    const float4 gi_ao = src_aux.SampleLevel(lin_smp, uv, 0);
    const float daylight = pc.sun_screen.w;

    if (!is_sky) {
        const float emissive_protect = smoothstep(2.4, 5.5, luma(hdr));
        const float occ = saturate(gi_ao.a);

        // Combine 2048x2048 CSM Directional Shadow & Contact/Chassis AO smoothly in Linear HDR
        // WITHOUT double-multiplying into a pitch-black void under the bumper!
        float3 total_atten = occ.xxx;
        if ((light_meta.y & 2u) != 0) {
            const float csm_vis = src_aux2.SampleLevel(lin_smp, uv, 0).r;
            const float sh = saturate(1.0 - csm_vis);
            const float3 shadow_trans = lerp(float3(0.36, 0.40, 0.48), float3(0.28, 0.34, 0.46), daylight);
            const float3 csm_atten = lerp(1.0.xxx, shadow_trans, sh);
            // Soft union: under-car contact AO deepens the core slightly without crushing to 0.02 black!
            total_atten = min(csm_atten, occ.xxx) * lerp(1.0, 0.82, sh * (1.0 - occ));
        }

        hdr *= lerp(total_atten, 1.0.xxx, emissive_protect);
        hdr += gi_ao.rgb;
    } else {
        hdr += gi_ao.rgb * 0.85;
    }

    const float haze_strength = saturate(pc.grade_params.y);
    const float3 P = view_pos_from_uv(uv, is_sky ? 400.0 : z);
    const float3 V_dir = normalize(P);
    const float3 W_dir = normalize(V_dir.x * cam_right.xyz + V_dir.y * cam_down.xyz + V_dir.z * cam_fwd.xyz);

    if (is_sky && daylight < 0.30) {
        const float elev = saturate(-W_dir.y);
        const float dark_sky = 1.0 - smoothstep(0.02, 0.12, luma(hdr));
        if (elev > 0.08 && dark_sky > 0.05) {
            const float2 sky_p = float2(atan2(W_dir.z, W_dir.x), asin(clamp(-W_dir.y, -1.0, 1.0)));
            const float2 g1 = sky_p * 210.0;
            const float2 id1 = floor(g1);
            const float2 f1 = frac(g1) - 0.5;
            const float h1 = frac(sin(dot(id1, float2(127.1, 311.7))) * 43758.5453);
            const float h2 = frac(sin(dot(id1, float2(269.5, 183.3))) * 43758.5453);
            const float2 jitter = (float2(h1, h2) - 0.5) * 0.65;
            const float sd = length(f1 - jitter);
            const float star = smoothstep(0.095, 0.012, sd) * step(0.986, h1);
            const float3 star_col = lerp(float3(0.75, 0.88, 1.0), float3(1.0, 0.92, 0.76), h2);
            const float vis = smoothstep(0.08, 0.25, elev) * dark_sky * (1.0 - smoothstep(0.05, 0.30, daylight));
            hdr += star_col * (star * vis * 2.2);
        }
    } else if (!is_sky && haze_strength > 0.01) {
        // Clean, crisp Rayleigh blue-sky aerial perspective (ZERO yellow/brown piss filter on the horizon!)
        const float dist = length(P);
        const float rel_height = max(0.0, -W_dir.y * dist);
        const float height_atten = exp(-rel_height * 0.045);
        const float base_density = lerp(0.0014, 0.00065, daylight) * haze_strength * (0.45 + 0.55 * height_atten);
        const float fog_factor = saturate((1.0 - exp(-max(dist - 45.0, 0.0) * base_density)) * 0.42);

        const float3 L_dir = normalize(pc.sun_view_dir.xyz);
        const float cos_theta = saturate(dot(V_dir, L_dir));
        const float sun_scatter = pow(cos_theta, 14.0) * 0.22;

        // Match the track's own crisp horizon sky color + cool Rayleigh blue (never yellow!)
        const float3 day_haze   = lerp(pc.sky_horizon.rgb, float3(0.42, 0.58, 0.84), 0.25) + float3(0.95, 0.90, 0.82) * sun_scatter;
        const float3 night_haze = pc.sky_horizon.rgb * 0.95 + float3(0.022, 0.028, 0.045);
        hdr = lerp(hdr, lerp(night_haze, day_haze, daylight), fog_factor);
    }

    return float4(max(hdr, 0.0), raw_col.a);
}

// ---------------------------------------------------------------------------
// Pass 3b: Temporal Anti-Aliasing (TAA) with YCoCg Variance Clipping
// ---------------------------------------------------------------------------
float3 rgb_to_ycocg(float3 c) {
    return float3(
         0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
         0.5  * c.r - 0.5 * c.b,
        -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}
float3 ycocg_to_rgb(float3 y) {
    return max(float3(
        y.x + y.y - y.z,
        y.x + y.z,
        y.x - y.y - y.z), 0.0);
}

float4 ps_taa_resolve(VOut i) : SV_Target0 {
    const float2 uv = i.uv;
    const float4 cur = src_color.SampleLevel(lin_smp, uv, 0);
    if (!inside_view(uv) || (light_meta.y & 1u) == 0) return cur;

    const float raw_z = src_depth.SampleLevel(lin_smp, uv, 0);
    if (raw_z <= 1e-5) return cur;
    const float z = linear_depth(raw_z);
    if (z > 900.0) return cur;
    const float3 P = view_pos_from_uv(uv, z);
    const float4 p4 = float4(P, 1.0);
    const float4 prev_c = float4(
        dot(prev_reproj[0], p4),
        dot(prev_reproj[1], p4),
        dot(prev_reproj[2], p4),
        dot(prev_reproj[3], p4));
    if (prev_c.w <= 0.05) return cur;

    const float2 prev_ndc = prev_c.xy / prev_c.w;
    const float2 prev_uv = pc.view_rect.xy + (prev_ndc * 0.5 + 0.5) * pc.view_rect.zw;
    if (!inside_view(prev_uv)) return cur;

    const float2 t = pc.inv_size.xy;
    float3 m1 = 0.0, m2 = 0.0;
    [unroll]
    for (int dy = -1; dy <= 1; ++dy) {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx) {
            const float3 s = src_color.SampleLevel(lin_smp, uv + float2(dx, dy) * t, 0).rgb;
            const float3 sy = rgb_to_ycocg(s / (1.0 + luma(s)));
            m1 += sy;
            m2 += sy * sy;
        }
    }
    const float3 mu = m1 * (1.0 / 9.0);
    const float3 sigma = sqrt(max(m2 * (1.0 / 9.0) - mu * mu, 0.0));
    const float3 box_min = mu - sigma * 1.15;
    const float3 box_max = mu + sigma * 1.15;

    const float3 hist_raw = src_aux.SampleLevel(lin_smp, prev_uv, 0).rgb;
    const float3 hist_y = clamp(rgb_to_ycocg(hist_raw / (1.0 + luma(hist_raw))), box_min, box_max);
    const float3 cur_y  = rgb_to_ycocg(cur.rgb / (1.0 + luma(cur.rgb)));

    const float vel = length(uv - prev_uv);
    const float alpha = lerp(0.18, 0.36, saturate(vel * 45.0));
    const float3 out_tm = ycocg_to_rgb(lerp(hist_y, cur_y, alpha));
    const float3 out_hdr = out_tm / max(1.0 - luma(out_tm), 0.02);
    return float4(max(out_hdr, 0.0), cur.a);
}

// ---------------------------------------------------------------------------
// Pass 4: Half-Res Karis-Weighted HDR Bloom Prefilter (threshold ~ 1.25)
// ---------------------------------------------------------------------------
float3 karis_avg(float3 a, float3 b, float3 c, float3 d) {
    const float wa = 1.0 / (1.0 + luma(a));
    const float wb = 1.0 / (1.0 + luma(b));
    const float wc = 1.0 / (1.0 + luma(c));
    const float wd = 1.0 / (1.0 + luma(d));
    return (a * wa + b * wb + c * wc + d * wd) / (wa + wb + wc + wd);
}

float4 ps_bloom_prefilter(VOut i) : SV_Target0 {
    const float2 uv = i.uv;
    if (!inside_view(uv)) return 0.0;
    const float2 t = pc.inv_size.zw;
    const float3 a = src_color.SampleLevel(lin_smp, uv + float2(-0.5, -0.5) * t, 0).rgb;
    const float3 b = src_color.SampleLevel(lin_smp, uv + float2( 0.5, -0.5) * t, 0).rgb;
    const float3 c = src_color.SampleLevel(lin_smp, uv + float2(-0.5,  0.5) * t, 0).rgb;
    const float3 d = src_color.SampleLevel(lin_smp, uv + float2( 0.5,  0.5) * t, 0).rgb;
    float3 col = karis_avg(a, b, c, d);

    // High threshold in daylight (2.15) so ONLY real specular sun glints, chrome & sparks bloom — NEVER diffuse yellow paint or trees!
    const float threshold = lerp(0.95, 2.15, pc.sun_screen.w);
    const float knee = threshold * 0.35;
    const float br = max(col.r, max(col.g, col.b));
    float soft = clamp(br - threshold + knee, 0.0, 2.0 * knee);
    soft = (soft * soft) / max(4.0 * knee, 1e-4);
    const float contrib = max(soft, br - threshold) / max(br, 1e-4);
    return float4(min(col * contrib, 4.5), 1.0);
}

// ---------------------------------------------------------------------------
// Pass 5: 13-Tap CoD-Next-Gen HDR Bloom Downsample
// ---------------------------------------------------------------------------
float4 ps_bloom_down(VOut i) : SV_Target0 {
    const float2 uv = i.uv;
    const float2 t = pc.inv_size.zw;
    const float3 A = src_color.SampleLevel(lin_smp, uv + float2(-2.0, -2.0) * t, 0).rgb;
    const float3 B = src_color.SampleLevel(lin_smp, uv + float2( 0.0, -2.0) * t, 0).rgb;
    const float3 C = src_color.SampleLevel(lin_smp, uv + float2( 2.0, -2.0) * t, 0).rgb;
    const float3 D = src_color.SampleLevel(lin_smp, uv + float2(-1.0, -1.0) * t, 0).rgb;
    const float3 E = src_color.SampleLevel(lin_smp, uv + float2( 1.0, -1.0) * t, 0).rgb;
    const float3 F = src_color.SampleLevel(lin_smp, uv + float2(-2.0,  0.0) * t, 0).rgb;
    const float3 G = src_color.SampleLevel(lin_smp, uv,                           0).rgb;
    const float3 H = src_color.SampleLevel(lin_smp, uv + float2( 2.0,  0.0) * t, 0).rgb;
    const float3 I = src_color.SampleLevel(lin_smp, uv + float2(-1.0,  1.0) * t, 0).rgb;
    const float3 J = src_color.SampleLevel(lin_smp, uv + float2( 1.0,  1.0) * t, 0).rgb;
    const float3 K = src_color.SampleLevel(lin_smp, uv + float2(-2.0,  2.0) * t, 0).rgb;
    const float3 L = src_color.SampleLevel(lin_smp, uv + float2( 0.0,  2.0) * t, 0).rgb;
    const float3 M = src_color.SampleLevel(lin_smp, uv + float2( 2.0,  2.0) * t, 0).rgb;
    float3 out_col = (D + E + I + J) * 0.125;
    out_col += (A + B + G + F) * 0.03125;
    out_col += (B + C + H + G) * 0.03125;
    out_col += (F + G + L + K) * 0.03125;
    out_col += (G + H + M + L) * 0.03125;
    return float4(max(out_col, 0.0), 1.0);
}

// ---------------------------------------------------------------------------
// Pass 6: 9-Tap Tent-Filter HDR Bloom Upsample (Additive Blend)
// ---------------------------------------------------------------------------
float4 ps_bloom_up(VOut i) : SV_Target0 {
    const float2 uv = i.uv;
    const float2 t = pc.inv_size.zw * 1.15;
    float3 sum = 0.0;
    sum += src_color.SampleLevel(lin_smp, uv + float2(-1.0, -1.0) * t, 0).rgb * 1.0;
    sum += src_color.SampleLevel(lin_smp, uv + float2( 0.0, -1.0) * t, 0).rgb * 2.0;
    sum += src_color.SampleLevel(lin_smp, uv + float2( 1.0, -1.0) * t, 0).rgb * 1.0;
    sum += src_color.SampleLevel(lin_smp, uv + float2(-1.0,  0.0) * t, 0).rgb * 2.0;
    sum += src_color.SampleLevel(lin_smp, uv,                           0).rgb * 4.0;
    sum += src_color.SampleLevel(lin_smp, uv + float2( 1.0,  0.0) * t, 0).rgb * 2.0;
    sum += src_color.SampleLevel(lin_smp, uv + float2(-1.0,  1.0) * t, 0).rgb * 1.0;
    sum += src_color.SampleLevel(lin_smp, uv + float2( 0.0,  1.0) * t, 0).rgb * 2.0;
    sum += src_color.SampleLevel(lin_smp, uv + float2( 1.0,  1.0) * t, 0).rgb * 1.0;
    return float4(sum * (1.0 / 16.0), 1.0);
}

// ---------------------------------------------------------------------------
// Pass 6b: GPU-Resident 1x1 Temporal Log-Luminance Auto-Exposure (UE5 Eye Adaptation)
// Center/track-weighted 16-tap log-average scene luminance -> smooth eye adaptation!
// ---------------------------------------------------------------------------
float4 ps_auto_exposure(VOut i) : SV_Target0 {
    float log_sum = 0.0;
    float w_sum = 0.0;
    [unroll]
    for (int gy = 0; gy < 4; ++gy) {
        [unroll]
        for (int gx = 0; gx < 4; ++gx) {
            const float2 luv = float2((float(gx) + 0.5) * 0.25, (float(gy) + 0.5) * 0.25);
            const float2 suv = pc.view_rect.xy + luv * pc.view_rect.zw;
            const float3 s = src_color.SampleLevel(lin_smp, suv, 0).rgb;
            const float lum = clamp(luma(s), 0.035, 14.0);
            // Weight foreground track & cars (lower/center screen) higher than upper sky!
            const float w = (luv.y > 0.35 ? 1.65 : 0.42) * (abs(luv.x - 0.5) < 0.30 ? 1.35 : 0.85);
            log_sum += log(lum) * w;
            w_sum += w;
        }
    }
    const float avg_lum = exp(log_sum / max(w_sum, 1e-4));
    // Gentle, natural eye-adaptation range (0.85..1.18 in daylight) so bright scenes never wash out or clip!
    const float target_key = lerp(0.16, 0.28, pc.sun_screen.w);
    const float min_exp    = lerp(0.90, 0.82, pc.sun_screen.w);
    const float max_exp    = lerp(1.38, 1.15, pc.sun_screen.w);
    const float target_exp = clamp(target_key / max(avg_lum, 0.04), min_exp, max_exp);

    float adapted = target_exp;
    if ((light_meta.y & 4u) != 0) {
        const float prev_exp = clamp(src_aux2.Load(int3(0, 0, 0)).r, 0.35, 2.5);
        adapted = lerp(prev_exp, target_exp, 0.08);
    }
    return float4(adapted, avg_lum, target_exp, 1.0);
}

// ---------------------------------------------------------------------------
// Extended-Headroom AgX Filmic Tonemapper (smooth highlight desaturation to white!)
// ---------------------------------------------------------------------------
float3 agx_default_contrast_approx(float3 x) {
    const float3 x2 = x * x;
    const float3 x4 = x2 * x2;
    return + 15.5     * x4 * x2
           - 40.14    * x4 * x
           + 31.96    * x4
           - 6.868    * x2 * x
           + 0.4298   * x2
           + 0.1191   * x
           - 0.00232;
}

float3 agx_tonemap(float3 val) {
    static const float3x3 agx_mat = float3x3(
        0.842479062253094,  0.0784335999999992, 0.0792237451477643,
        0.0423282422610123, 0.878468636469772,  0.0791661274605434,
        0.0423756549057051, 0.0784336,          0.879142973793104
    );
    static const float3x3 agx_mat_inv = float3x3(
         1.19687900512017,   -0.0980208811401368, -0.0990297440797205,
        -0.0528968517574562,  1.15190312990417,   -0.0989611768448433,
        -0.0529716355144438, -0.0980434501171241,  1.15107367264116
    );
    const float min_ev = -12.47393;
    const float max_ev = 5.35; // Extended HDR headroom for 10:1 sun glints, chrome & sparks!
    val = mul(agx_mat, max(val, 1e-6));
    val = clamp(log2(val), min_ev, max_ev);
    val = (val - min_ev) / (max_ev - min_ev);
    val = agx_default_contrast_approx(val);
    val = mul(agx_mat_inv, val);
    return saturate(val);
}

float3 agx_punchy_look(float3 val, float grade) {
    const float3 lw = float3(0.2126, 0.7152, 0.0722);
    const float l = dot(val, lw);
    const float3 slope = float3(1.02, 1.00, 0.99);
    const float3 power = lerp(1.0.xxx, float3(1.09, 1.09, 1.11), grade);
    const float sat = lerp(1.0, 1.08, grade);
    val = pow(max(val * slope, 0.0), power);
    return saturate(l + sat * (val - l));
}

// ---------------------------------------------------------------------------
// Pass 7: Final Composite (Motion Blur + CAS + HDR Bloom + Auto-Exposure + AgX)
// ---------------------------------------------------------------------------
float4 ps_remaster_composite(VOut i) : SV_Target0 {
    const float2 uv = i.uv;
    const float4 center_sample = src_color.SampleLevel(lin_smp, uv, 0);
    if (!inside_view(uv)) return center_sample;

    float3 cC = center_sample.rgb;
    if ((light_meta.y & 1u) != 0 && light_meta.w == 0) {
        const float raw_z = src_depth.SampleLevel(lin_smp, uv, 0);
        const float z = linear_depth(raw_z);
        if (raw_z > 1e-5 && z < 140.0) {
            const float3 P = view_pos_from_uv(uv, z);
            const float4 p4 = float4(P, 1.0);
            const float4 prev_c = float4(
                dot(prev_reproj[0], p4),
                dot(prev_reproj[1], p4),
                dot(prev_reproj[2], p4),
                dot(prev_reproj[3], p4));
            if (prev_c.w > 0.05) {
                const float2 prev_ndc = prev_c.xy / prev_c.w;
                const float2 prev_uv = pc.view_rect.xy + (prev_ndc * 0.5 + 0.5) * pc.view_rect.zw;
                const float2 local_uv = (uv - pc.view_rect.xy) / max(pc.view_rect.zw, 1e-5);
                const float2 edge_d = (local_uv - float2(0.5, 0.56)) * float2(1.45, 1.25);
                const float periph_mask = smoothstep(0.48, 1.05, dot(edge_d, edge_d));
                float2 vel = (uv - prev_uv) * (0.10 * periph_mask);
                const float vlen = length(vel);
                if (vlen > pc.inv_size.x * 1.2) {
                    vel *= min(vlen, 0.0045) / vlen;
                    float3 msum = cC;
                    float mw = 1.0;
                    const float jitter = ign(i.pos.xy) - 0.5;
                    [unroll]
                    for (int s = -2; s <= 2; ++s) {
                        if (s == 0) continue;
                        const float2 suv = uv + vel * ((float(s) + jitter * 0.5) * 0.35);
                        if (!inside_view(suv)) continue;
                        const float sz = linear_depth(src_depth.SampleLevel(lin_smp, suv, 0));
                        const float dw = smoothstep(1.5, 0.0, abs(sz - z) / max(z * 0.12, 0.4));
                        msum += src_color.SampleLevel(lin_smp, suv, 0).rgb * dw;
                        mw += dw;
                    }
                    cC = msum / mw;
                }
            }
        }
    }

    // Contrast-Adaptive Sharpening (CAS) in Linear HDR
    const float2 t = pc.inv_size.xy;
    const float3 cN = src_color.SampleLevel(lin_smp, uv + float2( 0.0, -t.y), 0).rgb;
    const float3 cS = src_color.SampleLevel(lin_smp, uv + float2( 0.0,  t.y), 0).rgb;
    const float3 cW = src_color.SampleLevel(lin_smp, uv + float2(-t.x,  0.0), 0).rgb;
    const float3 cE = src_color.SampleLevel(lin_smp, uv + float2( t.x,  0.0), 0).rgb;
    const float3 mn = min(cC, min(min(cN, cS), min(cW, cE)));
    const float3 mx = max(cC, max(max(cN, cS), max(cW, cE)));
    const float3 amp = saturate(min(mn, max(2.0 - mx, 0.0)) / max(mx, 1e-4));
    const float3 w_cas = sqrt(amp) * (-0.16);
    float3 hdr = max((cC + (cN + cS + cW + cE) * w_cas) / (1.0 + 4.0 * w_cas), 0.0);

    // 5-Mip Optical HDR Bloom + Night-Only Anamorphic Stadium Glare
    const float bloom_amt = max(pc.sky_horizon.w, 0.0);
    if (bloom_amt > 0.001) {
        const float3 bloom = src_aux.SampleLevel(lin_smp, uv, 0).rgb;
        const float night_boost = lerp(1.55, 0.55, pc.sun_screen.w);
        hdr += bloom * (0.17 * bloom_amt * night_boost);
        const float ana_w = lerp(0.042, 0.0, smoothstep(0.25, 0.58, pc.sun_screen.w));
        if (ana_w > 0.001) {
            const float3 ana = (
                src_aux.SampleLevel(lin_smp, uv + float2(-8.0 * t.x, 0.0), 0).rgb +
                src_aux.SampleLevel(lin_smp, uv + float2( 8.0 * t.x, 0.0), 0).rgb +
                src_aux.SampleLevel(lin_smp, uv + float2(-18.0 * t.x, 0.0), 0).rgb * 0.5 +
                src_aux.SampleLevel(lin_smp, uv + float2( 18.0 * t.x, 0.0), 0).rgb * 0.5
            ) * (1.0 / 3.0);
            hdr += ana * float3(0.88, 0.94, 1.12) * (ana_w * bloom_amt * night_boost);
        }
    }

    // Temporal Auto-Exposure (Eye Adaptation) + User Exposure + Extended AgX Filmic Tonemap
    const float auto_exp = clamp(src_aux2.Load(int3(0, 0, 0)).r, 0.45, 2.2);
    const float exposure = max(pc.grade_params.x, 0.1) * auto_exp;
    hdr *= exposure;
    float3 ldr = agx_tonemap(hdr);
    ldr = agx_punchy_look(ldr, saturate(pc.grade_params.z));

    const float l = luma(ldr);
    const float grade = saturate(pc.grade_params.z);
    const float3 shadow_tint = float3(0.95, 0.99, 1.05);
    const float3 high_tint   = float3(1.03, 1.00, 0.96);
    ldr = saturate(ldr * lerp(1.0.xxx, lerp(shadow_tint, high_tint, smoothstep(0.15, 0.80, l)), grade * 0.55));

    const float vig_amt = max(pc.grade_params.w, 0.0);
    if (vig_amt > 0.001) {
        const float2 local_uv = (uv - pc.view_rect.xy) / max(pc.view_rect.zw, 1e-5);
        const float2 d = (local_uv - 0.5) * float2(1.4, 1.0);
        ldr *= 1.0 - smoothstep(0.45, 1.15, dot(d, d)) * 0.32 * vig_amt;
    }

    return float4(saturate(ldr), center_sample.a);
}
