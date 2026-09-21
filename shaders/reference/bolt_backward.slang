// bolt_backward.slang — Two-stage backward pass for bolt influence surface
//
// Stage 1 (optical): renderBackwardBolt
//   - Each sample computes dL/dy, dL/dyu, dL/dyv via bwd_diff
//   - Per-grid-point accumulation via groupshared (each gridIdx is unique per group)
//   - Writes to gradPartial for cross-group reduction
//
// Stage 1b: reduceSurfaceGradients
//   - Sums gradPartial across all groups → surfaceGradient[gridIdx * 3 + ch]
//
// Stage 2 (mechanical): projectBoltGradients
//   - dL/dh_b = sum_p [dL/dy_p * phi_b(p) + dL/dyu_p * dphi_b/du(p) + dL/dyv_p * dphi_b/dv(p)]
//   - Writes boltHeightGradient[b] += result (accumulates across sun directions)
//
// Glass optics body copied VERBATIM from backward.slang:104-142.

module bolt_backward;
import common;
import bolt_common;
import sunshape;

static const uint kBWGroupSize    = 256u;
static const uint totalSpp        = kGridSize * kGridSize;
static const uint totalPixels     = 7850u;
static const uint kSurfaceChannels = 3u;
static const uint totalGridPoints = kGridSize * kGridSize;

// Shared bindings
[[vk::binding(12)]] [format("r32f")] RWTexture2D<float> fluxGradient;
// binding 9 removed — Phase 1: inline Box-Muller replaces gaussianPool
[[vk::binding(15)]] RWStructuredBuffer<float> debugBuf;

// Phase 5: fixed-point per-grid-point accumulator (replaces 386 MB gradPartial)
// Size: totalGridPoints * 3 ints = ~12 KB. Scale: 1e4 (handles up to ±2.1e5 per grid point)
// Using uint (not int) for InterlockedAdd compatibility — negative values handled via two's complement
static const float kGradFixedScale = 100000.0f;  // Phase 5: 1e5 fixed-point scale
[[vk::binding(11)]] RWStructuredBuffer<uint> gradPartialTile;  // Phase 5: reuses binding 11

// Push constant
struct BoltBackwardPC { uint numBolts; float _pad[3]; };
[[vk::push_constant]] BoltBackwardPC pushConst;

// ---- computePixelEnergyBolt ----
// Identical to backward.slang:computePixelEnergy except:
// - Takes (y, yu, yv) as 3 differentiable params instead of 16 CPs
// - Glass optics body is copied VERBATIM from backward.slang:104-142
[Differentiable]
float computePixelEnergyBolt(
    float y, float yu, float yv,
    no_diff float u, no_diff float v,
    no_diff float3 origin, no_diff float3 normP,
    no_diff float3 perturb1, no_diff float3 perturb2,
    no_diff float hx, no_diff float hz,
    no_diff float3 macroN, no_diff OrthonormalBasis macroBasis,
    no_diff float3 sunDir, no_diff float dni, no_diff float refIdx,
    no_diff float4 shapeParams, no_diff float shapeInt, no_diff float shapeType,
    no_diff float refArea)
{
    float3 posLocal = float3(hx, y, hz);
    float3 tu = float3(getHeliostatSize().x, yu, 0.0f);
    float3 tv = float3(0.0f, yv, getHeliostatSize().y);
    float3 nrmLocal = -normalize(cross(tu, tv));

    float3 he = basisToWorldPosition(macroBasis, posLocal, heliostatPosition);
    float3 tn = basisToWorldDirection(macroBasis, nrmLocal);

    float3 ray = he - origin;
    float rl = max(length(ray), kEpsilon);
    float3 dir = ray / rl;
    if (dot(normP, float3(dir.x, 0.0f, dir.z)) <= 0.0f) return 0.0f;

    // 2-layer glass optics (VERBATIM from backward.slang:115-142)
    OrthonormalBasis surfBasis = computeBasis(tn);
    float3 nor1 = basisToWorldDirection(surfBasis, perturb1);
    float3 nor2 = basisToWorldDirection(surfBasis, perturb2);
    float etaE = 1.0f / refIdx;
    float etaX = refIdx;
    float ndi = dot(nor1, dir);
    float kEnter = 1.0f - etaE * etaE * (1.0f - ndi * ndi);
    float3 d2 = (kEnter > 0.0f) ? normalize(etaE * dir - (etaE * ndi + sqrt(kEnter)) * nor1) : float3(0,0,0);
    float3 outRG = float3(0,0,0);
    if (length(d2) > kEpsilon) {
        float3 d3 = normalize(reflect(d2, nor2));
        float ndx = dot(d3, -nor1);
        float kExit = 1.0f - etaX * etaX * (1.0f - ndx * ndx);
        if (kExit > 0.0f) {
            outRG = normalize(etaX * d3 - (etaX * ndx + sqrt(kExit)) * (-nor1));
        }
    }
    if (length(outRG) <= kEpsilon) outRG = normalize(reflect(-dir, nor1));

    float rat = max(0.0f, refArea * dot(normP, dir) * dot(nor1, -dir)
                   / (rl * rl) * computeDistanceAttenuation(rl));
    if (rat <= 0.0f) return 0.0f;

    float ct = clamp(dot(outRG, normalize(sunDir)), -0.99999f, 0.99999f);
    float st = (abs(ct - 1.0f) < kEpsilon) ? 0.0f : acos(ct);
    float ssVal = computeSunshape(st, shapeParams, shapeType);
    return dni * rat * ssVal / max(2.0f * kPi * shapeInt * float(totalSpp), kEpsilon);
}

