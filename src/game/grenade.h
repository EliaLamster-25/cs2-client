#pragma once
#include "math/vector.h"

/// @file grenade.h
/// @brief POD struct holding live data for a single thrown grenade projectile.

enum class GrenadeType : int {
    HE      = 0,
    Smoke   = 1,
    Flash   = 2,
    Molotov = 3,
    Decoy   = 4
};

struct GrenadeData {
    bool        isValid  = false;
    GrenadeType type     = GrenadeType::HE;
    Vec3        origin{};    // current world position (via scene node)
    Vec3        velocity{};  // current velocity (C_BaseEntity::m_vecVelocity)

    // ── Fuse / timer data ──────────────────────────────────────────────────
    // hasFuse      = true for HE (1.5 s) and Molotov (4.0 s max air).
    // fuseTime     = total detonation time in seconds.
    // timeAlive    = seconds since throw (from game m_flSpawnTime, or wall-clock fallback).
    // damageRadius = world-unit warning radius (HE ~350, Molotov ~250).
    bool   hasFuse      = false;
    float  fuseTime     = 0.f;
    float  timeAlive    = 0.f;
    float  damageRadius = 0.f;

    // ── Post-landing / deploy state ───────────────────────────────────────
    // isDeployed   = true when molotov fire (inferno entity) is active, or smoke cloud deployed.
    // burnRemaining = seconds of burn / smoke cover remaining.
    // deployPos    = world position where the effect is happening.
    bool  isDeployed    = false;
    float burnRemaining = 0.f;
    Vec3  deployPos{};

    // Stable predicted landing point – only updated on first frame or after a bounce,
    // so it doesn't jump when the grenade flies past the old prediction.
    Vec3 stableLandPos{};
    bool hasStableLandPos = false;

    // Forward-simulated trajectory from current position+velocity (with bouncing).
    static constexpr int kMaxPredPoints = 256;
    Vec3 predPoints[kMaxPredPoints]{};
    int  predCount = 0;
};

inline const char* grenadeLabel(GrenadeType t) {
    switch (t) {
        case GrenadeType::HE:      return "HE";
        case GrenadeType::Smoke:   return "Smoke";
        case GrenadeType::Flash:   return "Flash";
        case GrenadeType::Molotov: return "Molotov";
        case GrenadeType::Decoy:   return "Decoy";
        default:                   return "?";
    }
}

/// Pre-throw trajectory — simulated arc the local player's grenade will follow
/// when they release the throw.  Updated every frame while winding up.
struct PreThrowData {
    bool        isActive = false;                ///< true when player is arming a grenade
    GrenadeType type     = GrenadeType::Smoke;   ///< grenade type being held
    float       fuseTime = 0.f;                  ///< fuse duration (0 = no fuse)
    static constexpr int kMaxPredPoints = 256;
    Vec3 predPoints[kMaxPredPoints]{};
    int  predCount = 0;
};

struct BombData {
    bool  isPlanted       = false;
    bool  isTicking       = false;
    bool  isPlanting      = false;
    bool  isBeingDefused  = false;
    int   site            = -1;   // 0 = A, 1 = B
    Vec3  origin{};
    float timerLength     = 40.f;
    float timeRemaining   = 0.f;
    float plantLength     = 3.2f;
    float plantRemaining  = 0.f;
    float defuseLength    = 0.f;
    float defuseRemaining = 0.f;
};
