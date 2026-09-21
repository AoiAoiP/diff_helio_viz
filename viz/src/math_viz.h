#pragma once

// math_viz.h — the tiny vector/matrix toolkit the viewer needs (no glm).
//
// Conventions (matching PLAN.md §6.3):
//   * world units are metres, right-handed, +Y up;
//   * matrices are plain float[16] **row-major** and are uploaded as four
//     float4 rows, so the shader can do dot(p, rowN) explicitly. That avoids any
//     ambiguity about Slang/HLSL vs SPIR-V matrix layout;
//   * the projection is reversed-Z with an infinite far plane: near -> 1,
//     infinity -> 0, depth test GREATER, depth clear 0.

#include <cmath>
#include <cstdint>

namespace viz {

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    Vec3() = default;
    Vec3(float a, float b, float c) : x(a), y(b), z(c) {}
    Vec3 operator+(const Vec3 &o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3 &o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 &operator+=(const Vec3 &o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3 &operator-=(const Vec3 &o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    float dot(const Vec3 &o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 cross(const Vec3 &o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
    float length() const { return std::sqrt(x * x + y * y + z * z); }
    Vec3 normalized() const {
        const float l = length();
        return l > 1e-20f ? Vec3{x / l, y / l, z / l} : Vec3{0, 0, 0};
    }
};

inline Vec3 lerp(const Vec3 &a, const Vec3 &b, float t) { return a * (1.0f - t) + b * t; }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Row-major 4x4. Element (r, c) is m[r * 4 + c]; the shader receives rows.
struct Mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    static Mat4 identity() { return Mat4{}; }

    // Standard (column-vector) product: result = a * b, applied as a*(b*p).
    Mat4 operator*(const Mat4 &b) const {
        Mat4 r;
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++) {
                float s = 0.0f;
                for (int k = 0; k < 4; k++) s += m[i * 4 + k] * b.m[k * 4 + j];
                r.m[i * 4 + j] = s;
            }
        return r;
    }

    void setRow(int row, float a, float b, float c, float d) {
        m[row * 4 + 0] = a;
        m[row * 4 + 1] = b;
        m[row * 4 + 2] = c;
        m[row * 4 + 3] = d;
    }
};

// Right-handed look-at (camera looks down -Z in view space).
inline Mat4 lookAt(const Vec3 &eye, const Vec3 &target, const Vec3 &up) {
    const Vec3 f = (target - eye).normalized();
    const Vec3 r = f.cross(up).normalized();
    const Vec3 u = r.cross(f);
    Mat4 v;
    v.setRow(0, r.x, r.y, r.z, -r.dot(eye));
    v.setRow(1, u.x, u.y, u.z, -u.dot(eye));
    v.setRow(2, -f.x, -f.y, -f.z, f.dot(eye));
    v.setRow(3, 0, 0, 0, 1);
    return v;
}

// Reversed-Z, infinite far plane. near -> 1, infinity -> 0.
inline Mat4 perspectiveReversedZ(float fovYRadians, float aspect, float nearPlane) {
    const float f = 1.0f / std::tan(fovYRadians * 0.5f);
    Mat4 p;
    p.setRow(0, f / (aspect > 1e-6f ? aspect : 1.0f), 0, 0, 0);
    p.setRow(1, 0, f, 0, 0);
    p.setRow(2, 0, 0, 0, nearPlane);
    p.setRow(3, 0, 0, -1, 0);
    return p;
}

// NOTE: no general 4x4 inverse here on purpose. The sky pass reconstructs its
// view ray from the camera basis vectors (forward/right/up + tan(fov/2)) instead
// of from an inverse view-projection matrix, which is both cheaper and one less
// place to get a convention wrong.

} // namespace viz
