#include <iostream>
#include <cmath>
#include <algorithm>
#include <string>

struct Vec3 { float x, y, z; };
struct SpectatorData {
    bool isValid;
    bool watchingLocal;
    int mode;
    std::string name;
    std::string targetName;
};

static float estimateBombRawDamageAtPoint(const Vec3& samplePos, const Vec3& bombPos, float damageRadius) {
    Vec3 delta{ samplePos.x - bombPos.x, samplePos.y - bombPos.y, samplePos.z - bombPos.z };
    const double distance = std::sqrt(static_cast<double>(delta.x) * delta.x + static_cast<double>(delta.y) * delta.y + static_cast<double>(delta.z) * delta.z);
    
    constexpr double kConservativeDistanceBias = 96.0;
    constexpr double kDamageTailMargin = 128.0;
    if (distance >= static_cast<double>(damageRadius) + kDamageTailMargin)
        return 0.f;
    
    constexpr double kBombGaussianBase = 450.7;
    constexpr double kBombGaussianOffset = 75.68;
    constexpr double kBombGaussianScale = 789.2;
    const double effectiveDistance = std::max(0.0, distance - kConservativeDistanceBias);
    const double scaledDistance = (effectiveDistance - kBombGaussianOffset) / kBombGaussianScale;
    const float rawDamage = static_cast<float>(kBombGaussianBase * std::exp(-(scaledDistance * scaledDistance)));
    return rawDamage >= 0.5f ? rawDamage : 0.f;
}

static float applyBombArmor(float damage, int armor) {
    if (damage <= 0.f || armor <= 0) return damage;
    constexpr float kArmorRatio = 0.5f;
    constexpr float kArmorBonus = 0.5f;
    float reducedDamage = damage * kArmorRatio;
    float armorDamage = (damage - reducedDamage) * kArmorBonus;
    if (armorDamage > static_cast<float>(armor)) {
        armorDamage = static_cast<float>(armor) * (1.f / kArmorBonus);
        reducedDamage = damage - armorDamage;
    }
    return reducedDamage;
}

int main() {
    float dmgDist[] = { 0, 500, 1000, 1500, 1700 };
    for (float dist : dmgDist) {
        Vec3 bomb{0,0,0};
        Vec3 hero{dist, 0, 0};
        
        float raw = estimateBombRawDamageAtPoint(hero, bomb, 1750.f);
        float ar100 = applyBombArmor(raw, 100);
        std::cout << "Dist: " << dist << " | Raw: " << raw << " | Ar100: " << ar100 << std::endl;
    }
    return 0;
}
