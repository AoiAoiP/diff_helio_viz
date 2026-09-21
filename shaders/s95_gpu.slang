// s95_gpu.slang — GPU-side S95 level find + buffer-fed S95 loss
//
// Replaces the CPU readFlux() + binary-search round trip in the bolt hot loop.
// The S95 level is computed on GPU by a single-workgroup cooperative binary
// search with the *same* semantics as the CPU version (20 iterations, strict
// f > mid energy sum, > 0.95 energy-fraction test), so the sigmoid loss sees a
// level matching the CPU one to within float reduction-order noise (~1e-6
// relative). This avoids the reverted Phase-3 design's flaw: a fixed-range
// 256-bin histogram whose ~1.5 W/m^2 level bias saturated the sigmoid loss.
//
// Chain per sun direction (single submit, no host sync):
//   finalizeFlux → computeS95FindLevel → computeS95LossBUF → boltBackward
//
// NOTE: group reductions use the WaveActiveSum idiom proven in forward.slang
// (reduceFWSum). A hand-rolled groupshared tree reduction with per-level
// barriers compiled pathologically slow under Slang (~19 ms/dispatch).
module s95_gpu;
import common;

[[vk::binding(8)]] [format("r32f")]
RWTexture2D<float> renderedFlux;
[[vk::binding(12)]] [format("r32f")]
RWTexture2D<float> fluxGradient;

// [0]=s95 level (out), [2]=total energy (debug), [3]=max flux (debug)
[[vk::binding(52)]] RWStructuredBuffer<float> s95State;
// Scalar sigmoid loss accumulator, fixed-point x1e3 (zeroed once per iteration)
[[vk::binding(53)]] RWStructuredBuffer<uint> lossAccumFixed;

static const uint kS95GroupSize = 256u;
static const uint kS95Waves = kS95GroupSize / 32u;  // 8 (wave size 32, as forward.slang)

groupshared float g_s95Waves[kS95Waves];
groupshared float gs_low, gs_high, gs_level, gs_total;

// Group sum; result valid in wave-0 lanes only (caller uses lane 0).
// Trailing barrier protects g_s95Waves from the next call's overwrite.
float s95GroupSum(uint lid, float val) {
    uint wid = lid / 32u;
    val = WaveActiveSum(val);
    if (WaveIsFirstLane()) g_s95Waves[wid] = val;
    GroupMemoryBarrierWithGroupSync();
    val = (lid < kS95Waves) ? g_s95Waves[WaveGetLaneIndex()] : 0.0f;
    if (wid == 0u) val = WaveActiveSum(val);
    GroupMemoryBarrierWithGroupSync();
    return val;
}

// Group max; result valid in wave-0 lanes only. Flux is non-negative, so
// asuint() preserves ordering (avoids relying on float WaveActiveMax).
float s95GroupMax(uint lid, float val) {
    uint wid = lid / 32u;
    uint v = WaveActiveMax(asuint(val));
    if (WaveIsFirstLane()) g_s95Waves[wid] = asfloat(v);
    GroupMemoryBarrierWithGroupSync();
    v = (lid < kS95Waves) ? asuint(g_s95Waves[WaveGetLaneIndex()]) : 0u;
    if (wid == 0u) v = WaveActiveMax(v);
    GroupMemoryBarrierWithGroupSync();
    return asfloat(v);
}

