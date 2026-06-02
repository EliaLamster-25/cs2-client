#include "aim_assist.h"
#include "game/entity_manager.h"
#include "game/player.h"
#include "offsets/offsets.h"
#include "offsets/netvars.h"
#include "memory/rpm.h"
#include "config.h"
#include <Windows.h>
#include <cmath>
#include <algorithm>
#include <cctype>
#include <cstring>

static constexpr float kPi = 3.14159265f;
static constexpr float kRad2Deg = 180.f / kPi;
static constexpr float kDeg2Rad = kPi / 180.f;
static constexpr float kRcsXCompBias = 1.12f;
static inline float normalizeAngle(float ang) {
    while (ang > 180.f) ang -= 360.f;
    while (ang < -180.f) ang += 360.f;
    return ang;
}

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

Vec3 AimAssist::calcAngle(const Vec3& src, const Vec3& dst) const {
    Vec3 delta = dst - src;
    float len = delta.length();
    if (len < 0.001f) return { 0.f, 0.f, 0.f };

    float pitch = -std::asinf(delta.z / len) * kRad2Deg;
    float yaw   = std::atan2f(delta.y, delta.x) * kRad2Deg;
    return { pitch, yaw, 0.f };
}

void AimAssist::update(const Process& proc, const EntityManager& em) {
    if (!g_cfg.aimAssistEnabled || g_cfg.menuVisible)
        return;

    const int aimKey = (g_cfg.aimAssistKey >= 1 && g_cfg.aimAssistKey <= 255) ? g_cfg.aimAssistKey : 0;
    if (aimKey != 0 && !(GetAsyncKeyState(aimKey) & 0x8000))
        return;

    uintptr_t clientBase = em.clientBase();

    auto localPawn = em.localPawn();
    if (!localPawn) return;

    const AimWeaponGroup activeGroup = resolveActiveWeaponGroup(proc, clientBase, localPawn);
    const AimGroupConfig& aimCfg = g_cfg.aimByWeaponGroup[aimGroupIndex(activeGroup)];
    const float boneOffsetZ = std::clamp(g_cfg.aimBoneOffsetZ, -25.f, 35.f);
    const float headForward = std::clamp(g_cfg.aimHeadForward, -25.f, 35.f);

    Vec2 eyeAngles = mem::read<Vec2>(
        proc, localPawn + netvars::pawn::m_angEyeAngles);
    Vec3 currentAngles{ eyeAngles.x, eyeAngles.y, 0.f };
    if (!std::isfinite(currentAngles.x) || !std::isfinite(currentAngles.y)
        || (std::fabs(currentAngles.x) < 0.001f && std::fabs(currentAngles.y) < 0.001f)) {
        currentAngles = mem::read<Vec3>(
            proc, clientBase + offsets::client::dwViewAngles);
    }

    auto sceneNode = mem::read<std::uintptr_t>(
        proc, localPawn + netvars::pawn::m_pGameSceneNode);
    if (!sceneNode) return;

    Vec3 localOrigin = mem::read<Vec3>(
        proc, sceneNode + netvars::scene_node::m_vecAbsOrigin);
    Vec3 viewOffset = mem::read<Vec3>(
        proc, localPawn + netvars::pawn::m_vecViewOffset);
    Vec3 eyePos = localOrigin + viewOffset;

    int localTeam = em.localTeam();
    auto players = em.players();

    float bestFov = std::clamp(aimCfg.aimFov, 2.0f, 30.0f);
    Vec3 bestTarget{};
    bool foundTarget = false;
    bool useHead = aimCfg.hitboxHead;
    bool useStomach = aimCfg.hitboxStomach;
    bool useChest = aimCfg.hitboxChest;
    bool usePelvis = aimCfg.hitboxPelvis;
    bool useArms = aimCfg.hitboxArms;
    bool useLegs = aimCfg.hitboxLegs;
    if (!useHead && !useStomach && !useChest && !usePelvis && !useArms && !useLegs)
        return;

    for (const auto& player : players) {
        if (!player.isValid || !player.isAlive || player.isLocalPlayer)
            continue;
        if (player.isDormant)
            continue;
        if (g_cfg.aimRequireVisibility
            && (!player.visibilityChecked || !player.isVisible))
            continue;
        if (player.teamNum == 0 || player.teamNum == localTeam)
            continue;
        if (player.health <= 0)
            continue;

        auto testTarget = [&](Vec3 targetPos, bool isHead) {
            targetPos.z += boneOffsetZ;
            if (isHead) {
                float yawRad = player.eyeYaw * kDeg2Rad;
                targetPos.x += std::cosf(yawRad) * headForward;
                targetPos.y += std::sinf(yawRad) * headForward;
            }

            Vec3 aimAngles = calcAngle(eyePos, targetPos);
            float pitchFov = std::fabs(normalizeAngle(aimAngles.x - currentAngles.x));
            float yawFov   = std::fabs(normalizeAngle(aimAngles.y - currentAngles.y));
            float fov      = std::sqrt(pitchFov * pitchFov + yawFov * yawFov);

            if (fov <= bestFov) {
                bestFov = fov;
                bestTarget = aimAngles;
                foundTarget = true;
            }
        };

        if (useHead) {
            Vec3 head = player.headPos;
            if (player.bonesValid)
                head = player.bones[6];
            testTarget(head, true);
        }
        if (useStomach) {
            Vec3 stomach = player.origin + Vec3{ 0.f, 0.f, 40.f };
            if (player.bonesValid)
                stomach = player.bones[2];
            testTarget(stomach, false);
        }
        if (useChest) {
            Vec3 chest = player.origin + Vec3{ 0.f, 0.f, 54.f };
            if (player.bonesValid)
                chest = player.bones[4];
            testTarget(chest, false);
        }
        if (usePelvis) {
            Vec3 pelvis = player.origin + Vec3{ 0.f, 0.f, 34.f };
            if (player.bonesValid)
                pelvis = player.bones[0];
            testTarget(pelvis, false);
        }
        if (useArms) {
            if (player.bonesValid) {
                Vec3 lArm = (player.bones[8] + player.bones[9]) * 0.5f;
                Vec3 rArm = (player.bones[13] + player.bones[14]) * 0.5f;
                testTarget(lArm, false);
                testTarget(rArm, false);
            } else {
                float yawRad = player.eyeYaw * kDeg2Rad;
                Vec3 right{ -std::sinf(yawRad), std::cosf(yawRad), 0.f };
                Vec3 armBase = player.origin + Vec3{ 0.f, 0.f, 56.f };
                testTarget(armBase + right * 14.f, false);
                testTarget(armBase + right * -14.f, false);
            }
        }
        if (useLegs) {
            if (player.bonesValid) {
                testTarget(player.bones[23], false);
                testTarget(player.bones[26], false);
            } else {
                float yawRad = player.eyeYaw * kDeg2Rad;
                Vec3 right{ -std::sinf(yawRad), std::cosf(yawRad), 0.f };
                Vec3 legBase = player.origin + Vec3{ 0.f, 0.f, 22.f };
                testTarget(legBase + right * 7.f, false);
                testTarget(legBase + right * -7.f, false);
            }
        }
    }

    float aimPitchStep = 0.f;
    float aimYawStep = 0.f;
    if (foundTarget) {
        float smooth = (std::max)(1.f, aimCfg.aimSmooth);
        float pitchDelta = normalizeAngle(bestTarget.x - currentAngles.x);
        float yawDelta   = normalizeAngle(bestTarget.y - currentAngles.y);

        // Clamp pitch to prevent looking too far up/down
        float newPitch = currentAngles.x + pitchDelta / smooth;
        if (newPitch > 89.f)  newPitch = 89.f;
        if (newPitch < -89.f) newPitch = -89.f;
        aimPitchStep = newPitch - currentAngles.x;
        aimYawStep = yawDelta / smooth;
    }

    bool rcsEnabled = aimCfg.rcsEnabled;
    int rcsMode = aimCfg.rcsMode;
    if (rcsMode < 0) rcsMode = 0;
    if (rcsMode > 1) rcsMode = 1;
    const bool rcsStandalone = (rcsMode == 1);
    float rcsX = std::clamp(aimCfg.rcsX, 0.f, 1.f);
    float rcsY = std::clamp(aimCfg.rcsY, 0.f, 1.f);
    float rcsSmooth = (std::max)(0.f, aimCfg.rcsSmooth);
    float sens = (g_cfg.aimSensitivity < 0.1f) ? 0.1f : g_cfg.aimSensitivity;
    float rcsDxPix = 0.f;
    float rcsDyPix = 0.f;
    if (rcsEnabled && (rcsStandalone || foundTarget) && (rcsX > 0.001f || rcsY > 0.001f)) {
        int shotsFired = mem::read<int>(proc, localPawn + netvars::pawn::m_iShotsFired);
        if (shotsFired > 1) {
            auto aimPunchSvc = mem::read<std::uintptr_t>(
                proc, localPawn + netvars::pawn::m_pAimPunchServices);
            if (aimPunchSvc) {
                Vec3 predictable = mem::read<Vec3>(
                    proc, aimPunchSvc + netvars::aim_punch_services::m_predictableBaseAngle);
                Vec3 unpredictable = mem::read<Vec3>(
                    proc, aimPunchSvc + netvars::aim_punch_services::m_unpredictableBaseAngle);
                Vec3 punch{
                    predictable.x + unpredictable.x,
                    predictable.y + unpredictable.y,
                    predictable.z + unpredictable.z
                };
                if (std::isfinite(predictable.x) && std::isfinite(predictable.y)
                    && std::isfinite(unpredictable.x) && std::isfinite(unpredictable.y)) {
                    // Convert punch delta directly to mouse-space correction.
                    const float dPitch = punch.x - m_prevPunchPitch;
                    const float dYaw = punch.y - m_prevPunchYaw;
                    const float rawDx = ((dYaw * 2.0f) / sens) * 50.0f * rcsX * kRcsXCompBias;
                    const float rawDy = (-(dPitch * 2.0f) / sens) * 50.0f * rcsY;
                    m_prevPunchPitch = punch.x;
                    m_prevPunchYaw = punch.y;

                    // rcsSmooth=0 means no smoothing (full correction immediately).
                    if (rcsSmooth <= 0.001f) {
                        m_rcsYawComp = rawDx;
                        m_rcsPitchComp = rawDy;
                    } else {
                        m_rcsYawComp += (rawDx - m_rcsYawComp) / rcsSmooth;
                        m_rcsPitchComp += (rawDy - m_rcsPitchComp) / rcsSmooth;
                    }
                    rcsDxPix = m_rcsYawComp;
                    rcsDyPix = m_rcsPitchComp;
                } else {
                    m_rcsPitchComp = 0.f;
                    m_rcsYawComp = 0.f;
                    m_prevPunchPitch = 0.f;
                    m_prevPunchYaw = 0.f;
                }
            } else {
                m_rcsPitchComp = 0.f;
                m_rcsYawComp = 0.f;
                m_prevPunchPitch = 0.f;
                m_prevPunchYaw = 0.f;
            }
        } else {
            m_rcsPitchComp = 0.f;
            m_rcsYawComp = 0.f;
            m_prevPunchPitch = 0.f;
            m_prevPunchYaw = 0.f;
        }
    } else {
        m_rcsPitchComp = 0.f;
        m_rcsYawComp = 0.f;
        m_prevPunchPitch = 0.f;
        m_prevPunchYaw = 0.f;
    }

    float pitchStep = aimPitchStep;
    float yawStep   = aimYawStep;

    // Convert aimbot angle-step to mouse pixels and combine with mouse-space RCS.
    float degPerPx = sens * 0.022f;
    float aimDxPix = -yawStep / degPerPx;
    float aimDyPix = pitchStep / degPerPx;

    float finalDxPix = aimDxPix + rcsDxPix;
    float finalDyPix = aimDyPix + rcsDyPix;

    if (!foundTarget && std::fabs(finalDxPix) < 0.0001f && std::fabs(finalDyPix) < 0.0001f)
        return;

    // Accumulate sub-pixel remainders
    m_dxRem += finalDxPix;
    m_dyRem += finalDyPix;

    LONG dx = static_cast<LONG>(m_dxRem);
    LONG dy = static_cast<LONG>(m_dyRem);
    m_dxRem -= dx;
    m_dyRem -= dy;

    dx = (std::max)(-100L, (std::min)(dx, 100L));
    dy = (std::max)(-100L, (std::min)(dy, 100L));

    if (dx == 0 && dy == 0) {
        if (std::fabs(finalDxPix) > 0.45f)
            dx = (finalDxPix > 0.f) ? 1 : -1;
        if (std::fabs(finalDyPix) > 0.45f)
            dy = (finalDyPix > 0.f) ? 1 : -1;
    }

    if (dx == 0 && dy == 0)
        return;

    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &input, sizeof(INPUT));
}
