#include "aim_assist.h"
#include "game/entity_manager.h"
#include "game/game_sensitivity.h"
#include "game/player.h"
#include "game/aim_style.h"
#include "game/weapon_group.h"
#include "debug/aim_debug.h"
#include "offsets/offsets.h"
#include "offsets/netvars.h"
#include "memory/rpm.h"
#include "input/input_router.h"
#include "config.h"
#include "overlay/overlay_metrics.h"
#include <Windows.h>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <cstring>

static constexpr float kPi = 3.14159265f;
static constexpr float kRad2Deg = 180.f / kPi;
static constexpr float kDeg2Rad = kPi / 180.f;
static constexpr float kMaxAimPxPerTick = 48.f;
static constexpr float kAimDeadzoneDeg = 0.28f;
static constexpr float kRcsYawDeadzone = 0.0004f;
static constexpr float kSupportAssistFov = 4.5f;
static constexpr float kSupportUserMoveMin = 0.012f;
static constexpr float kSoftSettleMaxErr = 0.42f;
static constexpr float kSoftSettleMinErr = 0.035f;
static constexpr float kMicroAssistCutoff = 0.22f;
static constexpr float kMaxAimPointJump = 72.f;
static constexpr float kFovDistanceWeight = 0.02f;
static constexpr float kFovDistanceBias = 50.f;

static inline float normalizeAngle(float ang) {
    while (ang > 180.f) ang -= 360.f;
    while (ang < -180.f) ang += 360.f;
    return ang;
}

static float effectiveAimSmooth(float slider) {
    const float t = std::clamp(slider, 1.f, 50.f);
    return 4.f + std::powf(t, 2.35f) * 0.42f;
}

static float supportResponseDiv(float slider) {
    return std::clamp(slider * 1.1f + 4.f, 5.f, 48.f);
}

static float supportGroupSpeedScale(AimWeaponGroup group) {
    switch (group) {
    case AimWeaponGroup::Snipers: return 1.15f;
    case AimWeaponGroup::Rifles:  return 1.08f;
    case AimWeaponGroup::Pistols: return 1.05f;
    default: return 1.f;
    }
}

static std::uint64_t supportGraceMs(AimWeaponGroup group) {
    switch (group) {
    case AimWeaponGroup::Snipers: return 140;
    case AimWeaponGroup::Rifles:  return 75;
    default: return 60;
    }
}

static void capAssistToUserMove(float& pitchStep, float& yawStep,
                                float userPitch, float userYaw, float strength) {
    const float userMag = std::sqrtf(userPitch * userPitch + userYaw * userYaw);
    if (userMag < 0.001f)
        return;
    const float assistMag = std::sqrtf(pitchStep * pitchStep + yawStep * yawStep);
    const float maxAssist = userMag * (0.18f + strength * 0.22f);
    if (assistMag > maxAssist && assistMag > 0.001f) {
        const float s = maxAssist / assistMag;
        pitchStep *= s;
        yawStep *= s;
    }
}

static float supportMinBrakeErr(AimWeaponGroup group) {
    switch (group) {
    case AimWeaponGroup::Snipers: return 0.42f;
    case AimWeaponGroup::Rifles:  return 0.58f;
    default: return 0.5f;
    }
}

static float supportOvershootDelta(float errMag) {
    if (errMag > 1.2f) return 0.04f;
    if (errMag > 0.75f) return 0.07f;
    return 0.12f;
}

static float supportTickScale(float dtMs, bool supportMode, AimWeaponGroup group) {
    if (!supportMode)
        return std::clamp(dtMs / (1000.f / 60.f), 0.35f, 2.f);
    const float refMs = (group == AimWeaponGroup::Snipers) ? 5.5f : 7.f;
    return std::clamp(dtMs / refMs, 0.55f, 2.5f);
}