// Cooperative exact binary search for the S95 level — mirrors the CPU
// computeS95Level(): 20 iterations; sumAbove/total > 0.95 raises the lower
// bound, otherwise the level candidate is recorded from above.
// Single workgroup: ~21 parallel passes over the 7850-texel flux texture.
[shader("compute")]
[numthreads(kS95GroupSize, 1, 1)]
void computeS95FindLevel(uint3 gtid : SV_GroupThreadID) {
    uint lid = gtid.x;
    uint2 dims = getReceiverDims();
    uint np = dims.x * dims.y;

    // Pass 0: total energy + max flux
    float partSum = 0.0f, partMax = 0.0f;
    for (uint i = lid; i < np; i += kS95GroupSize) {
        float f = renderedFlux[uint2(i % dims.x, i / dims.x)];
        partSum += f;
        partMax = max(partMax, f);
    }
    float total = s95GroupSum(lid, partSum);
    float maxFlux = s95GroupMax(lid, partMax);

    if (lid == 0u) {
        gs_total = total;
        gs_low = 0.0f;
        gs_high = maxFlux;
        gs_level = 0.0f;
    }
    GroupMemoryBarrierWithGroupSync();

    // NOTE: the no-energy check must be UNIFORM across the workgroup — the
    // reduced `total` is only valid in wave-0 lanes; branching on it directly
    // lets waves 1-7 exit early and diverges the barriers inside the loop.
    if (gs_total > 1e-6f) {
        for (int it = 0; it < 20; it++) {
            float mid = 0.5f * (gs_low + gs_high);
            float part = 0.0f;
            for (uint i = lid; i < np; i += kS95GroupSize) {
                float f = renderedFlux[uint2(i % dims.x, i / dims.x)];
                if (f > mid) part += f;
            }
            float sumAbove = s95GroupSum(lid, part);  // valid in wave-0 lanes
            if (lid == 0u) {
                if (sumAbove / gs_total > 0.95f) gs_low = mid;
                else { gs_high = mid; gs_level = mid; }
            }
            GroupMemoryBarrierWithGroupSync();
        }
    }

    if (lid == 0u) {
        s95State[0] = gs_level;
        s95State[2] = gs_total;
        s95State[3] = maxFlux;
    }
}

// S95 sigmoid loss with the level read from the GPU buffer (no host round
// trip). Writes dL/dflux per pixel (same formula as computeS95Loss) and
// accumulates the scalar loss as fixed-point x1e3 for per-iteration readback.
//
// L1 efficiency term (ARCAim-style energy guard):
//   L_eff = lambda * M * E_ref / E,  dL_eff/df_i = -lambda * M * E_ref / E^2
// where M = receiver pixel count (keeps lambda ~ O(1) against the sigmoid
// sum), E = s95State[2] (total flux, already computed by the level find) and
// E_ref the per-sun reference captured at iteration 0. lambda = 0 reduces to
// the plain S95 sigmoid loss bit-exactly (+0.0f is exact in IEEE-754).
struct S95LossPC {
    float eRef;
    float lambdaEff;
    float _pad0;
    float _pad1;
};
[[vk::push_constant]] S95LossPC lossPC;

[shader("compute")]
[numthreads(16, 16, 1)]
void computeS95LossBUF(uint3 tid : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID) {
    uint2 dims = getReceiverDims();
    float lv = max(s95State[0], kEpsilon);
    float E = max(s95State[2], kEpsilon);
    bool useEff = lossPC.lambdaEff != 0.0f;
    float npix = float(dims.x * dims.y);
    float effGrad  = useEff ? -lossPC.lambdaEff * npix * lossPC.eRef / (E * E) : 0.0f;
    float effShare = useEff ?  lossPC.lambdaEff * lossPC.eRef / E : 0.0f;  // per-pixel share of L_eff
    float s = 0.0f;
    if (tid.x < dims.x && tid.y < dims.y) {
        float flux = renderedFlux[tid.xy];
        float nf = flux / lv;
        s = 1.0f / (1.0f + exp(-6.0f * (nf - 1.0f)));
        fluxGradient[tid.xy] = 6.0f * s * (1.0f - s) / lv + effGrad;
        s += effShare;
    }
    // 16x16 = 256 threads == kS95GroupSize; reduce and accumulate globally
    uint lid = gtid.y * 16u + gtid.x;
    float groupSum = s95GroupSum(lid, s);  // valid in wave-0 lanes
    if (lid == 0u && groupSum > 0.0f) {
        InterlockedAdd(lossAccumFixed[0], uint(groupSum * 1000.0f + 0.5f));
    }
}
