// bolt_common.slang — Bolt influence function surface representation
// Replaces Bezier CP parameterization with linear bolt-height superposition.
// Imports common for shared UBOs (bindings 0-4) and coordinate transforms.
module bolt_common;
import common;

// Bolt count constants
// Raised 50 -> 128 for Phase 5.4 free-layout / dense-start (up to 99+ bolts).
public static const uint kMaxBolts  = 128u;

// ---- Bolt-mode buffer bindings (extend set 0 beyond Bezier's 0-16) ----
// Binding 17: bolt heights (read-only during forward/backward, RW during adam)
[[vk::binding(17)]] public uniform StructuredBuffer<float> boltHeights;
// Binding 18: bolt height gradient accumulator (RW)
[[vk::binding(18)]] public RWStructuredBuffer<float> boltHeightGradient;
// Bindings 19-21: precomputed influence function textures (RO)
// Layout: [boltIdx * gridSize^2 + row * gridSize + col]
[[vk::binding(19)]] public uniform StructuredBuffer<float> influencePhi;
[[vk::binding(20)]] public uniform StructuredBuffer<float> influencePhiU;
[[vk::binding(21)]] public uniform StructuredBuffer<float> influencePhiV;
// Binding 22: gravity sag precomputation (RO, legacy - kept for compat)
[[vk::binding(22)]] public uniform StructuredBuffer<float> gravityY;
// Binding 30: FEA gravity merged buffer (RO)
// Layout: 20 bins × 3 planes (w, dw/du, dw/dv) × 1024 grid points = 61440 floats
// Plane offset: w at offset 0, dw/du at +1024, dw/dv at +2048 within each bin
// Bin offset: binIdx * 3072
// Re-generate with: python scripts/generate_proxy_model.py gravity
[[vk::binding(30)]] public uniform StructuredBuffer<float> gravityMerged;
// Bindings 23-24: surface derivative grids (RW, populated by computeBoltSurface)
[[vk::binding(23)]] public RWStructuredBuffer<float> yuGrid;
[[vk::binding(24)]] public RWStructuredBuffer<float> yvGrid;
// Binding 25: per-grid-point surface gradient accumulator (RW, 3 * gridSize^2)
// Phase 5: uint-typed for atomic float add via InterlockedCompareExchange (matching s95_histogram.slang)
[[vk::binding(25)]] public RWStructuredBuffer<uint> surfaceGradient;

// Bolt count from host push constant (set at dispatch time)
// Access via a global that gets set by push constant in each kernel

// ---- Influence function evaluation at a grid point ----
public float evaluateInfluence(uint boltIdx, uint gridU, uint gridV) {
    uint idx = boltIdx * kGridSize * kGridSize + gridV * kGridSize + gridU;
    return influencePhi[idx];
}

public float evaluateInfluenceU(uint boltIdx, uint gridU, uint gridV) {
    uint idx = boltIdx * kGridSize * kGridSize + gridV * kGridSize + gridU;
    return influencePhiU[idx];
}

public float evaluateInfluenceV(uint boltIdx, uint gridU, uint gridV) {
    uint idx = boltIdx * kGridSize * kGridSize + gridV * kGridSize + gridU;
    return influencePhiV[idx];
}

// ---- Multi-angle gravity sampling (20 bins, 10-80 deg) ----
// Known gravity bin angles in degrees, sorted ascending.
// Must match the C++ side in pipeline.cpp and generate_proxy_model.py.
static const uint kNumGravityBins = 20;
static const float kGravityAngles[20] = {10.0, 14.0, 18.0, 22.0, 26.0, 30.0, 34.0, 38.0, 42.0, 46.0,
                                         50.0, 54.0, 58.0, 62.0, 66.0, 70.0, 73.0, 76.0, 78.0, 80.0};

// Plane strides within the merged gravity buffer.
// Each bin = 3 planes × kGridSize² = 3072 floats.
static const uint kGravityBinStride = 3u * kGridSize * kGridSize;   // 3072
static const uint kGravityPlaneStride = kGridSize * kGridSize;       // 1024

