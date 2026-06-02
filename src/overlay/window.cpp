#include "window.h"
#include <dwmapi.h>
#include <iostream>
#include <imgui.h>
#include <cstdlib>
#include <ctime>
#include <string>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static std::wstring randomWindowName(int len) {
    static const wchar_t chars[] = L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::wstring s;
    s.reserve(len);
    for (int i = 0; i < len; ++i)
        s += chars[rand() % (sizeof(chars)/sizeof(wchar_t) - 1)];
    return s;
}

// ─── WndProc ───────────────────────────────────────────────────────────────────

LRESULT CALLBACK OverlayWindow::wndProc(HWND hwnd, UINT msg,
                                         WPARAM wp, LPARAM lp)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp))
        return true;

    switch (msg) {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ─── Lifecycle ─────────────────────────────────────────────────────────────────

OverlayWindow::~OverlayWindow() {
    if (m_hwnd) DestroyWindow(m_hwnd);
}

bool OverlayWindow::create(HINSTANCE hInstance, int width, int height) {
    std::srand(static_cast<unsigned>(std::time(nullptr)));

    m_width = width; m_height = height;
    m_left = 0; m_top = 0;

    std::wstring className = randomWindowName(12);
    std::wstring windowTitle = randomWindowName(10);

    WNDCLASSEXW wc{};
    wc.cbSize       = sizeof(wc);
    wc.style        = 0;
    wc.lpfnWndProc  = wndProc;
    wc.hInstance    = hInstance;
    wc.lpszClassName = className.c_str();
    RegisterClassExW(&wc);

    constexpr DWORD exStyle =
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;

    m_hwnd = CreateWindowExW(exStyle, className.c_str(), windowTitle.c_str(),
                              WS_POPUP, 0, 0, width, height,
                              nullptr, nullptr, hInstance, nullptr);
    if (!m_hwnd) { std::cerr << "[Window] CreateWindowEx failed\n"; return false; }

    // Prevent screen capture tools (Discord, OBS) from capturing the overlay.
    SetWindowDisplayAffinity(m_hwnd, WDA_MONITOR);

    SetLayeredWindowAttributes(m_hwnd, RGB(0, 0, 0), BYTE(255), LWA_ALPHA);
    MARGINS margins = { -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(m_hwnd, &margins);

    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(m_hwnd);
    std::cout << "[Window] Created " << width << "x" << height << "\n";
    return true;
}

// ─── Click-through toggle ──────────────────────────────────────────────────────

void OverlayWindow::setClickThrough(bool on) {
    LONG_PTR style = GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
    if (on) style |=  WS_EX_TRANSPARENT;
    else    style &= ~WS_EX_TRANSPARENT;
    SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, style);
}

// ─── Sync ──────────────────────────────────────────────────────────────────────

void OverlayWindow::syncWithGameWindow(HWND game) {
    if (!game || !m_hwnd) return;
    RECT rc;
    // DwmGetWindowAttribute with DWMWA_EXTENDED_FRAME_BOUNDS gives the true
    // window region including the DWM shadow area; for fullscreen-windowed
    // CS2 it matches the visible client region more accurately than GetWindowRect.
    HRESULT hr = DwmGetWindowAttribute(game, DWMWA_EXTENDED_FRAME_BOUNDS, &rc, sizeof(rc));
    if (FAILED(hr))
        hr = GetWindowRect(game, &rc) ? S_OK : E_FAIL;
    if (SUCCEEDED(hr)) {
        int w = rc.right - rc.left, h = rc.bottom - rc.top;
        if (rc.left != m_left || rc.top != m_top || w != m_width || h != m_height) {
            SetWindowPos(
                m_hwnd,
                nullptr,
                rc.left,
                rc.top,
                w,
                h,
                SWP_NOZORDER | SWP_NOACTIVATE
            );
            m_left = rc.left;
            m_top = rc.top;
            m_width = w;
            m_height = h;
        }
    }
}

// ─── Messages ──────────────────────────────────────────────────────────────────

bool OverlayWindow::processMessages() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return false;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return true;
}
