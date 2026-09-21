#pragma once

// engine.h — the forward (inference) engine.
//
// One sun direction, one heliostat plate:
//
//   clearFlux -> computeBoltSurface -> renderForward -> finalizeFlux -> computeS95FindLevel
//
// This is the same dispatch chain as the upstream optimizer's per-sun forward
// path, minus the backward/Adam stages. It is the piece the real-time viewer
// drives every frame (see PLAN.md §8).

#include "config.h"
#include "data.h"
#include "vk.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace hviz {

// Descriptor binding numbers — identical to the upstream bolt descriptor set so
// that upstream SPIR-V can be reused verbatim.
namespace binding {
constexpr uint32_t kReceiver = 0;
constexpr uint32_t kHeliostat = 1;
constexpr uint32_t kSun = 2;
constexpr uint32_t kHelioPos = 3;
constexpr uint32_t kAimPoint = 4;
constexpr uint32_t kDummy5 = 5;      // controlY (Bezier path, unused)
constexpr uint32_t kYGrid = 6;
constexpr uint32_t kNGrid = 7;
constexpr uint32_t kFluxImage = 8;
constexpr uint32_t kDummy9 = 9;
constexpr uint32_t kFluxPartial = 10;
constexpr uint32_t kDummy11 = 11;
constexpr uint32_t kDummyImage12 = 12;
constexpr uint32_t kDummy13 = 13;
constexpr uint32_t kDummy14 = 14;
constexpr uint32_t kDummy15 = 15;
constexpr uint32_t kDummy16 = 16;
constexpr uint32_t kBoltHeights = 17;
constexpr uint32_t kDummy18 = 18;
constexpr uint32_t kInfluencePhi = 19;
constexpr uint32_t kInfluencePhiU = 20;
constexpr uint32_t kInfluencePhiV = 21;
constexpr uint32_t kDummy22 = 22;
constexpr uint32_t kYuGrid = 23;
constexpr uint32_t kYvGrid = 24;
constexpr uint32_t kDummy25 = 25;
constexpr uint32_t kDummy26 = 26;
constexpr uint32_t kDummy27 = 27;
constexpr uint32_t kDummy29 = 29;
constexpr uint32_t kGravityMerged = 30;
constexpr uint32_t kDummy31 = 31;
constexpr uint32_t kSunBatchFlat = 51;
constexpr uint32_t kS95State = 52;
constexpr uint32_t kDummy53 = 53;
constexpr uint32_t kActivePixelList = 55;
} // namespace binding

struct FluxStats {
    float sum = 0.0f;
    float max = 0.0f;
    float s95LevelGpu = 0.0f;   // bisection run on the GPU (shaders/s95_gpu.slang)
    float s95LevelCpu = 0.0f;   // independent CPU reference (data.cpp)
    float s95AreaGpu = 0.0f;    // m^2, using the GPU level
    float s95AreaCpu = 0.0f;    // m^2, using the CPU level
    float energyRetention = 0.0f;
};

class ForwardEngine {
public:
    ForwardEngine(VkCore &vk, const Config &cfg);
    ~ForwardEngine();

    ForwardEngine(const ForwardEngine &) = delete;
    ForwardEngine &operator=(const ForwardEngine &) = delete;

    // Loads SPIR-V from shaderDir, creates the descriptor set, buffers,
    // textures, pipelines and the active-pixel list for 'heliostat'.
    void init(const std::string &shaderDir, const HeliostatConfig &heliostat);

    // Bolt stroke heights (shader convention, one per bolt). Size must equal
    // cfg.numBolts. Uploaded immediately (host-visible buffer).
    void setBolts(const std::vector<float> &heights);

    // Packs every UBO for this sun direction / heliostat / aim point.
    void setSun(const std::array<float, 3> &sunDir, const HeliostatConfig &heliostat);

    // Viewer bridge: the sparse active-receiver-pixel list is built once for the
    // heliostat passed to init(). Moving to another heliostat of the field (a
    // mirror that faces the receiver from a different direction) needs the list
    // rebuilt, otherwise every traced ray starts on the wrong half of the cylinder
    // and the flux comes out as zero.
    void setActiveHeliostat(const HeliostatConfig &heliostat);

    // Runs the whole forward chain (single submit). Returns GPU ms when outGpuMs
    // is provided (timestamp query).
    void render(float *outGpuMs = nullptr);

    std::vector<float> readFlux() const;
    std::array<float, 4> readS95State() const;
    FluxStats computeStats(const std::vector<float> &flux) const;

    uint32_t totalPixels() const { return m_totalPixels; }
    uint32_t totalSpp() const { return m_totalSpp; }
    uint32_t activePixelCount() const { return m_activePixelCount; }
    float pixelArea() const { return m_pixelArea; }
    const std::array<float, 3> &macroNormal() const { return m_macroNormal; }

    // Surface grid readback: y (height) and world-space normal, for the viewer's
    // vertex buffers. Layout: [gridV * gridSize + gridU].
    std::vector<float> readSurfaceY() const;
    std::vector<float> readSurfaceNormalsLocal() const;

