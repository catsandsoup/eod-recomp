// overlay_vk.hlsl - the in-game front end (Dear ImGui draw lists) for the Vulkan
// presenter: window-pixel positions, one texture (font atlas or decoded game UI
// art), vertex colours, alpha blending. The vertices use the GS draw layout
// (32 bytes: pos, depth, colour, texcoord, fog) so the existing pipeline vertex
// input serves; depth and fog are unused. Compiled by tools/build_vk_shaders.py
// into overlay_vk.inc. Same maths as vs_overlay/ps_overlay of the Direct3D 11
// shaders (runtime/gs/d3d11/gs_shaders.hlsl).
[[vk::push_constant]] cbuffer Overlay {
    float4 o_scale;  // xy: 2 / overlay size
};
[[vk::binding(0, 0)]] Texture2D<float4> t_src;
[[vk::binding(9, 0)]] SamplerState s_linear;

struct OverlayIn {
    [[vk::location(0)]] float2 pos : POSITION;
    [[vk::location(2)]] float4 col : COLOR;
    [[vk::location(3)]] float3 uv : TEXCOORD0;
};
struct OverlayOut {
    float4 pos : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
    [[vk::location(1)]] float4 col : COLOR;
};

OverlayOut vs_overlay(OverlayIn i) {
    OverlayOut o;
    o.pos = float4(i.pos.x * o_scale.x - 1.0, 1.0 - i.pos.y * o_scale.y, 0.0, 1.0);
    o.uv = i.uv.xy;
    o.col = i.col;
    return o;
}

[[vk::location(0)]] float4 ps_overlay(OverlayOut i) : SV_Target0 {
    return i.col * t_src.Sample(s_linear, i.uv);
}
