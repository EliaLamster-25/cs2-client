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

} // namespace

void GrenadeHelper::render(Renderer& r, const EntityManager::Snapshot& snap,
                           const ViewMatrix& vm, const FontAtlas* font) {
    if (!g_cfg.grenadeHelperEnabled || !font)
        return;

    const auto& mgr = GrenadeLineupManager::instance();
    const auto spots = mgr.spotsForMap(snap.currentMapName);
    if (spots.empty())
        return;

    const float sw = static_cast<float>(r.screenWidth());
    const float sh = static_cast<float>(r.screenHeight());
    const float cx = sw * 0.5f;
    const float cy = sh * 0.5f;

    const Vec3& origin = snap.localOrigin;
    const Vec3& viewAng = snap.localViewAngles;
    if (!snap.hasLocalPlayer)
        return;

    const GrenadeLineupManager::SpotEval* active = mgr.bestSpotForPlayer(
        snap.currentMapName, origin, viewAng, cx, cy, vm, sw, sh);

    for (const auto* spotPtr : spots) {
        const auto& spot = *spotPtr;
        GrenadeLineupManager::SpotEval ev = mgr.evaluateSpot(
            spot, origin, viewAng, cx, cy, vm, sw, sh);

        Vec2 standSc{};
        if (!vm.worldToScreen(spot.stand, standSc, sw, sh))
            continue;

        const unsigned int typeCol = grenadeTypeColor(spot.grenade);
        const float markerR = ev.inStandPos ? 7.f : 5.f;
        const unsigned int markerCol = ev.inStandPos ? 0xFF44FF88 : typeCol;

        r.drawFilledCircle(standSc.x, standSc.y, markerR + 2.f, 0xAA000000);
        r.drawFilledCircle(standSc.x, standSc.y, markerR, markerCol);

        char labelBuf[128];
        std::snprintf(labelBuf, sizeof(labelBuf), "%s", spot.title.c_str());
        drawLabel(r, *font, standSc.x + 10.f, standSc.y - 8.f, labelBuf, 0xFFFFFFFF, 13.f);
        if (!spot.description.empty())
            drawLabel(r, *font, standSc.x + 10.f, standSc.y + 6.f,
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
                drawLabel(r, *font, aimSc.x - tw * 0.5f, aimSc.y - 18.f, instr, dotCol, 12.f);
            }
        }
    }

    if (active && active->inStandPos && !active->aimAligned && active->spot) {
        char hint[160];
        std::snprintf(hint, sizeof(hint), "Line up: %s", active->spot->instruction.c_str());
        drawLabel(r, *font, cx - 80.f, cy + 28.f, hint, 0xFFFFFFAA, 13.f);
    } else if (active && active->aimAligned && active->spot) {
        drawLabel(r, *font, cx - 48.f, cy + 28.f, "Ready — throw!", 0xFF33FF66, 13.f);
    }
}
