// flux_lite.slang — the viewer's ray-tracing kernel.
//
// Derived from shaders/forward.slang (which stays untouched: it is the parity
// reference). Differences, all deliberate:
//
//   1. spp and tileCount come from a push constant instead of being compile-time
//      constants, so the samples-per-pixel can be changed live (16..1024).
//   2. The per-ray diagnostic global atomic (`InterlockedAdd(diagBuf[5], 1u)` on a
//      single address, 4 M times per frame) can be switched off — this is the
//      A/B switch that reproduces the 5.2x finding from BASELINE.md.
//   3. clear/finalize use the runtime tile count (upstream's are fixed at 4).
//
// Everything else is character-for-character the same physics: the two-layer
// glass refraction with the TIR fallback, the slope-error perturbation, the Buie
// sun shape, the A1 angular pre-cull, the 1/(2*pi*shapeIntegral*spp) energy
// normalisation and the wave-based tile reduction.
//
// PARITY: at spp == kGridSize*kGridSize (1024) the stride below is 1, the ray
// hashes are identical to forward.slang, and this kernel reproduces the
// reference flux map bit-for-bit at the accumulation-order level.

import common;
import sunshape;

static const uint kFWGroupSize = 256u;
static const uint kFWWarpSize = 32u;
static const uint kFWWaves = kFWGroupSize / kFWWarpSize;

// The receiver size and the surface grid size are fixed by the config; they are
// pushed so the shader never hard-codes a scene.
struct FluxPC {
    uint spp;           // samples per pixel (surface grid points actually used)
    uint tileCount;     // ceil(spp / 256)
    uint totalPixels;   // receiver pixel count (7850 for 157x50)
    uint cols;          // 2D subsample: number of grid columns sampled
    uint rows;          // 2D subsample: number of grid rows sampled
    uint diagAtomics;   // A/B switch: 1 = keep the upstream per-ray global atomic
};
[[vk::push_constant]] FluxPC pushConst;

// Diagnostic counter (binding 31). Only touched when diagAtomics == 1.
[[vk::binding(31)]] RWStructuredBuffer<uint> diagBuf;

[[vk::binding(8)]] [format("r32f")]
RWTexture2D<float> renderedFlux;

// Per-tile partial sums, one plane of 'tileCount' floats per pixel.
[[vk::binding(10)]] RWStructuredBuffer<float> fluxPartial;

groupshared float g_fwSums[kFWWaves];

float reduceFWSum(uint lid, float val) {
    uint wid = lid / kFWWarpSize;
    val = WaveActiveSum(val);
    if (WaveIsFirstLane()) g_fwSums[wid] = val;
    GroupMemoryBarrierWithGroupSync();
    val = (lid < kFWWaves) ? g_fwSums[WaveGetLaneIndex()] : 0.0f;
    if (wid == 0u) val = WaveActiveSum(val);
    return val;
}

// ---- Clear ----
[shader("compute")]
[numthreads(16, 16, 1)]
void clearFluxLite(uint3 tid : SV_DispatchThreadID) {
    uint2 dims = getReceiverDims();
    if (tid.x < dims.x && tid.y < dims.y) {
        renderedFlux[tid.xy] = 0.0f;
        uint base = tid.y * dims.x + tid.x;
        for (uint t = 0u; t < pushConst.tileCount; t++) {
            fluxPartial[base * pushConst.tileCount + t] = 0.0f;
        }
    }
}

// ---- Finalize: sum the tile partials into the flux texture ----
[shader("compute")]
[numthreads(16, 16, 1)]
void finalizeFluxLite(uint3 tid : SV_DispatchThreadID) {
    uint2 dims = getReceiverDims();
    if (tid.x < dims.x && tid.y < dims.y) {
        uint base = tid.y * dims.x + tid.x;
        float sum = 0.0f;
        for (uint t = 0u; t < pushConst.tileCount; t++) {
            sum += fluxPartial[base * pushConst.tileCount + t];
        }
        renderedFlux[tid.xy] = sum;
    }
}

