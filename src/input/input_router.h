#pragma once

#include <Windows.h>
#include <cstdint>

/// Routes mouse/keyboard injection away from SendInput when kernel mode is active.
/// Uses NtUserInjectMouseInput / NtUserInjectKeyboardInput from win32u.dll.
namespace input_router {

bool initialize();
bool isReady();

/// True when NtUser inject path is active (default with kernel memory backend).
bool usesStealthInput();

void mouseMoveRelative(long dx, long dy);
void mouseLeftClick();
void tapKey(WORD vk);

} // namespace input_router