// P1-A2: compile-time specialized version of computePixelEnergyBolt
[Differentiable]
float computePixelEnergyBoltTyped<let SUNSHAPE_TYPE : uint>(
    float y, float yu, float yv,
    no_diff float u, no_diff float v,
    no_diff float3 origin, no_diff float3 normP,
    no_diff float3 perturb1, no_diff float3 perturb2,
    no_diff float hx, no_diff float hz,
    no_diff float3 macroN, no_diff OrthonormalBasis macroBasis,
    no_diff float3 sunDir, no_diff float dni, no_diff float refIdx,
    no_diff float4 shapeParams, no_diff float shapeInt, no_diff float shapeType,
    no_diff float refArea)
{
    float3 posLocal = float3(hx, y, hz);
    float3 tu = float3(getHeliostatSize().x, yu, 0.0f);
    float3 tv = float3(0.0f, yv, getHeliostatSize().y);
    float3 nrmLocal = -normalize(cross(tu, tv));

    float3 he = basisToWorldPosition(macroBasis, posLocal, heliostatPosition);
    float3 tn = basisToWorldDirection(macroBasis, nrmLocal);

    float3 ray = he - origin;
    float rl = max(length(ray), kEpsilon);
    float3 dir = ray / rl;
    if (dot(normP, float3(dir.x, 0.0f, dir.z)) <= 0.0f) return 0.0f;

    // 2-layer glass optics (VERBATIM from backward.slang:115-142)
    OrthonormalBasis surfBasis = computeBasis(tn);
    float3 nor1 = basisToWorldDirection(surfBasis, perturb1);
    float3 nor2 = basisToWorldDirection(surfBasis, perturb2);
    float etaE = 1.0f / refIdx;
    float etaX = refIdx;
    float ndi = dot(nor1, dir);
    float kEnter = 1.0f - etaE * etaE * (1.0f - ndi * ndi);
    float3 d2 = (kEnter > 0.0f) ? normalize(etaE * dir - (etaE * ndi + sqrt(kEnter)) * nor1) : float3(0,0,0);
    float3 outRG = float3(0,0,0);
    if (length(d2) > kEpsilon) {
        float3 d3 = normalize(reflect(d2, nor2));
        float ndx = dot(d3, -nor1);
        float kExit = 1.0f - etaX * etaX * (1.0f - ndx * ndx);
        if (kExit > 0.0f) {
            outRG = normalize(etaX * d3 - (etaX * ndx + sqrt(kExit)) * (-nor1));
        }
    }
    if (length(outRG) <= kEpsilon) outRG = normalize(reflect(-dir, nor1));

    float rat = max(0.0f, refArea * dot(normP, dir) * dot(nor1, -dir)
                   / (rl * rl) * computeDistanceAttenuation(rl));
    if (rat <= 0.0f) return 0.0f;

    float ct = clamp(dot(outRG, normalize(sunDir)), -0.99999f, 0.99999f);
    float st = (abs(ct - 1.0f) < kEpsilon) ? 0.0f : acos(ct);
    // P1-A2: compile-time specialized sunshape dispatch
    float ssVal = computeSunshapeTyped<SUNSHAPE_TYPE>(st, shapeParams);
    return dni * rat * ssVal / max(2.0f * kPi * shapeInt * float(totalSpp), kEpsilon);
}

