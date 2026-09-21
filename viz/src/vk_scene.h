#pragma once

// vk_scene.h — the scene pass: sky, ground, tower, receiver (emissive flux heat
// map), plate (vertex-pulled from the compute-written yGrid/nGrid buffers),
// bolts and the volumetric-ish light beams.
//
// Renders into an HDR (RGBA16F) target with a reversed-Z D32 depth buffer; the
// post stack then tonemaps it into the swapchain image.

#include "camera.h"
#include "engine.h"
#include "vk.h"
#include "vk_gfx.h"

#include <string>
#include <array>

namespace viz {

class SceneRenderer {
public:
    // Everything the scene pass needs per frame. Filled by viz_main from the
    // camera, the engine's packed uniforms and the UI state.
    struct FrameInputs {
        Vec3 sunDir{0, 1, 0};
        Vec3 helioPos{0, 0, -300};
        Vec3 receiverPos{0, 180, 0};
        std::array<float, 3> aimPoint{0.0f, 180.0f, 10.0f};   // physics aim point (beam end)
        float receiverRadius = 10.0f;
        // The heliostat field: every mirror is drawn, only 'selected' is traced.
        struct Mirror {
            Vec3 pos{0, 0, -300};
            Vec3 macroN{0, 1, 0};
            Vec3 macroU{1, 0, 0};
            Vec3 macroV{0, 0, 1};
            bool selected = false;
        };
        std::vector<Mirror> field;
        bool showCardinals = true;          // NSWE markers painted on the field
        bool showFluxMap = false;           // bottom-right flux inset
        float s95Level = 0.0f;              // flux contour on the inset
        float fluxPeak = 0.0f;
        // Flux-weighted centroid of the map (0..1 texel space): the inset rolls the
        // periodic u axis to it, so the spot is always centred whichever mirror runs.
        float fluxCenterU = 0.5f;
        float fluxCenterV = 0.5f;
        float receiverHeight = 20.0f;
        float plateWidth = 12.84f;
        float plateLength = 9.45f;
        float gridSize = 32.0f;
        float deformScale = 1.0f;       // F6: deformation exaggeration
        float fluxScale = 1.0f;         // heat-map multiplier
        float fluxLogFloor = 1.0f;
        float fluxLogCeil = 700.0f;
        float exposure = 1.0f;
        float debugView = 0.0f;         // 1 = normals, 2 = wireframe-ish, 3 = slope error
        float showBeams = 1.0f;
        float heatMode = 0.0f;          // 0 = heat LUT, 1 = grayscale, 2 = raw log
        float time = 0.0f;
        float beamIntensity = 1.0f;
        float boltRadiusMm = 12.0f;
        // Actuator marker size in metres (mode 4). Raise it to inspect the markers.
        float boltRadius = 0.22f;
    };

    void init(hviz::VkCore &vk, hviz::ForwardEngine &engine, VkExtent2D extent, const std::string &shaderDir,
              VkFormat swapchainFormat);
    void resize(VkExtent2D extent);
    void destroy();

    void render(VkCommandBuffer cmd, uint32_t frameIndex, const Camera &camera, const FrameInputs &in);

    const hviz::Texture &hdrTexture(uint32_t frame = 0) const { return m_hdr[frame & 1u]; }
    VkImageView depthView() const { return m_depth.view; }
    VkExtent2D extent() const { return m_extent; }
    // Triangle counts of each generated object (for the HUD's geometry readout).
    uint32_t plateTriangles() const;
    uint32_t receiverTriangles() const;
    uint32_t boltTriangles() const { return 35u * 8u; }

    // Records the composite-overlay pieces that must run *after* the post stack:
    // the flux-map inset (bloom must not pick it up).
    void renderOverlays(VkCommandBuffer cmd, uint32_t frameIndex, VkImageView target, VkExtent2D extent);

    // Where the flux-map inset goes (bottom-right), shared with the HUD so the
    // caption can be placed without duplicating the formula.
    static VkRect2D fluxInsetRect(VkExtent2D extent);

private:
    void createTargets(VkExtent2D extent);
    void destroyTargets();
    void createPipelines(const std::string &shaderDir);
    void createDescriptors();
    void writeDescriptors();
    void writeUbo(uint32_t frameIndex, const Camera &camera, const FrameInputs &in);
    // One index-driven draw: 'mode' selects the object the VS expands.
    void drawObject(VkCommandBuffer cmd, uint32_t mode, uint32_t vertexCount, uint32_t res, uint32_t extra,
                    float scale);

    hviz::VkCore *m_vk = nullptr;
    hviz::ForwardEngine *m_engine = nullptr;

    VkExtent2D m_extent{};
    // One HDR target per frame in flight: frame N+1 must not overwrite the target
    // frame N's bloom/composite still reads.
    hviz::Texture m_hdr[2];
    hviz::Texture m_depth;
    VkSampler m_fluxSampler = VK_NULL_HANDLE;

    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorSet m_sets[2] = {};
    VkPipelineLayout m_pipeLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeSky = VK_NULL_HANDLE;
    VkPipeline m_pipeScene = VK_NULL_HANDLE;
    VkPipeline m_pipeBeam = VK_NULL_HANDLE;
    VkPipeline m_pipeFluxMap = VK_NULL_HANDLE;   // screen-space inset, no depth

    hviz::Buffer m_ubo;             // 2 slices, persistently mapped
    VkFormat m_swapFormat = VK_FORMAT_B8G8R8A8_SRGB;
    uint32_t m_gridSize = 32;
    float m_beamWidth = 1.2f;
    bool m_fluxMapEnabled = false;
};

} // namespace viz
