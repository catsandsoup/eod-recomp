// The retail 0x35BE08 PATH2 quad uses fixed UVs and bilinear CT32 sampling.
// Match the Vulkan GS frontend's half-pixel offset, UV quantization and
// dual-source alpha blend. The checked UV window only needs a 32x32 texture.
[[vk::push_constant]] cbuffer QuadConstants {
    float2 scale;
    float2 offset;
};
[[vk::binding(0, 0)]] Texture2D<float4> image;
[[vk::binding(1, 0)]] SamplerState linear_clamp;

struct VOut {
    float4 pos : SV_Position;
    [[vk::location(0)]] noperspective float2 uv : TEXCOORD0;
};

VOut vs_bootstrap_quad([[vk::location(0)]] float3 p : POSITION,
                       [[vk::location(1)]] float2 uv : TEXCOORD0) {
    VOut result;
    result.pos = float4((p.xy + 0.5) * scale + offset, 0, 1);
    result.uv = uv;
    return result;
}

struct POut {
    [[vk::location(0)]] float4 color : SV_Target0;
    [[vk::location(1)]] float4 factor : SV_Target1;
};

POut ps_bootstrap_quad(VOut input) {
    float2 uv = floor(input.uv * 16.0) / (16.0 * 32.0);
    int4 rgba = (int4)round(image.SampleLevel(linear_clamp, uv, 0) * 255.0);
    rgba.a >>= 1;  // RGBAQ = (0x80, 0x80, 0x80, 0x40), TCC and MODULATE
    POut result;
    result.color = float4(rgba) / 255.0;
    result.factor = float4(0, 0, 0, rgba.a / 128.0);
    return result;
}
