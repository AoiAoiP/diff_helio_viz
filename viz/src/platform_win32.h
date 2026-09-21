#pragma once

// platform_win32.h — hand-written Win32 platform layer (no GLFW/SDL).
//
// Scope: window creation, message pump, keyboard/mouse state, client-size
// queries, window-title updates. Everything the viewer needs and nothing else.

#include <cstdint>
#include <string>

// Avoid pulling all of <windows.h> into headers that do not need it.
struct HWND__;
using HWND = HWND__ *;

namespace viz {

// ------------------------------------------------------------------ input --
struct Input {
    static constexpr int kKeyCount = 256;

    bool down[kKeyCount] = {};
    bool pressed[kKeyCount] = {};   // rising edge inside the current frame
    bool released[kKeyCount] = {};

    bool mouseDown[3] = {};
    bool mousePressed[3] = {};
    bool mouseReleased[3] = {};

    float mouseX = 0.0f, mouseY = 0.0f;       // client-area pixels
    float prevMouseX = 0.0f, prevMouseY = 0.0f;
    float wheel = 0.0f;                        // accumulated notches this frame

    void beginFrame();   // copies mouse position, clears edges/wheel
    float mouseDX() const { return mouseX - prevMouseX; }
    float mouseDY() const { return mouseY - prevMouseY; }

    bool keyDown(int vk) const { return vk >= 0 && vk < kKeyCount && down[vk]; }
    bool keyPressed(int vk) const { return vk >= 0 && vk < kKeyCount && pressed[vk]; }
    bool mouseHeld(int b) const { return b >= 0 && b < 3 && mouseDown[b]; }
    bool mouseClicked(int b) const { return b >= 0 && b < 3 && mousePressed[b]; }
};

// ----------------------------------------------------------------- window --
class Window {
public:
    Window() = default;
    ~Window();
    Window(const Window &) = delete;
    Window &operator=(const Window &) = delete;

    bool create(const wchar_t *title, int width, int height);
    void destroy();

    // Drains the message queue. Returns false when the app should exit
    // (WM_CLOSE / Alt+F4 / ESC handled by the caller).
    bool pump(Input &in);

    void setTitle(const wchar_t *title);
    void setTitleUtf8(const std::string &title);

    HWND hwnd() const { return m_hwnd; }
    uint32_t width() const { return m_width; }
    uint32_t height() const { return m_height; }
    bool minimized() const { return m_minimized; }
    bool focused() const { return m_focused; }

    // True once since the last call: the client area changed size.
    bool consumeResize();

private:
    static long long __stdcall wndProc(HWND hwnd, unsigned int msg, unsigned long long wparam,
                                       long long lparam);
    long long handleMessage(unsigned int msg, unsigned long long wparam, long long lparam);

    HWND m_hwnd = nullptr;
    uint32_t m_width = 0, m_height = 0;
    bool m_minimized = false, m_focused = true, m_resized = false;
    bool m_classRegistered = false;
};

// ------------------------------------------------------------ frame clock --
// Wall-clock frame timer (QueryPerformanceCounter). The GPU-side numbers come
// from timestamp queries (gpu_timer.h); both are reported side by side.
class FrameClock {
public:
    void reset();
    double tickMs();        // ms since the previous tick
    double elapsedSec() const;

private:
    long long m_freq = 0;
    long long m_start = 0;
    long long m_last = 0;
};

} // namespace viz
