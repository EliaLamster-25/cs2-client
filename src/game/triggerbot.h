#pragma once
#include "memory/process.h"
#include <chrono>

/// @file triggerbot.h
/// @brief Automatically fires when the crosshair is over a living enemy.

class EntityManager;

class Triggerbot {
public:
    /// Call this from the entity-update thread (~120 Hz).
    void update(const Process& proc, const EntityManager& em);

private:
    std::chrono::steady_clock::time_point m_lastFireTime{};
    std::chrono::steady_clock::time_point m_onTargetSince{};
    bool m_onTarget = false;
};
