#pragma once

// hud.h — hand-written bitmap-font HUD (no ImGui, no stb).
//
// The panel is built on the CPU as a list of coloured quads: background, slider
// tracks/knobs, per-pass millisecond bars and glyph quads that sample a 5x7 font
// atlas. The layout rects double as hit regions, so the same numbers drive both
// drawing and mouse interaction (dragging the sun / convergence sliders).

#include "gpu_timer.h"
#include "vk.h"
#include "vk_gfx.h"

#include <cstdint>
#include <string>
#include <vector>

namespace viz {

// One draggable slider registered by the panel.
struct Slider {
    int id = 0;
    float x = 0, y = 0, w = 0, h = 0;   // pixels
    float value = 0.0f;                 // 0..1 normalised
    bool dragging = false;
    const char *label = "";
    std::string valueText;
};

struct HudStats {
    double fps = 0.0;
    double wallMs = 0.0;
    const GpuTimer *timer = nullptr;
    float raysPerFrame = 0.0f;
    uint32_t spp = 0;
    bool rayCull = true;
    bool diagAtomics = false;
    bool gravity = true;
    float s95Area = 0.0f;
    float fluxSum = 0.0f;
    float fluxPeak = 0.0f;
    float convergence = 0.0f;
    float deformScale = 1.0f;
    float exposure = 1.0f;
    float bloomStrength = 0.0f;
    float bloomThreshold = 2.5f;
    int debugView = 0;
    int boltMode = 0;
    const char *boltModeName = "";
    const char *cameraName = "";
    const char *presentMode = "";
    const char *deviceName = "";
    uint32_t triangles = 0;
    bool vsync = false;
    float sunAzimuth = 0.0f, sunElevation = 0.0f;
    float aimOffsetDeg = 0.0f;
    float fluxCeil = 700.0f;
    bool heatAutoRange = true;
    bool paused = false;
    const char *caption = nullptr;   // demo banner, drawn under the panel
    // --- compass / field / sun-path / inset readouts ---
    float cameraAzimuthDeg = 0.0f;   // camera forward, degrees from +Z (south)
    const char *heliostatName = "";  // selected field mirror (only it is traced)
    bool heliostatTraced = true;
    int sunPathIndex = 0;            // 0 = off, 1..3 = Delingha day paths (solstices/equinox)
    // Human-readable state (the panel used to print bare numbers for these).
    const char *debugViewName = "shaded";   // F7: shaded / normals / slope error / height
    const char *beamName = "normal";        // F8: off / normal / strong
    bool fieldTraced = true;                // '8': false = the compute chain is skipped
    float uiScale = 1.0f;                   // current HUD scale ('U' cycles it)
    const char *toast = nullptr;            // last key action, shown for ~3.5 s
    float sunPathLatitude = 0.0f;
    float sunPathHourDeg = 0.0f;
    bool sunPathAnimating = true;
    // Delingha day-path readouts: the site, the date's declination, where the sun is
    // in *true solar time* and the equivalent Beijing time, plus the day length.
    const char *sunPathLabel = nullptr;
    const char *sunPathDate = nullptr;
    float sunPathLongitude = 97.37f;
    float sunPathDeclination = 0.0f;
    float sunPathNoonElevation = 0.0f;
    float sunPathDayLengthHours = 0.0f;
    float sunPathSolarHours = 12.0f;
    float sunPathBeijingHours = 13.5f;
    bool showFluxMap = true;
    bool showCardinals = true;
    // Software frame-rate cap ("60" / "120" / "off"), shown in the header line.
    const char *fpsCapName = "60";
    // Flux inset rectangle in pixels (0 size = hidden), for the caption.
    float insetX = 0, insetY = 0, insetW = 0, insetH = 0;
};

class Hud {
public:
    bool init(hviz::VkCore &vk, VkFormat swapchainFormat, VkExtent2D extent, const std::string &shaderDir);
    void destroy();
    void resize(VkExtent2D extent);
    // Uniform UI scale: the layout is authored in "HUD units" and scaled at emit
    // time, so recordings downscaled for a GIF keep their text readable.
    void setScale(float scale) { m_scale = scale > 0.1f ? scale : 1.0f; }
    // The scale actually in use: build() may shrink it so the panel fits the window.
    float scale() const { return m_scale; }
    // Prints the panel/slider rectangles (HUD units, before m_scale) whenever the
    // layout changes. This is what tools/acceptance_interaction.ps1 drives, and it
    // doubles as documentation of the on-screen widget geometry.
    void setDumpLayout(bool on) { m_dumpLayout = on; }

    // Rebuilds the quad list for this frame. Returns the panel height.
    void build(const HudStats &stats, float dt);
    void render(VkCommandBuffer cmd, VkImageView target, VkExtent2D extent, bool *outHasLoad);

    // Mouse handling: dragging a slider returns its id and updates 'value'.
    // Returns -1 when nothing was hit.
    int handleMouse(float mouseX, float mouseY, bool pressed, bool held, float *value);
    bool wantsMouse() const { return m_mouseOverPanel; }

    float panelHeight() const { return m_panelHeight; }

private:
    struct Vertex {
        float x, y;
        float u, v;
        float r, g, b, a;
    };

    void rect(float x, float y, float w, float h, float r, float g, float b, float a = 1.0f);
    void compass(float cx, float cy, float radius, const HudStats &st);    void text(float x, float y, const std::string &s, float r, float g, float b, float scale = 1.0f);
    // Same, with a 1 px dark drop shadow (for labels drawn straight onto the scene).
    void textShadow(float x, float y, const std::string &s, float r, float g, float b, float scale = 1.0f);
    float textWidth(const std::string &s, float scale = 1.0f) const;
    void bar(float x, float y, float w, float h, float frac, float r, float g, float b);
    void slider(int id, float x, float y, float w, const char *label, float value, const std::string &valueText);
    void buildFontAtlas();
    // HUD-unit layout size (set by build(); used for mouse hit testing).
    float m_panelHeight = 0.0f;
    float m_panelWidth = 0.0f;

    hviz::VkCore *m_vk = nullptr;
    VkFormat m_swapFormat = VK_FORMAT_B8G8R8A8_SRGB;
    VkExtent2D m_extent{};
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_pipeLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    hviz::Texture m_atlas;
    hviz::Buffer m_vbo;
    VkDescriptorSet m_set = VK_NULL_HANDLE;
    uint32_t m_vertexCount = 0;
    uint32_t m_capacity = 0;
    std::vector<Vertex> m_cpu;

    // build() calls buildOnce() and, if the content does not fit the window,
    // again at a slightly smaller scale (so nothing is ever clipped).
    void buildOnce(const HudStats &st, float dt);
    bool m_lastContentFits = true;
    std::vector<Slider> m_sliders;
    bool m_mouseOverPanel = false;
    int m_activeSlider = -1;
    float m_atlasU0 = 0, m_atlasV0 = 0;
    float m_scale = 1.0f;
    bool m_dumpLayout = false;
    float m_layoutHash = -1.0f;
};

} // namespace viz
