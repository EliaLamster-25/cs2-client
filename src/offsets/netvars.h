#pragma once
#include <cstdint>

/// @file netvars.h
/// @brief Per-entity networked variable offsets (schema fields).
///
/// Source: a2x/cs2-dumper client_dll.json — generated 2026-05-21 (CS2 update)
/// Regenerate after each CS2 update with: https://github.com/a2x/cs2-dumper

namespace netvars {

    // ── CCSPlayerController ────────────────────────────────────────────────────
    // Inherits CBasePlayerController which inherits C_BaseEntity.
    namespace controller {
        /// m_hPlayerPawn  →  handle to CCSPlayerPawn* (uint32).  Offset: 2316 dec.
        constexpr std::uintptr_t m_hPlayerPawn          = 0x90C;

        /// m_sSanitizedPlayerName  →  char[128].  Offset: 2144 dec.
        constexpr std::uintptr_t m_sSanitizedPlayerName = 0x860;

        /// m_iszPlayerName  →  char[128], raw player name field.  Offset: 1780 dec.
        constexpr std::uintptr_t m_iszPlayerName = 0x6F4;

        /// m_steamID  →  uint64 Steam ID (bots are typically 0).  Offset: 1920 dec.
        constexpr std::uintptr_t m_steamID = 0x780;

        /// m_bPawnHasDefuser  →  bool (pawn currently has defuse kit).  Offset: 2336 dec.
        constexpr std::uintptr_t m_bPawnHasDefuser = 0x920;

        /// m_bPawnIsAlive  →  bool.  Offset: 2324 dec.
        constexpr std::uintptr_t m_bPawnIsAlive         = 0x914;
    }

    // ── C_BaseEntity (base of all pawn types) ──────────────────────────────────
    namespace pawn {
        /// m_iHealth  →  int32.  Offset: 844 dec.
        constexpr std::uintptr_t m_iHealth        = 0x34C;

        /// m_ArmorValue  →  int32.  Offset: 7292 dec.
        constexpr std::uintptr_t m_ArmorValue     = 0x1C7C;

        /// m_iTeamNum  →  int32  (2 = T, 3 = CT).  Offset: 1003 dec.
        constexpr std::uintptr_t m_iTeamNum       = 0x3EB;

        /// m_vOldOrigin  →  Vector (float[3]).  Offset on C_BasePlayerPawn: 5008 dec.
        constexpr std::uintptr_t m_vOldOrigin     = 0x1390;

        /// m_bDormant lives on CGameSceneNode, not pawn — read via scene node.
        /// Kept here as a convenience alias: scene_node::m_bDormant.
        constexpr std::uintptr_t m_bDormant       = 0x103; // not a pawn field; use scene node

        /// m_vecViewOffset  →  Vector (float[3]).  Offset on C_BaseModelEntity: 3696 dec.
        constexpr std::uintptr_t m_vecViewOffset  = 0xE70;

        /// m_pGameSceneNode  →  CGameSceneNode*.  Offset on C_BaseEntity: 816 dec.
        constexpr std::uintptr_t m_pGameSceneNode = 0x330;

        /// m_iIDEntIndex  →  int32 (entity index/handle under the crosshair, 0 = none).
        /// On CCSPlayerPawnBase.  Offset: 13308 dec.
        constexpr std::uintptr_t m_iIDEntIndex    = 0x33FC;

        /// m_angEyeAngles  →  Vec2 (pitch, yaw) — the player's actual view angles.
        /// On C_CSPlayerPawn.  Offset: 13088 dec (0x3320).
        constexpr std::uintptr_t m_angEyeAngles   = 0x3320;

        /// m_pWeaponServices  →  CPlayer_WeaponServices*.  Offset on C_BasePlayerPawn: 4576 dec.
        /// Used to resolve the active weapon entity for pre-throw grenade prediction.
        constexpr std::uintptr_t m_pWeaponServices = 0x11E0;

        /// m_pObserverServices  →  CPlayer_ObserverServices*.  Offset on C_BasePlayerPawn: 4600 dec.
        constexpr std::uintptr_t m_pObserverServices = 0x11F8;

        /// m_iShotsFired  →  int32 shots fired since last attack reset.  Offset: 7268 dec.
        constexpr std::uintptr_t m_iShotsFired = 0x1C64;

        /// m_pAimPunchServices  →  CCSPlayer_AimPunchServices*.  Offset: 5264 dec.
        constexpr std::uintptr_t m_pAimPunchServices = 0x1490;

