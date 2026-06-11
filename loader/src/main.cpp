#include "loader_app.h"
#include "loader_ui.h"

#include "overlay/renderer.h"

#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <Ole2.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

LoaderApp g_app;
Renderer g_renderer;
bool g_running = true;
constexpr int kDragHeaderPx = 32;

void disableWindowBlur(HWND hwnd) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
    const int policy = 1;
    DwmSetWindowAttribute(hwnd, 2, &policy, sizeof(policy));
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui::GetCurrentContext() &&
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp))
        return true;

    switch (msg) {
    case WM_NCHITTEST: {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &pt);
        if (pt.y >= 0 && pt.y < kDragHeaderPx && pt.x >= 0 && pt.x < rc.right - 40)
            return HTCAPTION;
        return HTCLIENT;
    }
    case WM_CHAR:
        loaderUiOnChar(static_cast<wchar_t>(wp));
        return 0;
    case WM_KEYDOWN:
        loaderUiOnKeyDown(wp);
        return 0;
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED && g_renderer.isReady())
            g_renderer.resize(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_DESTROY:
        g_running = false;
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool coUninit = (coHr == S_OK);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = L"crymore_loader_wnd";
    RegisterClassExW(&wc);

    constexpr int kW = 1000;
    constexpr int kH = 560;
    const int sx = GetSystemMetrics(SM_CXSCREEN);
    const int sy = GetSystemMetrics(SM_CYSCREEN);

    HWND hwnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        wc.lpszClassName,
        L"crymore.pw",
        WS_POPUP,
        (sx - kW) / 2,
        (sy - kH) / 2,
        kW, kH,
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd) {
        if (coUninit)
            CoUninitialize();
        return 1;
    }

    disableWindowBlur(hwnd);

    if (!g_renderer.init(hwnd, kW, kH)) {
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, hInstance);
        if (coUninit)
            CoUninitialize();
        return 2;
    }

    g_app.init(hwnd);
    if (!loaderUiInit(g_renderer, hwnd)) {
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, hInstance);
        if (coUninit)
            CoUninitialize();
        return 3;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    LARGE_INTEGER freq{};
    LARGE_INTEGER last{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);

    while (g_running) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT)
                g_running = false;
        }
        if (!g_running)
            break;

        if (loaderUiCloseRequested()) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            continue;
        }

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const float dt = static_cast<float>(now.QuadPart - last.QuadPart) /
                         static_cast<float>(freq.QuadPart);
        last = now;

        g_app.tick(dt);
        loaderUiRender(g_app, dt);
    }

    loaderUiShutdown();
    g_app.shutdown();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInstance);
    if (coUninit)
        CoUninitialize();
    return 0;
}
