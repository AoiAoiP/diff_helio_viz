#include "camera.h"

#include <windows.h>

namespace viz {

void Camera::update(const Input &in, float dt) {
    const bool orbit = in.mouseHeld(0);
    const bool pan = in.mouseHeld(1) || in.mouseHeld(2);
    const bool shift = in.keyDown(VK_SHIFT);

    if (orbit) {
        m_yawDeg -= in.mouseDX() * 0.25f;
        m_pitchDeg = clampf(m_pitchDeg + in.mouseDY() * 0.25f, -85.0f, 85.0f);
        while (m_yawDeg > 360.0f) m_yawDeg -= 360.0f;
        while (m_yawDeg < -360.0f) m_yawDeg += 360.0f;
    }
    if (pan) {
        const float s = m_distance * 0.0016f;
        m_target += right() * (-in.mouseDX() * s);
        m_target += up() * (in.mouseDY() * s);
    }
    if (in.wheel != 0.0f) {
        // Zoom proportional to the distance: the scene spans 300 m *and* a 13 m
        // plate, so a linear zoom step would be useless at one end.
        m_distance = clampf(m_distance * std::pow(0.9f, in.wheel), 2.0f, 1200.0f);
    }

    // WASD / QE free fly. The two speeds cover the two working scales.
    const float scale = shift ? 8.0f : 1.0f;
    const float speed = m_moveSpeed * scale * dt;
    Vec3 move{0, 0, 0};
    if (in.keyDown('W')) move += forward() * speed;
    if (in.keyDown('S')) move -= forward() * speed;
    if (in.keyDown('A')) move -= right() * speed;
    if (in.keyDown('D')) move += right() * speed;
    if (in.keyDown('Q')) move.y -= speed;
    if (in.keyDown('E')) move.y += speed;
    m_target += move;

    m_fovY = clampf(m_fovY, 8.0f * 3.14159265358979f / 180.0f, 100.0f * 3.14159265358979f / 180.0f);
}

void Camera::applyPreset(int index, const Vec3 &platePos, const Vec3 &receiverPos, const Vec3 &plateNormal) {
    // The scene spans 300 m horizontally and 180 m vertically; the plate is
    // 12.84 x 9.45 m. Presets are tuned so a single key press frames each story
    // the demo tells (PLAN.md §9).
    //
    // 'platePos'/'plateNormal' describe the *selected* field mirror, which may be
    // north, east, south or west of the tower, so nothing here may assume the
    // built-in north mirror. The plate closeup in particular has to sit on the
    // *reflecting* side of the surface (along +normal): framing it from behind hides
    // everything that lives on the front face -- the actuator markers, the glass
    // coating and the specular highlight -- which is exactly what used to happen
    // (the old fixed yaw of 196 deg put the camera behind the north mirror).
    const Vec3 mid = lerp(platePos, receiverPos, 0.5f);
    const float bearing = std::atan2(platePos.x, platePos.z) * 180.0f / 3.14159265358979f;
    // Azimuth/elevation of the surface normal, i.e. where the mirror is looking.
    const float nAz = std::atan2(plateNormal.x, plateNormal.z) * 180.0f / 3.14159265358979f;
    const float nEl = std::asin(clampf(plateNormal.y, -1.0f, 1.0f)) * 180.0f / 3.14159265358979f;
    switch (index) {
    case 0:   // overview: whole field, sun low in frame
        m_target = Vec3{0.0f, 90.0f, -150.0f};
        setOrbit(180.0f, 18.0f, 300.0f);
        m_presetName = "overview";
        break;
    case 1:   // plate closeup: the reflecting face, deformation and actuators visible
        m_target = platePos;
        setOrbit(nAz + 22.0f, clampf(nEl + 14.0f, -80.0f, 80.0f), 16.0f);
        m_presetName = "plate closeup";
        break;
    case 2:   // receiver closeup: flux heat map + bloom, seen from the mirror's side
        // Look straight at the receiver axis: the spot sits on the face that points
        // at the selected mirror, and the orbit yaw is that mirror's bearing.
        m_target = receiverPos + Vec3{0.0f, -2.0f, 0.0f};
        setOrbit(bearing + 20.0f, 2.0f, 34.0f);
        m_presetName = "receiver spot";
        break;
    case 3:   // cinematic side view: beams from the side
        m_target = mid + Vec3{0.0f, -10.0f, 0.0f};
        setOrbit(bearing + 60.0f, 4.0f, 190.0f);
        m_presetName = "beam side";
        break;
    default:
        break;
    }
}

Mat4 Camera::viewMatrix() const {
    return lookAt(eye(), m_target, Vec3{0.0f, 1.0f, 0.0f});
}

Mat4 Camera::projMatrix(float aspect) const {
    return perspectiveReversedZ(m_fovY, aspect, m_near);
}

} // namespace viz
