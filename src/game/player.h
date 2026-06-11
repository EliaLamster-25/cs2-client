#pragma once

#include "math/vector.h"
#include <array>
#include <string>

/// @file player.h
/// @brief POD struct representing the data we read per-player per-frame.
///
/// The EntityManager fills these from memory.  The ESP renderer only
/// reads them — no Win32 calls in the rendering path.

struct PlayerData {
    bool        isValid     = false;   ///< Successfully resolved controller → pawn.
    bool        isAlive     = false;
    bool        isDormant   = false;   ///< Server stopped sending updates.
    bool        isLocalPlayer = false;
    bool        screenRelevant = false;///< Inside the current view; offscreen players can skip render-side work.
    bool        bonesValid  = false;   ///< True when bones[] contains valid world positions.
    bool        visibilityChecked = false; ///< True when world-occlusion visibility was evaluated.
    bool        isVisible   = true;    ///< Visibility result from shared checker.
    float       visibilityConfidence = 1.f; ///< 0..1 visibility confidence from multi-point samples.
    bool        isFlashed   = false;
    bool        isDefusing  = false;
    bool        isScoped    = false;
    bool        hasDefuseKit = false;
    bool        isBot       = false;

    std::uint64_t steamId   = 0;       ///< SteamID64 from controller (0 for bots).

    int         health      = 0;       ///< 0–100.
    int         armor       = 0;       ///< 0–100.
    int         ammoClip    = -1;      ///< Current ammo in active magazine.
    int         ammoMaxClip = -1;      ///< Max ammo in active magazine.
    int         shotsFired  = 0;       ///< m_iShotsFired — used by sound ESP.
    int         teamNum     = 0;       ///< 2 = T,  3 = CT.

    std::uintptr_t pawn     = 0;       ///< CCSPlayerPawn address in the target process.
    float       eyePitch    = 0.f;     ///< View pitch (degrees).
    float       eyeYaw      = 0.f;     ///< View yaw (degrees) — used for head forward offset.
    bool        headFacingValid = false; ///< True when headFacingDir was resolved this frame.
    Vec3        headFacingDir{};       ///< World-unit head forward (bone axis aligned to view).

    Vec3        origin{};              ///< Feet position (world-space).
    Vec3        headPos{};             ///< Approximate head (origin + viewOffset).
    Vec3        velocity{};            ///< Pawn velocity (units/sec), used for optional overlay prediction.

    /// World-space bone positions, indices 0–27 (filled by EntityManager).
    /// Layout: [pelvis=0, ?, spine=2, ?, chest=4, neck=5, head=6, ?,
    ///          Lshoulder=8, Lelbow=9, ?, Lhand=11, ?,
    ///          Rshoulder=13, Relbow=14, ?, Rhand=16,
    ///          …, Lhip=22, Lknee=23, Lankle=24,
    ///          Rhip=25, Rknee=26, Rankle=27]
    static constexpr int kBoneCount = 28;
    Vec3        bones[kBoneCount]{};

    /// Per-bone line-of-sight for chams clipping (entity thread writes, render thread reads).
    bool        chamsPartVisChecked = false;
    std::array<bool, kBoneCount> chamsPartVisible{};
    /// Midpoint visibility for cham segments (indexes match kChamsSegDefs in entity_manager).
    static constexpr int kChamsSegCount = 16;
    std::array<bool, kChamsSegCount> chamsSegMidVisible{};

    std::string name;                  ///< Sanitised player name.
    std::string weaponName;            ///< Active weapon designer-name label.
    std::string weaponId;              ///< Raw weapon designer name (e.g. weapon_ak47).
};

struct SpectatorData {
    bool        isValid = false;
    bool        watchingLocal = false;
    bool        isBot = false;
    int         teamNum = 0;
    int         mode = 0;
    std::uint64_t steamId = 0;
    std::string name;
    std::string targetName;
};