        /// m_flFlashMaxAlpha  →  float (current flash alpha intensity).  Offset: 5116 dec.
        constexpr std::uintptr_t m_flFlashMaxAlpha = 0x13FC;

        /// m_flFlashDuration  →  float (remaining flash duration).  Offset: 5120 dec.
        constexpr std::uintptr_t m_flFlashDuration = 0x1400;

        /// m_bIsScoped  →  bool (true while scoped).  Offset: 7248 dec.
        constexpr std::uintptr_t m_bIsScoped = 0x1C50;

        /// m_bIsDefusing  →  bool (true while defusing C4).  Offset: 7250 dec.
        constexpr std::uintptr_t m_bIsDefusing = 0x1C52;
    }

    // ── CCSPlayer_AimPunchServices ───────────────────────────────────────────
    namespace aim_punch_services {
        /// m_predictableBaseAngle  →  Vector (recoil / punch angle).  Offset: 80 dec.
        constexpr std::uintptr_t m_predictableBaseAngle = 0x50;

        /// m_unpredictableBaseAngle  →  Vector (additional recoil / view punch).  Offset: 164 dec.
        constexpr std::uintptr_t m_unpredictableBaseAngle = 0xA4;
    }

    // ── CGameSceneNode ─────────────────────────────────────────────────────────
    namespace scene_node {
        /// m_vecAbsOrigin  →  Vector (float[3]).  Offset: 200 dec.
        constexpr std::uintptr_t m_vecAbsOrigin = 0xC8;

        /// m_bDormant  →  bool.  Offset: 259 dec.
        constexpr std::uintptr_t m_bDormant     = 0x103;
    }

    // ── CSkeletonInstance (non-schema / empirical) ─────────────────────────────
    // pawn → m_pGameSceneNode (0x330) → CSkeletonInstance*
    // CSkeletonInstance::m_modelState embedded at +0x150 (schema, 336 dec)
    // Bone world-position cache pointer lives inside CModelState at +0x80
    // → bone array ptr = sceneNode + 0x1D0  (= 0x150 + 0x80)
    // Each bone: 48 bytes  (matrix3x4_t — position at column 3: m[0][3], m[1][3], m[2][3])
    namespace skeleton {
        constexpr std::uintptr_t m_boneArrayPtr = 0x1D0; ///< sceneNode + this = ptr to bone positions
        constexpr std::uintptr_t kBoneStride    = 0x30;  ///< 48 bytes per bone (matrix3x4_t)
    }

    // ── CPlayer_WeaponServices ─────────────────────────────────────────────────
    namespace weapon_services {
        /// m_hActiveWeapon  →  entity handle (uint32) to the currently held weapon.
        /// Offset in CPlayer_WeaponServices: 96 dec (0x60).
        constexpr std::uintptr_t m_hActiveWeapon = 0x60;
    }

    // ── C_CSWeaponBase ────────────────────────────────────────────────────────
    namespace weapon {
        /// m_iClip1  →  int32 current ammo in active magazine.  Offset: 5848 dec.
        constexpr std::uintptr_t m_iClip1 = 0x16D8;

        /// m_iClip2  →  int32 secondary ammo in active magazine.  Offset: 5852 dec.
        constexpr std::uintptr_t m_iClip2 = 0x16DC;
    }

    namespace observer_services {
        /// m_iObserverMode  →  uint8 observer mode.  Offset: 72 dec.
        constexpr std::uintptr_t m_iObserverMode = 0x48;

        /// m_hObserverTarget  →  entity handle (uint32) currently spectated.  Offset: 76 dec.
        constexpr std::uintptr_t m_hObserverTarget = 0x4C;
    }

    // ── Grenade projectiles ────────────────────────────────────────────────────
    // Designer names (CEntityIdentity::m_designerName) identify grenade types.
    // Position read via pawn::m_pGameSceneNode → scene_node::m_vecAbsOrigin.
    namespace grenade {
        /// CEntityIdentity::m_designerName  →  char* (pointer to class name).  Offset: 32 dec.
        constexpr std::uintptr_t m_designerName = 0x20;

        /// C_BaseEntity::m_vecVelocity  →  Vector (float[3]).++  Offset: 1072 dec.
        constexpr std::uintptr_t m_vecVelocity   = 0x430;

        /// C_BaseEntity::m_flElasticity  →  float (bounce factor, ~0.45 for HE/smoke/flash).  Offset: 1340 dec.
        constexpr std::uintptr_t m_flElasticity  = 0x53C;

