// platform_win32.cpp — window, message pump and input. Hand-written: the project
// deliberately links nothing but vulkan-1.lib and the Win32 import libraries.

#include "platform_win32.h"

#include <windows.h>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM

#include <cstdio>

namespace viz {

// ------------------------------------------------------------------ input --
void Input::beginFrame() {
    prevMouseX = mouseX;
    prevMouseY = mouseY;
    for (int i = 0; i < kKeyCount; i++) {
        pressed[i] = false;
        released[i] = false;
    }
    for (int i = 0; i < 3; i++) {
        mousePressed[i] = false;
        mouseReleased[i] = false;
    }
    wheel = 0.0f;
}

// ----------------------------------------------------------------- window --
namespace {
const wchar_t *kClassName = L"HeliostatStudioWindow";
Input *g_input = nullptr;   // valid only inside Window::pump()
} // namespace

Window::~Window() { destroy(); }

bool Window::create(const wchar_t *title, int width, int height) {
    SetProcessDPIAware();   // 1:1 pixel<->client-area mapping for the swapchain

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;   // we present every frame
    wc.lpszClassName = kClassName;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!RegisterClassExW(&wc)) {
        std::fprintf(stderr, "[win32] RegisterClassExW failed (%lu)\n", GetLastError());
        return false;
    }
    m_classRegistered = true;

    RECT rect{0, 0, width, height};
    const DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRect(&rect, style, FALSE);

    m_hwnd = CreateWindowExW(0, kClassName, title, style, CW_USEDEFAULT, CW_USEDEFAULT,
                             rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, inst, this);
    if (!m_hwnd) {
        std::fprintf(stderr, "[win32] CreateWindowExW failed (%lu)\n", GetLastError());
        return false;
    }
    m_width = static_cast<uint32_t>(width);
    m_height = static_cast<uint32_t>(height);
    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);
    return true;
}

void Window::destroy() {
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    if (m_classRegistered) {
        UnregisterClassW(kClassName, GetModuleHandleW(nullptr));
        m_classRegistered = false;
    }
}

bool Window::pump(Input &in) {
    g_input = &in;
    in.beginFrame();

    MSG msg;
    bool quit = false;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            quit = true;
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_input = nullptr;
    return !quit;
}

void Window::setTitle(const wchar_t *title) {
    if (m_hwnd) SetWindowTextW(m_hwnd, title);
}

void Window::setTitleUtf8(const std::string &title) {
    if (!m_hwnd) return;
    wchar_t buf[512];
    const int n = MultiByteToWideChar(CP_UTF8, 0, title.c_str(), static_cast<int>(title.size()), buf,
                                      static_cast<int>(std::size(buf)) - 1);
    if (n <= 0) return;
    buf[n] = 0;
    SetWindowTextW(m_hwnd, buf);
}

bool Window::consumeResize() {
    const bool r = m_resized;
    m_resized = false;
    return r;
}

long long __stdcall Window::wndProc(HWND hwnd, unsigned int msg, unsigned long long wparam,
                                   long long lparam) {
    Window *self = nullptr;
    if (msg == WM_NCCREATE) {
        auto *cs = reinterpret_cast<CREATESTRUCTW *>(lparam);
        self = static_cast<Window *>(cs->lpCreateParams);
        self->m_hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<Window *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wparam, lparam);
    return self->handleMessage(msg, wparam, lparam);
}

long long Window::handleMessage(unsigned int msg, unsigned long long wparam, long long lparam) {
    auto wake = [&](bool pressed, int vk) {
        if (!g_input || vk < 0 || vk >= Input::kKeyCount) return;
        if (pressed) {
            if (!g_input->down[vk]) g_input->pressed[vk] = true;
            g_input->down[vk] = true;
        } else {
            if (g_input->down[vk]) g_input->released[vk] = true;
            g_input->down[vk] = false;
        }
    };

    switch (msg) {
    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        m_minimized = (wparam == SIZE_MINIMIZED);
        if (!m_minimized) {
            const uint32_t w = LOWORD(lparam), h = HIWORD(lparam);
            if (w != m_width || h != m_height) {
                m_width = w;
                m_height = h;
                m_resized = true;
            }
        }
        return 0;
    case WM_GETMINMAXINFO: {
        auto *mmi = reinterpret_cast<MINMAXINFO *>(lparam);
        mmi->ptMinTrackSize.x = 320;
        mmi->ptMinTrackSize.y = 240;
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SETFOCUS:
        m_focused = true;
        return 0;
    case WM_KILLFOCUS:
        m_focused = false;
        // Drop every key so a stuck modifier cannot survive an alt-tab.
        if (g_input) {
            for (int i = 0; i < Input::kKeyCount; i++) g_input->down[i] = false;
            for (int i = 0; i < 3; i++) g_input->mouseDown[i] = false;
        }
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        wake(true, static_cast<int>(wparam & 0xFF));
        if (msg == WM_SYSKEYDOWN && (wparam & 0xFF) == VK_F10) return 0;   // no system menu
        return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        wake(false, static_cast<int>(wparam & 0xFF));
        return 0;
    case WM_MOUSEMOVE:
        if (g_input) {
            g_input->mouseX = static_cast<float>(GET_X_LPARAM(lparam));
            g_input->mouseY = static_cast<float>(GET_Y_LPARAM(lparam));
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN: {
        const int b = msg == WM_LBUTTONDOWN ? 0 : (msg == WM_RBUTTONDOWN ? 1 : 2);
        if (g_input) {
            // A click can arrive without a preceding WM_MOUSEMOVE (a synthetic
            // click, or a fast click right after the pointer warped), so take the
            // position from this message too: the UI hit test reads mouseX/mouseY.
            g_input->mouseX = static_cast<float>(GET_X_LPARAM(lparam));
            g_input->mouseY = static_cast<float>(GET_Y_LPARAM(lparam));
            if (!g_input->mouseDown[b]) g_input->mousePressed[b] = true;
            g_input->mouseDown[b] = true;
        }
        SetCapture(m_hwnd);
        return 0;
    }
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP: {
        const int b = msg == WM_LBUTTONUP ? 0 : (msg == WM_RBUTTONUP ? 1 : 2);
        if (g_input) {
            g_input->mouseX = static_cast<float>(GET_X_LPARAM(lparam));
            g_input->mouseY = static_cast<float>(GET_Y_LPARAM(lparam));
            if (g_input->mouseDown[b]) g_input->mouseReleased[b] = true;
            g_input->mouseDown[b] = false;
            if (!g_input->mouseDown[0] && !g_input->mouseDown[1] && !g_input->mouseDown[2]) ReleaseCapture();
        }
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (g_input) g_input->wheel += static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / 120.0f;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(m_hwnd, msg, wparam, lparam);
}

// ------------------------------------------------------------ frame clock --
void FrameClock::reset() {
    LARGE_INTEGER f, n;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&n);
    m_freq = f.QuadPart;
    m_start = n.QuadPart;
    m_last = n.QuadPart;
}

double FrameClock::tickMs() {
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    const double ms = static_cast<double>(n.QuadPart - m_last) * 1000.0 / static_cast<double>(m_freq);
    m_last = n.QuadPart;
    return ms;
}

double FrameClock::elapsedSec() const {
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    return static_cast<double>(n.QuadPart - m_start) / static_cast<double>(m_freq);
}

} // namespace viz