// ---- Stage 1: renderBackwardBolt (generic, P1-A2) ----
// Groupshared: per-grid-point surface gradient accumulation within one group
// totalGridPoints * 3 channels = 1024 * 3 = 3072 floats = 12288 bytes
groupshared float gs_gridGrads[totalGridPoints][kSurfaceChannels];

void renderBackwardBoltTyped<let SUNSHAPE_TYPE : uint>(uint3 gid, uint3 gtid) {
    // A1: gid.x indexes the compacted active pixel list (sparse culling)
    uint lid = gtid.x, pixIdx = activePixelList[gid.x], spStart = gid.y * kBWGroupSize, spIdx = spStart + lid;
    if (pixIdx >= totalPixels) return;

    // Zero all groupshared entries using strided access
    for (uint i = lid; i < totalGridPoints; i += kBWGroupSize) {
        gs_gridGrads[i][0] = 0.0f;
        gs_gridGrads[i][1] = 0.0f;
        gs_gridGrads[i][2] = 0.0f;
    }
    GroupMemoryBarrierWithGroupSync();

    float3 macroN = computeHeliostatNormal(getSunDirection(), heliostatPosition, aimPoint);
    OrthonormalBasis macroBasis = computeBasis(macroN);
    float3 sunDir = getSunDirection();
    float dni = getSunDni();
    float refIdx = getHeliostatRefractiveIndex();
    float4 shapeParams = getSunShapeParams();
    float shapeInt = getSunShapeIntegral();
    float shapeType = getSunShapeType();
    float refArea = getHeliostatReflectiveArea();

    uint2 pc = uint2(pixIdx % getReceiverDims().x, pixIdx / getReceiverDims().x);
    float dL = fluxGradient[pc];

    uint myGridIdx = lid; // default: sequential mapping for inactive threads

    if (spIdx < totalSpp) {
        float3 rsPos, rsNrm;
        getReceiverSampleData(pc, rsPos, rsNrm);
        uint xid = spIdx / kGridSize, zid = spIdx % kGridSize;
        float u = float(xid) / float(kGridSize - 1u);
        float v = float(zid) / float(kGridSize - 1u);
        float2 hs = getHeliostatSize();
        float hx = -hs.x * 0.5f + float(xid) * (hs.x / float(kGridSize - 1u));
        float hz = -hs.y * 0.5f + float(zid) * (hs.y / float(kGridSize - 1u));

        uint rayIndex = spIdx * totalPixels + pixIdx;
        // P2: skip dead rays
        if (0u == ((rayValidity[rayIndex >> 5u] >> (rayIndex & 31u)) & 1u)) { myGridIdx = 0; }
        else {
        float bg0, bg1, bg2, bg3;
        generateGaussianSamples(rayIndex, pixIdx, spIdx, bg0, bg1, bg2, bg3);
        float sigma = getHeliostatSlopeError();
        float3 pp1 = normalize(float3(bg0 * sigma, 1.0f, bg1 * sigma));
        float3 pp2 = normalize(float3(bg2 * sigma, 1.0f, bg3 * sigma));

        myGridIdx = zid * kGridSize + xid;

        var d_y  = diffPair(yGrid[myGridIdx], 0.0f);
        var d_yu = diffPair(yuGrid[myGridIdx], 0.0f);
        var d_yv = diffPair(yvGrid[myGridIdx], 0.0f);

        bwd_diff(computePixelEnergyBoltTyped<SUNSHAPE_TYPE>)(d_y, d_yu, d_yv,
            u, v, rsPos, rsNrm, pp1, pp2, hx, hz,
            macroN, macroBasis, sunDir, dni, refIdx,
            shapeParams, shapeInt, shapeType, refArea, dL);

        // Each gridIdx is unique within the group (SPP == gridPoints)
        gs_gridGrads[myGridIdx][0] = d_y.d;
        gs_gridGrads[myGridIdx][1] = d_yu.d;
        gs_gridGrads[myGridIdx][2] = d_yv.d;
        } // end else (ray valid)
    }
    GroupMemoryBarrierWithGroupSync();

    // Phase 5: InterlockedAdd to per-grid-point fixed-point accumulator (12 KB, replaces 386 MB gradPartial)
    // Uses uint with two's complement for negative values (InterlockedAdd requires uint)
    if (spIdx < totalSpp) {
        uint fixedY  = uint(int(gs_gridGrads[myGridIdx][0] * kGradFixedScale));
        uint fixedYu = uint(int(gs_gridGrads[myGridIdx][1] * kGradFixedScale));
        uint fixedYv = uint(int(gs_gridGrads[myGridIdx][2] * kGradFixedScale));
        uint base = myGridIdx * kSurfaceChannels;
        uint dummy;
        InterlockedAdd(gradPartialTile[base + 0u], fixedY, dummy);
        InterlockedAdd(gradPartialTile[base + 1u], fixedYu, dummy);
        InterlockedAdd(gradPartialTile[base + 2u], fixedYv, dummy);
    }
}

