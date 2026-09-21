// hud.slang — the on-screen performance panel and slider overlay.
//
// Everything is one vertex buffer of coloured quads: the panel background, the
// slider tracks/knobs, the per-pass millisecond bars and the glyphs (which sample
// a 5x7 font atlas built on the host from font5x7.h). Positions arrive in pixels
// with the origin at the top-left, so the layout code on the CPU stays readable.

struct HudPC {
    float2 viewport;   // pixels
    float2 pad;
};
[[vk::push_constant]] HudPC hud;

[[vk::binding(0)]] Sampler2D fontAtlas;

struct VSIn {
    float2 pos : POSITION;     // pixels, top-left origin
    float2 uv : TEXCOORD0;     // atlas uv (or a solid colour when uv.x < 0)
    float4 color : COLOR0;
};

struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

[shader("vertex")]
VSOut vsHud(VSIn i) {
    VSOut o;
    // The HUD draws straight into the swapchain image, so the mapping has to
    // account for Vulkan's framebuffer convention: the origin is the TOP-left and
    // y grows downward, which means NDC y = -1 is the top row. (The scene pass is
    // different: it renders with y-up NDC into the HDR target and the composite
    // pass maps that to the swapchain, which is why the two paths do not share a
    // single convention here.)
    float2 ndc = float2(i.pos.x / hud.viewport.x * 2.0f - 1.0f,
                        i.pos.y / hud.viewport.y * 2.0f - 1.0f);
    o.pos = float4(ndc, 0.0f, 1.0f);
    o.uv = i.uv;
    o.color = i.color;
    return o;
}

[shader("fragment")]
float4 fsHud(VSOut i) : SV_Target {
    // uv.x < 0 marks a solid quad (panel, slider, bar); anything else samples the
    // glyph atlas, which is stored as coverage in the red channel.
    if (i.uv.x < 0.0f) return i.color;
    float coverage = fontAtlas.SampleLevel(i.uv, 0.0f).r;
    return float4(i.color.rgb, i.color.a * coverage);
}
