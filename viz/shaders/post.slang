// post.slang — fullscreen passes: the bloom chain, ACES tonemap, sRGB encode.
//
// The swapchain image is VK_FORMAT_B8G8R8A8_SRGB, so the hardware performs the
// linear -> sRGB transfer function on write; every pass here outputs *linear*
// display-referred colour.
//
// Bloom chain (5 levels, half resolution and down): bright pass with a soft knee
// -> 4x 13-tap downsample (Jimenez) -> 4x 9-tap tent upsample, additively blended
// back into the larger level -> composite with the scene.

struct PostUBO {
    float4 params;   // x = bloom strength, y = exposure, z = bloom threshold, w = time
    float4 view;     // x = viewport width, y = height, z = texel width, w = texel height
};

// Sampler2D is Slang's *combined* texture+sampler type: one
// VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER at the given binding, which is what
// the C++ side creates. Method form is tex.SampleLevel(uv, lod).
[[vk::binding(0)]] ConstantBuffer<PostUBO> post;
[[vk::binding(1)]] Sampler2D sceneTex;
[[vk::binding(2)]] Sampler2D bloomTex;

// Bloom passes take their source texel size and threshold through push constants,
// so no per-pass descriptor or UBO update is needed while recording.
struct BloomPC {
    float2 srcTexel;    // 1 / source size
    float threshold;    // soft-knee knee position
    float intensity;
    float2 dstTexel;    // 1 / destination size
    float clampPeak;    // firefly guard (bright pass only)
    float pad0;
};
[[vk::push_constant]] BloomPC bloomPC;

struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VSOut vsFullscreen(uint id : SV_VertexID) {
    float2 p = float2((id == 2u) ? 3.0f : -1.0f, (id == 1u) ? 3.0f : -1.0f);
    VSOut o;
    o.pos = float4(p, 0.0f, 1.0f);
    // uv in [0,1] with y down, matching the render-target convention.
    o.uv = float2((p.x + 1.0f) * 0.5f, (1.0f - p.y) * 0.5f);
    return o;
}

