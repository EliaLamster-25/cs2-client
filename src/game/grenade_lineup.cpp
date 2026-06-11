#include "game/grenade_lineup.h"
#include "math/matrix.h"
#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <fstream>
#include <memory>

using json = nlohmann::json;

namespace {

std::string normalizeMapName(std::string map) {
    for (char& c : map) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (map.rfind("de_", 0) != 0 && map.rfind("cs_", 0) != 0 && map.rfind("ar_", 0) != 0) {
        if (map.find('_') == std::string::npos)
            map = "de_" + map;
    }
    return map;
}

Vec3 readVec3(const json& arr) {
    if (!arr.is_array() || arr.size() < 3)
        return {};
    return Vec3{ arr[0].get<float>(), arr[1].get<float>(), arr[2].get<float>() };
}

float horizontalDist(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

float angleDeltaDeg(float a, float b) {
    float d = std::fmod(a - b + 180.f, 360.f);
    if (d < 0.f) d += 360.f;
    d -= 180.f;
    return std::fabs(d);
}

} // namespace

GrenadeLineupManager& GrenadeLineupManager::instance() {
    static GrenadeLineupManager mgr;
    return mgr;
}

bool GrenadeLineupManager::loadPackFromJson(const std::string& jsonText, std::string& error) {
    try {
        json j = json::parse(jsonText);
        auto pack = std::make_unique<GrenadeLineupPack>();
        pack->id = j.value("id", "");
        pack->name = j.value("name", j.value("title", "Lineup Pack"));
        pack->map = normalizeMapName(j.value("map", ""));
        pack->description = j.value("description", "");

        if (!j.contains("spots") || !j["spots"].is_array()) {
            error = "Missing spots array.";
            return false;
        }

        int idx = 0;
        for (const auto& s : j["spots"]) {
            GrenadeLineupSpot spot{};
            spot.id = s.value("id", "spot_" + std::to_string(idx));
            spot.title = s.value("title", spot.id);
            spot.description = s.value("description", "");
            spot.grenade = s.value("grenade", "smoke");
            spot.stand = readVec3(s["stand"]);
            spot.standRadius = s.value("stand_radius", 18.f);
            if (s.contains("aim_point"))
                spot.aimPoint = readVec3(s["aim_point"]);
            if (s.contains("aim_pitch") && s.contains("aim_yaw")) {
                spot.aimPitch = s["aim_pitch"].get<float>();
                spot.aimYaw = s["aim_yaw"].get<float>();
                spot.hasAimAngles = true;
            }
            spot.throwType = s.value("throw", "throw");
            spot.instruction = s.value("instruction", spot.throwType);
            if (spot.instruction.empty()) {
                if (spot.throwType == "jump_throw") spot.instruction = "Jump + throw";
                else if (spot.throwType == "run_throw") spot.instruction = "Run + throw";
                else spot.instruction = "Throw";
            }
            pack->spots.push_back(std::move(spot));
            ++idx;
        }

        m_active = std::move(pack);
        m_activePath.clear();
        error.clear();
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

bool GrenadeLineupManager::loadPackFromFile(const std::string& path, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "Could not open lineup file.";
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!loadPackFromJson(text, error))
        return false;
    m_activePath = path;
    return true;
}

void GrenadeLineupManager::clearActivePack() {
    m_active.reset();
    m_activePath.clear();
}

std::vector<const GrenadeLineupSpot*>
GrenadeLineupManager::spotsForMap(const std::string& mapName) const {
    std::vector<const GrenadeLineupSpot*> out;
    if (!m_active)
        return out;

    const std::string norm = normalizeMapName(mapName);
    if (!m_active->map.empty() && normalizeMapName(m_active->map) != norm)
        return out;

    out.reserve(m_active->spots.size());
    for (const auto& spot : m_active->spots)
        out.push_back(&spot);
    return out;
}

GrenadeLineupManager::SpotEval
GrenadeLineupManager::evaluateSpot(const GrenadeLineupSpot& spot,
                                   const Vec3& localOrigin,
                                   const Vec3& localViewAngles,
                                   float screenCx, float screenCy,
                                   const ViewMatrix& vm,
                                   float sw, float sh) const {
    SpotEval ev{};
    ev.spot = &spot;
    ev.standDist = horizontalDist(localOrigin, spot.stand);
    ev.inStandPos = ev.standDist <= spot.standRadius;

    Vec2 aimSc{};
    bool hasAimScreen = false;
    if (spot.aimPoint.lengthSq() > 1.f)
        hasAimScreen = vm.worldToScreen(spot.aimPoint, aimSc, sw, sh);

    if (hasAimScreen) {
        const float dx = aimSc.x - screenCx;
        const float dy = aimSc.y - screenCy;
        ev.aimScreenDist = std::sqrt(dx * dx + dy * dy);
        ev.aimAligned = ev.inStandPos && ev.aimScreenDist <= 6.f;
    } else if (spot.hasAimAngles && ev.inStandPos) {
        const float pitchOk = angleDeltaDeg(localViewAngles.x, spot.aimPitch) <= 1.2f;
        const float yawOk = angleDeltaDeg(localViewAngles.y, spot.aimYaw) <= 1.5f;
        ev.aimAligned = pitchOk && yawOk;
        ev.aimScreenDist = pitchOk && yawOk ? 0.f : 999.f;
    }

    return ev;
}

const GrenadeLineupManager::SpotEval*
GrenadeLineupManager::bestSpotForPlayer(const std::string& mapName,
                                        const Vec3& localOrigin,
                                        const Vec3& localViewAngles,
                                        float screenCx, float screenCy,
                                        const ViewMatrix& vm,
                                        float sw, float sh) const {
    static thread_local SpotEval best{};
    best = {};

    const auto spots = spotsForMap(mapName);
    if (spots.empty())
        return nullptr;

    float bestScore = 1e9f;
    for (const auto* spot : spots) {
        SpotEval ev = evaluateSpot(*spot, localOrigin, localViewAngles,
                                   screenCx, screenCy, vm, sw, sh);
        float score = ev.standDist;
        if (ev.inStandPos)
            score -= 500.f;
        if (ev.aimAligned)
            score -= 1000.f;
        if (score < bestScore) {
            bestScore = score;
            best = ev;
        }
    }

    return best.spot ? &best : nullptr;
}
