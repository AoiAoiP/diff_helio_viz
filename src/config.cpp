#include "config.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace hviz {

// The upstream pipeline hard-codes the Taichi/scipy reference value for
// CSR = 0.01:  ∫₀^0.0436 L(θ)·θ dθ = 9.2286445021e-06.
float computeSunShapeIntegral(const Config &cfg) {
    (void)cfg;
    return 9.2286445021e-06f;
}

static std::string readFile(const std::string &path) {
    std::ifstream f(path);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Tiny flat key scanner — same approach as the upstream config.cpp. It is not a
// JSON parser; it looks up "key" then reads the first number/string after ':'.
static float extractFloat(const std::string &json, const char *key, float fallback) {
    auto pos = json.find(std::string("\"") + key + "\"");
    if (pos == std::string::npos) return fallback;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return fallback;
    const char *s = json.c_str() + pos + 1;
    char *end = nullptr;
    float v = std::strtof(s, &end);
    return (end != s) ? v : fallback;
}

static int extractInt(const std::string &json, const char *key, int fallback) {
    auto pos = json.find(std::string("\"") + key + "\"");
    if (pos == std::string::npos) return fallback;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return fallback;
    const char *s = json.c_str() + pos + 1;
    char *end = nullptr;
    long v = std::strtol(s, &end, 10);
    return (end != s) ? static_cast<int>(v) : fallback;
}

static std::string extractString(const std::string &json, const char *key, const std::string &fallback) {
    auto pos = json.find(std::string("\"") + key + "\"");
    if (pos == std::string::npos) return fallback;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return fallback;
    pos = json.find('"', pos);
    if (pos == std::string::npos) return fallback;
    auto end = json.find('"', pos + 1);
    if (end == std::string::npos) return fallback;
    return json.substr(pos + 1, end - pos - 1);
}

Config loadConfig(const std::string &path) {
    Config cfg;
    const std::string json = readFile(path);
    if (json.empty()) {
        cfg.sunShapeIntegral = computeSunShapeIntegral(cfg);
        return cfg;
    }

    cfg.sunFile = extractString(json, "sun_file", cfg.sunFile);
    cfg.ellipseFile = extractString(json, "ellipse_file", cfg.ellipseFile);
    cfg.boltFile = extractString(json, "bolt_file", cfg.boltFile);
    cfg.proxyPath = extractString(json, "proxy_path", cfg.proxyPath);
    cfg.outputDir = extractString(json, "output_dir", cfg.outputDir);

    cfg.receiverRadius = extractFloat(json, "receiver_radius", cfg.receiverRadius);
    cfg.receiverHeight = extractFloat(json, "receiver_height", cfg.receiverHeight);
    cfg.pixelWidth = static_cast<uint32_t>(extractInt(json, "pixel_width", static_cast<int>(cfg.pixelWidth)));
    cfg.pixelHeight = static_cast<uint32_t>(extractInt(json, "pixel_height", static_cast<int>(cfg.pixelHeight)));

    cfg.plateWidth = extractFloat(json, "heliostat_width", cfg.plateWidth);
    cfg.plateLength = extractFloat(json, "heliostat_length", cfg.plateLength);
    cfg.gridSize = static_cast<uint32_t>(extractInt(json, "grid_size", static_cast<int>(cfg.gridSize)));
    cfg.glassDepth = extractFloat(json, "glass_depth", cfg.glassDepth);
    cfg.refractiveIndex = extractFloat(json, "refractive_index", cfg.refractiveIndex);
    cfg.slopeError = extractFloat(json, "slope_error", cfg.slopeError);
    cfg.reflectivity = extractFloat(json, "reflectivity", cfg.reflectivity);

    {
        const std::string st = extractString(json, "sun_type", "buie");
        if (st == "pillbox") cfg.sunType = SunShapeType::PILLBOX;
        else if (st == "gaussian") cfg.sunType = SunShapeType::GAUSSIAN;
        else cfg.sunType = SunShapeType::BUIE;
    }
    cfg.dni = extractFloat(json, "dni", cfg.dni);
    cfg.csr = extractFloat(json, "csr", cfg.csr);
    cfg.sigma = extractFloat(json, "sun_sigma", cfg.sigma);
    cfg.thetaMax = extractFloat(json, "sun_theta_max", cfg.thetaMax);

    cfg.numBolts = static_cast<uint32_t>(extractInt(json, "num_bolts", static_cast<int>(cfg.numBolts)));
    cfg.numBoltsX = static_cast<uint32_t>(extractInt(json, "num_bolts_x", static_cast<int>(cfg.numBoltsX)));
    cfg.numBoltsZ = static_cast<uint32_t>(extractInt(json, "num_bolts_z", static_cast<int>(cfg.numBoltsZ)));
    cfg.boltMargin = extractFloat(json, "bolt_margin", cfg.boltMargin);
    cfg.disableGravity = extractInt(json, "disable_gravity", 0) != 0;
    cfg.gravityNormalCoupling = extractInt(json, "gravity_normal_coupling", 1) != 0;

    cfg.rayCull = extractInt(json, "ray_cull", 1) != 0;
    cfg.rayCullMarginMrad = extractFloat(json, "ray_cull_margin_mrad", cfg.rayCullMarginMrad);

    // Buie constants derived from CSR, exactly as upstream.
    cfg.buieThetaInner = 0.00465f;
    cfg.buieKappa = 0.9f * std::log(13.5f * cfg.csr) * std::pow(cfg.csr, -0.3f);
    cfg.buieGamma = 2.2f * std::log(0.52f * cfg.csr) * std::pow(cfg.csr, 0.43f) - 0.1f;
    cfg.sunShapeIntegral = computeSunShapeIntegral(cfg);
    return cfg;
}

} // namespace hviz
