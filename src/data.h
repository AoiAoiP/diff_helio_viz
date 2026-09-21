#pragma once

// data.h — asset loaders and small physics helpers.
//
// Everything here mirrors the upstream optimizer's loading code so that numbers
// produced by this project stay comparable with the research pipeline
// (see NOTES_provenance.md for the exact upstream locations).

#include "config.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace hviz {

struct HeliostatConfig {
    std::string name;
    std::array<float, 3> position{};
    float A = 0.0f, B = 0.0f, C = 0.0f;  // ideal ellipse w = A x^2 + B z^2 + C x z
};

// ---- Text assets -----------------------------------------------------------

// data/sundir/*.txt : one direction per line (x y z), extra columns ignored,
// automatically normalized.
std::vector<std::array<float, 3>> loadSunDirections(const std::string &path);

// data/ellipse_*.txt : "name x y z A B C"
std::vector<HeliostatConfig> loadHeliostats(const std::string &path);

// Bolt heights. Accepts the three formats found in the upstream repo:
//   1 column  -> value                   (stroke, shader convention)
//   2 columns -> "idx value"             (value used)
//   3 columns -> "idx h_pipe h_stroke"   (h_stroke used, i.e. the last column)
// '#' comments and blank lines are skipped.
std::vector<float> loadBolts(const std::string &path, uint32_t numBolts);

// ---- Binary proxy data -----------------------------------------------------

// influence_phi{,_u,_v}.bin : numBolts * gridSize^2 float32, indexed [bolt][v][u]
std::vector<float> loadInfluencePlane(const std::string &dir, const char *fileName,
                                      uint32_t numBolts, uint32_t gridSize);

// gravity_<angle>deg.bin for the 20 known bins, merged into one buffer:
//   [bin][plane][gridIdx]  with plane = {w, dw/du, dw/dv}, each gridSize^2 floats
// Legacy single-plane bins (4096 B) are accepted: du/dv stay zero.
std::vector<float> loadGravityMerged(const std::string &dir, uint32_t gridSize);

extern const float kGravityAnglesDeg[20];

// ---- Small physics helpers (must match upstream exactly) -------------------

// Gravity tilt angle of the plate: cos(theta) = |n.y| / |n| with
// n = normalize(sunDir) + normalize(aimPoint - heliostatPos).
float computeCosTheta(const std::array<float, 3> &sunDir, const std::array<float, 3> &heliostatPos,
                      const std::array<float, 3> &aimPoint);

// Aim point on the receiver cylinder facing the heliostat.
std::array<float, 3> computeAimPoint(const std::array<float, 3> &heliostatPos,
                                     const std::array<float, 3> &receiverPos, float receiverRadius);

// Two-bin linear interpolation indices for the gravity field.
void packGravityParams(float cosTheta, uint32_t &lo, uint32_t &hi, float &t);

// Receiver pixel area used by S95 reporting.
float computePixelArea(const Config &cfg);

// ---- Flux helpers ----------------------------------------------------------

// Completely independent CPU reference implementation of the S95 threshold
// search (same 20-step bisection as upstream). Used to cross-check the GPU
// implementation found in shaders/s95_gpu.slang.
float computeS95LevelCPU(const std::vector<float> &flux);

// Number of receiver pixels at or above 'level', times pixel area (m^2).
float computeS95Area(const std::vector<float> &flux, float level, float pixelArea);

// NPY (float32, C-order, 2D) writer — same header layout as upstream main.cpp.
void saveNpy2D(const std::string &path, const std::vector<float> &data, uint32_t width, uint32_t height);

// Read back a NPY float32 file written by saveNpy2D (used for parity checks).
bool loadNpy2D(const std::string &path, std::vector<float> &data, uint32_t &width, uint32_t &height);

// ---- Path resolution -------------------------------------------------------

// Directory of the running executable (so assets can be found regardless of cwd).
std::string executableDir();

// Resolve an asset path written relative to the project root (as in the config
// files) no matter which directory the binary is launched from. Tries, in order:
// the path as given, the config file's directory and its parents, and the
// executable directory and its parents. Returns the first candidate that exists,
// otherwise the original path (so the error message stays informative).
std::string resolveAssetPath(const std::string &path, const std::string &configPath,
                             const std::string &exeDir);

} // namespace hviz