// ACES filmic approximation (Narkowicz 2015).
float3 acesFilm(float3 x) {
    const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// ---------------------------------------------------------------- bloom ----
// 13-tap downsample kernel: 5 bilinear taps in a cross plus the 4 corners, which
// stops small bright features from flickering between levels.
float3 downsample13(Sampler2D src, float2 uv, float2 texel) {
    const float2 o = texel;
    float3 a = src.SampleLevel(uv + float2(-2.0f, -2.0f) * o, 0.0f).rgb;
    float3 b = src.SampleLevel(uv + float2(0.0f, -2.0f) * o, 0.0f).rgb;
    float3 c = src.SampleLevel(uv + float2(2.0f, -2.0f) * o, 0.0f).rgb;
    float3 d = src.SampleLevel(uv + float2(-2.0f, 0.0f) * o, 0.0f).rgb;
    float3 e = src.SampleLevel(uv, 0.0f).rgb;
    float3 f = src.SampleLevel(uv + float2(2.0f, 0.0f) * o, 0.0f).rgb;
    float3 g = src.SampleLevel(uv + float2(-2.0f, 2.0f) * o, 0.0f).rgb;
    float3 h = src.SampleLevel(uv + float2(0.0f, 2.0f) * o, 0.0f).rgb;
    float3 i = src.SampleLevel(uv + float2(2.0f, 2.0f) * o, 0.0f).rgb;
    float3 j = src.SampleLevel(uv + float2(-1.0f, -1.0f) * o, 0.0f).rgb;
    float3 k = src.SampleLevel(uv + float2(1.0f, -1.0f) * o, 0.0f).rgb;
    float3 l = src.SampleLevel(uv + float2(-1.0f, 1.0f) * o, 0.0f).rgb;
    float3 m = src.SampleLevel(uv + float2(1.0f, 1.0f) * o, 0.0f).rgb;
    float3 result = e * 0.125f;
    result += (a + c + g + i) * 0.03125f;
    result += (b + d + f + h) * 0.0625f;
    result += (j + k + l + m) * 0.125f;
    return result;
}

// 9-tap tent upsample: smooth, and cheap enough for the whole chain.
float3 upsampleTent(Sampler2D src, float2 uv, float2 texel, float radius) {
    const float2 o = texel * radius;
    float3 sum = src.SampleLevel(uv, 0.0f).rgb * 4.0f;
    sum += src.SampleLevel(uv + float2(-o.x, 0.0f), 0.0f).rgb * 2.0f;
    sum += src.SampleLevel(uv + float2(o.x, 0.0f), 0.0f).rgb * 2.0f;
    sum += src.SampleLevel(uv + float2(0.0f, -o.y), 0.0f).rgb * 2.0f;
    sum += src.SampleLevel(uv + float2(0.0f, o.y), 0.0f).rgb * 2.0f;
    sum += src.SampleLevel(uv + float2(-o.x, -o.y), 0.0f).rgb;
    sum += src.SampleLevel(uv + float2(o.x, -o.y), 0.0f).rgb;
    sum += src.SampleLevel(uv + float2(-o.x, o.y), 0.0f).rgb;
    sum += src.SampleLevel(uv + float2(o.x, o.y), 0.0f).rgb;
    return sum * (1.0f / 16.0f);
}

// Bright pass with a soft knee: no hard cut-off, so the spot never pops.
// 'clampPeak' is a firefly guard: without it a single very bright pixel (the
// stylised sun disc is 16x, a mirror specular hit reaches ~8x) dominates the whole
// chain and turns into a large, unexpected blob when the bloom strength is raised.
float3 brightPass(float3 c, float threshold, float clampPeak) {
    c = min(c, float3(clampPeak));
    float br = max(c.r, max(c.g, c.b));
    const float knee = 0.35f;
    float soft = clamp(br - threshold + knee, 0.0f, 2.0f * knee);
    soft = soft * soft / (4.0f * knee);
    float contribution = max(soft, br - threshold) / max(br, 1e-5f);
    return c * contribution;
}

[shader("fragment")]
float4 fsBloomPre(VSOut i) : SV_Target {
    float3 c = downsample13(sceneTex, i.uv, bloomPC.srcTexel);
    return float4(brightPass(c, bloomPC.threshold, bloomPC.clampPeak) * bloomPC.intensity, 1.0f);
}

[shader("fragment")]
float4 fsBloomDown(VSOut i) : SV_Target {
    return float4(downsample13(sceneTex, i.uv, bloomPC.srcTexel), 1.0f);
}

[shader("fragment")]
float4 fsBloomUp(VSOut i) : SV_Target {
    // Additive: alpha = 1 so the SRC_ALPHA/ONE blend acts as src + dst.
    return float4(upsampleTent(sceneTex, i.uv, bloomPC.srcTexel, 1.0f) * bloomPC.intensity, 1.0f);
}

[shader("fragment")]
float4 fsComposite(VSOut i) : SV_Target {
    float3 hdr = sceneTex.SampleLevel(i.uv, 0.0f).rgb;
    float3 bloom = bloomTex.SampleLevel(i.uv, 0.0f).rgb;

    float exposure = post.params.y;
    float3 col = hdr + bloom * post.params.x;
    col *= exposure;
    col = acesFilm(col);
    // Slight vignette: cheap, and it keeps the eye on the spot.
    float2 d = i.uv - 0.5f;
    col *= 1.0f - saturate(dot(d, d) * 0.55f);
    return float4(col, 1.0f);
}

// Straight copy: used as a fallback when the bloom chain is disabled.
[shader("fragment")]
float4 fsCopy(VSOut i) : SV_Target {
    return float4(sceneTex.SampleLevel(i.uv, 0.0f).rgb, 1.0f);
}