// ---- Forward render (receiver pixel -> plate -> sun) ----
void renderFluxTyped<let SUNSHAPE_TYPE : uint>(uint3 gid, uint3 gtid) {
    uint lid = gtid.x, tileIdx = gid.x, pixelIdx = activePixelList[gid.y];
    if (pixelIdx >= pushConst.totalPixels) return;

    uint2 dims = getReceiverDims();
    uint2 pc = uint2(pixelIdx % dims.x, pixelIdx / dims.x);
    float3 origin, normP;
    getReceiverSampleData(pc, origin, normP);

    // P4: cylinder half-face culling (uniform across the workgroup: the whole
    // group shares gid.y, so every barrier below stays convergent).
    float2 cullDir = getHeliostatCullDir();
    if (normP.x * cullDir.x + normP.z * cullDir.y <= 0.0f) return;

    float3 macroN = computeHeliostatNormal(getSunDirection(), heliostatPosition, aimPoint);
    OrthonormalBasis macroBasis = computeBasis(macroN);

    float slopeErr = getHeliostatSlopeError();
    float refArea  = getHeliostatReflectiveArea();
    float dni      = getSunDni();
    float4 shapeP  = getSunShapeParams();
    float shapeInt = getSunShapeIntegral();
    float3 sunDir  = getSunDirection();

    uint wi = tileIdx * kFWGroupSize + lid;
    float energy = 0.0f;

    if (wi < pushConst.spp) {
        uint sp = wi;
        // 2D subsample of the surface grid. With (cols, rows) == (32, 32) this is
        // bit-identical to forward.slang's `xid = sp / 32, zid = sp % 32`, so the
        // full-quality pass keeps exact parity with the reference renderer; at
        // lower spp the samples stay spread over the whole plate instead of
        // collapsing onto a couple of grid rows.
        uint sx = kGridSize / pushConst.cols;   // stride in x
        uint sz = kGridSize / pushConst.rows;   // stride in z
        uint xid = (sp / pushConst.rows) * sx;
        uint zid = (sp % pushConst.rows) * sz;
        uint gridIdx = zid * kGridSize + xid;
        float2 hsize = getHeliostatSize();
        float hx = -hsize.x * 0.5f + float(xid) * (hsize.x / float(kGridSize - 1u));
        float hz = -hsize.y * 0.5f + float(zid) * (hsize.y / float(kGridSize - 1u));

        float3 localPos = float3(hx, yGrid[gridIdx], hz);
        float3 localNrm = nGrid[gridIdx].xyz;
        float3 worldPos = basisToWorldPosition(macroBasis, localPos, heliostatPosition);
        float3 surfNrm  = basisToWorldDirection(macroBasis, localNrm);

        float3 ray = worldPos - origin;
        float rayLen = max(length(ray), kEpsilon);
        float3 dir = ray / rayLen;

        float dc = dot(normP, float3(dir.x, 0.0f, dir.z));
        bool rayAlive = dc > 0.0f;

        // A1 per-ray angular pre-cull (cullCosCutoff < -1.5 disables it).
        float cullCos = getSunCullCos();
        if (rayAlive && cullCos > -1.5f) {
            float3 outApprox = reflect(dir, surfNrm);
            rayAlive = dot(outApprox, normalize(sunDir)) >= cullCos;
        }

        uint rayIndex = sp * pushConst.totalPixels + pixelIdx;

        if (rayAlive) {
            float g0, g1, g2, g3;
            generateGaussianSamples(rayIndex, pixelIdx, sp, g0, g1, g2, g3);

            // Two-layer glass optics (slope error on both surfaces + refraction).
            OrthonormalBasis sBasis = computeBasis(surfNrm);
            float3 nor1 = basisToWorldDirection(sBasis, normalize(float3(g0 * slopeErr, 1.0f, g1 * slopeErr)));
            float3 nor2 = basisToWorldDirection(sBasis, normalize(float3(g2 * slopeErr, 1.0f, g3 * slopeErr)));
            float etaE = 1.0f / getHeliostatRefractiveIndex();
            float etaX = getHeliostatRefractiveIndex();
            float ndi = dot(nor1, dir);
            float kEnter = 1.0f - etaE * etaE * (1.0f - ndi * ndi);
            float3 d2 = (kEnter > 0.0f) ? normalize(etaE * dir - (etaE * ndi + sqrt(kEnter)) * nor1)
                                        : float3(0.0f, 0.0f, 0.0f);
            float3 outRG = float3(0.0f, 0.0f, 0.0f);
            if (length(d2) > kEpsilon) {
                float3 d3 = normalize(reflect(d2, nor2));
                float ndx = dot(d3, -nor1);
                float kExit = 1.0f - etaX * etaX * (1.0f - ndx * ndx);
                if (kExit > 0.0f) {
                    outRG = normalize(etaX * d3 - (etaX * ndx + sqrt(kExit)) * (-nor1));
                }
            }
            if (length(outRG) <= kEpsilon) {
                outRG = normalize(reflect(-dir, nor1));
                if (pushConst.diagAtomics != 0u) InterlockedAdd(diagBuf[0], 1u);   // TIR fallback
            }

            float cos_incident = dot(nor1, -dir);
            float rat = max(0.0f, refArea * dot(normP, dir) * cos_incident
                           / (rayLen * rayLen) * computeDistanceAttenuation(rayLen));
            if (rat > 0.0f) {
                float ct = clamp(dot(outRG, normalize(sunDir)), -0.99999f, 0.99999f);
                float st = (abs(ct - 1.0f) < kEpsilon) ? 0.0f : acos(ct);
                float ssVal = computeSunshapeTyped<SUNSHAPE_TYPE>(st, shapeP);
                energy = dni * rat * ssVal / max(2.0f * kPi * shapeInt * float(pushConst.spp), kEpsilon);
            }
        }

        // A/B switch: reproduce the upstream diagnostic traffic exactly.
        //   1. `InterlockedAdd(diagBuf[5], 1u)` — one atomic per ray on a *single*
        //      address (the hardware can coalesce a warp's worth of these, which is
        //      why this one alone is nearly free).
        //   2. `InterlockedOr(rayValidity[rayIndex >> 5], bit)` — a *scattered*
        //      read-modify-write into a 1 MB bitmap, one per surviving ray. This is
        //      the traffic that made the upstream build 5.2x slower: the addresses
        //      differ per ray, so nothing can be coalesced.
        // Both are what shaders/forward.slang does unconditionally; here they are
        // behind the switch so the cost can be measured live.
        if (pushConst.diagAtomics != 0u) {
            InterlockedAdd(diagBuf[5], 1u);
            if (energy > 0.0f) {
                InterlockedOr(rayValidity[rayIndex >> 5u], 1u << (rayIndex & 31u));
            }
        }
    }

    float red = reduceFWSum(lid, energy);
    if (lid == 0u) fluxPartial[pixelIdx * pushConst.tileCount + tileIdx] = red;
}

[shader("compute")]
[numthreads(kFWGroupSize, 1, 1)]
void fluxLite(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    uint st = uint(getSunShapeType());
    if (st == kSunshapePillbox)        { renderFluxTyped<kSunshapePillbox>(gid, gtid); }
    else if (st == kSunshapeGaussian)  { renderFluxTyped<kSunshapeGaussian>(gid, gtid); }
    else                               { renderFluxTyped<kSunshapeBuie>(gid, gtid); }
}
