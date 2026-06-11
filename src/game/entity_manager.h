#pragma once

#include "memory/process.h"
#include "game/player.h"
#include "game/grenade.h"
#include "math/matrix.h"
#include "bsp/bsp_world.h"
#include "game/game_trace.h"
#include "config.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

/// @file entity_manager.h
/// @brief Reads the CS2 entity list every frame and fills PlayerData structs.
///
/// ## CS2 Entity Architecture (Source 2)
///
///   dwEntityList ──► EntityListEntry[0] ──► CCSPlayerController
///                                               │
///                                               ├─ m_sSanitizedPlayerName
///                                               ├─ m_bPawnIsAlive
///                                               └─ m_hPlayerPawn ──► CCSPlayerPawn
///                                                                       │
///                                                                       ├─ m_iHealth
///                                                                       ├─ m_iTeamNum
///                                                                       ├─ m_vOldOrigin
///                                                                       ├─ m_vecViewOffset
///                                                                       └─ m_bDormant

class EntityManager {
public:
    struct Snapshot {
        std::array<PlayerData, cfg::kMaxPlayers> players{};
        std::array<SpectatorData, cfg::kMaxPlayers> spectators{};
        std::array<GrenadeData, 16> grenades{};
        PreThrowData preThrow{};
        BombData bomb{};
        GameRulesData gameRules{};
        ViewMatrix viewMatrix{};
        std::uintptr_t localPawn = 0;
        int localTeam = 0;
        int localPing = -1;
        bool hasLocalPlayer = false;
        Vec3 localOrigin{};
        Vec3 localViewAngles{};
        std::string currentMapName;
    };

    using PublishedSnapshot = std::shared_ptr<const Snapshot>;

    /// Initialise with module base addresses.
    bool init(const Process& proc);

    /// Re-read every entity from game memory.  Call once per frame.
    void update(Process& proc);

    /// Latest immutable frame for the render thread (no copy, no mutex).
    PublishedSnapshot publishedFrame() const {
        const int slot = m_readSlot.load(std::memory_order_acquire);
        return m_frames[slot];
    }

    /// Access the player array — returns a snapshot safe to use off-thread.
    std::array<PlayerData, cfg::kMaxPlayers> players() const {
        const auto pub = publishedFrame();
        if (!pub) return {};
        return pub->players;
    }

    static constexpr int kMaxSpectators = cfg::kMaxPlayers;
    std::array<SpectatorData, kMaxSpectators> spectators() const {
        const auto pub = publishedFrame();
        if (!pub) return {};
        return pub->spectators;
    }

    /// The current view matrix (thread-safe copy).
    ViewMatrix viewMatrix() const {
        const auto pub = publishedFrame();
        if (!pub) return {};
        return pub->viewMatrix;
    }

    /// Team number of the local player (thread-safe copy).
    int localTeam() const {
        const auto pub = publishedFrame();
        if (!pub) return 0;
        return pub->localTeam;
    }

    std::uintptr_t localPawn() const {
        const auto pub = publishedFrame();
        if (!pub) return 0;
        return pub->localPawn;
    }

    std::uintptr_t clientBase() const { return m_clientBase; }

    /// The current grenade list (thread-safe copy).
    static constexpr int kMaxGrenades = 16;
    std::array<GrenadeData, kMaxGrenades> grenades() const {
        const auto pub = publishedFrame();
        if (!pub) return {};
        return pub->grenades;
    }

    /// Pre-throw trajectory for the local player (thread-safe copy).
    PreThrowData preThrow() const {
        const auto pub = publishedFrame();
        if (!pub) return {};
        return pub->preThrow;
    }

    BombData bomb() const {
        const auto pub = publishedFrame();
        if (!pub) return {};
        return pub->bomb;
    }

    std::string currentMapName() const {
        const auto pub = publishedFrame();
        if (!pub) return m_currentMapName;
        return pub->currentMapName;
    }

