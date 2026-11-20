// Native depth-only caster passes and same-frame depth receiver resolve.
struct Constants {
    float4 ray_x, ray_y, ray_z;
    float4 near_box, far_box;
    float4 depth_fade;  // inverse near / far map depth spans, far fade start / end
    float4 surface;     // inverse target size, mask mode, opacity
    float4 transition;  // near blend start / end, view near plane, map size
};
[[vk::push_constant]] ConstantBuffer<Constants> sun;
[[vk::binding(0, 0)]] Texture2D<float> scene_depth;
[[vk::binding(1, 0)]] Texture2D<float> near_depth;
[[vk::binding(2, 0)]] Texture2D<float> far_depth;

float4 vs_sun_caster([[vk::location(0)]] float3 position : POSITION) : SV_Position {
    const float4 p = float4(position, 1.0);
    return float4(dot(sun.ray_x, p), dot(sun.ray_y, p), dot(sun.ray_z, p), 1.0);
}

float4 vs_sun_receiver(uint vertex : SV_VertexID) : SV_Position {
    return float4(vertex == 1 ? 3.0 : -1.0, vertex == 2 ? 3.0 : -1.0, 0.0, 1.0);
}

float occlusion(Texture2D<float> map, float3 p, float4 box, float inv_span, float slope) {
    const float2 uv = (p.xy - box.xy) / box.w + 0.5;
    const float d = (p.z - box.z) * inv_span;
    const float map_size = sun.transition.w;
    const float pad = 1.5 / map_size;
    if (any(uv < pad) || any(uv > 1 - pad) || d <= 0 || d >= 1)
        return 0;
    const float world_texel = box.w / map_size;
    // A constant world-space push and a bounded slope term keep a receiver from shadowing itself.
    const float reference = d - (world_texel * (1.1 + slope) + 0.012) * inv_span;
    const float2 t = uv * map_size - 0.5;
    const int2 base = int2(floor(t));
    const float2 w = frac(t);
    const float a = reference > map.Load(int3(base, 0));
    const float b = reference > map.Load(int3(base + int2(1, 0), 0));
    const float c = reference > map.Load(int3(base + int2(0, 1), 0));
    const float e = reference > map.Load(int3(base + int2(1, 1), 0));
    const float coverage = min(min(uv.x, uv.y), min(1 - uv.x, 1 - uv.y));
    return lerp(lerp(a, b, w.x), lerp(c, e, w.x), w.y) * smoothstep(pad, 0.04, coverage);
}

float4 ps_sun_receiver(float4 position : SV_Position) : SV_Target0 {
    const float depth = scene_depth.Load(int3(int2(position.xy), 0));
    const float depth_offset = sun.ray_y.w;
    const float z = sun.ray_x.w / max(depth - depth_offset, 1e-7);
    const float3 ray = float3(position.xy * sun.surface.xy * 2 - 1, 1);
    const float3 p = z * float3(dot(sun.ray_x.xyz, ray), dot(sun.ray_y.xyz, ray), dot(sun.ray_z.xyz, ray));
    const float3 normal = cross(ddx(p), ddy(p));
    const float slope = min(length(normal.xy) / max(abs(normal.z), 1e-7), 2.5);
    if (depth <= 0 || z < sun.transition.z * 0.99 || z >= sun.depth_fade.w)
        discard;

    const float blend = smoothstep(sun.transition.x, sun.transition.y, z);
    const float fade = 1 - smoothstep(sun.depth_fade.z, sun.depth_fade.w, z);
    float n = 0, f = 0;
    if (blend < 1)
        n = (1 - blend) * occlusion(near_depth, p, sun.near_box, sun.depth_fade.x, slope);
    if (blend > 0)
        f = blend * fade * occlusion(far_depth, p, sun.far_box, sun.depth_fade.y, slope);
    const float shadow = n + f;
    if (sun.surface.z == 1)
        return float4(n / max(shadow, 1e-7), f / max(shadow, 1e-7), 0, shadow);
    return float4(0, 0, 0, shadow * sun.surface.w);
}
