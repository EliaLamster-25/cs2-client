#include "input/input_router.h"
#include "config.h"

#include <cstdint>
#include <string>
#include <winuser.h>

namespace input_router {
namespace {

using FnInjectMouse = BOOL(NTAPI*)(void*, int);
using FnInjectKeyboard = BOOL(NTAPI*)(void*, int);

static std::string decodeObf(const uint8_t* data, size_t len, uint8_t key) {
    std::string out(len, '\0');
    for (size_t i = 0; i < len; ++i)
        out[i] = static_cast<char>(data[i] ^ key);
    return out;
}

static const uint8_t kEncInjectMouse[] = {
    0xE5, 0xDF, 0xFE, 0xD8, 0xCE, 0xD9, 0xE2, 0xC5, 0xC1, 0xCE, 0xC8, 0xDF,
    0xE6, 0xC4, 0xDE, 0xD8, 0xCE, 0xE2, 0xC5, 0xDB, 0xDE, 0xDF,
};
static const uint8_t kEncInjectKeyboard[] = {
    0xE5, 0xDF, 0xFE, 0xD8, 0xCE, 0xD9, 0xE2, 0xC5, 0xC1, 0xCE, 0xC8, 0xDF,
    0xE0, 0xCE, 0xD2, 0xC9, 0xC4, 0xCA, 0xD9, 0xCF, 0xE2, 0xC5, 0xDB, 0xDE, 0xDF,
};

struct MouseInfo {
    POINT pt;
    unsigned long data;
    unsigned long flags;
    unsigned long time;
    std::uintptr_t extra_info;
};

struct KeyboardInfo {
    std::uint16_t vk;
    std::uint16_t scan;
    unsigned long flags;
    unsigned long time;
    std::uintptr_t extra_info;
};

FnInjectMouse     g_injectMouse = nullptr;
FnInjectKeyboard  g_injectKeyboard = nullptr;
bool              g_initialized = false;

void sendInputMouseRelative(long dx, long dy) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &input, sizeof(INPUT));
}

void sendInputMouseClick() {
    INPUT inputs[2]{};
    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, inputs, sizeof(INPUT));
}

void sendInputTapKey(WORD vk) {
    INPUT inputs[2]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = vk;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = vk;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, inputs, sizeof(INPUT));
}

void injectMouseRelative(long dx, long dy) {
    if (!g_injectMouse)
        return;

    MouseInfo info{};
    info.pt.x = dx;
    info.pt.y = dy;
    info.flags = MOUSEEVENTF_MOVE;
    g_injectMouse(&info, 1);
}

void injectMouseClick() {
    if (!g_injectMouse)
        return;

    MouseInfo down{};
    down.flags = MOUSEEVENTF_LEFTDOWN;
    g_injectMouse(&down, 1);

    MouseInfo up{};
    up.flags = MOUSEEVENTF_LEFTUP;
    g_injectMouse(&up, 1);
}

void injectTapKey(WORD vk) {
    if (!g_injectKeyboard)
        return;

    KeyboardInfo down{};
    down.vk = vk;
    down.scan = static_cast<std::uint16_t>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    g_injectKeyboard(&down, 1);

    KeyboardInfo up{};
    up.vk = vk;
    up.scan = down.scan;
    up.flags = KEYEVENTF_KEYUP;
    g_injectKeyboard(&up, 1);
}

} // namespace

bool initialize() {
    if (g_initialized)
        return isReady();

    const HMODULE win32u = GetModuleHandleW(L"win32u.dll");
    HMODULE loaded = win32u;
    if (!loaded)
        loaded = LoadLibraryW(L"win32u.dll");
    if (!loaded)
        return false;

    g_injectMouse = reinterpret_cast<FnInjectMouse>(
        GetProcAddress(loaded, decodeObf(kEncInjectMouse, sizeof(kEncInjectMouse), 0xAB).c_str()));
    g_injectKeyboard = reinterpret_cast<FnInjectKeyboard>(
        GetProcAddress(loaded, decodeObf(kEncInjectKeyboard, sizeof(kEncInjectKeyboard), 0xAB).c_str()));

    g_initialized = true;
    return isReady();
}

bool isReady() {
    return g_injectMouse != nullptr && g_injectKeyboard != nullptr;
}

bool usesStealthInput() {
    return g_cfg.inputBackend == 1 && isReady();
}

void mouseMoveRelative(long dx, long dy) {
    if (dx == 0 && dy == 0)
        return;
    if (usesStealthInput())
        injectMouseRelative(dx, dy);
    else
        sendInputMouseRelative(dx, dy);
}

void mouseLeftClick() {
    if (usesStealthInput())
        injectMouseClick();
    else
        sendInputMouseClick();
}

void tapKey(WORD vk) {
    if (usesStealthInput())
        injectTapKey(vk);
    else
        sendInputTapKey(vk);
}

} // namespace input_router
