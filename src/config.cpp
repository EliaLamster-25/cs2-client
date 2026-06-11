#include "config.h"
#include "game/weapon_group.h"
#include <atomic>

/// @file config.cpp
/// @brief Global config instance with default values.

OverlayConfig g_cfg;
std::atomic<bool> g_requestShutdown{ false };

namespace {
struct AimDefaultsInit {
    AimDefaultsInit() {
        for (std::size_t i = 0; i < kAimWeaponGroupCount; ++i)
            g_cfg.aimByWeaponGroup[i] = defaultAimGroupConfig(static_cast<AimWeaponGroup>(i));
    }
};
const AimDefaultsInit kAimDefaultsInit{};
} // namespace
