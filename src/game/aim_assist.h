#pragma once

#include "memory/process.h"
#include "math/vector.h"

class EntityManager;

class AimAssist {
public:
    void update(const Process& proc, const EntityManager& em);

private:
    Vec3 calcAngle(const Vec3& src, const Vec3& dst) const;
    float m_dxRem = 0.f;  // accumulated sub-pixel X remainder
    float m_dyRem = 0.f;  // accumulated sub-pixel Y remainder
    float m_rcsPitchComp = 0.f; // smoothed vertical recoil compensation in angle space
    float m_rcsYawComp = 0.f;   // smoothed horizontal recoil compensation in angle space
    float m_prevPunchPitch = 0.f;
    float m_prevPunchYaw = 0.f;
};
