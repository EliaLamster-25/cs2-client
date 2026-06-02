#pragma once

#include <Windows.h>

/// @file window.h
/// @brief Transparent, click-through, always-on-top overlay window.
///
/// Creates a WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST window
/// that renders on top of CS2.  CS2 must be in "Fullscreen Windowed" or
/// "Windowed" mode for this to work.

class OverlayWindow {
public:
    OverlayWindow() = default;
    ~OverlayWindow();

    /// Create the overlay window and show it.
    bool create(HINSTANCE hInstance, int width, int height);

    /// Reposition the overlay to match the game window.
    void syncWithGameWindow(HWND gameWindow);

    /// Process pending window messages.  Returns false when WM_QUIT received.
    bool processMessages();

    void setClickThrough(bool clickThrough);

    HWND hwnd()   const { return m_hwnd; }
    int  width()  const { return m_width; }
    int  height() const { return m_height; }

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);

    HWND m_hwnd   = nullptr;
    int  m_left   = 0;
    int  m_top    = 0;
    int  m_width  = 0;
    int  m_height = 0;
};
