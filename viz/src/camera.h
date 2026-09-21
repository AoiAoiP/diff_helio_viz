#pragma once

// camera.h — orbit camera + free fly, plus the recording presets (F1-F4).

#include "math_viz.h"
#include "platform_win32.h"

namespace viz {

class Camera {
public:
    void setTarget(const Vec3 &t) { m_target = t; }
    void setOrbit(float yawDeg, float pitchDeg, float distance) {
        m_yawDeg = yawDeg;
        m_pitchDeg = pitchDeg;
        m_distance = distance;
    }
    void setFov(float degrees) { m_fovY = degrees; }
    void setNear(float n) { m_near = n; }

    // Mouse drag / wheel / WASD. 'dt' is the frame time in seconds.
    void update(const Input &in, float dt);

    // plateNormal = the macro normal of the selected mirror's reflecting face (the
    // closeup has to be framed from that side).
    void applyPreset(int index, const Vec3 &platePos, const Vec3 &receiverPos, const Vec3 &plateNormal);

    Vec3 eye() const { return m_target + offset(); }
    Vec3 target() const { return m_target; }
    Vec3 forward() const { return (m_target - eye()).normalized(); }
    Vec3 right() const { return forward().cross(Vec3{0, 1, 0}).normalized(); }
    Vec3 up() const { return right().cross(forward()); }

    float fovY() const { return m_fovY; }
    float nearPlane() const { return m_near; }
    float distance() const { return m_distance; }
    float yawDeg() const { return m_yawDeg; }
    float pitchDeg() const { return m_pitchDeg; }
    const char *presetName() const { return m_presetName; }

    Mat4 viewMatrix() const;
    Mat4 projMatrix(float aspect) const;
    Mat4 viewProjMatrix(float aspect) const { return projMatrix(aspect) * viewMatrix(); }

private:
    Vec3 offset() const {
        const float y = m_yawDeg * 3.14159265358979f / 180.0f;
        const float p = m_pitchDeg * 3.14159265358979f / 180.0f;
        return Vec3{std::cos(p) * std::sin(y), std::sin(p), std::cos(p) * std::cos(y)} * m_distance;
    }

    Vec3 m_target{0.0f, 90.0f, -150.0f};
    float m_yawDeg = 180.0f;
    float m_pitchDeg = 20.0f;
    float m_distance = 260.0f;
    float m_fovY = 45.0f * 3.14159265358979f / 180.0f;
    float m_near = 1.0f;
    float m_moveSpeed = 60.0f;
    const char *m_presetName = "overview";
};

} // namespace viz
