// common.slang — Shared math, Bezier surface, coordinate transforms, uniforms
module common;

public static const float kPi = 3.14159265358979323846f;
public static const float kEpsilon = 1e-6f;
public static const uint kGridSize = 32;
public static const uint kControlSize = 4;
public static const uint kSamplingSeed = 12345u;  // P0: deterministic seed

// ---- Sun shape type enum (must match config.h) ----
public static const uint kSunshapeBuie     = 0u;
public static const uint kSunshapePillbox  = 1u;
public static const uint kSunshapeGaussian = 2u;

// ---- Uniform structs (std140 layout) ----
public struct ReceiverParams {
    float3 position;
    float radius;
    float height;
    uint2 dims;
    float onePixelHeight;
    float onePixelWidth;
};

public struct HeliostatParams {
    float2 size;
    float depth;
    float refractiveIndex;
    float slopeError;
    float reflectiveArea;
    float reflectivity;
    float _pad0;            // was reflectionOnly
    float _pad1;            // was poolMask (Phase 1: gaussianPool removed)
    float cullDirX;         // P4: dir to heliostat X (cylinder half-cull)
    float cullDirZ;         // P4: dir to heliostat Z
};

public struct SunParams {
    float3 direction;
    float dni;
    float4 shapeParams;
    float shapeIntegral;
    float type;            // kSunshape* (0=Buie, 1=Pillbox, 2=Gaussian)
    float iterationSeed;   // P1-L3: per-iteration seed for Gaussian randomization (0=fixed)
    float cullCosCutoff;   // A1: cos(sun support + margin); < -1.5 disables the pre-cull
    float _pad3;
};

public struct OrthonormalBasis : IDifferentiable {
    float3 u;
    float3 v;
    float3 normal;
};

// ---- Vulkan descriptor bindings (set 0) ----
[[vk::binding(0)]] public ConstantBuffer<ReceiverParams> receiver;
[[vk::binding(1)]] public ConstantBuffer<HeliostatParams> heliostat;
[[vk::binding(2)]] public ConstantBuffer<SunParams> sun;
[[vk::binding(3)]] public ConstantBuffer<float3> heliostatPosition;
[[vk::binding(4)]] public ConstantBuffer<float3> aimPoint;

[[vk::binding(5)]] public uniform StructuredBuffer<float> controlY;
[[vk::binding(6)]] public RWStructuredBuffer<float> yGrid;
[[vk::binding(7)]] public RWStructuredBuffer<float4> nGrid;

// ---- Bernstein basis (for Bezier surface) ----
public float bernstein3(uint i, float t) {
    if (i == 0u) { float o = 1.0f - t; return o * o * o; }
    if (i == 1u) return 3.0f * t * (1.0f - t) * (1.0f - t);
    if (i == 2u) return 3.0f * t * t * (1.0f - t);
    return t * t * t;
}

public float bernstein3_diff(uint i, float t) {
    if (i == 0u) return -3.0f * (1.0f - t) * (1.0f - t);
    if (i == 1u) return 3.0f * (1.0f - t) * (1.0f - t) - 6.0f * t * (1.0f - t);
    if (i == 2u) return 6.0f * t * (1.0f - t) - 3.0f * t * t;
    return 3.0f * t * t;
}

public float bernstein3_diff2(uint i, float t) {
    if (i == 0u) return  6.0f * (1.0f - t);
    if (i == 1u) return  6.0f * (3.0f * t - 2.0f);
    if (i == 2u) return  6.0f * (1.0f - 3.0f * t);
    return 6.0f * t;
}

// ---- Bezier surface evaluation ----
public float3 bezierSurface(float u, float v) {
    float x = -heliostat.size.x * 0.5f + heliostat.size.x * u;
    float z = -heliostat.size.y * 0.5f + heliostat.size.y * v;
    float y = 0.0f;
    for (uint i = 0u; i < kControlSize; i++) {
        float biv = bernstein3(i, v);
        for (uint j = 0u; j < kControlSize; j++) {
            y += biv * bernstein3(j, u) * controlY[i * kControlSize + j];
        }
    }
    return float3(x, y, z);
}

