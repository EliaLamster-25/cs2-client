#include "esp/grenade_helper.h"
#include "game/grenade_lineup.h"
#include "config.h"

#include <cmath>
#include <cstring>

namespace {

unsigned int grenadeTypeColor(const std::string& type) {
    if (type.find("smoke") != std::string::npos) return 0xFFAAAAAA;
    if (type.find("flash") != std::string::npos) return 0xFFFFFFCC;
    if (type.find("he") != std::string::npos || type.find("frag") != std::string::npos)
        return 0xFFFF5555;
    if (type.find("molotov") != std::string::npos || type.find("inc") != std::string::npos)
        return 0xFFFF8800;
    return 0xFF55FF88;
}

void drawLabel(Renderer& r, const FontAtlas& font, float x, float y,
               const char* text, unsigned int color, float size) {
    if (!text || !*text) return;
    r.drawText(font, x, y, text, color, size);
}

Vec3 forwardFromViewAngles(const Vec3& ang) {
    const float pitch = ang.x * (3.14159265f / 180.f);
    const float yaw = ang.y * (3.14159265f / 180.f);
    const float cp = std::cosf(pitch);
    return { cp * std::cosf(yaw), cp * std::sinf(yaw), -std::sinf(pitch) };
}

GrenadeLineupSpot buildTestSpot(const EntityManager::Snapshot& snap) {
    GrenadeLineupSpot spot{};
    spot.id = "test_spot";
    spot.title = "Test smoke";
    spot.description = "Example lineup at your feet";
    spot.grenade = "smoke";
    spot.stand = snap.localOrigin;
    spot.standRadius = 48.f;
    const Vec3 fwd = forwardFromViewAngles(snap.localViewAngles);
    spot.aimPoint = {
        snap.localOrigin.x + fwd.x * 420.f,
        snap.localOrigin.y + fwd.y * 420.f,
        snap.localOrigin.z + fwd.z * 420.f + 80.f
    };
    spot.throwType = "throw";
    spot.instruction = "Throw";
    return spot;
}

void renderSpot(Renderer& r, const FontAtlas& font, const GrenadeLineupSpot& spot,
                const EntityManager::Snapshot& snap, const ViewMatrix& vm,
                float cx, float cy, float sw, float sh,
                GrenadeLineupManager& mgr) {
    GrenadeLineupManager::SpotEval ev = mgr.evaluateSpot(
        spot, snap.localOrigin, snap.localViewAngles, cx, cy, vm, sw, sh);

    Vec2 standSc{};
    if (!vm.worldToScreen(spot.stand, standSc, sw, sh))
        return;

    const unsigned int typeCol = grenadeTypeColor(spot.grenade);
    const float markerR = ev.inStandPos ? 7.f : 5.f;
    const unsigned int markerCol = ev.inStandPos ? 0xFF44FF88 : typeCol;

    r.drawFilledCircle(standSc.x, standSc.y, markerR + 2.f, 0xAA000000);
    r.drawFilledCircle(standSc.x, standSc.y, markerR, markerCol);

    char labelBuf[128];
    std::snprintf(labelBuf, sizeof(labelBuf), "%s", spot.title.c_str());
    drawLabel(r, font, standSc.x + 10.f, standSc.y - 8.f, labelBuf, 0xFFFFFFFF, 13.f);
    if (!spot.description.empty())
        drawLabel(r, font, standSc.x + 10.f, standSc.y + 6.f,
                  spot.description.c_str(), 0xFFAAAAAA, 11.f);

    if (ev.inStandPos && spot.aimPoint.lengthSq() > 1.f) {
        Vec2 aimSc{};
        if (vm.worldToScreen(spot.aimPoint, aimSc, sw, sh)) {
            const unsigned int dotCol = ev.aimAligned ? 0xFF33FF66 : 0xFFFFFFFF;
            r.drawLine(cx, cy, aimSc.x, aimSc.y, 0x88FFFFFF, 1.f);
            r.drawFilledCircle(aimSc.x, aimSc.y, 5.f, 0xAA000000);
            r.drawFilledCircle(aimSc.x, aimSc.y, 4.f, dotCol);
            const char* instr = spot.instruction.c_str();
            const float tw = static_cast<float>(std::strlen(instr)) * 6.5f;
            drawLabel(r, font, aimSc.x - tw * 0.5f, aimSc.y - 18.f, instr, dotCol, 12.f);
        }
    }
}

} // namespace

void GrenadeHelper::render(Renderer& r, const EntityManager::Snapshot& snap,
                           const ViewMatrix& vm, const FontAtlas* font) {
    if (!g_cfg.grenadeHelperEnabled || !font)
        return;

    if (!snap.hasLocalPlayer)
        return;

    const float sw = static_cast<float>(r.screenWidth());
    const float sh = static_cast<float>(r.screenHeight());
    const float cx = sw * 0.5f;
    const float cy = sh * 0.5f;

    auto& mgr = GrenadeLineupManager::instance();
    const auto spots = mgr.spotsForMap(snap.currentMapName);

    if (g_cfg.grenadeHelperTestSpot) {
        const GrenadeLineupSpot testSpot = buildTestSpot(snap);
        renderSpot(r, *font, testSpot, snap, vm, cx, cy, sw, sh, mgr);
    }

    const GrenadeLineupManager::SpotEval* active = nullptr;
    if (!spots.empty()) {
        active = mgr.bestSpotForPlayer(
            snap.currentMapName, snap.localOrigin, snap.localViewAngles, cx, cy, vm, sw, sh);
    } else if (g_cfg.grenadeHelperTestSpot) {
        static GrenadeLineupSpot testSpot{};
        static GrenadeLineupManager::SpotEval testEval{};
        testSpot = buildTestSpot(snap);
        testEval = mgr.evaluateSpot(testSpot, snap.localOrigin, snap.localViewAngles,
                                   cx, cy, vm, sw, sh);
        testEval.spot = &testSpot;
        active = &testEval;
    }

    for (const auto* spotPtr : spots)
        renderSpot(r, *font, *spotPtr, snap, vm, cx, cy, sw, sh, mgr);

    if (active && active->inStandPos && !active->aimAligned && active->spot) {
        char hint[160];
        std::snprintf(hint, sizeof(hint), "Line up: %s", active->spot->instruction.c_str());
        drawLabel(r, *font, cx - 80.f, cy + 28.f, hint, 0xFFFFFFAA, 13.f);
    } else if (active && active->aimAligned && active->spot) {
        drawLabel(r, *font, cx - 48.f, cy + 28.f, "Ready — throw!", 0xFF33FF66, 13.f);
    }
}