    Snapshot snapshot() const {
        const auto pub = publishedFrame();
        if (!pub) return {};
        return *pub;
    }

    /// Tick count (ms) when entity data was last published to the render thread.
    std::uint64_t lastUpdateTickMs() const {
        return m_lastUpdateTickMs.load(std::memory_order_acquire);
    }

private:
    std::uintptr_t m_clientBase = 0;

    std::shared_ptr<Snapshot> m_frames[2];
    std::atomic<int> m_readSlot{ 0 };
    std::array<GrenadeData, kMaxGrenades> m_lastPublishedGrenades{};
    std::atomic<std::uint64_t> m_lastUpdateTickMs{ 0 };

    // Persistent per-grenade state for bounce floor tracking (entity-update thread only).
    struct GrenadePersist {
        std::uintptr_t lastEntPtr       = 0;
        float          lastBounceZ      = 0.f;
        float          prevVelZ         = 0.f;
        bool           hasFloor         = false;
        std::uint64_t  firstSeenMs      = 0;  ///< GetTickCount64() when entity first appeared
        GrenadeType    lastGtype         = GrenadeType::HE;
        Vec3           lastOrigin{};          ///< Actual entity position last frame (for PendingInferno)
        // Stable landing prediction – locked on first frame, never recalculated.
        Vec3           storedLandPos{};
        bool           hasStoredLandPos  = false;
        bool           predictionLocked  = false;
        Vec3           lockedPredPoints[GrenadeData::kMaxPredPoints]{};
        int            lockedPredCount   = 0;
        float          lockedSimTime     = 0.f;
        // Post-deploy tracking (smoke cloud deployment)
        bool           deployDetected    = false;
        std::uint64_t  deployDetectedMs  = 0;
        Vec3           deployPos{};
    };
    std::array<GrenadePersist, kMaxGrenades> m_grenadePersist{};

    // Pending post-molotov infernos: when a molotov entity disappears we start a
    // 7-second burn timer at the last predicted landing position.
    struct PendingInferno {
        bool           active  = false;
        Vec3           pos{};
        std::uint64_t  startMs = 0;
    };
    static constexpr int kMaxInfernos = 4;
    PendingInferno m_pendingInfernos[kMaxInfernos]{};
    bool m_grenadesNeedFastUpdate = true;
    int  m_grenadeSlowTick = 0;

    struct PreThrowPersist {
        bool           valid = false;
        GrenadeType    type = GrenadeType::Smoke;
        float          throwStrength = 0.f;
        Vec3           viewAngles{};
        Vec3           startPos{};
        Vec3           playerVelocity{};
        std::uint64_t  lastSimMs = 0;
        PreThrowData   cached{};
    };
    PreThrowPersist m_preThrowPersist{};

    struct BombPersist {
        bool           plantingActive = false;
        std::uint64_t  plantingStartMs = 0;
        float          plantLength = 3.2f;

        bool           plantedActive = false;
        std::uint64_t  plantedStartMs = 0;
        float          plantedLength = 40.f;

        bool           defusingActive = false;
        std::uint64_t  defusingStartMs = 0;
        float          defuseLength = 10.f;
    };
    BombPersist m_bombPersist{};

    struct VisibilityPersist {
        int   visibleStreak = 0;
        int   occludedStreak = 0;
        int   nextEvalTick = 0;
        bool  latchedVisible = false;
        bool  hasState = false;
        float smoothedConfidence = 1.f;
    };
    std::array<VisibilityPersist, cfg::kMaxPlayers> m_visibilityPersist{};
    int m_visibilityTick = 0;

    // ── BSP world collision ─────────────────────────────────────────────────
    std::uintptr_t m_engine2Base    = 0;
    std::string    m_currentMapName;
    std::string    m_cs2Path;          ///< Detected at init() from Steam registry
    BspWorld       m_bspWorld;
    GameTraceVis   m_gameTrace;
    bool           m_gameTraceInitAttempted = false;
    bool           m_gameTraceInitFailed = false;
};
