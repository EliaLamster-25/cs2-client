#include "triggerbot.h"
#include "game/aim_style.h"
#include "game/entity_manager.h"
#include "offsets/offsets.h"
#include "offsets/netvars.h"
#include "memory/rpm.h"
#include "input/input_router.h"
#include "config.h"
#include <Windows.h>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>

static constexpr float kPi = 3.14159265f;
static constexpr float kRad2Deg = 180.f / kPi;
static constexpr float kDeg2Rad = kPi / 180.f;

static AimWeaponGroup classifyWeaponGroup(const char* weaponName) {
    if (!weaponName || !*weaponName)
        return AimWeaponGroup::Other;

    char lower[64]{};
    std::size_t i = 0;
    for (; weaponName[i] && i < (sizeof(lower) - 1); ++i)
        lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(weaponName[i])));
    lower[i] = '\0';

    auto has = [&](const char* token) { return std::strstr(lower, token) != nullptr; };

    if (has("awp") || has("ssg08") || has("scar20") || has("g3sg1"))
        return AimWeaponGroup::Snipers;

    if (has("ak47") || has("m4a1") || has("m4a1_silencer") || has("famas")
        || has("galilar") || has("aug") || has("sg556"))
        return AimWeaponGroup::Rifles;

    if (has("mac10") || has("mp9") || has("mp7") || has("mp5sd")
        || has("ump45") || has("p90") || has("bizon"))
        return AimWeaponGroup::Smg;

    if (has("nova") || has("xm1014") || has("mag7") || has("sawedoff")
        || has("m249") || has("negev"))
        return AimWeaponGroup::Heavy;

    if (has("deagle") || has("elite") || has("fiveseven") || has("glock")
        || has("hkp2000") || has("p250") || has("tec9") || has("cz75a")
        || has("revolver") || has("usp_silencer") || has("pistol"))
        return AimWeaponGroup::Pistols;

    return AimWeaponGroup::Other;
}

static AimWeaponGroup resolveActiveWeaponGroup(const Process& proc, uintptr_t clientBase, uintptr_t localPawn) {
    if (!clientBase || !localPawn)
        return AimWeaponGroup::Other;

    const std::uintptr_t entityList = mem::read<std::uintptr_t>(
        proc, clientBase + offsets::client::dwEntityList);
    if (!entityList)
        return AimWeaponGroup::Other;

    const auto weapSvcPtr = mem::read<std::uintptr_t>(
        proc, localPawn + netvars::pawn::m_pWeaponServices);
    if (!weapSvcPtr)
        return AimWeaponGroup::Other;

    const auto weapHandle = mem::read<std::uint32_t>(
        proc, weapSvcPtr + netvars::weapon_services::m_hActiveWeapon);
    if (!weapHandle || weapHandle == 0xFFFFFFFFu)
        return AimWeaponGroup::Other;

    const auto weapChunk = mem::read<std::uintptr_t>(
        proc, entityList + 0x10 + 0x8 * ((weapHandle & 0x7FFF) >> 9));
    if (!weapChunk)
        return AimWeaponGroup::Other;

    const std::uintptr_t weapIdentBase = weapChunk + std::uintptr_t(0x70) * (weapHandle & 0x1FF);
    const auto namePtr = mem::read<std::uintptr_t>(proc, weapIdentBase + netvars::grenade::m_designerName);
    if (!namePtr)
        return AimWeaponGroup::Other;

    char weaponName[64]{};
    mem::readArray(proc, namePtr, weaponName, static_cast<std::uint32_t>(sizeof(weaponName) - 1));
    return classifyWeaponGroup(weaponName);
}

static Vec3 viewForward(float pitchDeg, float yawDeg) {
    const float pitch = pitchDeg * kDeg2Rad;
    const float yaw = yawDeg * kDeg2Rad;
    const float cp = std::cosf(pitch);
    return { cp * std::cosf(yaw), cp * std::sinf(yaw), -std::sinf(pitch) };
}

static float angleToPointDeg(const Vec3& eye, const Vec3& viewFwd, const Vec3& target) {
    Vec3 toTarget = target - eye;
    const float dist = toTarget.length();
    if (dist < 0.001f)
        return 0.f;
    toTarget = toTarget * (1.f / dist);
    const float dot = std::clamp(
        viewFwd.x * toTarget.x + viewFwd.y * toTarget.y + viewFwd.z * toTarget.z,
        -1.f, 1.f);
    return std::acosf(dot) * kRad2Deg;
}

