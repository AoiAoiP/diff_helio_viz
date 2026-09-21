// bolt_forward.slang — Batched multi-sun bolt surface (Phase 2)
module bolt_forward;
import common;
import bolt_common;

struct BoltSurfacePC { uint numBolts; uint disableGravity; uint sunBatchCount; uint gravityNormalCoupling; };
[[vk::push_constant]] BoltSurfacePC pushConst;

[shader("compute")]
[numthreads(kGridSize, kGridSize, 1)]
void computeBoltSurface(uint3 tid : SV_DispatchThreadID, uint3 gid : SV_GroupID) {
    if (tid.x >= kGridSize || tid.y >= kGridSize) return;
    uint si = gid.z;
    if (si >= pushConst.sunBatchCount) return;

    uint base = si * kSunStride;
    uint lo = asuint(sunBatchFlat[base + 4]);
    uint hi = asuint(sunBatchFlat[base + 5]);
    float t = sunBatchFlat[base + 6];

    float y, yu, yv;
    float3 nrm;
    boltSurfaceAtGrid(tid.x, tid.y, pushConst.numBolts, lo, hi, t,
        pushConst.disableGravity, pushConst.gravityNormalCoupling, y, yu, yv, nrm);

    uint idx = si * kGridSize * kGridSize + tid.y * kGridSize + tid.x;
    yGrid[idx] = y; yuGrid[idx] = yu; yvGrid[idx] = yv;
    nGrid[idx] = float4(nrm, 0.0f);
}
