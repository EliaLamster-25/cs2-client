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

    // ── engine2.dll ────────────────────────────────────────────────────────────
    namespace engine2 {
        /// Pointer to the CNetworkGameClient object.
        /// Verified 2026-05-16 via a2x/cs2-dumper (was 0x90B1E0).
        inline std::uintptr_t dwNetworkGameClient = 0x90A1A0;
    }

    // ── client.dll ─────────────────────────────────────────────────────────────
    namespace client {
        /// Global entity list base pointer (dwEntityList).
        inline std::uintptr_t dwEntityList            = 0x24E44E0;

        /// Local player controller pointer (dwLocalPlayerController).
        inline std::uintptr_t dwLocalPlayerController = 0x231D830;

        /// 4×4 view-projection matrix, float[16] (dwViewMatrix).
        inline std::uintptr_t dwViewMatrix            = 0x2343AB0;

        /// Global current-time state pointer (dwGlobalVars).
        /// Source: a2x/cs2-dumper — generated 2026-05-28.
        inline std::uintptr_t dwGlobalVars            = 33941200;

        /// Local player pawn pointer (dwLocalPlayerPawn).
        inline std::uintptr_t dwLocalPlayerPawn       = 0x2069800;

        /// Local player view angles — float[3] (pitch, yaw, roll), read directly.
        /// Source: a2x/cs2-dumper dwViewAngles — 37042200 dec (CS2 May 22)
        inline std::uintptr_t dwViewAngles            = 0x2353818;

        /// Global planted-C4 pointer holder (dwPlantedC4).
        /// Source: a2x/cs2-dumper — generated 2026-05-28.
        inline std::uintptr_t dwPlantedC4             = 37015304;

        /// Global carried/arming C4 pointer holder (dwWeaponC4).
        /// Source: a2x/cs2-dumper — generated 2026-05-28.
        inline std::uintptr_t dwWeaponC4              = 36421168;
    }

} // namespace offsets
