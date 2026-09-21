// forward.slang — Forward rendering with Bezier surface normals
// Ray path: receiver pixel → Bezier surface point → reflect off Bezier normal → sun
// Matches Taichi multi_moment_S95 baseline: reflection uses Bezier surface normal.
module forward;
import common;
import sunshape;

static const uint kFWGroupSize = 256u;
static const uint kFWWarpSize  = 32u;
static const uint kFWWaves     = kFWGroupSize / kFWWarpSize;

static const uint totalSpp    = kGridSize * kGridSize;
static const uint totalPixels = 7850u;
// Number of 256-wide sample tiles per pixel. Grows with grid resolution:
// 25x25 -> 3, 32x32 -> 4. fluxPartial is sized totalPixels*kTileCount on the host.
static const uint kTileCount  = (totalSpp + 255u) / 256u;

// Diagnostic counters for branch boundary analysis
// [0]=TIR fallback, [1]=near back-face (dot<0.01), [2]=near sun outer cutoff
// [3]=near sun inner cutoff, [4]=near receiver edge (dc<0.01), [5]=total rays
[[vk::binding(31)]] RWStructuredBuffer<uint> diagBuf;

[[vk::binding(8)]] [format("r32f")]
RWTexture2D<float> renderedFlux;

// Atomic float accumulation buffer (CAS-loop), one uint per pixel
[[vk::binding(10)]] RWStructuredBuffer<float> fluxPartial;

// binding 9 removed — Phase 1: inline Box-Muller replaces gaussianPool

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

// ---- Bezier surface grid compute ----
[shader("compute")]
[numthreads(kGridSize, kGridSize, 1)]
void computeBezierSurface(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= kGridSize || tid.y >= kGridSize) return;
    float u = float(tid.x) / float(kGridSize - 1u);
    float v = float(tid.y) / float(kGridSize - 1u);
    float3 pos = bezierSurface(u, v);
    float3 nrm = bezierSurfaceNormal(u, v);
    uint idx = tid.y * kGridSize + tid.x;
    yGrid[idx] = pos.y;
    nGrid[idx] = float4(nrm, 0.0f);
}

// ---- Clear flux ----
[shader("compute")]
[numthreads(16, 16, 1)]
void clearFlux(uint3 tid : SV_DispatchThreadID) {
    uint2 dims = uint2(157, 50);
    if (tid.x < dims.x && tid.y < dims.y) {
        renderedFlux[tid.xy] = 0.0f;
        uint base = tid.y * 157u + tid.x;
        for (uint t = 0u; t < kTileCount; t++) {
            fluxPartial[base * kTileCount + t] = 0.0f;
        }
    }
}

// ---- Finalize: sum 3 tile partials per pixel into texture ----
[shader("compute")]
[numthreads(16, 16, 1)]
void finalizeFlux(uint3 tid : SV_DispatchThreadID) {
    uint2 dims = uint2(157, 50);
    if (tid.x < dims.x && tid.y < dims.y) {
        uint base = tid.y * 157u + tid.x;
        float sum = 0.0f;
        for (uint t = 0u; t < kTileCount; t++) {
            sum += fluxPartial[base * kTileCount + t];
        }
        renderedFlux[tid.xy] = sum;
    }
}

