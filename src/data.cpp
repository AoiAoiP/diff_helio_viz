#include "data.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace hviz {

const float kGravityAnglesDeg[20] = {10.0f, 14.0f, 18.0f, 22.0f, 26.0f, 30.0f, 34.0f, 38.0f, 42.0f, 46.0f,
                                     50.0f, 54.0f, 58.0f, 62.0f, 66.0f, 70.0f, 73.0f, 76.0f, 78.0f, 80.0f};

std::vector<std::array<float, 3>> loadSunDirections(const std::string &path) {
    std::vector<std::array<float, 3>> out;
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Cannot open sun file: " + path);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        float x, y, z;
        if (ss >> x >> y >> z) {
            const float len = std::sqrt(x * x + y * y + z * z);
            if (len > 0.0f) out.push_back({x / len, y / len, z / len});
        }
    }
    if (out.empty()) throw std::runtime_error("No sun directions in " + path);
    return out;
}

std::vector<HeliostatConfig> loadHeliostats(const std::string &path) {
    std::vector<HeliostatConfig> out;
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Cannot open ellipse file: " + path);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        HeliostatConfig hc;
        if (ss >> hc.name >> hc.position[0] >> hc.position[1] >> hc.position[2] >> hc.A >> hc.B >> hc.C)
            out.push_back(hc);
    }
    if (out.empty()) throw std::runtime_error("No heliostats in " + path);
    return out;
}

std::vector<float> loadBolts(const std::string &path, uint32_t numBolts) {
    std::vector<float> values;
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Cannot open bolt file: " + path);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::vector<float> cols;
        float v;
        while (ss >> v) cols.push_back(v);
        if (cols.empty()) continue;
        // 1 col: value | 2 cols: idx value | 3 cols: idx h_pipe h_stroke
        values.push_back(cols.back());
    }
    if (values.size() != numBolts) {
        throw std::runtime_error("Bolt file " + path + " has " + std::to_string(values.size()) +
                                 " values, expected " + std::to_string(numBolts));
    }
    return values;
}

static std::vector<float> readFloatBin(const std::string &path, size_t expectedFloats, bool hardCheck) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("Cannot open binary asset: " + path);
    const size_t bytes = static_cast<size_t>(f.tellg());
    f.seekg(0);
    if (hardCheck && bytes != expectedFloats * sizeof(float)) {
        throw std::runtime_error(path + ": size " + std::to_string(bytes) + " B != expected " +
                                 std::to_string(expectedFloats * sizeof(float)) + " B");
    }
    std::vector<float> data(expectedFloats, 0.0f);
    f.read(reinterpret_cast<char *>(data.data()), std::min(bytes, expectedFloats * sizeof(float)));
    return data;
}

std::vector<float> loadInfluencePlane(const std::string &dir, const char *fileName,
                                      uint32_t numBolts, uint32_t gridSize) {
    const size_t n = static_cast<size_t>(numBolts) * gridSize * gridSize;
    return readFloatBin(dir + "/" + fileName, n, /*hardCheck=*/true);
}

std::vector<float> loadGravityMerged(const std::string &dir, uint32_t gridSize) {
    const size_t gridPts = static_cast<size_t>(gridSize) * gridSize;
    const size_t planeSize = gridPts * sizeof(float);
    const size_t binSize3 = 3 * planeSize;
    std::vector<float> merged(20 * 3 * gridPts, 0.0f);
    bool anyLegacy = false;

    for (int i = 0; i < 20; i++) {
        std::string path = dir + "/gravity_" + std::to_string(static_cast<int>(kGravityAnglesDeg[i])) + "deg.bin";
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        size_t fileSize = 0;
        if (f) fileSize = static_cast<size_t>(f.tellg());
        if (f) f.seekg(0);

        float *dst = merged.data() + static_cast<size_t>(i) * 3 * gridPts;
        if (!f) {
            throw std::runtime_error("Missing gravity bin: " + path);
        } else if (fileSize == binSize3) {
            f.read(reinterpret_cast<char *>(dst), binSize3);  // 3-plane v2 format
        } else if (fileSize == planeSize) {
            anyLegacy = true;
            f.read(reinterpret_cast<char *>(dst), planeSize);  // legacy: du/dv remain 0
        } else {
            throw std::runtime_error(path + ": unexpected gravity bin size " + std::to_string(fileSize));
        }
    }
    if (anyLegacy) {
        std::fprintf(stderr,
                     "[warn] legacy 1-plane gravity bins detected (du/dv = 0). Regenerate with:\n"
                     "       python scripts/generate_proxy_model.py gravity\n");
    }
    return merged;
}

float computeCosTheta(const std::array<float, 3> &sunDir, const std::array<float, 3> &heliostatPos,
                      const std::array<float, 3> &aimPoint) {
    const float sl = std::sqrt(sunDir[0] * sunDir[0] + sunDir[1] * sunDir[1] + sunDir[2] * sunDir[2]);
    const float rx = aimPoint[0] - heliostatPos[0];
    const float ry = aimPoint[1] - heliostatPos[1];
    const float rz = aimPoint[2] - heliostatPos[2];
    const float rl = std::sqrt(rx * rx + ry * ry + rz * rz);
    const float ny = sunDir[1] / sl + ry / rl;
    const float nx = sunDir[0] / sl + rx / rl;
    const float nz = sunDir[2] / sl + rz / rl;
    const float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
    return std::abs(ny) / nl;
}

