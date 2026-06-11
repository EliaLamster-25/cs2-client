#pragma once

#include "math/vector.h"
#include <memory>
#include <string>
#include <vector>

struct GrenadeLineupSpot {
    std::string id;
    std::string title;
    std::string description;
    std::string grenade;       // smoke, flash, he, molotov, decoy
    Vec3        stand{};
    float       standRadius = 18.f;
    Vec3        aimPoint{};
    float       aimPitch = 0.f;
    float       aimYaw = 0.f;
    bool        hasAimAngles = false;
    std::string throwType;     // throw, jump_throw, run_throw, etc.
    std::string instruction;
};

struct GrenadeLineupPack {
    std::string id;
    std::string name;
    std::string map;
    std::string description;
    std::vector<GrenadeLineupSpot> spots;
};

class GrenadeLineupManager {
public:
    static GrenadeLineupManager& instance();

    bool loadPackFromJson(const std::string& jsonText, std::string& error);
    bool loadPackFromFile(const std::string& path, std::string& error);
    void clearActivePack();

    const GrenadeLineupPack* activePack() const { return m_active.get(); }
    const std::string& activePackPath() const { return m_activePath; }

    std::vector<const GrenadeLineupSpot*> spotsForMap(const std::string& mapName) const;

    struct SpotEval {
        const GrenadeLineupSpot* spot = nullptr;
        bool  inStandPos = false;
        bool  aimAligned = false;
        float standDist = 0.f;
        float aimScreenDist = 0.f;
    };

    SpotEval evaluateSpot(const GrenadeLineupSpot& spot,
                          const Vec3& localOrigin,
                          const Vec3& localViewAngles,
                          float screenCx, float screenCy,
                          const struct ViewMatrix& vm,
                          float sw, float sh) const;

    const SpotEval* bestSpotForPlayer(const std::string& mapName,
                                      const Vec3& localOrigin,
                                      const Vec3& localViewAngles,
                                      float screenCx, float screenCy,
                                      const struct ViewMatrix& vm,
                                      float sw, float sh) const;

private:
    GrenadeLineupManager() = default;

    std::unique_ptr<GrenadeLineupPack> m_active;
    std::string m_activePath;
};
