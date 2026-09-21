#pragma once

// config.h — minimal configuration for the forward (inference) engine.
//
// Deliberately smaller than the upstream optimizer's Config: only keys that the
// forward ray tracer / visualization path actually reads are kept. Optimizer-only
// keys (Adam, regularization suite, B-spline, anchor/bend/soft-stroke, MSE loss)
// stay upstream.

#include <array>
#include <cstdint>
#include <string>

namespace hviz {

enum class SunShapeType : uint32_t { BUIE = 0, PILLBOX = 1, GAUSSIAN = 2 };

struct Config {
    // ---- Files ----
    std::string sunFile = "data/sundir/sundir_36.txt";
    std::string ellipseFile = "data/ellipse_north.txt";
    std::string boltFile;                 // empty -> zero bolts
    std::string proxyPath = "data_proxy";  // influence_phi*.bin + gravity_*.bin
    std::string outputDir = "out";

    // ---- Receiver (cylinder) ----
    std::array<float, 3> receiverPos = {0.0f, 180.0f, 0.0f};
    float receiverRadius = 10.0f;
    float receiverHeight = 20.0f;
    uint32_t pixelWidth = 157;
    uint32_t pixelHeight = 50;

    // ---- Heliostat plate ----
    float plateWidth = 12.84f;   // local x
    float plateLength = 9.45f;   // local z
    uint32_t gridSize = 32;      // 32x32 surface samples
    float glassDepth = 0.004f;
    float refractiveIndex = 1.523f;
    float slopeError = 0.001f;   // rad
    float reflectivity = 0.88f;

    // ---- Sun ----
    SunShapeType sunType = SunShapeType::BUIE;
    float dni = 1000.0f;
    float csr = 0.01f;
    float sigma = 0.00251f;
    float thetaMax = 0.00465f;
    float buieThetaInner = 0.00465f;
    float buieKappa = 0.0f;
    float buieGamma = 0.0f;
    float sunShapeIntegral = 0.0f;   // computed in loadConfig()

    // ---- Plate proxy model ----
    uint32_t numBolts = 35;
    uint32_t numBoltsX = 7;
    uint32_t numBoltsZ = 5;
    float boltMargin = 0.08f;
    bool disableGravity = false;
    bool gravityNormalCoupling = true;  // 1 = gravity slopes enter the normal (default)

    // ---- Forward renderer flags ----
    bool rayCull = true;                // A1 per-ray angular pre-cull
    float rayCullMarginMrad = 8.0f;
};

// Numerically integrated Buie normalization constant (matches upstream).
float computeSunShapeIntegral(const Config &cfg);

// Load configuration from a JSON file. Missing keys keep their defaults; a
// missing/unreadable file returns defaults (matching upstream behaviour).
Config loadConfig(const std::string &path);

} // namespace hviz
