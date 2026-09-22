#include "hud.h"

#include "font5x7.h"
#include "math_viz.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

using hviz::checkVk;

namespace viz {

namespace {
constexpr uint32_t kMaxVertices = 65536;
constexpr uint32_t kCellW = kFontWidth + 1;    // 1 px gutter so NEAREST sampling
constexpr uint32_t kCellH = kFontHeight + 1;   // never bleeds between glyphs
constexpr uint32_t kAtlasCols = 16;
constexpr uint32_t kAtlasRows = 6;

// Palette: kept close to the flux heat map so the overlay looks of a piece.
constexpr float kBg[4] = {0.035f, 0.045f, 0.065f, 0.86f};
constexpr float kPanelLine[4] = {0.16f, 0.20f, 0.26f, 1.0f};
constexpr float kText[4] = {0.84f, 0.87f, 0.91f, 1.0f};
constexpr float kDim[4] = {0.55f, 0.60f, 0.66f, 1.0f};
constexpr float kGreen[4] = {0.20f, 0.75f, 0.48f, 1.0f};
constexpr float kAmber[4] = {0.99f, 0.62f, 0.16f, 1.0f};
constexpr float kRed[4] = {0.92f, 0.32f, 0.26f, 1.0f};
constexpr float kBlue[4] = {0.25f, 0.55f, 0.95f, 1.0f};
} // namespace

bool Hud::init(hviz::VkCore &vk, VkFormat swapchainFormat, VkExtent2D extent, const std::string &shaderDir) {
    m_vk = &vk;
    m_swapFormat = swapchainFormat;
    m_extent = extent;
    VkDevice dev = vk.device();

    buildFontAtlas();
    m_sampler = createSampler(dev, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo lci{};
    lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    lci.bindingCount = 1;
    lci.pBindings = &binding;
    checkVk(vkCreateDescriptorSetLayout(dev, &lci, nullptr, &m_setLayout), "vkCreateDescriptorSetLayout(hud)");

    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = vk.descriptorPool();
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &m_setLayout;
    checkVk(vkAllocateDescriptorSets(dev, &ai, &m_set), "vkAllocateDescriptorSets(hud)");
    writeCombinedImage(dev, m_set, 0, m_sampler, m_atlas.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    const VkDescriptorSetLayout sets[1] = {m_setLayout};
    struct HudPC {
        float vx, vy, px, py;
    };
    m_pipeLayout = createPipelineLayout(dev, sets, sizeof(HudPC), VK_SHADER_STAGE_VERTEX_BIT);

    VkShaderModule vs = loadShaderModule(dev, shaderDir + "/vsHud.spv");
    VkShaderModule fs = loadShaderModule(dev, shaderDir + "/fsHud.spv");
    GraphicsPipelineDesc desc{};
    desc.layout = m_pipeLayout;
    desc.vs = vs;
    desc.fs = fs;
    desc.colorFormat = m_swapFormat;
    desc.blend = true;             // straight alpha over the composited frame
    desc.depthAttachment = false;
    desc.cull = VK_CULL_MODE_NONE;
    // Vertex layout: pos.xy, uv.xy, colour.rgba (must match struct Vertex and the
    // VSIn declaration in viz/shaders/hud.slang).
    const VkVertexInputBindingDescription bindingDesc{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attrDesc[3] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, x)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, u)},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, r)},
    };
    desc.vertexBindings = &bindingDesc;
    desc.vertexBindingCount = 1;
    desc.vertexAttributes = attrDesc;
    desc.vertexAttributeCount = 3;
    m_pipeline = createGraphicsPipeline(dev, desc);
    destroyShaderModules(dev, {vs, fs});

    m_capacity = kMaxVertices;
    m_vbo = vk.createBuffer(sizeof(Vertex) * m_capacity, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
    m_cpu.reserve(m_capacity);
    return true;
}