// P1-A2: backward-compatible runtime dispatch entry point
[shader("compute")]
[numthreads(kBWGroupSize, 1, 1)]
void renderBackwardBolt(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    uint st = uint(getSunShapeType());
    if (st == kSunshapePillbox)      { renderBackwardBoltTyped<kSunshapePillbox>(gid, gtid); }
    else if (st == kSunshapeGaussian) { renderBackwardBoltTyped<kSunshapeGaussian>(gid, gtid); }
    else                              { renderBackwardBoltTyped<kSunshapeBuie>(gid, gtid); }
}

// P1-A2: compile-time specialized entry points
[shader("compute")]
[numthreads(kBWGroupSize, 1, 1)]
void renderBackwardBoltBuie(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    renderBackwardBoltTyped<kSunshapeBuie>(gid, gtid);
}
[shader("compute")]
[numthreads(kBWGroupSize, 1, 1)]
void renderBackwardBoltPillbox(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    renderBackwardBoltTyped<kSunshapePillbox>(gid, gtid);
}
[shader("compute")]
[numthreads(kBWGroupSize, 1, 1)]
void renderBackwardBoltGaussian(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    renderBackwardBoltTyped<kSunshapeGaussian>(gid, gtid);
}

// ---- Stage 1b: reduceSurfaceGradients ----
// Phase 5: converts fixed-point uint accumulation from gradPartialTile → float → surfaceGradient.
// gradPartialTile is already summed across all pixel groups by InterlockedAdd in renderBackwardBolt.
// Uses int cast to recover two's complement negative values from uint.
[shader("compute")]
[numthreads(kBWGroupSize, 1, 1)]
void reduceSurfaceGradients(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    uint lid = gtid.x;
    for (uint gi = lid; gi < totalGridPoints; gi += kBWGroupSize) {
        float sumY  = float(int(gradPartialTile[gi * kSurfaceChannels + 0u])) / kGradFixedScale;
        float sumYu = float(int(gradPartialTile[gi * kSurfaceChannels + 1u])) / kGradFixedScale;
        float sumYv = float(int(gradPartialTile[gi * kSurfaceChannels + 2u])) / kGradFixedScale;
        surfaceGradient[gi * kSurfaceChannels + 0u] = asuint(sumY);
        surfaceGradient[gi * kSurfaceChannels + 1u] = asuint(sumYu);
        surfaceGradient[gi * kSurfaceChannels + 2u] = asuint(sumYv);
    }
}

// ---- Stage 2: projectBoltGradients ----
// dL/dh_b = sum_p [dL/dy_p * phi_b(p) + dL/dyu_p * dphi_b/du(p) + dL/dyv_p * dphi_b/dv(p)]
// Chain rule: dy/dh = phi (TPS local coordinates, no cosTheta).
// Accumulates into boltHeightGradient using += (for multi-direction accumulation).
[shader("compute")]
[numthreads(kMaxBolts, 1, 1)]
void projectBoltGradients(uint3 tid : SV_DispatchThreadID) {
    uint b = tid.x;
    if (b >= pushConst.numBolts) return;

    float grad = 0.0f;
    for (uint p = 0u; p < totalGridPoints; p++) {
        float dLdy  = asfloat(surfaceGradient[p * kSurfaceChannels + 0u]);
        float dLdyu = asfloat(surfaceGradient[p * kSurfaceChannels + 1u]);
        float dLdyv = asfloat(surfaceGradient[p * kSurfaceChannels + 2u]);
        uint infBase = b * totalGridPoints + p;
        grad += dLdy  * influencePhi[infBase];
        grad += dLdyu * influencePhiU[infBase];
        grad += dLdyv * influencePhiV[infBase];
    }
    boltHeightGradient[b] += grad;
}

// ---- clearSurfaceGradient ----
// Zeros surfaceGradient buffer. Called before each sun direction's backward pass.
[shader("compute")]
[numthreads(kBWGroupSize, 1, 1)]
void clearSurfaceGradient(uint3 tid : SV_DispatchThreadID) {
    uint idx = tid.x;
    uint totalSlots = totalGridPoints * kSurfaceChannels;
    if (idx < totalSlots) surfaceGradient[idx] = 0u;  // Phase 5: uint-typed buffer
}
