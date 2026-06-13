#pragma once

#include "memory/process.h"
#include "math/vector.h"

class EntityManager;

class AimAssist {
public:
    void update(const Process& proc, const EntityManager& em, float screenW, float screenH);

private:
    Vec3 calcAngle(const Vec3& src, const Vec3& dst) const;

    float m_dxRem = 0.f;
    float m_dyRem = 0.f;
    float m_rcsPitchComp = 0.f;
    float m_rcsYawComp = 0.f;
    float m_prevPunchPitch = 0.f;
    float m_prevPunchYaw = 0.f;
    int   m_prevShotsFired = 0;

    std::uintptr_t m_lockedPawn = 0;
    int   m_lockedBone = 6;
    Vec3  m_lockedAimPoint{};
    Vec3  m_smoothedAimPoint{};
    bool  m_hasSmoothedAim = false;
    float m_filtPitchErr = 0.f;
    float m_filtYawErr = 0.f;
    float m_prevViewPitch = 0.f;
    float m_prevViewYaw = 0.f;
    float m_prevErrMag = 999.f;
    float m_prevPitchErr = 0.f;
    float m_prevYawErr = 0.f;
    float m_lastAssistPitch = 0.f;
    float m_lastAssistYaw = 0.f;
    float m_filtUserPitch = 0.f;
    float m_filtUserYaw = 0.f;
    float m_filtSettlePitch = 0.f;
    float m_filtSettleYaw = 0.f;
    int   m_lastSupportAction = 0;
    float m_supportOutDx = 0.f;
    float m_supportOutDy = 0.f;
    std::uint64_t m_supportLockMs = 0;
    std::uint64_t m_graceUntilMs = 0;
    Vec3  m_graceAimPoint{};
    bool  m_hasGraceAim = false;
    bool  m_viewInit = false;
    float m_rcsTotalDx = 0.f;
    float m_rcsTotalDy = 0.f;
    std::uint64_t m_lostTargetMs = 0;
};
