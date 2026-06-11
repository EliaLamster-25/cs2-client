#pragma once

#include "config.h"
#include "memory/process.h"

#include <cstdint>

AimWeaponGroup classifyWeaponGroup(const char* weaponName);
AimWeaponGroup resolveActiveWeaponGroup(const Process& proc, std::uintptr_t clientBase, std::uintptr_t localPawn);

const char* aimWeaponGroupLabel(AimWeaponGroup group);
const char* aimWeaponGroupEquipHint(AimWeaponGroup group);

/// Automatic weapons that benefit from spray RCS (SMG, rifles, LMGs).
bool weaponGroupSupportsRcs(AimWeaponGroup group);

AimGroupConfig defaultAimGroupConfig(AimWeaponGroup group);

/// Current ammo in the active weapon magazine (-1 if unknown).
int readActiveWeaponClip(const Process& proc, std::uintptr_t clientBase, std::uintptr_t localPawn);

inline constexpr int kCalibrationGroupCount = 5;
inline constexpr AimWeaponGroup kCalibrationGroups[kCalibrationGroupCount] = {
    AimWeaponGroup::Pistols,
    AimWeaponGroup::Heavy,
    AimWeaponGroup::Smg,
    AimWeaponGroup::Rifles,
    AimWeaponGroup::Snipers,
};

inline int calibrationGroupIndex(AimWeaponGroup group) {
    for (int i = 0; i < kCalibrationGroupCount; ++i) {
        if (kCalibrationGroups[i] == group)
            return i;
    }
    return -1;
}
