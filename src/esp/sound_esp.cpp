#include "esp/sound_esp.h"
#include "config.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>

namespace {

float horizSpeed(const Vec3& vel) {
    return std::sqrt(vel.x * vel.x + vel.y * vel.y);
}

float horizMoveDist(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

void drawGroundRing(Renderer& r, const ViewMatrix& vm, Vec3 center, float radius,
                    unsigned int col, float thickness, float sw, float sh) {
    constexpr int kSegs = 28;
    Vec2 prev{};
    bool havePrev = false;
    center.z -= 4.f;

    for (int i = 0; i <= kSegs; ++i) {
        const float ang = (static_cast<float>(i) / static_cast<float>(kSegs)) * 6.2831853f;
        const Vec3 pt = {
            center.x + std::cosf(ang) * radius,
            center.y + std::sinf(ang) * radius,
            center.z
        };
        Vec2 sc{};
        if (!vm.worldToScreen(pt, sc, sw, sh)) {
            havePrev = false;
            continue;
        }
        if (havePrev)
            r.drawLine(prev.x, prev.y, sc.x, sc.y, col, thickness);
        prev = sc;
        havePrev = true;
    }
}

} // namespace

void SoundEsp::update(const EntityManager::Snapshot& snap, float dtSec) {
    if (!g_cfg.soundEspEnabled)
        return;

    const float dt = (dtSec > 0.f && dtSec < 0.25f) ? dtSec : (1.f / 128.f);
    for (auto& ev : m_events)
        ev.age += dt;
    m_events.erase(std::remove_if(m_events.begin(), m_events.end(),
        [](const Event& e) { return e.age >= e.maxAge; }), m_events.end());

    const std::uint64_t nowMs = GetTickCount64();
    int slot = 0;
    for (const auto& p : snap.players) {
        if (slot >= 64) break;
        if (!p.isValid || !p.isAlive || p.isLocalPlayer || p.isDormant) {
            m_prev[slot] = {};
            ++slot;
            continue;
        }

        PrevPlayer& prev = m_prev[slot];
        const float speed = horizSpeed(p.velocity);
        const bool visible = !p.visibilityChecked || p.isVisible;

        if (prev.pawn == p.pawn) {
            if (g_cfg.soundEspGunshots && p.shotsFired > prev.shotsFired) {
                Event ev{};
                ev.pos = p.origin;
                ev.type = 1;
                ev.maxAge = 2.0f;
                ev.team = p.teamNum;
                ev.fromVisible = visible;
                m_events.push_back(ev);
            }

            if (g_cfg.soundEspFootsteps) {
                const float moveH = horizMoveDist(p.origin, prev.origin);
                const float dz = std::fabs(p.origin.z - prev.origin.z);
                const float speedDelta = std::fabs(speed - prev.horizSpeed);
                const bool moving = speed > 5.f || moveH > 0.15f || speedDelta > 8.f;
                const bool plausibleStep = (moveH > 0.15f && moveH < 140.f && dz < 48.f)
                    || (speed > 18.f && speedDelta > 10.f && moveH > 0.08f);
                const bool cooledDown = (nowMs - prev.lastStepMs) >= 160ull;

                if (moving && plausibleStep && cooledDown) {
                    Event ev{};
                    ev.pos = p.origin;
                    ev.type = 0;
                    ev.maxAge = 1.5f;
                    ev.team = p.teamNum;
                    ev.fromVisible = visible;
                    m_events.push_back(ev);
                    prev.lastStepMs = nowMs;
                }
            }
        } else {
            prev.lastStepMs = 0;
        }

        prev.pawn = p.pawn;
        prev.origin = p.origin;
        prev.shotsFired = p.shotsFired;
        prev.horizSpeed = speed;
        ++slot;
    }
}

void SoundEsp::render(Renderer& r, const ViewMatrix& vm, const FontAtlas* font) {
    (void)font;
    if (!g_cfg.soundEspEnabled || m_events.empty())
        return;

    const float sw = static_cast<float>(r.screenWidth());
    const float sh = static_cast<float>(r.screenHeight());
    const float thickness = std::clamp(g_cfg.soundEspLineThickness, 0.5f, 6.f);
    const float expand = std::clamp(g_cfg.soundEspRingExpand, 8.f, 120.f);

    for (const auto& ev : m_events) {
        if (ev.fromVisible && !g_cfg.soundEspVisibleEnabled)
            continue;
        if (!ev.fromVisible && !g_cfg.soundEspOccludedEnabled)
            continue;

        const float t = 1.f - (ev.age / ev.maxAge);
        if (t <= 0.f) continue;

        const float* rgba = (ev.type == 1) ? g_cfg.soundEspGunshotColor : g_cfg.soundEspFootstepColor;
        const unsigned int base = rgbaToArgb(rgba);
        const unsigned int col = (base & 0x00FFFFFFu)
            | (static_cast<unsigned int>(rgba[3] * 255.f * t) << 24);

        if (g_cfg.soundEspMode == 1) {
            const float worldR = (ev.type == 1 ? 28.f : 22.f) + ev.age * (expand * 0.55f);
            drawGroundRing(r, vm, ev.pos, worldR, col, thickness, sw, sh);
            Vec2 centerSc{};
            if (t > 0.55f && vm.worldToScreen(ev.pos, centerSc, sw, sh))
                r.drawFilledCircle(centerSc.x, centerSc.y, 2.f, col);
            continue;
        }

        Vec2 sc{};
        if (!vm.worldToScreen(ev.pos, sc, sw, sh))
            continue;

        const float baseR = (ev.type == 1) ? 14.f : 10.f;
        const float radius = baseR + ev.age * expand;

        r.drawCircle(sc.x, sc.y, radius, col, thickness);
        if (t > 0.55f)
            r.drawFilledCircle(sc.x, sc.y, 2.5f, col);
    }
}