public float3 bezierSurfaceNormal(float u, float v) {
    float3 tu = float3(heliostat.size.x, 0.0f, 0.0f);
    float3 tv = float3(0.0f, 0.0f, heliostat.size.y);
    for (uint i = 0u; i < kControlSize; i++) {
        float biv = bernstein3(i, v), div = bernstein3_diff(i, v);
        for (uint j = 0u; j < kControlSize; j++) {
            float bju = bernstein3(j, u), dju = bernstein3_diff(j, u);
            float yc = controlY[i * kControlSize + j];
            tu.y += dju * biv * yc;
            tv.y += div * bju * yc;
        }
    }
    return normalize(-cross(tu, tv));
}

// ---- Coordinate transforms (differentiable) ----
[Differentiable]
public OrthonormalBasis computeBasis(float3 normal) {
    OrthonormalBasis basis;
    basis.normal = normal;
    if (abs(normal.x) < kEpsilon && abs(normal.z) < kEpsilon) {
        basis.u = float3(1.0f, 0.0f, 0.0f);
        basis.v = float3(0.0f, 0.0f, 1.0f);
    } else {
        float3 up = float3(0.0f, 1.0f, 0.0f);
        basis.u = normalize(cross(up, normal));
        basis.v = cross(basis.u, normal);
    }
    return basis;
}

[Differentiable]
public float3 basisToWorldDirection(OrthonormalBasis basis, float3 local) {
    return local.x * basis.u + local.y * basis.normal + local.z * basis.v;
}

[Differentiable]
public float3 basisToWorldPosition(OrthonormalBasis basis, float3 local, float3 origin) {
    return basisToWorldDirection(basis, local) + origin;
}

// ---- Heliostat macro-normal (Taichi: normalize(sunDir + aimV)) ----
[Differentiable]
public float3 computeHeliostatNormal(float3 sunDir, float3 helioPos, float3 aimPt) {
    return normalize(normalize(sunDir) + normalize(aimPt - helioPos));
}

// ---- Slope-error perturbation: apply micro-facet normal perturbation ----
// Returns perturbed normal in world space, given macro normal and 2 random values
public float3 perturbNormal(float3 macroNormal, float slopeError, float rx, float rz) {
    // rx, rz are standard normal samples (from gaussianPool)
    // The perturbation angles are rx * slopeError and rz * slopeError
    // In the local tangent frame: perturbed = (rx*σ, 1.0, rz*σ) / normalized
    return normalize(float3(rx * slopeError, 1.0f, rz * slopeError));
}

// ---- Receiver sampling (cylinder, pixel corner convention matching Taichi) ----
public struct ReceiverSample { float3 position; float3 normal; };

public ReceiverSample computeReceiverSample(uint2 pixelCoord, uint2 dims, float onePixelHeight,
                                      float radius, float3 position, float height) {
    // Pixel corner convention (matches Taichi reference)
    float angleNo = float(pixelCoord.x) * (2.0f * kPi / float(dims.x));
    float yNo = float(pixelCoord.y) * onePixelHeight;
    float px = position.x + radius * sin(angleNo);
    float pz = position.z - radius * cos(angleNo);
    float py = position.y + yNo - height * 0.5f;
    ReceiverSample s;
    s.position = float3(px, py, pz);
    s.normal = normalize(float3(px - position.x, 0.0f, pz - position.z));
    return s;
}

// ---- Distance attenuation (matches diffspt, differentiable) ----
[Differentiable]
public float computeDistanceAttenuation(float rayLen) {
    if (rayLen <= 1000.0f) {
        return 0.99331f - 0.0001176f * rayLen + 1.97e-8f * rayLen * rayLen;
    }
    return exp(-0.0001106f * rayLen);
}

// ---- Pseudo-random hash ----
public uint mixBits(uint v) {
    v ^= v >> 16; v *= 0x7FEB352Du; v ^= v >> 15;
    v *= 0x846CA68Bu; v ^= v >> 16; return v;
}

public float random01(uint a, uint b, uint c, uint seed) {
    return (float(mixBits(a * 0x9E3779B9u + b * 0x7F4A7C15u + c * 0x94D049BBu + seed * 0x5BD1E995u)) + 0.5f) * (1.0f / 4294967296.0f);
}

public static const float kInvU32 = 1.0f / 4294967296.0f;