void Hud::buildFontAtlas() {
    const uint32_t W = kAtlasCols * kCellW;
    const uint32_t H = kAtlasRows * kCellH;
    std::vector<uint8_t> pixels(static_cast<size_t>(W) * H, 0);
    const int glyphCount = kFontLastChar - kFontFirstChar + 1;
    for (int g = 0; g < glyphCount; g++) {
        const uint32_t cx = (static_cast<uint32_t>(g) % kAtlasCols) * kCellW;
        const uint32_t cy = (static_cast<uint32_t>(g) / kAtlasCols) * kCellH;
        for (uint32_t row = 0; row < kFontHeight; row++) {
            const uint8_t bits = kFont5x7[g][row];
            for (uint32_t col = 0; col < kFontWidth; col++) {
                if (bits & (1u << (kFontWidth - 1u - col))) {
                    pixels[static_cast<size_t>(cy + row) * W + (cx + col)] = 255;
                }
            }
        }
    }

    m_atlas = createImage2D(*m_vk, W, H, VK_FORMAT_R8_UNORM,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    hviz::Buffer staging = m_vk->createBuffer(W * H, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    std::memcpy(staging.mapped, pixels.data(), pixels.size());
    m_vk->submitOneShot([&](VkCommandBuffer cmd) {
        imageBarrier(cmd, m_atlas.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                     VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {W, H, 1};
        vkCmdCopyBufferToImage(cmd, staging.buffer, m_atlas.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &region);
        imageBarrier(cmd, m_atlas.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_ACCESS_SHADER_READ_BIT);
    });
    m_vk->destroyBuffer(staging);
    std::printf("[hud] font atlas %ux%u R8 (%d glyphs, %ux%u cells)\n", W, H, glyphCount, kCellW, kCellH);
}

void Hud::destroy() {
    if (!m_vk) return;
    VkDevice dev = m_vk->device();
    if (m_pipeline) vkDestroyPipeline(dev, m_pipeline, nullptr);
    if (m_pipeLayout) vkDestroyPipelineLayout(dev, m_pipeLayout, nullptr);
    if (m_setLayout) vkDestroyDescriptorSetLayout(dev, m_setLayout, nullptr);
    if (m_sampler) vkDestroySampler(dev, m_sampler, nullptr);
    m_vk->destroyTexture(m_atlas);
    if (m_vbo.buffer) m_vk->destroyBuffer(m_vbo);
    m_vk = nullptr;
}

void Hud::resize(VkExtent2D extent) { m_extent = extent; }

// ------------------------------------------------------------- primitives --
void Hud::rect(float x, float y, float w, float h, float r, float g, float b, float a) {
    // uv.x < 0 tells the fragment shader this is a solid quad.
    //
    // Snap to whole device pixels: an overlay drawn on half-pixel boundaries has
    // every edge blended with the scene behind it, which is exactly what made the
    // panel rules, the compass ticks and the map frame look "dirty". Everything
    // here is axis aligned, so snapping costs nothing and buys crispness.
    x = std::floor(x * m_scale + 0.5f);
    y = std::floor(y * m_scale + 0.5f);
    w = std::floor(w * m_scale + 0.5f);
    h = std::floor(h * m_scale + 0.5f);
    if (w < 1.0f) w = 1.0f;
    if (h < 1.0f) h = 1.0f;
    const Vertex v0{x, y, -1.0f, 0.0f, r, g, b, a};
    const Vertex v1{x + w, y, -1.0f, 0.0f, r, g, b, a};
    const Vertex v2{x + w, y + h, -1.0f, 0.0f, r, g, b, a};
    const Vertex v3{x, y + h, -1.0f, 0.0f, r, g, b, a};
    for (const Vertex &v : {v0, v1, v2, v0, v2, v3}) {
        if (m_cpu.size() < m_capacity) m_cpu.push_back(v);
    }
}

float Hud::textWidth(const std::string &s, float scale) const {
    return static_cast<float>(s.size()) * (kFontWidth + 1) * scale * m_scale;
}

void Hud::text(float x, float y, const std::string &s, float r, float g, float b, float scale) {
    x = std::round(x * m_scale);
    y = std::round(y * m_scale);
    // Integer glyph blocks: the atlas is sampled with NEAREST, so a fractional
    // glyph size would leave some source columns one pixel wider than others and
    // the letters come out uneven ("blurry"). Rounding the block and the advance
    // gives clean pixel-art scaling at any --hud-scale.
    const float glyphW = std::max(1.0f, std::round(kFontWidth * scale * m_scale));
    const float glyphH = std::max(1.0f, std::round(kFontHeight * scale * m_scale));
    const float advance = std::max(1.0f, std::round((kFontWidth + 1) * scale * m_scale));
    for (char ch : s) {
        int ci = static_cast<int>(static_cast<unsigned char>(ch)) - kFontFirstChar;
        if (ci < 0 || ci >= kFontLastChar - kFontFirstChar + 1) {
            x += advance;
            continue;
        }
        const float u0 = static_cast<float>((static_cast<uint32_t>(ci) % kAtlasCols) * kCellW) / (kAtlasCols * kCellW);
        const float v0 = static_cast<float>((static_cast<uint32_t>(ci) / kAtlasCols) * kCellH) / (kAtlasRows * kCellH);
        const float du = static_cast<float>(kFontWidth) / (kAtlasCols * kCellW);
        const float dv = static_cast<float>(kFontHeight) / (kAtlasRows * kCellH);
        const Vertex v0v{x, y, u0, v0, r, g, b, 1.0f};
        const Vertex v1v{x + glyphW, y, u0 + du, v0, r, g, b, 1.0f};
        const Vertex v2v{x + glyphW, y + glyphH, u0 + du, v0 + dv, r, g, b, 1.0f};
        const Vertex v3v{x, y + glyphH, u0, v0 + dv, r, g, b, 1.0f};
        for (const Vertex &v : {v0v, v1v, v2v, v0v, v2v, v3v}) {
            if (m_cpu.size() < m_capacity) m_cpu.push_back(v);
        }
        x += advance;
    }
}

// Text with a 1 px dark drop shadow: without it, labels drawn straight onto the
// scene (compass letters, the flux-map caption) lose their edges.
void Hud::textShadow(float x, float y, const std::string &s, float r, float g, float b, float scale) {
    text(x + 1.0f / m_scale, y + 1.0f / m_scale, s, 0.0f, 0.0f, 0.0f, scale);
    text(x, y, s, r, g, b, scale);
}

// The HUD has exactly two primitives now: pixel-snapped axis-aligned rects and
// bitmap text. The one shape that cannot be axis aligned (the compass needle) is
// emitted as an explicit filled triangle inside compass(), because a diagonal
// stroke aliased into dotted, half-lit pixels -- the "dirty compass" look.

// A small compass rose in the top-right corner: N/E/S/W ticks (world axes of this
// scene: +Z is south, +X is east), a needle for the camera heading, a marker for
// the selected heliostat and one for the sun.
//
// Everything here is deliberately built out of *axis-aligned* quads and opaque
// fills: diagonal 1 px `line()` strokes alias into dotted, half-lit pixels (the
// "dirty" look), and translucent shapes drawn on top of each other blend twice
// where they overlap.
void Hud::compass(float cx, float cy, float radius, const HudStats &st) {
    cx = std::round(cx);
    cy = std::round(cy);
    rect(cx - radius - 10.0f, cy - radius - 10.0f, 2.0f * radius + 20.0f, 2.0f * radius + 22.0f, kBg[0], kBg[1],
         kBg[2], kBg[3]);
    rect(cx - radius - 10.0f, cy - radius - 10.0f, 2.0f * radius + 20.0f, 2.0f, kBlue[0], kBlue[1], kBlue[2]);

    // Ring + ticks. Screen convention: up = north (-Z), right = east (+X), so a
    // world azimuth (degrees from +Z, positive towards +X) maps to the screen angle
    // a = 180 - az (counter-clockwise from +x).
    auto toScreen = [&](float azDeg, float r) {
        const float a = (180.0f - azDeg) * 3.14159265358979f / 180.0f;
        return std::pair<float, float>{cx + r * std::cos(a), cy - r * std::sin(a)};
    };
    // 12 ticks every 30 deg, drawn as small squares on the ring (no diagonals).
    for (int i = 0; i < 12; i++) {
        const float az = static_cast<float>(i) * 30.0f;
        const bool major = (i % 3) == 0;
        const auto p = toScreen(az, radius - (major ? 1.5f : 1.0f));
        const float s = major ? 3.0f : 2.0f;
        rect(p.first - s * 0.5f, p.second - s * 0.5f, s, s, kDim[0], kDim[1], kDim[2], 1.0f);
    }
    struct Tick {
        const char *label;
        float az;
        float r, g, b;
    };
    const Tick ticks[4] = {{"N", 180.0f, 0.35f, 0.60f, 1.00f},
                           {"E", 90.0f, 0.30f, 0.90f, 0.50f},
                           {"S", 0.0f, 1.00f, 0.40f, 0.35f},
                           {"W", -90.0f, 1.00f, 0.76f, 0.25f}};
    for (const Tick &t : ticks) {
        const auto p = toScreen(t.az, radius + 7.0f);
        // A 12x16 dark chip behind each letter, with the glyph at scale 2 (10x14 px).
        // Two reasons: the 5x7 font is simply too small to read over the scene, and a
        // blue "N" on a blue sky is invisible without a backing.
        rect(p.first - 6.0f, p.second - 8.0f, 12.0f, 16.0f, 0.02f, 0.03f, 0.05f, 0.88f);
        text(p.first - 5.0f, p.second - 7.0f, t.label, t.r, t.g, t.b, 2.0f);
    }

    // Camera needle: one opaque filled triangle from the hub to the rim, with a
    // square hub. Single layer, so nothing double-blends.
    {
        const auto tip = toScreen(st.cameraAzimuthDeg, radius - 10.0f);
        const auto l = toScreen(st.cameraAzimuthDeg + 7.0f, radius - 34.0f);
        const auto r = toScreen(st.cameraAzimuthDeg - 7.0f, radius - 34.0f);
        const Vertex a{cx, cy, -1.0f, 0.0f, kText[0], kText[1], kText[2], 1.0f};
        const Vertex b{l.first, l.second, -1.0f, 0.0f, kText[0], kText[1], kText[2], 1.0f};
        const Vertex c{r.first, r.second, -1.0f, 0.0f, kText[0], kText[1], kText[2], 1.0f};
        const Vertex d{tip.first, tip.second, -1.0f, 0.0f, kText[0], kText[1], kText[2], 1.0f};
        for (const Vertex &v : {a, b, d, a, d, c}) {
            if (m_cpu.size() >= m_capacity) break;
            Vertex s = v;
            s.x *= m_scale;
            s.y *= m_scale;
            m_cpu.push_back(s);
        }
        rect(cx - 2.0f, cy - 2.0f, 4.0f, 4.0f, kBg[0], kBg[1], kBg[2], 1.0f);
        rect(cx - 1.0f, cy - 1.0f, 2.0f, 2.0f, kText[0], kText[1], kText[2], 1.0f);
    }

    // Sun marker: an opaque diamond built from four triangles that share the centre
    // vertex, so no pixel is covered twice.
    {
        const auto p = toScreen(st.sunAzimuth, radius - 16.0f);
        const float s = 5.0f;
        const Vertex c{p.first, p.second, -1.0f, 0.0f, 1.0f, 0.90f, 0.35f, 1.0f};
        const Vertex n{p.first, p.second - s, -1.0f, 0.0f, 1.0f, 0.90f, 0.35f, 1.0f};
        const Vertex e{p.first + s, p.second, -1.0f, 0.0f, 1.0f, 0.90f, 0.35f, 1.0f};
        const Vertex so{p.first, p.second + s, -1.0f, 0.0f, 1.0f, 0.90f, 0.35f, 1.0f};
        const Vertex w{p.first - s, p.second, -1.0f, 0.0f, 1.0f, 0.90f, 0.35f, 1.0f};
        for (const Vertex &v : {c, n, e, c, e, so, c, so, w, c, w, n}) {
            if (m_cpu.size() >= m_capacity) break;
            Vertex q = v;
            q.x *= m_scale;
            q.y *= m_scale;
            m_cpu.push_back(q);
        }
    }

    // Selected field mirror: a ring of squares at its azimuth.
    if (st.heliostatName && *st.heliostatName) {
        float az = 0.0f;
        if (!std::strcmp(st.heliostatName, "North")) az = 180.0f;
        else if (!std::strcmp(st.heliostatName, "East")) az = 90.0f;
        else if (!std::strcmp(st.heliostatName, "West")) az = -90.0f;
        const auto p = toScreen(az, radius - 28.0f);
        rect(p.first - 4.0f, p.second - 4.0f, 8.0f, 8.0f, kGreen[0], kGreen[1], kGreen[2]);
        rect(p.first - 2.0f, p.second - 2.0f, 4.0f, 4.0f, kBg[0], kBg[1], kBg[2]);
    }
    // No text under the rose: the "cam az / sun az" line overlapped the compass and was
    // clipped, and both values are already shown in the panel and on the ground markers.
}

void Hud::bar(float x, float y, float w, float h, float frac, float r, float g, float b) {    frac = clampf(frac, 0.0f, 1.0f);
    rect(x, y, w, h, 0.10f, 0.12f, 0.15f);
    rect(x, y, w * frac, h, r, g, b);
}

void Hud::slider(int id, float x, float y, float w, const char *label, float value, const std::string &valueText) {
    const float trackH = 6.0f;
    const float trackY = y + 12.0f;
    rect(x, trackY, w, trackH, 0.13f, 0.15f, 0.19f);
    rect(x, trackY, w * clampf(value, 0.0f, 1.0f), trackH, kBlue[0], kBlue[1], kBlue[2]);
    const float knobX = x + w * clampf(value, 0.0f, 1.0f);
    rect(knobX - 3.0f, trackY - 4.0f, 6.0f, trackH + 8.0f, kAmber[0], kAmber[1], kAmber[2]);

    Slider s;
    s.id = id;
    s.x = x;
    s.y = trackY - 8.0f;
    s.w = w;
    s.h = trackH + 16.0f;
    s.value = value;
    s.label = label;
    s.valueText = valueText;
    m_sliders.push_back(s);

    text(x, y, label, kDim[0], kDim[1], kDim[2], 1.0f);
    // Right-align the value inside the track, but never let it run over the label
    // (that is what made "beam target" and its value sit on top of each other).
    const float labelW = textWidth(label, 1.0f) / m_scale;
    const float valueW = textWidth(valueText, 1.0f) / m_scale;
    float valueX = x + w - valueW;
    if (valueX < x + labelW + 8.0f) valueX = x + labelW + 8.0f;
    text(valueX, y, valueText, kText[0], kText[1], kText[2], 1.0f);
}

// ---------------------------------------------------------------- build ----
void Hud::build(const HudStats &st, float dt) {
    // The layout is rebuilt at a smaller scale until the panel fits the window: cheap
    // (a few thousand vertices) and it means a bigger UI can never clip its own
    // readouts -- which is exactly what used to happen to the day-path lines.
    for (int attempt = 0; attempt < 10; attempt++) {
        buildOnce(st, dt);
        if (m_lastContentFits) break;
        m_scale = std::max(0.6f, m_scale * 0.85f);
    }
}

void Hud::buildOnce(const HudStats &st, float dt) {
    (void)dt;
    m_cpu.clear();
    m_sliders.clear();

    const float pad = 12.0f;
    const float lineH = 15.0f;
    const float scale = 1.35f;
    // Everything below is authored in HUD units and scaled by m_scale at emit
    // time, so the usable layout space is the frame size divided by the scale.
    const float hudW = static_cast<float>(m_extent.width) / m_scale;
    const float hudH = static_cast<float>(m_extent.height) / m_scale;
    // The panel is a status column, not a full-width bar.
    //
    // The width is FIXED for a given window size (it does not follow the content), so
    // it never jumps when a readout changes width -- S95 46.26 -> 226.67, a toast
    // appearing, a longer mirror name. Lines that would be wider than the panel are
    // truncated in line() instead. Tune the two numbers below to change the width:
    //   hudW * 0.46  = share of the window width
    //   300 .. 460   = clamps, in HUD units (1 HUD unit = 1 px at --hud-scale 1)
    const float panelW = clampf(hudW * 0.46f, 300.0f, 460.0f);
    const float panelTop = pad;
    const float panelLeft = pad;
    const float x0 = panelLeft + pad;
    const bool tiny = hudH < 240.0f;          // drop the hint lines entirely
    const bool showPassBars = hudH > 200.0f;

    // ---- pass table (built first so the panel height is known) ----
    struct Row {
        const char *name;
        float ms;
    };
    Row rows[8];
    int rowCount = 0;
    if (st.timer && showPassBars) {
        // Five rows instead of seven: the three bloom stages are summed, because the
        // panel is a status display -- the full per-stage breakdown is printed at
        // exit (and in docs/perf_log.md).
        const float bloomMs = st.timer->smoothedStageMs(kStageBloomPre) +
                              st.timer->smoothedStageMs(kStageBloomDown) +
                              st.timer->smoothedStageMs(kStageBloomUp);
        rows[rowCount++] = {"deform", st.timer->smoothedStageMs(kStageDeform)};
        rows[rowCount++] = {"flux", st.timer->smoothedStageMs(kStageFlux)};
        rows[rowCount++] = {"scene", st.timer->smoothedStageMs(kStageScene)};
        rows[rowCount++] = {"bloom", bloomMs};
        rows[rowCount++] = {"compst", st.timer->smoothedStageMs(kStageComposite)};
    }

    const float passRows = static_cast<float>(rowCount);
    // The panel keeps *status only*: header (3 lines) + pass table + sliders
    // (4 rows of 2 lines each) + a short state tail. The operating guide lives at the
    // bottom-left of the screen instead (see the help block below).
    //
    // The background is *not* sized from a formula: the content is emitted first and
    // the box is then fitted to what was actually drawn (both dimensions), so a new
    // readout can never spill outside the panel. If the content still does not fit the
    // window, build() retries at a smaller scale instead of clipping.
    const float panelW0 = panelW;
    m_panelWidth = panelW0 + pad;

    char buf[256];
    float y = panelTop + pad;
    auto newline = [&]() { y += lineH; };
    // Every panel line goes through this: it truncates to the panel width (so a long
    // readout can never spill outside the box, and never widens it either).
    const float textBudget = panelW - 2.0f * pad;
    const size_t maxChars = static_cast<size_t>(textBudget / (kFontWidth + 1.0f));
    auto line = [&](const char *s, const float *col) {
        std::string t(s);
        if (t.size() > maxChars) t = t.substr(0, maxChars > 2 ? maxChars - 2 : maxChars) + "..";
        text(x0, y, t, col[0], col[1], col[2], 1.0f);
    };
    const size_t bgStart = m_cpu.size();
    text(x0, y, "HELIOSTAT STUDIO", kGreen[0], kGreen[1], kGreen[2], scale);
    text(x0 + 200.0f, y, st.deviceName ? st.deviceName : "", kDim[0], kDim[1], kDim[2], 1.0f);
    newline();

    std::snprintf(buf, sizeof(buf), "%.1f FPS  cap %s  %.2f ms wall  %.3f ms GPU  %.0f Mray/s", st.fps,
                  st.fpsCapName ? st.fpsCapName : "off", st.wallMs,
                  st.timer ? st.timer->smoothedTotalMs() : 0.0f,
                  st.timer && st.timer->smoothedStageMs(kStageFlux) > 0.0f
                      ? static_cast<double>(st.raysPerFrame) / st.timer->smoothedStageMs(kStageFlux)
                      : 0.0);
    line(buf, kText);
    newline();

    std::snprintf(buf, sizeof(buf), "spp %u  cull %s  atomics %s  S95 %.2f m2  flux %.0f W  %s", st.spp,
                  st.rayCull ? "ON" : "OFF", st.diagAtomics ? "ON" : "OFF", st.s95Area, st.fluxSum,
                  st.presentMode ? st.presentMode : "");
    line(buf, kText);
    newline();
    y += 4.0f;

    // ---- per-pass ms with bars ----
    if (passRows > 0.0f) {
        float maxMs = 0.001f;
        for (int i = 0; i < rowCount; i++) maxMs = std::max(maxMs, rows[i].ms);
        const float barX = x0 + 52.0f;
        const float barW = std::max(60.0f, panelW - 52.0f - 24.0f - 80.0f);
        for (int i = 0; i < rowCount; i++) {
            text(x0, y, rows[i].name, kDim[0], kDim[1], kDim[2], 1.0f);
            const bool hot = rows[i].ms > 1.0f;
            bar(barX, y + 1.0f, barW, 8.0f, rows[i].ms / maxMs, hot ? kAmber[0] : kBlue[0],
                hot ? kAmber[1] : kBlue[1], hot ? kAmber[2] : kBlue[2]);
            std::snprintf(buf, sizeof(buf), "%6.3f", rows[i].ms);
            text(barX + barW + 8.0f, y, buf, kText[0], kText[1], kText[2], 1.0f);
            newline();
        }
        y += 6.0f;
    }

    // ---- sliders ----
    const float sw = panelW - 2.0f * pad - 96.0f;
    std::snprintf(buf, sizeof(buf), "%.1f deg", st.sunAzimuth);
    slider(1, x0, y, sw, "sun azimuth", (st.sunAzimuth + 60.0f) / 120.0f, buf);
    newline();
    newline();
    std::snprintf(buf, sizeof(buf), "%.1f deg", st.sunElevation);
    slider(2, x0, y, sw, "sun elevation", (st.sunElevation - 5.0f) / 80.0f, buf);
    newline();
    newline();
    // Beam target: the aim point on the receiver, in degrees around the cylinder.
    // The spot always lands on the aim point (that is what a tracking heliostat
    // does), so this is the control that walks the spot across the receiver.
    std::snprintf(buf, sizeof(buf), "%+.1f deg = %+.0f px", st.aimOffsetDeg,
                  st.aimOffsetDeg / 360.0f * 157.0f);
    slider(6, x0, y, sw, "beam target", (st.aimOffsetDeg + 60.0f) / 120.0f, buf);
    newline();
    newline();
    std::snprintf(buf, sizeof(buf), "t = %.2f", st.convergence);
    slider(3, x0, y, sw, "convergence t", st.convergence, buf);
    newline();
    newline();
    std::snprintf(buf, sizeof(buf), "%.2f", st.exposure);
    slider(4, x0, y, sw * 0.31f, "exposure", (st.exposure - 0.25f) / 2.75f, buf);
    std::snprintf(buf, sizeof(buf), "%.2f", st.bloomStrength);
    slider(5, x0 + sw * 0.345f, y, sw * 0.30f, "bloom", st.bloomStrength / 2.0f, buf);
    // Threshold is the control that decides *what* blooms: too low and the sky
    // glow / mirror highlights halo, too high and the spot stops glowing.
    std::snprintf(buf, sizeof(buf), "%.1f", st.bloomThreshold);
    slider(7, x0 + sw * 0.69f, y, sw * 0.31f, "bloom thr", (st.bloomThreshold - 0.5f) / 7.5f, buf);
    newline();

    // ---- state lines (short: the panel is a status column, not a log) ----
    y += 6.0f;
    if (tiny) {
        std::snprintf(buf, sizeof(buf), "%s  t=%.2f  spp %u  ESC quit", st.boltModeName ? st.boltModeName : "",
                      st.convergence, st.spp);
        line(buf, kDim);
    } else {
        // Names, not bare numbers: "view 2" told the user nothing about which of the
        // four plate views was on screen. Kept narrow enough to stay inside the panel.
        std::snprintf(buf, sizeof(buf), "mirror %s%s  bolts %s  t %.2f  deform %.0fx  ui x%.1f",
                      st.heliostatName ? st.heliostatName : "", st.heliostatTraced ? "*" : "",
                      st.boltModeName ? st.boltModeName : "", st.convergence, st.deformScale, st.uiScale);
        line(buf, kText);
        newline();
        std::snprintf(buf, sizeof(buf), "field %s  beams %s  cam %s",
                      st.fieldTraced ? "TRACING" : "OFF(8)", st.beamName ? st.beamName : "normal",
                      st.cameraName ? st.cameraName : "");
        line(buf, st.fieldTraced ? kDim : kAmber);
        newline();
        std::snprintf(buf, sizeof(buf), "view %s (F7)  heat %s %.0f", 
                      st.debugViewName ? st.debugViewName : "shaded", st.heatAutoRange ? "auto" : "FIXED",
                      st.fluxCeil);
        line(buf, kDim);
        newline();
        // Last key action (amber, ~3.5 s): the acknowledgement that a toggle did fire.
        // Only occupies a line while it is live.
        if (st.toast && *st.toast) {
            std::snprintf(buf, sizeof(buf), "> %s", st.toast);
            line(buf, kAmber);
            newline();
        }
        if (st.sunPathIndex > 0) {
            // Delingha day path, two compact lines: where the sun is, and when.
            std::snprintf(buf, sizeof(buf), "path %s  dec%+.1f  noon%.1f  day%.1fh  lat%.1fN",
                          st.sunPathLabel ? st.sunPathLabel : "day", st.sunPathDeclination,
                          st.sunPathNoonElevation, st.sunPathDayLengthHours, st.sunPathLatitude);
            line(buf, kAmber);
            newline();
            const int sh = static_cast<int>(st.sunPathSolarHours);
            const int sm = static_cast<int>((st.sunPathSolarHours - static_cast<float>(sh)) * 60.0f + 0.5f);
            const int bh2 = static_cast<int>(st.sunPathBeijingHours);
            const int bm = static_cast<int>((st.sunPathBeijingHours - static_cast<float>(bh2)) * 60.0f + 0.5f);
            std::snprintf(buf, sizeof(buf), "H%+.1f  LST %02d:%02d  BJT %02d:%02d  %s", st.sunPathHourDeg, sh, sm,
                          bh2, bm, st.sunPathAnimating ? "anim" : "hold");
            line(buf, kDim);
        }
    }

    // ---- fit the panel box to what was actually drawn ----
    // Height comes from the emitted content; the width is the fixed value above. The
    // background quads are prepended to the vertex list because the HUD blends in draw
    // order, so the box has to be drawn before the text it sits behind.
    {
        const float contentH = y + pad - panelTop;
        const float avail = hudH - 2.0f * pad;
        m_lastContentFits = (contentH <= avail) && (panelW + pad <= hudW - pad);
        const float panelH = std::min(contentH, avail);
        m_panelHeight = panelH + pad;
        m_panelWidth = panelW + pad;

        const std::vector<Vertex> content(m_cpu.begin() + static_cast<ptrdiff_t>(bgStart), m_cpu.end());
        m_cpu.resize(bgStart);
        rect(panelLeft, panelTop, panelW, panelH, kBg[0], kBg[1], kBg[2], kBg[3]);
        rect(panelLeft, panelTop, panelW, 2.0f, kBlue[0], kBlue[1], kBlue[2]);
        rect(panelLeft, panelTop + panelH - 1.0f, panelW, 1.0f, kPanelLine[0], kPanelLine[1],
             kPanelLine[2]);
        m_cpu.insert(m_cpu.end(), content.begin(), content.end());
    }

    // ---- operating guide, bottom-left of the screen (outside the panel) ----
    // Hidden while a demo caption is on screen: they would overlap at the bottom.
    if (!tiny && !(st.caption && *st.caption)) {
        const char *help[5] = {
            "LMB orbit   RMB/MMB pan   wheel zoom   WASD/QE fly   Shift fast   drag the panel sliders",
            "1 cull A/B   2 spp   3 atomics A/B   4/5/6/7 select + FLY TO mirror N/E/S/W   8 field tracing on/off",
            "F flux map   C compass + N/E/S/W markers   H panel   M heat range   K beam centre   L day-path anim",
            "F1 field   F2 fly to the selected mirror   F3 receiver   F4 beam side   F5 bolts   F6 deform",
            "F7 plate view (shaded/normals/slope/height)   F8 beams   F9 exposure   F10 present   F11 fps   U UI scale",
        };
        float helpW = 0.0f;
        for (const char *s : help) helpW = std::max(helpW, textWidth(s, 1.0f) / m_scale);
        const float bh = lineH * 5.0f + 6.0f;
        const float by = hudH - bh - 6.0f;
        rect(6.0f, by, helpW + 18.0f, bh, 0.02f, 0.03f, 0.05f, 0.60f);
        rect(6.0f, by, 3.0f, bh, kBlue[0], kBlue[1], kBlue[2]);
        for (int i = 0; i < 5; i++)
            text(13.0f, by + 3.0f + lineH * static_cast<float>(i), help[i], kDim[0], kDim[1], kDim[2], 1.0f);
    }

    // ---- compass, top-right ----
    if (!tiny) {
        const float cx = hudW - 78.0f;
        const float cy = 88.0f;
        compass(cx, cy, 52.0f, st);
    }

    // ---- caption under the flux inset ----
    if (st.insetW > 1.0f && st.insetH > 1.0f) {
        const float ix = st.insetX / m_scale;
        const float iy = st.insetY / m_scale;
        const float iw = st.insetW / m_scale;
        const float ih = st.insetH / m_scale;
        char cap[192];
        std::snprintf(cap, sizeof(cap),
                      "flux map 157x50 (angle x height, spot centred)   S95 %.1f m2   peak %.0f W/px", st.s95Area,
                      st.fluxPeak);
        // Backing strip + drop shadow: this text sits on the scene, not on a panel.
        const float cw = textWidth(cap, 1.0f) / m_scale;
        rect(ix - 3.0f, iy + ih + 1.0f, cw + 6.0f, 11.0f, 0.02f, 0.03f, 0.05f, 0.66f);
        textShadow(ix, iy + ih + 3.0f, cap, kText[0], kText[1], kText[2], 1.0f);
    }

    // Demo caption banner: centred near the bottom, over a dark strip. These are
    // HUD units and rect()/text() scale them, so the frame size must be divided by
    // the scale first -- otherwise a scaled-up UI pushes the banner off-screen.
    if (st.caption && *st.caption) {
        const float scale2 = 1.6f;
        const float w = textWidth(st.caption, scale2) / m_scale;
        const float bx = (hudW - w) * 0.5f;
        const float by = hudH - 34.0f;
        rect(bx - 14.0f, by - 8.0f, w + 28.0f, 30.0f, 0.02f, 0.03f, 0.05f, 0.72f);
        rect(bx - 14.0f, by - 8.0f, 3.0f, 30.0f, kGreen[0], kGreen[1], kGreen[2]);
        text(bx, by, st.caption, kText[0], kText[1], kText[2], scale2);
    }

    m_vertexCount = static_cast<uint32_t>(m_cpu.size());

    // Optional layout dump (see setDumpLayout): emitted only when the geometry
    // actually changes, so it never pollutes a frame loop.
    if (m_dumpLayout) {
        float hash = m_extent.width * 7.0f + m_extent.height * 13.0f + m_scale * 1000.0f;
        for (const Slider &s : m_sliders) hash += s.x + s.y * 2.0f + s.w * 3.0f;
        if (hash != m_layoutHash) {
            m_layoutHash = hash;
            std::printf("[hud] panel x=12 y=12 w=%.0f h=%.0f  (hud-units, scale %.2f -> screen x%.2f)\n",
                        m_panelWidth, m_panelHeight, m_scale, m_scale);
            for (const Slider &s : m_sliders)
                std::printf("[hud] slider %d \"%s\" x=%.0f y=%.0f w=%.0f h=%.0f value=%.3f\n", s.id, s.label,
                            s.x, s.y, s.w, s.h, s.value);
            std::fflush(stdout);
        }
    }
}

// ---------------------------------------------------------------- mouse ----
int Hud::handleMouse(float mouseX, float mouseY, bool pressed, bool held, float *value) {
    mouseX /= m_scale;
    mouseY /= m_scale;
    m_mouseOverPanel = (mouseX >= 12.0f && mouseX <= 12.0f + m_panelWidth && mouseY >= 12.0f &&
                        mouseY <= 12.0f + m_panelHeight);
    if (pressed) {
        m_activeSlider = -1;
        for (const Slider &s : m_sliders) {
            if (mouseX >= s.x - 6.0f && mouseX <= s.x + s.w + 6.0f && mouseY >= s.y && mouseY <= s.y + s.h) {
                m_activeSlider = s.id;
                break;
            }
        }
        if (m_dumpLayout) {
            std::printf("[hud] press at (%.0f, %.0f) -> slider %d  %s\n", mouseX, mouseY, m_activeSlider,
                        m_activeSlider < 0 ? "(no hit)" : "");
            std::fflush(stdout);
        }
    }
    if (!held) {
        m_activeSlider = -1;
        return -1;
    }
    if (m_activeSlider < 0) return -1;
    for (const Slider &s : m_sliders) {
        if (s.id != m_activeSlider) continue;
        const float v = clampf((mouseX - s.x) / std::max(s.w, 1.0f), 0.0f, 1.0f);
        if (value) *value = v;
        if (m_dumpLayout) {
            std::printf("[hud] drag slider %d to %.3f (mouse %.0f)\n", s.id, v, mouseX);
            std::fflush(stdout);
        }
        return m_activeSlider;
    }
    return -1;
}

// --------------------------------------------------------------- render ----
void Hud::render(VkCommandBuffer cmd, VkImageView target, VkExtent2D extent, bool *outHasLoad) {
    if (m_vertexCount == 0 || !m_vbo.mapped) return;
    std::memcpy(m_vbo.mapped, m_cpu.data(), sizeof(Vertex) * m_vertexCount);

    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height),
                              0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    VkRenderingAttachmentInfo color =
        colorAttachment(target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false, nullptr);
    if (outHasLoad) *outHasLoad = true;
    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea = scissor;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &ri);
    const float vp[4] = {static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 0.0f};
    vkCmdPushConstants(cmd, m_pipeLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(vp), vp);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeLayout, 0, 1, &m_set, 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m_vbo.buffer, &offset);
    vkCmdDraw(cmd, m_vertexCount, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

} // namespace viz