    // ---- Viewer bridge ---------------------------------------------------
    // Additive accessors used by the real-time front-end (viz/). They expose
    // existing objects; they do not change the dispatch chain, push constants,
    // binding numbers or the UBO bit layout, so parity is unaffected.
    //
    // The viewer binds the returned descriptor set to its own *additional*
    // compute pipelines (compiled from the same SPIR-V, same binding table) and
    // samples the flux texture from its graphics pass.
    VkDescriptorSetLayout setLayout() const { return m_setLayout; }
    VkDescriptorSet descriptorSet() const { return m_set; }
    const Pipeline &deformPipeline() const { return m_pipeDeform; }
    const Pipeline &s95Pipeline() const { return m_pipeS95Level; }
    // The reference ray tracer (shaders/forward.slang, 1024 spp fixed) — used by
    // the viewer's A/B parity mode to compare the real-time kernel against the
    // verified one inside the same process, UBOs and descriptor set.
    const Pipeline &clearPipeline() const { return m_pipeClear; }
    const Pipeline &forwardPipeline() const { return m_pipeForward; }
    const Pipeline &finalizePipeline() const { return m_pipeFinalize; }
    const Texture &fluxTexture() const { return m_flux; }
    const Buffer &yGridBuffer() const { return m_yGrid; }
    const Buffer &nGridBuffer() const { return m_nGrid; }
    const Buffer &boltHeightBuffer() const { return m_boltHeights; }
    const Buffer &s95StateBuffer() const { return m_s95State; }
    // Binding 31 (upstream diagnostic counters). Host-visible: index 5 counts the
    // rays the forward kernel processed, which is how the viewer's "per-ray
    // atomics" A/B switch is verified rather than merely timed.
    const Buffer &diagnosticBuffer() const { return m_dummyBuf; }
    const Config &config() const { return m_cfg; }
    uint32_t gridPoints() const { return m_gridPts; }
    // Tiles the reused clearFlux/finalizeFlux were sized for (gridSize^2 / 256).
    uint32_t tileCapacity() const { return m_tileCount; }

    // The aim point currently in the UBO (binding 4) — the viewer draws the
    // reflected light beam to exactly this point.
    const std::array<float, 3> &aimPoint() const { return m_aimPoint; }

    // Moves the aim point along the receiver circumference by 'degrees' (0 = the
    // point facing the heliostat, which is what setSun() packs). Repacks exactly
    // what setSun() packs for the aim point: the aim UBO, the gravity bin
    // interpolation and the macro normal. At 0 degrees the resulting buffers are
    // bit-identical to setSun()'s, so parity is unaffected.
    void setAimOffsetDeg(float degrees);
    float aimOffsetDeg() const { return m_aimOffsetDeg; }

    // A/B switch A1: rewrites SunParams.cullCosCutoff in place (the same value
    // setSun() packs when cfg.rayCull is set). Re-send the sun to restore.
    void setRayCull(bool enabled, float marginMrad);

private:
    void createDescriptorLayout();
    void createBuffers();
    void createPipelines(const std::string &shaderDir);
    void updateDescriptorSet();
    void buildActivePixelList(const std::array<float, 3> &heliostatPos);
    // Packs the aim point UBO + gravity bins + macro normal from the last sun.
    void updateAim();

    VkCore &m_vk;
    Config m_cfg;

    uint32_t m_gridPts = 0;
    uint32_t m_totalPixels = 0;
    uint32_t m_totalSpp = 0;
    uint32_t m_tileCount = 0;
    uint32_t m_activePixelCount = 0;
    float m_pixelArea = 0.0f;

    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;

    // Uniform buffers (host-visible, persistent mapping).
    Buffer m_uboReceiver, m_uboHeliostat, m_uboSun, m_uboHelioPos, m_uboAimPoint;
    Buffer m_sunBatchFlat;   // 6 suns * 8 floats, only slot 0 used here
    Buffer m_boltHeights;

    // Device-local model data.
    Buffer m_yGrid, m_nGrid, m_yuGrid, m_yvGrid;
    Buffer m_influencePhi, m_influencePhiU, m_influencePhiV;
    Buffer m_gravityMerged;
    Buffer m_fluxPartial;
    Buffer m_activePixelList;
    Buffer m_rayValidityScratch;   // binding 29: upstream per-ray validity bitmap
    Buffer m_s95State;
    Buffer m_dummyBuf;       // bound to every unused storage-buffer slot

    Texture m_flux;          // 157 x 50 R32F
    Texture m_dummyImage;    // 4 x 4 R32F, occupies binding 12

    Pipeline m_pipeClear, m_pipeDeform, m_pipeForward, m_pipeFinalize, m_pipeS95Level;

    std::array<float, 3> m_macroNormal{0.0f, 1.0f, 0.0f};
    std::array<float, 3> m_lastSunDir{0.0f, 0.0f, 1.0f};
    std::array<float, 3> m_aimPoint{0.0f, 180.0f, 10.0f};
    std::array<float, 3> m_activeHelioPos{};
    HeliostatConfig m_lastHeliostat;
    float m_aimOffsetDeg = 0.0f;
    bool m_activeHelioValid = false;
    bool m_initialized = false;
};

} // namespace hviz
