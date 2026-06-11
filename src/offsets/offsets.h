#pragma once
#include <cstdint>

class Process;

/// @file offsets.h
/// @brief Module-level offsets into client.dll.
///
/// Source: a2x/cs2-dumper — generated 2026-05-22 (CS2 update)
/// Regenerate after each CS2 update with: https://github.com/a2x/cs2-dumper

namespace offsets {

    /// Resolve frequently changing client globals at runtime.
    /// Keeps the compiled defaults if a signature is not found.
    bool resolveRuntime(const Process& proc, std::uintptr_t clientBase);

    /// Resolve engine2 globals (map client / BSP loading) at runtime.
    bool resolveEngine2Runtime(const Process& proc, std::uintptr_t engine2Base);

    // ── engine2.dll ────────────────────────────────────────────────────────────
    namespace engine2 {
        /// Pointer to the CNetworkGameClient object.
        /// Source: a2x/cs2-dumper — generated 2026-06-03.
        inline std::uintptr_t dwNetworkGameClient = 0x908520;
    }

    // ── client.dll ─────────────────────────────────────────────────────────────
    namespace client {
        /// Global entity list base pointer (dwEntityList / dwGameEntitySystem).
        inline std::uintptr_t dwEntityList            = 0x24E4A30;

        /// Highest allocated entity index within CGameEntitySystem (optional scan bound).
        inline std::uintptr_t dwGameEntitySystem_highestEntityIndex = 8336;

        /// Local player controller pointer (dwLocalPlayerController).
        inline std::uintptr_t dwLocalPlayerController = 0x231D800;

        /// 4×4 view-projection matrix, float[16] (dwViewMatrix).
        inline std::uintptr_t dwViewMatrix            = 0x2344810;

        /// Global current-time state pointer (dwGlobalVars).
        inline std::uintptr_t dwGlobalVars            = 0x205CE50;

        /// C_CSGameRules* (dwGameRules).
        inline std::uintptr_t dwGameRules             = 0x234CE58;

        /// Local player pawn pointer (dwLocalPlayerPawn).
        inline std::uintptr_t dwLocalPlayerPawn       = 0x2344078;

        /// Local player view angles — float[3] (pitch, yaw, roll), read directly.
        inline std::uintptr_t dwViewAngles            = 0x2354DD8;

        /// Sensitivity pointer + float field offset (a2x/cs2-dumper 2026-06-03).
        inline std::uintptr_t dwSensitivity           = 0x233DA68;
        inline std::uintptr_t dwSensitivity_sensitivity = 0x58;

        /// Global planted-C4 pointer holder (dwPlantedC4).
        inline std::uintptr_t dwPlantedC4             = 0x234EBA8;

        /// Global carried/arming C4 pointer holder (dwWeaponC4).
        inline std::uintptr_t dwWeaponC4              = 0x22CB100;
    }

} // namespace offsets