std::array<float, 3> computeAimPoint(const std::array<float, 3> &heliostatPos,
                                     const std::array<float, 3> &receiverPos, float receiverRadius) {
    const float dlen = std::sqrt(heliostatPos[0] * heliostatPos[0] + heliostatPos[2] * heliostatPos[2]);
    if (dlen > 1e-6f) {
        return {heliostatPos[0] / dlen * receiverRadius, receiverPos[1], heliostatPos[2] / dlen * receiverRadius};
    }
    return {0.0f, receiverPos[1], receiverRadius};
}

void packGravityParams(float cosTheta, uint32_t &lo, uint32_t &hi, float &t) {
    const float angDeg = std::acos(std::max(0.0f, std::min(1.0f, cosTheta))) * 180.0f / 3.14159265f;
    if (angDeg <= kGravityAnglesDeg[0]) {
        lo = 0; hi = 0; t = 0.0f;
    } else if (angDeg >= kGravityAnglesDeg[19]) {
        lo = 19; hi = 19; t = 0.0f;
    } else {
        for (int j = 0; j < 19; j++) {
            if (angDeg >= kGravityAnglesDeg[j] && angDeg <= kGravityAnglesDeg[j + 1]) {
                lo = static_cast<uint32_t>(j);
                hi = static_cast<uint32_t>(j + 1);
                t = (angDeg - kGravityAnglesDeg[j]) / (kGravityAnglesDeg[j + 1] - kGravityAnglesDeg[j]);
                return;
            }
        }
        lo = 0; hi = 0; t = 0.0f;
    }
}

float computePixelArea(const Config &cfg) {
    return (2.0f * 3.14159265f * cfg.receiverRadius * cfg.receiverHeight) /
           static_cast<float>(cfg.pixelWidth * cfg.pixelHeight);
}

float computeS95LevelCPU(const std::vector<float> &flux) {
    float total = 0.0f, maxVal = 0.0f;
    for (float f : flux) {
        total += f;
        if (f > maxVal) maxVal = f;
    }
    if (total <= 1e-6f) return 0.0f;
    float low = 0.0f, high = maxVal, level = 0.0f;
    for (int i = 0; i < 20; i++) {
        const float mid = (low + high) * 0.5f;
        float sumAbove = 0.0f;
        for (float f : flux)
            if (f > mid) sumAbove += f;
        if (sumAbove / total > 0.95f) {
            low = mid;
        } else {
            high = mid;
            level = mid;
        }
    }
    return level;
}

float computeS95Area(const std::vector<float> &flux, float level, float pixelArea) {
    if (level <= 0.0f) return 0.0f;
    int count = 0;
    for (float f : flux)
        if (f >= level) count++;
    return static_cast<float>(count) * pixelArea;
}

void saveNpy2D(const std::string &path, const std::vector<float> &data, uint32_t width, uint32_t height) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot write " + path);
    f.write("\x93NUMPY", 6);
    f.put(1);
    f.put(0);
    std::string header = "{'descr': '<f4', 'fortran_order': False, 'shape': (" + std::to_string(height) + ", " +
                         std::to_string(width) + ")}";
    const int pad = 16 - ((10 + static_cast<int>(header.size()) + 1) % 16);
    for (int i = 0; i < pad; i++) header += ' ';
    header += '\n';
    const uint16_t headerLen = static_cast<uint16_t>(header.size());
    f.write(reinterpret_cast<const char *>(&headerLen), 2);
    f.write(header.data(), static_cast<std::streamsize>(header.size()));
    f.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size() * sizeof(float)));
}

bool loadNpy2D(const std::string &path, std::vector<float> &data, uint32_t &width, uint32_t &height) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    char magic[6];
    f.read(magic, 6);
    if (std::memcmp(magic, "\x93NUMPY", 6) != 0) return false;
    f.get();  // major
    f.get();  // minor
    uint16_t headerLen = 0;
    f.read(reinterpret_cast<char *>(&headerLen), 2);
    std::string header(headerLen, '\0');
    f.read(header.data(), headerLen);
    // Parse "shape": (H, W)
    auto shapePos = header.find("'shape': (");
    if (shapePos == std::string::npos) return false;
    const char *s = header.c_str() + shapePos + 10;
    char *end = nullptr;
    const long h = std::strtol(s, &end, 10);
    if (end == s) return false;
    s = end;
    while (*s == ',' || *s == ' ') s++;
    const long w = std::strtol(s, &end, 10);
    if (end == s) return false;
    height = static_cast<uint32_t>(h);
    width = static_cast<uint32_t>(w);
    data.resize(static_cast<size_t>(width) * height);
    f.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size() * sizeof(float)));
    return static_cast<bool>(f);
}

// ------------------------------------------------------------ path helpers --

std::string executableDir() {
#ifdef _WIN32
    char buf[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0) return ".";
    std::string full(buf, n);
    const auto slash = full.find_last_of("\\/");
    return slash == std::string::npos ? std::string(".") : full.substr(0, slash);
#else
    return ".";
#endif
}

std::string resolveAssetPath(const std::string &path, const std::string &configPath,
                             const std::string &exeDir) {
    if (path.empty()) return path;
    if (fs::exists(path)) return path;  // already valid relative to the cwd

    // Candidate roots: the config file's directory and its parents, then the
    // executable directory and its parents (covers "run from build/Release").
    std::vector<fs::path> roots;
    auto addChain = [&](fs::path start, int levels) {
        for (int i = 0; i < levels; i++) {
            roots.push_back(start);
            if (!start.has_parent_path() || start.parent_path() == start) break;
            start = start.parent_path();
        }
    };
    addChain(fs::absolute(fs::path(configPath)).parent_path(), 4);
    addChain(fs::path(exeDir), 4);

    for (const auto &root : roots) {
        const fs::path candidate = root / path;
        if (fs::exists(candidate)) return candidate.string();
    }
    return path;  // unchanged: the caller's error message stays informative
}

} // namespace hviz