static bool crosshairAlignedOnTarget(const EntityManager& em, std::uintptr_t targetPawn,
                                     const Process& proc, std::uintptr_t localPawn) {
    const auto snap = em.publishedFrame();
    if (!snap || !localPawn)
        return false;

    const PlayerData* target = nullptr;
    for (const auto& p : snap->players) {
        if (p.isValid && p.isAlive && p.pawn == targetPawn) {
            target = &p;
            break;
        }
    }
    if (!target)
        return false;

    Vec3 localEye = snap->localOrigin;
    const Vec3 viewOffset = mem::read<Vec3>(proc, localPawn + netvars::pawn::m_vecViewOffset);
    localEye = localEye + viewOffset;

    const Vec2 eyeAng = mem::read<Vec2>(proc, localPawn + netvars::pawn::m_angEyeAngles);
    const Vec3 viewFwd = viewForward(eyeAng.x, eyeAng.y);

    Vec3 aimPoint = target->headPos;
    if (target->bonesValid)
        aimPoint = target->bones[6];

    const Vec3 toTarget = aimPoint - localEye;
    const float dist = toTarget.length();
    float maxAngle = 0.22f;
    if (dist > 1200.f)
        maxAngle = 0.42f;
    else if (dist > 600.f)
        maxAngle = 0.32f;

    const float ang = angleToPointDeg(localEye, viewFwd, aimPoint);
    return ang <= maxAngle;
}

void Triggerbot::update(const Process& proc, const EntityManager& em) {
    if (g_cfg.menuVisible || aimCalibrationBlocksFeatures())
        return;

    uintptr_t clientBase = em.clientBase();
    auto localPawn = em.localPawn();
    if (!localPawn) return;

    const AimWeaponGroup activeGroup = resolveActiveWeaponGroup(proc, clientBase, localPawn);
    const AimGroupConfig& aimCfg = g_cfg.aimByWeaponGroup[aimGroupIndex(activeGroup)];
    const bool triggerEnabled = aimCfg.triggerEnabled || g_cfg.triggerbotEnabled;
    if (!triggerEnabled)
        return;

    const int triggerKey = (aimCfg.triggerKey >= 1 && aimCfg.triggerKey <= 255)
        ? aimCfg.triggerKey
        : ((g_cfg.triggerbotKey >= 1 && g_cfg.triggerbotKey <= 255) ? g_cfg.triggerbotKey : 0);
    if (triggerKey != 0 && !(GetAsyncKeyState(triggerKey) & 0x8000))
        return;

    int crosshairId = mem::read<int>(proc, localPawn + netvars::pawn::m_iIDEntIndex);
    if (crosshairId <= 0) {
        m_onTarget = false;
        return;
    }

    auto entityList = mem::read<std::uintptr_t>(
        proc, clientBase + offsets::client::dwEntityList);
    if (!entityList) return;

    auto listEntry = mem::read<std::uintptr_t>(
        proc, entityList + 0x10 + 0x8 * ((crosshairId & 0x7FFF) >> 9));
    if (!listEntry) return;

    auto targetPawn = mem::read<std::uintptr_t>(
        proc, listEntry + 0x70 * (crosshairId & 0x1FF));
    if (!targetPawn) return;

    int targetTeam   = mem::read<int>(proc, targetPawn + netvars::pawn::m_iTeamNum);
    int targetHealth = mem::read<int>(proc, targetPawn + netvars::pawn::m_iHealth);
    int localTeam    = em.localTeam();

    if (targetTeam == 0 || targetTeam == localTeam || targetHealth <= 0) {
        m_onTarget = false;
        return;
    }

    if (!crosshairAlignedOnTarget(em, targetPawn, proc, localPawn)) {
        m_onTarget = false;
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const int shotCooldownMs = (std::max)(0, (std::min)(
        (aimCfg.triggerShotCooldownMs > 0 ? aimCfg.triggerShotCooldownMs : g_cfg.triggerbotShotCooldownMs),
        500));
    if (m_lastFireTime.time_since_epoch().count() != 0
        && now - m_lastFireTime < std::chrono::milliseconds(shotCooldownMs))
        return;

    if (!m_onTarget) {
        m_onTarget = true;
        m_onTargetSince = now;
    }

    const int delayMs = (std::max)(0, (std::min)(
        (aimCfg.triggerDelayMs > 0 ? aimCfg.triggerDelayMs : g_cfg.triggerbotDelayMs), 100));
    if (now - m_onTargetSince < std::chrono::milliseconds(delayMs))
        return;

    m_lastFireTime = now;
    input_router::mouseLeftClick();
}