static bool isFiniteVec3(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

static bool isBonePlausible(const Vec3& bone, const Vec3& origin) {
    if (!isFiniteVec3(bone))
        return false;
    const Vec3 d = bone - origin;
    return d.lengthSq() > 1.f && d.lengthSq() < 200.f * 200.f;
}

static bool isLikelyPtr(std::uintptr_t p) {
    return p > 0x10000ull && p < 0x00007FFFFFFFFFFFull;
}

static bool readAimPunch(const Process& proc, std::uintptr_t localPawn, Vec3& out) {
    const Vec3 direct = mem::read<Vec3>(proc, localPawn + netvars::pawn::m_aimPunchAngle);
    if (std::isfinite(direct.x) && std::isfinite(direct.y)) {
        out = direct;
        return true;
    }

    const std::uintptr_t cacheBase = localPawn + netvars::pawn::m_aimPunchCache;
    const int cacheCount = mem::read<int>(proc, cacheBase);
    const std::uintptr_t cacheData = mem::read<std::uintptr_t>(proc, cacheBase + 0x8);
    if (cacheCount > 0 && cacheCount < 256 && isLikelyPtr(cacheData)) {
        const std::uintptr_t lastOff = cacheData
            + static_cast<std::uintptr_t>(cacheCount - 1) * sizeof(Vec3);
        const Vec3 cached = mem::read<Vec3>(proc, lastOff);
        if (std::isfinite(cached.x) && std::isfinite(cached.y)) {
            out = cached;
            return true;
        }
    }

    const auto aimPunchSvc = mem::read<std::uintptr_t>(
        proc, localPawn + netvars::pawn::m_pAimPunchServices);
    if (isLikelyPtr(aimPunchSvc)) {
        const Vec3 predictable = mem::read<Vec3>(
            proc, aimPunchSvc + netvars::aim_punch_services::m_predictableBaseAngle);
        const Vec3 unpredictable = mem::read<Vec3>(
            proc, aimPunchSvc + netvars::aim_punch_services::m_unpredictableBaseAngle);
        if (std::isfinite(predictable.x) && std::isfinite(predictable.y)
            && std::isfinite(unpredictable.x) && std::isfinite(unpredictable.y)) {
            out = {
                predictable.x + unpredictable.x,
                predictable.y + unpredictable.y,
                predictable.z + unpredictable.z
            };
            return true;
        }
    }

    return false;
}

static Vec3 readLocalEyePos(const Process& proc, std::uintptr_t localPawn) {
    Vec3 localOrigin{};
    const auto sceneNode = mem::read<std::uintptr_t>(
        proc, localPawn + netvars::pawn::m_pGameSceneNode);
    if (isLikelyPtr(sceneNode)) {
        localOrigin = mem::read<Vec3>(
            proc, sceneNode + netvars::scene_node::m_vecAbsOrigin);
    }
    if (!isFiniteVec3(localOrigin) || localOrigin.lengthSq() <= 1.f)
        localOrigin = mem::read<Vec3>(proc, localPawn + netvars::pawn::m_vOldOrigin);
    const Vec3 viewOffset = mem::read<Vec3>(proc, localPawn + netvars::pawn::m_vecViewOffset);
    return localOrigin + viewOffset;
}

static Vec3 forwardFromAngles(float pitchDeg, float yawDeg) {
    const float pitch = pitchDeg * kDeg2Rad;
    const float yaw = yawDeg * kDeg2Rad;
    const float cp = std::cosf(pitch);
    return { cp * std::cosf(yaw), cp * std::sinf(yaw), -std::sinf(pitch) };
}

static Vec3 resolveHeadForwardDir(const PlayerData& player) {
    if (player.headFacingValid) {
        const float len = player.headFacingDir.length();
        if (len > 0.001f)
            return player.headFacingDir * (1.f / len);
    }
    Vec3 fwd = forwardFromAngles(player.eyePitch, player.eyeYaw);
    const float len = fwd.length();
    if (len < 0.001f)
        return { 1.f, 0.f, 0.f };
    return fwd * (1.f / len);
}

static void refreshAimPlayerPositions(const Process& proc, PlayerData& player, bool /*refreshBones*/) {
    if (!isLikelyPtr(player.pawn))
        return;

    player.isValid = true;
    player.health = mem::read<int>(proc, player.pawn + netvars::pawn::m_iHealth);
    player.teamNum = mem::read<int>(proc, player.pawn + netvars::pawn::m_iTeamNum);
    player.isAlive = player.health > 0;
    player.isDormant = false;

    const Vec2 eyeAng = mem::read<Vec2>(proc, player.pawn + netvars::pawn::m_angEyeAngles);
    if (std::isfinite(eyeAng.x) && std::isfinite(eyeAng.y)) {
        player.eyePitch = eyeAng.x;
        player.eyeYaw = eyeAng.y;
    }

    const auto sceneNode = mem::read<std::uintptr_t>(
        proc, player.pawn + netvars::pawn::m_pGameSceneNode);
    if (isLikelyPtr(sceneNode)) {
        player.isDormant = mem::read<bool>(
            proc, sceneNode + netvars::scene_node::m_bDormant);
        const Vec3 freshOrigin = mem::read<Vec3>(
            proc, sceneNode + netvars::scene_node::m_vecAbsOrigin);
        if (isFiniteVec3(freshOrigin) && freshOrigin.lengthSq() > 1.f)
            player.origin = freshOrigin;
    }

    if (!isFiniteVec3(player.origin) || player.origin.lengthSq() <= 1.f) {
        const Vec3 oldOrigin = mem::read<Vec3>(proc, player.pawn + netvars::pawn::m_vOldOrigin);
        if (isFiniteVec3(oldOrigin) && oldOrigin.lengthSq() > 1.f)
            player.origin = oldOrigin;
    }

    if (!player.bonesValid) {
        const Vec3 viewOffset = mem::read<Vec3>(
            proc, player.pawn + netvars::pawn::m_vecViewOffset);
        if (std::isfinite(viewOffset.z))
            player.headPos = player.origin + viewOffset;
    }
}

static Vec3 resolveBonePoint(const PlayerData& player, int slot, const Vec3& fallback) {
    if (player.bonesValid && slot >= 0 && slot < PlayerData::kBoneCount) {
        const Vec3& bone = player.bones[slot];
        if (isBonePlausible(bone, player.origin))
            return bone;
    }
    return fallback;
}

static Vec3 applyAimPointOffsets(Vec3 targetPos, const PlayerData& player,
                                 bool isHead, float boneOffsetZ, float headForward) {
    if (std::fabs(boneOffsetZ) > 0.001f)
        targetPos.z += boneOffsetZ;
    if (isHead && std::fabs(headForward) > 0.001f) {
        const Vec3 dir = resolveHeadForwardDir(player);
        targetPos.x += dir.x * headForward;
        targetPos.y += dir.y * headForward;
        targetPos.z += dir.z * headForward;
    }
    return targetPos;
}

static float targetFovDeg(const Vec3& eyePos, const Vec3& viewFwd, const Vec3& targetPos, float& outDist) {
    Vec3 toTarget = targetPos - eyePos;
    outDist = toTarget.length();
    if (outDist < 0.001f)
        return 999.f;
    toTarget = toTarget * (1.f / outDist);
    const float dot = std::clamp(
        viewFwd.x * toTarget.x + viewFwd.y * toTarget.y + viewFwd.z * toTarget.z,
        -1.f, 1.f);
    return std::acosf(dot) * kRad2Deg;
}

static bool isPlausibleAimPoint(const Vec3& aim, const Vec3& origin) {
    if (!isFiniteVec3(aim) || !isFiniteVec3(origin))
        return false;
    if (aim.z < origin.z + 28.f)
        return false;
    const Vec3 d = aim - origin;
    return d.lengthSq() > 4.f && d.lengthSq() < 200.f * 200.f;
}

static std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

static float effectiveMaxFov(float maxFov, float /*distance*/) {
    return maxFov;
}

static Vec3 resolveBoneAimPoint(const PlayerData& player, int boneSlot) {
    switch (boneSlot) {
    case 6: {
        if (player.skeleton.isValid()) {
            const Vec3 head = player.skeleton.position(6);
            if (isBonePlausible(head, player.origin))
                return head;
        }
        return resolveBonePoint(player, 6, player.headPos);
    }
    case 4:
        return resolveBonePoint(player, 4, player.origin + Vec3{ 0.f, 0.f, 54.f });
    case 2:
        return resolveBonePoint(player, 2, player.origin + Vec3{ 0.f, 0.f, 40.f });
    case 0:
        return resolveBonePoint(player, 0, player.origin + Vec3{ 0.f, 0.f, 34.f });
    case 89:
        if (player.bonesValid) {
            const Vec3 lArm = resolveBonePoint(player, 8, player.origin)
                + resolveBonePoint(player, 9, player.origin);
            return lArm * 0.5f;
        }
        return player.origin + Vec3{ 0.f, 0.f, 50.f };
    case 914:
        if (player.bonesValid) {
            const Vec3 rArm = resolveBonePoint(player, 13, player.origin)
                + resolveBonePoint(player, 14, player.origin);
            return rArm * 0.5f;
        }
        return player.origin + Vec3{ 0.f, 0.f, 50.f };
    case 23:
        return resolveBonePoint(player, 23, player.origin + Vec3{ 0.f, 0.f, 20.f });
    case 26:
        return resolveBonePoint(player, 26, player.origin + Vec3{ 0.f, 0.f, 20.f });
    default:
        return player.headPos;
    }
}

static bool aimTargetVisible(const PlayerData& player, bool stickyLock) {
    if (!g_cfg.aimRequireVisibility)
        return true;
    if (stickyLock)
        return true;
    if (!player.visibilityChecked)
        return true;
    if (player.isVisible)
        return true;
    return player.visibilityConfidence >= 0.35f;
}

Vec3 AimAssist::calcAngle(const Vec3& src, const Vec3& dst) const {
    Vec3 delta = dst - src;
    float len = delta.length();
    if (len < 0.001f) return { 0.f, 0.f, 0.f };

    float pitch = -std::asinf(delta.z / len) * kRad2Deg;
    float yaw   = std::atan2f(delta.y, delta.x) * kRad2Deg;
    return { pitch, yaw, 0.f };
}

void AimAssist::update(const Process& proc, const EntityManager& em, float screenW, float screenH) {
    (void)screenW;
    (void)screenH;

    AimDebugSnapshot dbg{};
    const std::uintptr_t prevLockedPawn = m_lockedPawn;
    auto submitDbg = [&](const char* skipReason = nullptr) {
        if (!g_cfg.aimDebugConsole)
            return;
        if (skipReason) {
            dbg.active = false;
            dbg.skipReason = skipReason;
        }
        aimDebugSubmit(dbg);
    };

    if (!g_cfg.aimAssistEnabled) {
        submitDbg("aim_disabled");
        return;
    }
    if (g_cfg.menuVisible || aimCalibrationBlocksFeatures()) {
        submitDbg("menu_or_calibration");
        return;
    }

    if (!g_cfg.aimSensitivityManual) {
        float liveSens = 0.f;
        if (readGameSensitivity(proc, em.clientBase(), liveSens, em.localPawn()))
            g_cfg.aimSensitivity = liveSens;
    }

    const int aimKey = (g_cfg.aimAssistKey >= 1 && g_cfg.aimAssistKey <= 255) ? g_cfg.aimAssistKey : 0;

    static auto s_lastTick = std::chrono::steady_clock::now();
    const auto tickNow = std::chrono::steady_clock::now();
    float dtMs = static_cast<float>(
        std::chrono::duration_cast<std::chrono::microseconds>(tickNow - s_lastTick).count()) / 1000.f;
    s_lastTick = tickNow;
    const float overlayFps = overlay_metrics::overlayFps();
    if (overlayFps > 30.f) {
        const float frameMs = 1000.f / overlayFps;
        if (dtMs <= 0.f || dtMs > frameMs * 3.f)
            dtMs = frameMs;
    } else if (dtMs <= 0.f || dtMs > 32.f) {
        dtMs = 8.f;
    }

    dbg.dtMs = dtMs;
    dbg.overlayFps = overlayFps;

    const uintptr_t clientBase = em.clientBase();
    const uintptr_t localPawn = em.localPawn();
    if (!localPawn) {
        submitDbg("no_local_pawn");
        return;
    }

    const AimWeaponGroup activeGroup = resolveActiveWeaponGroup(proc, clientBase, localPawn);
    const AimGroupConfig& aimCfg = g_cfg.aimByWeaponGroup[aimGroupIndex(activeGroup)];
    dbg.weaponGroup = aimWeaponGroupLabel(activeGroup);
    dbg.assistStyle = g_cfg.aimAssistStyle;
    dbg.aimSmooth = aimCfg.aimSmooth;
    dbg.supportStrength = g_cfg.aimSupportStrength;
    dbg.maxFov = std::clamp(aimCfg.aimFov, 2.0f, 30.0f);
    dbg.sens = g_cfg.aimSensitivity;

    const bool aimKeyHeld = (aimKey == 0) || ((GetAsyncKeyState(aimKey) & 0x8000) != 0);
    const bool supportAlwaysOn = (g_cfg.aimAssistStyle == 1) && g_cfg.aimSupportAlwaysOn;
    const bool wantAim = aimKeyHeld || supportAlwaysOn;
    const bool shooting = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const int quickShots = mem::read<int>(proc, localPawn + netvars::pawn::m_iShotsFired);
    const bool spraying = shooting || quickShots >= 1;
    const bool rcsStandaloneMode = aimCfg.rcsEnabled && aimCfg.rcsMode == 1;
    const bool wantRcs = aimCfg.rcsEnabled && spraying && (rcsStandaloneMode || wantAim);

    dbg.aimKeyHeld = aimKeyHeld;
    dbg.supportAlwaysOn = supportAlwaysOn;
    dbg.wantAim = wantAim;
    dbg.shooting = shooting;

    if (!wantAim && !wantRcs) {
        submitDbg("key_up");
        return;
    }

    const float boneOffsetZ = std::clamp(g_cfg.aimBoneOffsetZ, -25.f, 35.f);
    const float headForward = std::clamp(g_cfg.aimHeadForward, -25.f, 35.f);
    const float maxFov = std::clamp(aimCfg.aimFov, 2.0f, 30.0f);

    const Vec2 eyeAngles = mem::read<Vec2>(proc, localPawn + netvars::pawn::m_angEyeAngles);
    const float readPitch = eyeAngles.x;
    const float readYaw = eyeAngles.y;
    if (!std::isfinite(readPitch) || !std::isfinite(readYaw)) {
        submitDbg("bad_angles");
        return;
    }

    const Vec3 eyePos = readLocalEyePos(proc, localPawn);
    if (!isFiniteVec3(eyePos)) {
        submitDbg("bad_eye");
        return;
    }

    const Vec3 viewFwd = forwardFromAngles(readPitch, readYaw);

    const int localTeam = em.localTeam();
    const auto frame = em.publishedFrame();
    if (!frame) {
        submitDbg("no_snapshot");
        return;
    }
    const auto& players = frame->players;

    bool useHead = aimCfg.hitboxHead;
    bool useStomach = aimCfg.hitboxStomach;
    bool useChest = aimCfg.hitboxChest;
    bool usePelvis = aimCfg.hitboxPelvis;
    bool useArms = aimCfg.hitboxArms;
    bool useLegs = aimCfg.hitboxLegs;
    if (!useHead && !useStomach && !useChest && !usePelvis && !useArms && !useLegs)
        useHead = true;

    float bestScore = 1e9f;
    float bestTargetFov = maxFov;
    float bestTargetDist = 1e9f;
    Vec3 bestAimPoint{};
    bool foundTarget = false;
    std::uintptr_t bestPawn = 0;
    int winningBone = m_lockedBone;

    if (wantAim) {
    const auto testTarget = [&](const PlayerData& player, Vec3 targetPos, bool isHead, int boneSlot) {
        targetPos = applyAimPointOffsets(targetPos, player, isHead, boneOffsetZ, headForward);
        if (!isPlausibleAimPoint(targetPos, player.origin))
            return;

        float dist = 0.f;
        const float fovDeg = targetFovDeg(eyePos, viewFwd, targetPos, dist);
        const float fovLimit = effectiveMaxFov(maxFov, dist);
        if (fovDeg > fovLimit || fovDeg <= 0.f)
            return;

        const float score = fovDeg + dist * kFovDistanceWeight;
        if (score < bestScore || dist + kFovDistanceBias < bestTargetDist) {
            bestScore = score;
            bestTargetFov = fovDeg;
            bestTargetDist = dist;
            bestAimPoint = targetPos;
            bestPawn = player.pawn;
            winningBone = boneSlot;
            foundTarget = true;
        }
    };

    const auto evaluatePlayer = [&](const PlayerData& cached, bool liveRefresh) {
        PlayerData player = cached;
        const bool stickyLock = (player.pawn == m_lockedPawn);
        if (liveRefresh)
            refreshAimPlayerPositions(proc, player, false);

        if (!player.isValid || !player.isAlive || player.isLocalPlayer)
            return;
        if (player.isDormant)
            return;
        if (!aimTargetVisible(player, stickyLock))
            return;
        if (player.teamNum == 0 || player.teamNum == localTeam)
            return;
        if (player.health <= 0)
            return;

        if (stickyLock) {
            const bool isHead = (m_lockedBone == 6);
            testTarget(player, resolveBoneAimPoint(player, m_lockedBone), isHead, m_lockedBone);
            return;
        }

        if (useHead)
            testTarget(player, resolveBoneAimPoint(player, 6), true, 6);
        if (useStomach)
            testTarget(player, resolveBoneAimPoint(player, 2), false, 2);
        if (useChest)
            testTarget(player, resolveBoneAimPoint(player, 4), false, 4);
        if (usePelvis)
            testTarget(player, resolveBoneAimPoint(player, 0), false, 0);
        if (useArms && player.bonesValid) {
            testTarget(player, resolveBoneAimPoint(player, 89), false, 89);
            testTarget(player, resolveBoneAimPoint(player, 914), false, 914);
        }
        if (useLegs && player.bonesValid) {
            testTarget(player, resolveBoneAimPoint(player, 23), false, 23);
            testTarget(player, resolveBoneAimPoint(player, 26), false, 26);
        }
    };

    if (m_lockedPawn) {
        PlayerData lockedPlayer{};
        lockedPlayer.pawn = m_lockedPawn;
        lockedPlayer.isValid = true;
        for (const auto& player : players) {
            if (player.pawn != m_lockedPawn)
                continue;
            lockedPlayer.visibilityChecked = player.visibilityChecked;
            lockedPlayer.isVisible = player.isVisible;
            lockedPlayer.visibilityConfidence = player.visibilityConfidence;
            break;
        }
        evaluatePlayer(lockedPlayer, true);
        if (!foundTarget) {
            m_lockedPawn = 0;
            m_lostTargetMs = nowMs();
            bestScore = 1e9f;
        } else {
            const float fovLimit = effectiveMaxFov(maxFov, bestTargetDist);
            if (bestTargetFov > fovLimit) {
                m_lockedPawn = 0;
                m_lostTargetMs = nowMs();
                foundTarget = false;
            }
        }
    }

    if (!m_lockedPawn) {
        float preScore = 1e9f;
        std::uintptr_t prePawn = 0;
        for (const auto& player : players) {
            if (!player.isValid || !player.isAlive || player.isLocalPlayer || player.isDormant)
                continue;
            if (player.teamNum == 0 || player.teamNum == localTeam)
                continue;
            if (!aimTargetVisible(player, false))
                continue;

            Vec3 testHead = player.headPos;
            if (player.bonesValid)
                testHead = resolveBonePoint(player, 6, testHead);
            testHead = applyAimPointOffsets(testHead, player, true, boneOffsetZ, headForward);
            if (!isPlausibleAimPoint(testHead, player.origin))
                continue;

            float dist = 0.f;
            const float fovDeg = targetFovDeg(eyePos, viewFwd, testHead, dist);
            const float fovLimit = effectiveMaxFov(maxFov, dist);
            if (fovDeg > fovLimit || fovDeg <= 0.f)
                continue;

            const float score = fovDeg + dist * kFovDistanceWeight;
            if (score < preScore) {
                preScore = score;
                prePawn = player.pawn;
            }
        }

        if (prePawn) {
            for (const auto& player : players) {
                if (player.pawn != prePawn)
                    continue;
                evaluatePlayer(player, true);
                break;
            }
        }

        if (foundTarget) {
            m_lockedPawn = bestPawn;
            m_lockedBone = winningBone;
            m_lockedAimPoint = bestAimPoint;
            m_hasSmoothedAim = false;
            m_filtPitchErr = 0.f;
            m_filtYawErr = 0.f;
            m_lostTargetMs = 0;
            if (m_lockedPawn != prevLockedPawn) {
                dbg.eventLock = true;
                m_supportLockMs = nowMs();
                m_filtUserPitch = 0.f;
                m_filtUserYaw = 0.f;
                m_supportOutDx = 0.f;
                m_supportOutDy = 0.f;
            }
        }
    } else if (foundTarget) {
        const Vec3 prevAim = m_lockedAimPoint;
        m_lockedBone = winningBone;
        m_lockedAimPoint = bestAimPoint;
        if (isFiniteVec3(prevAim) && isFiniteVec3(m_lockedAimPoint)) {
            const Vec3 jump = m_lockedAimPoint - prevAim;
            const float jumpLen = jump.length();
            if (jumpLen > kMaxAimPointJump) {
                const float t = kMaxAimPointJump / jumpLen;
                m_lockedAimPoint = prevAim + jump * t;
            }
        }
        m_lostTargetMs = 0;
    }
    } else {
        m_hasSmoothedAim = false;
    }

    dbg.foundTarget = foundTarget;
    dbg.targetFov = bestTargetFov;
    dbg.targetDist = bestTargetDist;

    if (foundTarget && isFiniteVec3(m_lockedAimPoint)) {
        m_graceAimPoint = m_lockedAimPoint;
        m_hasGraceAim = true;
        m_graceUntilMs = nowMs() + supportGraceMs(activeGroup);
    }

    float aimPitchStep = 0.f;
    float aimYawStep = 0.f;
    float aimDxPix = 0.f;
    float aimDyPix = 0.f;
    float activeErrMag = 0.f;

    const bool hasLiveTarget = wantAim && foundTarget && isFiniteVec3(m_lockedAimPoint);
    const bool inSupportGrace = wantAim && !hasLiveTarget && m_hasGraceAim
        && nowMs() < m_graceUntilMs && isFiniteVec3(m_graceAimPoint);
    const Vec3 supportAimPoint = hasLiveTarget ? m_lockedAimPoint : m_graceAimPoint;

    if (wantAim && (hasLiveTarget || inSupportGrace)) {
        const float smooth = effectiveAimSmooth(aimCfg.aimSmooth);
        const bool supportMode = (g_cfg.aimAssistStyle == 1);
        const float tickScale = supportTickScale(dtMs, supportMode, activeGroup);
        const float degPerPx = (std::max)(0.1f, g_cfg.aimSensitivity) * 0.022f;
        dbg.supportMode = supportMode;
        dbg.degPerPx = degPerPx;
        dbg.tickScale = tickScale;

        if (!supportMode) {
            if (!m_hasSmoothedAim) {
                m_smoothedAimPoint = supportAimPoint;
                m_hasSmoothedAim = true;
            } else {
                const float pointAlpha = std::clamp(
                    tickScale * (6.f / (std::max)(1.f, smooth)), 0.04f, 0.22f);
                m_smoothedAimPoint.x += (supportAimPoint.x - m_smoothedAimPoint.x) * pointAlpha;
                m_smoothedAimPoint.y += (supportAimPoint.y - m_smoothedAimPoint.y) * pointAlpha;
                m_smoothedAimPoint.z += (supportAimPoint.z - m_smoothedAimPoint.z) * pointAlpha;
            }
        } else if (!m_hasSmoothedAim) {
            m_smoothedAimPoint = supportAimPoint;
            m_hasSmoothedAim = true;
        } else {
            const float aimAlpha = std::clamp(tickScale * 0.30f, 0.12f, 0.38f);
            m_smoothedAimPoint.x += (supportAimPoint.x - m_smoothedAimPoint.x) * aimAlpha;
            m_smoothedAimPoint.y += (supportAimPoint.y - m_smoothedAimPoint.y) * aimAlpha;
            m_smoothedAimPoint.z += (supportAimPoint.z - m_smoothedAimPoint.z) * aimAlpha;
        }

        const Vec3 errSource = m_smoothedAimPoint;
        const Vec3 aimAngles = calcAngle(eyePos, errSource);
        const float pitchErr = normalizeAngle(aimAngles.x - readPitch);
        const float yawErr = normalizeAngle(aimAngles.y - readYaw);
        const float errMag = std::sqrtf(pitchErr * pitchErr + yawErr * yawErr);
        activeErrMag = errMag;

        dbg.errMag = errMag;
        dbg.pitchErr = pitchErr;
        dbg.yawErr = yawErr;

        if (supportMode) {
            const float strength = std::clamp(g_cfg.aimSupportStrength, 0.f, 1.f);
            if (strength < 0.01f) {
                dbg.action = AimDebugAction::Idle;
            } else {
                float userPitch = 0.f;
                float userYaw = 0.f;
                if (m_viewInit) {
                    const float totalPitch = normalizeAngle(readPitch - m_prevViewPitch);
                    const float totalYaw = normalizeAngle(readYaw - m_prevViewYaw);
                    userPitch = totalPitch - m_lastAssistPitch * 0.25f;
                    userYaw = totalYaw - m_lastAssistYaw * 0.25f;
                } else {
                    m_viewInit = true;
                }
                m_prevViewPitch = readPitch;
                m_prevViewYaw = readYaw;

                const float userMoveMag = std::sqrtf(userPitch * userPitch + userYaw * userYaw);
                const float minMove = kSupportUserMoveMin * (1.05f - strength * 0.40f);
                dbg.userPitch = userPitch;
                dbg.userYaw = userYaw;
                dbg.userMoveMag = userMoveMag;
                dbg.userMoveMin = minMove;

                if (userMoveMag >= minMove && errMag <= maxFov) {
                    const float smoothVal = effectiveAimSmooth(aimCfg.aimSmooth);
                    float stepFrac = std::clamp(tickScale * (2.5f / smoothVal), 0.006f, 0.35f);
                    stepFrac *= strength;

                    aimPitchStep = pitchErr * stepFrac;
                    aimYawStep = yawErr * stepFrac;

                    const float toward = userPitch * pitchErr + userYaw * yawErr;
                    if (toward > 0.f && errMag > kAimDeadzoneDeg) {
                        const float align = toward / (userMoveMag * errMag + 0.001f);
                        const float boost = std::clamp(align, 0.f, 1.f) * strength * 0.10f;
                        aimPitchStep += userPitch * boost;
                        aimYawStep += userYaw * boost;
                    }

                    aimDxPix = -aimYawStep / degPerPx;
                    aimDyPix =  aimPitchStep / degPerPx;
                    dbg.action = AimDebugAction::Guide;
                    dbg.towardDot = toward;
                } else if (errMag > maxFov) {
                    dbg.action = AimDebugAction::OutOfFov;
                } else {
                    dbg.action = AimDebugAction::UserBelowMin;
                }
            }

            m_prevPitchErr = pitchErr;
            m_prevYawErr = yawErr;
            m_prevErrMag = errMag;
            m_lastAssistPitch = aimPitchStep;
            m_lastAssistYaw = aimYawStep;
        } else {
            m_viewInit = false;
            m_lastAssistPitch = 0.f;
            m_lastAssistYaw = 0.f;

            const float smoothVal = effectiveAimSmooth(aimCfg.aimSmooth);
            float stepFrac = std::clamp(tickScale * (2.5f / smoothVal), 0.006f, 0.35f);
            if (errMag < 2.5f)
                stepFrac = std::clamp(stepFrac * 0.85f, 0.03f, 0.28f);
            if (errMag < 0.55f)
                stepFrac = std::clamp(stepFrac * 0.65f, 0.03f, 0.18f);

            if (errMag > 0.05f) {
                dbg.action = AimDebugAction::Classic;
                aimPitchStep = pitchErr * stepFrac;
                aimYawStep = yawErr * stepFrac;
                aimDxPix = -aimYawStep / degPerPx;
                aimDyPix =  aimPitchStep / degPerPx;
            }
            m_prevErrMag = errMag;
        }
    } else if (wantAim) {
        m_lockedPawn = 0;
        if (prevLockedPawn)
            dbg.eventUnlock = true;
        dbg.action = AimDebugAction::NoTarget;
        m_lockedAimPoint = {};
        m_hasSmoothedAim = false;
        m_filtPitchErr = 0.f;
        m_filtYawErr = 0.f;
        m_prevErrMag = 999.f;
        m_prevPitchErr = 0.f;
        m_prevYawErr = 0.f;
        m_lastAssistPitch = 0.f;
        m_lastAssistYaw = 0.f;
        m_filtUserPitch = 0.f;
        m_filtUserYaw = 0.f;
        m_filtSettlePitch = 0.f;
        m_filtSettleYaw = 0.f;
        m_lastSupportAction = 0;
        m_supportOutDx = 0.f;
        m_supportOutDy = 0.f;
        m_supportLockMs = 0;
        if (nowMs() >= m_graceUntilMs) {
            m_hasGraceAim = false;
            m_graceAimPoint = {};
        }
        m_viewInit = false;
        if (m_lostTargetMs == 0)
            m_lostTargetMs = nowMs();
        m_dxRem *= 0.35f;
        m_dyRem *= 0.35f;
        AimHumanizer::instance().resetTargetState();
    }

    const bool rcsEnabled = aimCfg.rcsEnabled;
    const int rcsMode = std::clamp(aimCfg.rcsMode, 0, 1);
    const bool rcsStandalone = (rcsMode == 1);
    const float rcsX = std::clamp(aimCfg.rcsX, 0.f, 1.25f);
    const float rcsY = std::clamp(aimCfg.rcsY, 0.f, 1.25f);
    const float rcsSmooth = (std::max)(0.f, aimCfg.rcsSmooth);
    const float sens = (g_cfg.aimSensitivity < 0.1f) ? 0.1f : g_cfg.aimSensitivity;

    float rcsDxPix = 0.f;
    float rcsDyPix = 0.f;
    const bool runRcs = rcsEnabled && spraying && (rcsStandalone || foundTarget)
        && (rcsX > 0.001f || rcsY > 0.001f);

    if (runRcs) {
        const int shotsFired = mem::read<int>(proc, localPawn + netvars::pawn::m_iShotsFired);
        if (shotsFired >= 1) {
            Vec3 punch{};
            if (readAimPunch(proc, localPawn, punch)
                && std::isfinite(punch.x) && std::isfinite(punch.y)
                && std::fabs(punch.x) <= 15.f && std::fabs(punch.y) <= 15.f) {
                if (shotsFired < m_prevShotsFired) {
                    m_prevPunchPitch = 0.f;
                    m_prevPunchYaw = 0.f;
                }

                const float deltaPitch = punch.x - m_prevPunchPitch;
                const float deltaYaw = punch.y - m_prevPunchYaw;
                const float degPerPx = sens * 0.022f;

                rcsDxPix = (deltaYaw * 2.f) / degPerPx * rcsX;
                rcsDyPix = -(deltaPitch * 2.f) / degPerPx * rcsY;

                if (rcsSmooth > 0.001f) {
                    rcsDxPix /= (std::max)(1.f, rcsSmooth * 1.15f);
                    rcsDyPix /= (std::max)(1.f, rcsSmooth);
                }

                const float maxRcsPx = 22.f;
                const float rcsMag = std::sqrtf(rcsDxPix * rcsDxPix + rcsDyPix * rcsDyPix);
                if (rcsMag > maxRcsPx && rcsMag > 0.001f) {
                    const float s = maxRcsPx / rcsMag;
                    rcsDxPix *= s;
                    rcsDyPix *= s;
                }

                m_prevPunchPitch = punch.x;
                m_prevPunchYaw = punch.y;
            }
        } else {
            m_prevPunchPitch = 0.f;
            m_prevPunchYaw = 0.f;
        }
        m_prevShotsFired = shotsFired;
    } else {
        m_prevPunchPitch = 0.f;
        m_prevPunchYaw = 0.f;
        m_prevShotsFired = 0;
    }

    const bool supportEngaged = foundTarget
        || (m_hasGraceAim && nowMs() < m_graceUntilMs);
    const bool supportModeActive = wantAim && supportEngaged && g_cfg.aimAssistStyle == 1;
    const float supportStrength = std::clamp(g_cfg.aimSupportStrength, 0.f, 1.f);
    const float groupCap = supportModeActive ? supportGroupSpeedScale(activeGroup) : 1.f;
    const float maxAimStep = supportModeActive
        ? kMaxAimPxPerTick * (8.f / effectiveAimSmooth(aimCfg.aimSmooth))
            * (0.32f + supportStrength * 0.38f) * groupCap
        : kMaxAimPxPerTick * (12.f / effectiveAimSmooth(aimCfg.aimSmooth));
    const float aimMag = std::sqrtf(aimDxPix * aimDxPix + aimDyPix * aimDyPix);
    if (aimMag > maxAimStep && aimMag > 0.001f) {
        const float scale = maxAimStep / aimMag;
        aimDxPix *= scale;
        aimDyPix *= scale;
    }

    if (supportModeActive) {
        const float outBlend = 0.72f + supportStrength * 0.22f;
        m_supportOutDx += (aimDxPix - m_supportOutDx) * outBlend;
        m_supportOutDy += (aimDyPix - m_supportOutDy) * outBlend;
        aimDxPix = m_supportOutDx;
        aimDyPix = m_supportOutDy;
    } else {
        m_supportOutDx = 0.f;
        m_supportOutDy = 0.f;
    }

    const float humStrength = g_cfg.aimHumanizeEnabled
        ? (g_cfg.aimHumanizeMode == 0
            ? std::clamp(g_cfg.aimCalibAssistStrength, 0.f, 1.f)
            : std::clamp(g_cfg.aimHumanizeStrength, 0.f, 1.f))
        : 0.f;
    if (g_cfg.aimHumanizeEnabled && humStrength >= 0.01f && foundTarget
        && g_cfg.aimAssistStyle == 0) {
        if (g_cfg.aimHumanizeUseProfile)
            syncAimStyleForWeaponGroup(activeGroup);
        AimHumanizer::instance().apply(
            aimDxPix, aimDyPix, aimPitchStep, aimYawStep, true, (std::max)(1.f, dtMs));
    }

    dbg.aimPitchStep = aimPitchStep;
    dbg.aimYawStep = aimYawStep;
    dbg.aimDxPix = aimDxPix;
    dbg.aimDyPix = aimDyPix;
    dbg.maxAimStep = maxAimStep;
    dbg.aimMagAfterCap = std::sqrtf(aimDxPix * aimDxPix + aimDyPix * aimDyPix);
    dbg.supportOutDx = m_supportOutDx;
    dbg.supportOutDy = m_supportOutDy;

    const float finalDxPix = aimDxPix + rcsDxPix;
    const float finalDyPix = aimDyPix + rcsDyPix;

    if (std::fabs(finalDxPix) < 0.0001f && std::fabs(finalDyPix) < 0.0001f) {
        dbg.active = wantAim || wantRcs;
        dbg.dxRem = m_dxRem;
        dbg.dyRem = m_dyRem;
        if (!wantAim && wantRcs)
            dbg.action = AimDebugAction::RcsOnly;
        submitDbg();
        return;
    }

    m_dxRem += finalDxPix;
    m_dyRem += finalDyPix;

    const bool supportSend = g_cfg.aimAssistStyle == 1;
    const float sendBase = supportSend ? (0.58f - supportStrength * 0.22f) : 0.42f;
    const float sendThreshold = sendBase;
    LONG dx = 0;
    LONG dy = 0;
    if (std::fabs(m_dxRem) >= sendThreshold)
        dx = static_cast<LONG>(std::round(m_dxRem));
    if (std::fabs(m_dyRem) >= sendThreshold)
        dy = static_cast<LONG>(std::round(m_dyRem));
    if (dx == 0 && dy == 0 && !supportSend) {
        if (std::fabs(m_dxRem) >= 0.42f)
            dx = m_dxRem > 0.f ? 1L : -1L;
        if (std::fabs(m_dyRem) >= 0.42f)
            dy = m_dyRem > 0.f ? 1L : -1L;
    } else if (dx == 0 && dy == 0 && supportSend && supportStrength >= 0.45f) {
        const bool allowMicroSend = activeErrMag > 0.38f || shooting;
        if (allowMicroSend) {
            const float supportSendMin = 0.28f - supportStrength * 0.06f;
            if (std::fabs(m_dxRem) >= supportSendMin)
                dx = m_dxRem > 0.f ? 1L : -1L;
            if (std::fabs(m_dyRem) >= supportSendMin)
                dy = m_dyRem > 0.f ? 1L : -1L;
        }
    }
    if (dx == 0 && dy == 0) {
        dbg.active = wantAim || wantRcs;
        dbg.dxRem = m_dxRem;
        dbg.dyRem = m_dyRem;
        dbg.sendThreshold = sendThreshold;
        submitDbg();
        return;
    }

    m_dxRem -= static_cast<float>(dx);
    m_dyRem -= static_cast<float>(dy);

    dx = (std::max)(-127L, (std::min)(dx, 127L));
    dy = (std::max)(-127L, (std::min)(dy, 127L));

    if (dx == 0 && dy == 0) {
        dbg.active = wantAim || wantRcs;
        dbg.dxRem = m_dxRem;
        dbg.dyRem = m_dyRem;
        dbg.sendThreshold = sendThreshold;
        submitDbg();
        return;
    }

    dbg.active = wantAim || wantRcs;
    dbg.dxRem = m_dxRem;
    dbg.dyRem = m_dyRem;
    dbg.sendThreshold = sendThreshold;
    dbg.sentDx = static_cast<int>(dx);
    dbg.sentDy = static_cast<int>(dy);
    submitDbg();

    input_router::mouseMoveRelative(dx, dy);
}