// ---- Legacy gravity sampling (single-plane w-only, for coupling=0 backward compat) ----
float _readGravityLegacy(uint binIdx, uint gridIdx) {
    return gravityMerged[binIdx * kGravityBinStride + gridIdx];
}

public float sampleGravityUY(uint gridIdx, uint lo, uint hi, float t) {
    float gLo = _readGravityLegacy(lo, gridIdx);
    if (lo == hi) return gLo;
    float gHi = _readGravityLegacy(hi, gridIdx);
    return lerp(gLo, gHi, t);
}

// ---- New 3-plane gravity sampling (w, dw/du, dw/dv) ----
float3 _readGravity3(uint binIdx, uint gridIdx) {
    uint base = binIdx * kGravityBinStride + gridIdx;
    return float3(gravityMerged[base],
                  gravityMerged[base + kGravityPlaneStride],
                  gravityMerged[base + 2u * kGravityPlaneStride]);
}

// Samples the precomputed FEA gravity field (3-plane) by linear interpolation
// between the two nearest gravity angle bins.
// When gravityNormalCoupling==0, returns (w, 0, 0) — legacy phantom-gravity behavior.
public float3 sampleGravityField(uint gridIdx, uint lo, uint hi, float t, uint gravityNormalCoupling) {
    float3 gLo = _readGravity3(lo, gridIdx);
    if (lo == hi) {
        return float3(gLo.x,
                      gravityNormalCoupling != 0u ? gLo.y : 0.0f,
                      gravityNormalCoupling != 0u ? gLo.z : 0.0f);
    }
    float3 gHi = _readGravity3(hi, gridIdx);
    float3 g = lerp(gLo, gHi, t);
    return float3(g.x,
                  gravityNormalCoupling != 0u ? g.y : 0.0f,
                  gravityNormalCoupling != 0u ? g.z : 0.0f);
}

// ---- Bolt surface evaluation (replaces bezierSurface + bezierSurfaceNormal) ----
// Computes y, yu, yv, and normal at a single (u,v) grid point.
// w_total(u,v) = UY_grav^FEA(u,v,theta) + sum_b boltHeights[b] * phi_b(u,v)
// Gravity contributes to both height (w) and surface slopes when
// gravityNormalCoupling is enabled (default); =0 restores legacy phantom behavior.
// UNIT NOTE: gravity bins store PHYSICAL slopes (dw/dx, dw/dz in m/m), while
// yu/yv use the shader convention d/du = dw/dx * W and d/dv = dw/dz * L
// (same convention as influence_phi_u/v, see CLAUDE.md §2.1) — hence the hs scale.
public void boltSurfaceAtGrid(
    uint gridU, uint gridV, uint numBolts, uint gravityLo, uint gravityHi, float gravityT,
    uint disableGravity, uint gravityNormalCoupling,
    out float y, out float yu, out float yv, out float3 nrm)
{
    uint gridIdx = gridV * kGridSize + gridU;
    float2 hs = getHeliostatSize();

    if (disableGravity != 0u) {
        y  = 0.0f;
        yu = 0.0f;
        yv = 0.0f;
    } else {
        float3 gField = sampleGravityField(gridIdx, gravityLo, gravityHi, gravityT, gravityNormalCoupling);
        y  = gField.x;
        yu = gField.y * hs.x;   // dw/dx -> dw/du
        yv = gField.z * hs.y;   // dw/dz -> dw/dv
    }

    for (uint b = 0u; b < numBolts; b++) {
        float phi   = evaluateInfluence(b, gridU, gridV);
        float phi_u = evaluateInfluenceU(b, gridU, gridV);
        float phi_v = evaluateInfluenceV(b, gridU, gridV);
        float h = boltHeights[b];
        y  += h * phi;
        yu += h * phi_u;
        yv += h * phi_v;
    }

    // Normal from surface derivatives (matches bezierSurfaceNormal convention)
    float3 tu = float3(hs.x, yu, 0.0f);
    float3 tv = float3(0.0f, yv, hs.y);
    nrm = -normalize(cross(tu, tv));
}