// Phase 1: Inline Box-Muller — generates 4 independent N(0,1) samples
// deterministically from (rayIndex, pixelIndex, sppIndex).
// Replaces the 1.5 GB gaussianPool lookup table.
public void generateGaussianSamples(
    uint rayIndex, uint pixelIndex, uint sppIndex,
    out float g0, out float g1, out float g2, out float g3)
{
    uint base = rayIndex * 0x9E3779B9u;
    base ^= pixelIndex * 0x94D049BBu;
    base += sppIndex * 0x5BD1E995u;
    // P1-L3: mix in iteration seed for per-iteration randomization (0 = fixed)
    float iterSeed = getIterationSeed();
    uint iterBits = (iterSeed > 0.0f) ? asuint(iterSeed) : kSamplingSeed;
    base ^= iterBits * 0xA5A5A5A5u;

    uint h0 = mixBits(base);
    uint h1 = mixBits(base ^ 0xDEADBEEFu);
    uint h2 = mixBits(base ^ 0xCAFEBABEu);
    uint h3 = mixBits(base ^ 0x8BADF00Du);

    float u0 = (float(h0) + 0.5f) * kInvU32;
    float u1 = (float(h1) + 0.5f) * kInvU32;
    float u2 = (float(h2) + 0.5f) * kInvU32;
    float u3 = (float(h3) + 0.5f) * kInvU32;

    // Box-Muller pair 1
    float r1 = sqrt(-2.0f * log(max(u0, 1e-10f)));
    float theta1 = 2.0f * kPi * u1;
    g0 = r1 * cos(theta1);
    g1 = r1 * sin(theta1);

    // Box-Muller pair 2
    float r2 = sqrt(-2.0f * log(max(u2, 1e-10f)));
    float theta2 = 2.0f * kPi * u3;
    g2 = r2 * cos(theta2);
    g3 = r2 * sin(theta2);
}

// P2: Ray validity cache (binding 29)
// Written by forward, read by backward to skip dead rays
[[vk::binding(29)]] public RWStructuredBuffer<uint> rayValidity;

// ---- Convenience accessors (avoids cross-module struct field issues) ----
public uint2 getReceiverDims()              { return receiver.dims; }
public float getReceiverOnePixelHeight()     { return receiver.onePixelHeight; }
public float getReceiverRadius()            { return receiver.radius; }
public float3 getReceiverPosition()         { return receiver.position; }
public float getReceiverHeight()            { return receiver.height; }
public float2 getHeliostatSize()            { return heliostat.size; }
public float2 getHeliostatCullDir()          { return float2(heliostat.cullDirX, heliostat.cullDirZ); }
public float getHeliostatDepth()            { return heliostat.depth; }
public float getHeliostatRefractiveIndex()   { return heliostat.refractiveIndex; }
public float getHeliostatReflectiveArea()    { return heliostat.reflectiveArea; }
public float getHeliostatReflectivity()      { return heliostat.reflectivity; }
public float getHeliostatSlopeError()        { return heliostat.slopeError; }
public float3 getSunDirection()             { return sun.direction; }
public float getSunDni()                    { return sun.dni; }
public float4 getSunShapeParams()           { return sun.shapeParams; }
public float getSunShapeIntegral()           { return sun.shapeIntegral; }
public float getSunShapeType()               { return sun.type; }
public float getSunCullCos()                 { return sun.cullCosCutoff; }
public float getIterationSeed()              { return sun.iterationSeed; }

public ReceiverSample sampleReceiverPixel(uint2 pixelCoord) {
    return computeReceiverSample(pixelCoord, receiver.dims, receiver.onePixelHeight,
                                  receiver.radius, receiver.position, receiver.height);
}

public void getReceiverSampleData(uint2 pixelCoord, out float3 pos, out float3 nrm) {
    ReceiverSample s = sampleReceiverPixel(pixelCoord);
    pos = s.position;
    nrm = s.normal;
}

// Phase 2: Batched sun dispatch (flat float array, 8 floats per sun entry)
public static const uint kSunStride = 8u;
public static const uint kMaxSunBatch = 6u;
[[vk::binding(51)]] public uniform StructuredBuffer<float> sunBatchFlat;

// A1: Sparse pixel culling — compacted list of receiver pixel indices facing
// the heliostat (sun-independent). Built on CPU (buildActivePixelList), tail
// padded with 0xFFFFFFFF sentinels (>= totalPixels → shader early-out).
[[vk::binding(55)]] public uniform StructuredBuffer<uint> activePixelList;
