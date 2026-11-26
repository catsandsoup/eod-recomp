// Native depth-only + alpha-tested foliage caster passes and linear-HDR shadow mask receiver.
// 2026 UE5-Grade Remaster: Alpha-tested tree/fence dappled shadows + capped-resolution R8 shadow mask
// consumed in true Linear HDR by native_post.hlsl (ps_hdr_atmo).
struct Constants {
    float4 ray_x, ray_y, ray_z;
    float4 near_box, far_box;
    float4 depth_fade;  // inverse near / far map depth spans, far fade start / end
    float4 surface;     // inverse mask target size (xy), mode (>1.5: R8 mask w/ depth scale = mode-2, 1.0: debug mask, 0.0: legacy overlay), opacity (w)
    float4 transition;  // near blend start / end, view near plane, map size
};
[[vk::push_constant]] ConstantBuffer<Constants> sun;

// Receiver bindings (set 0)
[[vk::binding(0, 0)]] Texture2D<float> scene_depth;
[[vk::binding(1, 0)]] Texture2D<float> near_depth;
[[vk::binding(2, 0)]] Texture2D<float> far_depth;

// Alpha-tested caster bindings (matches scene::texture_desc set 0: binding 0 = tex, 1 = smp)
[[vk::binding(0, 0)]] Texture2D<float4> caster_tex;
[[vk::binding(1, 0)]] SamplerState      caster_smp;

struct CasterAteOut {
    float4 pos : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
};

float4 vs_sun_caster([[vk::location(0)]] float3 position : POSITION) : SV_Position {
    const float4 p = float4(position, 1.0);
    return float4(dot(sun.ray_x, p), dot(sun.ray_y, p), dot(sun.ray_z, p), 1.0);
}

CasterAteOut vs_sun_caster_ate([[vk::location(0)]] float3 position : POSITION,
                               [[vk::location(1)]] float2 uv : TEXCOORD0) {
    CasterAteOut o;
    const float4 p = float4(position, 1.0);
    o.pos = float4(dot(sun.ray_x, p), dot(sun.ray_y, p), dot(sun.ray_z, p), 1.0);
    o.uv = uv;
    return o;
}

void ps_sun_caster_ate(CasterAteOut i) {
    const float a = caster_tex.Sample(caster_smp, i.uv).a;
    if (a < 0.32) discard;
}

float4 vs_sun_receiver(uint vertex : SV_VertexID) : SV_Position {
    return float4(vertex == 1 ? 3.0 : -1.0, vertex == 2 ? 3.0 : -1.0, 0.0, 1.0);
}

float pcf_tap_bilinear(Texture2D<float> map, float2 texel_pos, float reference, int map_max) {
    const float2 f = frac(texel_pos);
    const int2 i0 = clamp(int2(floor(texel_pos)), 0, map_max);
    const int2 i1 = min(i0 + 1, map_max);
    const float s00 = (reference > map.Load(int3(i0.x, i0.y, 0))) ? 1.0 : 0.0;
    const float s10 = (reference > map.Load(int3(i1.x, i0.y, 0))) ? 1.0 : 0.0;
    const float s01 = (reference > map.Load(int3(i0.x, i1.y, 0))) ? 1.0 : 0.0;
    const float s11 = (reference > map.Load(int3(i1.x, i1.y, 0))) ? 1.0 : 0.0;
    return lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
}

float occlusion(Texture2D<float> map, float3 p, float4 box, float inv_span, float slope, float filter_texels, bool is_far) {
    const float2 uv = (p.xy - box.xy) / box.w + 0.5;
    const float d = (p.z - box.z) * inv_span;
    const float map_size = sun.transition.w;
    const float pad = (filter_texels + 2.0) / map_size;
    if (any(uv < pad) || any(uv > 1.0 - pad) || d <= 0.0 || d >= 1.0)
        return 0.0;

    const float world_texel = box.w / map_size;
    const float reference = d - (world_texel * (1.25 + slope * 1.15) + 0.015) * inv_span;

    const int map_max = int(map_size) - 1;
    const float2 center = uv * map_size - 0.5;

    float occ;
    if (is_far) {
        occ = pcf_tap_bilinear(map, center, reference, map_max);
    } else {
        occ = pcf_tap_bilinear(map, center + float2(-filter_texels * 0.45, -filter_texels * 0.45), reference, map_max) * 0.50;
        occ += pcf_tap_bilinear(map, center + float2( filter_texels * 0.45,  filter_texels * 0.45), reference, map_max) * 0.50;
    }

    const float coverage = min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y));
    return occ * smoothstep(pad, pad + 0.035, coverage);
}

float4 ps_sun_receiver(float4 position : SV_Position) : SV_Target0 {
    const bool r8_mask_mode = (sun.surface.z >= 1.5);
    const int depth_scale = r8_mask_mode ? max(1, int(round(sun.surface.z - 1.0))) : 1;
    const int2 depth_coord = int2(position.xy) * depth_scale;
    const float depth = scene_depth.Load(int3(depth_coord, 0));
    if (depth <= 1.0e-5) {
        if (r8_mask_mode) return float4(1.0, 1.0, 1.0, 1.0);
        discard;
    }
    const float depth_offset = sun.ray_y.w;
    const float z = sun.ray_x.w / max(depth - depth_offset, 1e-7);
    const float3 ray = float3(position.xy * sun.surface.xy * 2.0 - 1.0, 1.0);
    const float3 p = z * float3(dot(sun.ray_x.xyz, ray), dot(sun.ray_y.xyz, ray), dot(sun.ray_z.xyz, ray));
    const float3 normal = cross(ddx(p), ddy(p));
    const float slope = min(length(normal.xy) / max(abs(normal.z), 1e-7), 2.5);
    if (z < sun.transition.z * 0.99 || z >= sun.depth_fade.w) {
        if (r8_mask_mode) return float4(1.0, 1.0, 1.0, 1.0);
        discard;
    }

    const float blend = smoothstep(sun.transition.x, sun.transition.y, z);
    const float fade = 1.0 - smoothstep(sun.depth_fade.z, sun.depth_fade.w, z);
    const float near_radius = lerp(1.05, 1.75, saturate(z / 28.0));
    const float far_radius = 1.5;
    float n = 0.0, f = 0.0;
    if (blend < 1.0)
        n = (1.0 - blend) * occlusion(near_depth, p, sun.near_box, sun.depth_fade.x, slope, near_radius, false);
    if (blend > 0.0)
        f = blend * fade * occlusion(far_depth, p, sun.far_box, sun.depth_fade.y, slope, far_radius, true);
    const float shadow = saturate(n + f);
    if (r8_mask_mode) {
        // Output 1.0 = unshadowed, (1.0 - shadow * strength) = shadowed; consumed in true Linear HDR!
        const float vis = saturate(1.0 - shadow * saturate(sun.surface.w));
        return float4(vis, vis, vis, 1.0);
    }
    if (sun.surface.z == 1.0)
        return float4(n / max(shadow, 1e-7), f / max(shadow, 1e-7), 0.0, shadow);
    return float4(0.018, 0.022, 0.032, shadow * min(sun.surface.w, 0.64));
}