        /// C_BaseCSGrenadeProjectile::m_vInitialPosition  →  Vector (float[3]).  Offset: 4512 dec.
        constexpr std::uintptr_t m_vInitialPosition = 0x11A0;

        /// C_BaseCSGrenadeProjectile::m_vInitialVelocity  →  Vector (float[3]).  Offset: 4524 dec.
        constexpr std::uintptr_t m_vInitialVelocity = 0x11AC;

        /// C_BaseCSGrenadeProjectile::m_nBounces  →  int32 (bounce count so far).  Offset: 4536 dec.
        constexpr std::uintptr_t m_nBounces      = 0x11B8;

        /// C_BaseCSGrenade::m_flThrowStrength  →  float 0→1 wind-up progress.
        /// > 0 means the player is actively arming a grenade throw.
        /// Offset: 7360 dec (0x1CC0). — CS2 May 19 2026
        constexpr std::uintptr_t m_flThrowStrength = 0x1CC0;

        /// C_BaseGrenade::m_flDetonateTime  →  float, absolute game time of detonation.  Offset: 4448 dec.
        constexpr std::uintptr_t m_flDetonateTime  = 0x1160;

        /// C_BaseCSGrenadeProjectile::m_flSpawnTime  →  float, game time when projectile was spawned.  Offset: 4568 dec.
        constexpr std::uintptr_t m_flSpawnTime     = 0x11D8;

        /// C_SmokeGrenadeProjectile::m_bDidSmokeEffect  →  bool, smoke cloud has deployed.  Offset: 4692 dec.
        constexpr std::uintptr_t m_bDidSmokeEffect = 0x1254;
    }

    // ── C_PlantedC4 ───────────────────────────────────────────────────────────
    namespace bomb {
        /// m_bBombTicking  →  bool, active planted-bomb ticking state.  Offset: 4448 dec.
        constexpr std::uintptr_t m_bBombTicking      = 0x1160;

        /// m_nBombSite  →  int32, 0 = A, 1 = B.  Offset: 4452 dec.
        constexpr std::uintptr_t m_nBombSite         = 0x1164;

        /// m_flC4Blow  →  float, absolute game time when the bomb explodes.  Offset: 4496 dec.
        constexpr std::uintptr_t m_flC4Blow          = 0x1190;

        /// m_flTimerLength  →  float, total planted-bomb lifetime.  Offset: 4504 dec.
        constexpr std::uintptr_t m_flTimerLength     = 0x1198;

        /// m_bBeingDefused  →  bool.  Offset: 4508 dec.
        constexpr std::uintptr_t m_bBeingDefused     = 0x119C;

        /// m_bC4Activated  →  bool, planted and armed.  Offset: 4520 dec.
        constexpr std::uintptr_t m_bC4Activated      = 0x11A8;

        /// m_flDefuseLength  →  float, active defuse duration.  Offset: 4524 dec.
        constexpr std::uintptr_t m_flDefuseLength    = 0x11AC;

        /// m_flDefuseCountDown  →  float, absolute game time when defuse completes.  Offset: 4528 dec.
        constexpr std::uintptr_t m_flDefuseCountDown = 0x11B0;
    }

    // ── C_C4 ─────────────────────────────────────────────────────────────────
    namespace c4 {
        /// m_bStartedArming  →  bool, bomb planting interaction started.  Offset: 7352 dec.
        constexpr std::uintptr_t m_bStartedArming   = 0x1CB8;

        /// m_fArmedTime  →  float, absolute game time when planting completes.  Offset: 7356 dec.
        constexpr std::uintptr_t m_fArmedTime       = 0x1CBC;

        /// m_bIsPlantingViaUse  →  bool, planting via the use key is active.  Offset: 7361 dec.
        constexpr std::uintptr_t m_bIsPlantingViaUse = 0x1CC1;

        /// m_bBombPlanted  →  bool, weapon has transitioned into the planted state.  Offset: 7403 dec.
        constexpr std::uintptr_t m_bBombPlanted     = 0x1CEB;
    }

    // ── C_BaseEntity common fields ─────────────────────────────────────────────
    namespace entity {
        /// m_flSimulationTime  →  float, entity's last simulation timestamp (proxy for curtime).
        /// Subtract m_flSpawnTime to get elapsed, or subtract from m_flDetonateTime for remaining.
        /// Offset: 952 dec (0x3B8).
        constexpr std::uintptr_t m_flSimulationTime = 0x3B8;
    }

} // namespace netvars
