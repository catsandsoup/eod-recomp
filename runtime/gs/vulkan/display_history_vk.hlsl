// 0x35BBD0 blends over a persistent CT32 display target. Use a separate
// snapshot of the prior target and preserve the GS integer blend equation.
[[vk::push_constant]] cbuffer PassConsts {
    float4 dst, src, misc;
    uint4 format;  // FIX, COLCLAMP, PABE, FBA; matches the PassCB fmt field
};
[[vk::binding(0)]] Texture2D<float4> scene;
[[vk::binding(1)]] Texture2D<float4> previous;

float4 ps_display_history(float4 position : SV_Position) : SV_Target {
    int3 at = int3(int2(position.xy), 0);
    // The final scissor-edge sliver uses the last native pixel's UV.
    int2 uv = min(at.xy + (int)misc.x, int2(src.zw) - 1);
    int4 cs = (int4)round(scene.Load(int3(uv, 0)) * 255.0);
    int4 cd = (int4)round(previous.Load(at) * 255.0);
    int3 rgb = cs.rgb;
    if (!format.z || cs.a >= 128)
        rgb = cd.rgb + (((cs.rgb - cd.rgb) * (int)format.x) >> 7);
    rgb = format.y ? clamp(rgb, 0, 255) : (rgb & 255);
    return float4(rgb, cs.a | (format.w ? 128 : 0)) / 255.0;
}
