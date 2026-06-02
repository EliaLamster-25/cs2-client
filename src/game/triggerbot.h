#pragma once
#include "memory/process.h"
#include <atomic>

/// @file triggerbot.h
/// @brief Automatically fires when the crosshair is over a living enemy.

class EntityManager;

class Triggerbot {
public:
    /// Call this from the entity-update thread (~60 Hz).
    void update(const Process& proc, const EntityManager& em);

private:
    std::atomic<bool> m_firing{ false };
};