// ---- Forward render: Bezier-surface-normal-based reflection ----
// P1-A2: Generic implementation with compile-time sunshape specialization.
// The SUNSHAPE_TYPE let-generic is constant-folded by Slang — zero runtime branch.
void renderForwardTyped<let SUNSHAPE_TYPE : uint>(uint3 gid, uint3 gtid) {
    // A1: gid.y indexes the compacted active pixel list (sparse culling)
    uint lid = gtid.x, tileIdx = gid.x, pixelIdx = activePixelList[gid.y];
    if (pixelIdx >= totalPixels) return;

    uint2 dims = getReceiverDims();
    uint2 pc = uint2(pixelIdx % dims.x, pixelIdx / dims.x);
    float3 origin, normP;
    getReceiverSampleData(pc, origin, normP);

    // P4: cylinder half-face culling — skip pixels facing away from heliostat
    float2 cullDir = getHeliostatCullDir();
    if (normP.x * cullDir.x + normP.z * cullDir.y <= 0.0f) return;

    // Macro normal for coordinate frame (bisector of sun & aim)
    float3 macroN   = computeHeliostatNormal(getSunDirection(), heliostatPosition, aimPoint);
    OrthonormalBasis macroBasis = computeBasis(macroN);

    float slopeErr = getHeliostatSlopeError();
    float refArea  = getHeliostatReflectiveArea();
    float refIdx   = getHeliostatRefractiveIndex();
    float dni      = getSunDni();
    float4 shapeP   = getSunShapeParams();
    float shapeInt  = getSunShapeIntegral();
    float3 sunDir   = getSunDirection();

    uint wi = tileIdx * kFWGroupSize + lid;
    float energy = 0.0f;

    if (wi < totalSpp) {
        uint sp = wi;
        uint xid = sp / kGridSize, zid = sp % kGridSize;
        uint gridIdx = zid * kGridSize + xid;

        float2 hsize = getHeliostatSize();
        float hx = -hsize.x * 0.5f + float(xid) * (hsize.x / float(kGridSize - 1u));
        float hz = -hsize.y * 0.5f + float(zid) * (hsize.y / float(kGridSize - 1u));

        // Bezier surface position and normal in local frame, transform to world
        float3 localPos = float3(hx, yGrid[gridIdx], hz);
        float3 localNrm = nGrid[gridIdx].xyz;
        float3 worldPos = basisToWorldPosition(macroBasis, localPos, heliostatPosition);
        float3 surfNrm  = basisToWorldDirection(macroBasis, localNrm);   // world-space Bezier normal

        // Ray from receiver pixel to heliostat surface point
        float3 ray = worldPos - origin;
        float rayLen = max(length(ray), kEpsilon);
        float3 dir = ray / rayLen;

        // Facing check (horizontal component, receiver pixel must face heliostat)
        float dc = dot(normP, float3(dir.x, 0.0f, dir.z));
        bool rayAlive = dc > 0.0f;
        // A1: per-ray angular pre-cull — cheap macro-normal reflection test.
        // The full glass path (slope-error perturbations + refraction) can only
        // rotate the outgoing ray by a few mrad, so if the macro reflection of
        // the receiver->surface ray lands outside sun support + margin, the
        // true energy contribution is exactly zero: skip Box-Muller, the whole
        // 2-layer glass refraction and the sunshape evaluation. The cull cosine
        // is cos(support + margin) from the SunParams UBO (< -1.5 disables).
        float cullCos = getSunCullCos();
        if (rayAlive && cullCos > -1.5f) {
            float3 outApprox = reflect(dir, surfNrm);
            rayAlive = dot(outApprox, normalize(sunDir)) >= cullCos;
        }
        if (rayAlive) {
            // Phase 1: Inline Box-Muller replaces gaussianPool lookup
            uint rayIndex = sp * totalPixels + pixelIdx;
            float g0, g1, g2, g3;
            generateGaussianSamples(rayIndex, pixelIdx, sp, g0, g1, g2, g3);

            // 2-layer glass optics (original: nor1≠nor2, manual refract + fallback)
            OrthonormalBasis sBasis = computeBasis(surfNrm);
            float3 nor1 = basisToWorldDirection(sBasis,
                normalize(float3(g0 * slopeErr, 1.0f, g1 * slopeErr)));
            float3 nor2 = basisToWorldDirection(sBasis,
                normalize(float3(g2 * slopeErr, 1.0f, g3 * slopeErr)));
            float etaE = 1.0f / getHeliostatRefractiveIndex();
            float etaX = getHeliostatRefractiveIndex();
            float ndi = dot(nor1, dir);
            float kEnter = 1.0f - etaE * etaE * (1.0f - ndi * ndi);
            float3 d2 = (kEnter > 0.0f) ? normalize(etaE * dir - (etaE * ndi + sqrt(kEnter)) * nor1) : float3(0.0f,0.0f,0.0f);
            float3 outRG = float3(0.0f,0.0f,0.0f);
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
                InterlockedAdd(diagBuf[0], 1u);  // [0] TIR fallback
            }

            // [1] Near back-face: dot(nor1,-dir) almost zero → rat≈0 boundary
            float cos_incident = dot(nor1, -dir);
            if (cos_incident > 0.0f && cos_incident < 0.01f) {
                InterlockedAdd(diagBuf[1], 1u);
            }

            float rat = max(0.0f, refArea * dot(normP, dir) * cos_incident
                           / (rayLen * rayLen) * computeDistanceAttenuation(rayLen));
            if (rat > 0.0f) {
                float ct = clamp(dot(outRG, normalize(sunDir)), -0.99999f, 0.99999f);
                float st = (abs(ct - 1.0f) < kEpsilon) ? 0.0f : acos(ct);

                // [2] Near sun outer cutoff (0.0436 rad)
                if (st > 0.040f && st < 0.047f) {
                    InterlockedAdd(diagBuf[2], 1u);
                }
                // [3] Near sun inner cutoff (0.00465 rad)
                if (st > 0.0035f && st < 0.0060f) {
                    InterlockedAdd(diagBuf[3], 1u);
                }

                // P1-A2: compile-time specialized — SUNSHAPE_TYPE is a let-generic
                float ssVal = computeSunshapeTyped<SUNSHAPE_TYPE>(st, shapeP);
                energy = dni * rat * ssVal / max(2.0f * kPi * shapeInt * float(totalSpp), kEpsilon);
                // P2: mark this ray as valid
                if (energy > 0.0f) {
                    InterlockedOr(rayValidity[rayIndex >> 5u], 1u << (rayIndex & 31u));
                }
            }
        } else {
            // [4] Near receiver pixel edge: dc just below zero
            if (dc > -0.01f && dc <= 0.0f) {
                InterlockedAdd(diagBuf[4], 1u);
            }
        }

        // [5] Total rays processed — simple write from first thread to verify binding
        if (sp == 0u && pixelIdx == 0u && lid == 0u) {
            diagBuf[5] = totalPixels;  // verify binding: should read back as 7850
        }
        InterlockedAdd(diagBuf[5], 1u);
    }

    float red = reduceFWSum(lid, energy);
    if (lid == 0u) {
        // Per-tile partial sum, no atomic contribution
        fluxPartial[pixelIdx * kTileCount + tileIdx] = red;
    }
}

// P1-A2: backward-compatible runtime dispatch entry point (kept for existing code)
[shader("compute")]
[numthreads(kFWGroupSize, 1, 1)]
void renderForward(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    uint st = uint(getSunShapeType());
    if (st == kSunshapePillbox)      { renderForwardTyped<kSunshapePillbox>(gid, gtid); }
    else if (st == kSunshapeGaussian) { renderForwardTyped<kSunshapeGaussian>(gid, gtid); }
    else                              { renderForwardTyped<kSunshapeBuie>(gid, gtid); }
}

// P1-A2: compile-time specialized entry points (registered in CMakeLists.txt)
[shader("compute")]
[numthreads(kFWGroupSize, 1, 1)]
void renderForwardBuie(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    renderForwardTyped<kSunshapeBuie>(gid, gtid);
}
[shader("compute")]
[numthreads(kFWGroupSize, 1, 1)]
void renderForwardPillbox(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    renderForwardTyped<kSunshapePillbox>(gid, gtid);
}
[shader("compute")]
[numthreads(kFWGroupSize, 1, 1)]
void renderForwardGaussian(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    renderForwardTyped<kSunshapeGaussian>(gid, gtid);
}
