#include "esp/sound_esp.h"
#include "config.h"

#include <algorithm>
#include <cmath>

namespace {

float horizSpeed(const Vec3& vel) {
    return std::sqrt(vel.x * vel.x + vel.y * vel.y);
}

} // namespace

void SoundEsp::update(const EntityManager::Snapshot& snap) {
    if (!g_cfg.soundEspEnabled)
        return;

    const float dt = 1.f / 128.f;
    for (auto& ev : m_events)
        ev.age += dt;
    m_events.erase(std::remove_if(m_events.begin(), m_events.end(),
        [](const Event& e) { return e.age >= e.maxAge; }), m_events.end());

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

        if (prev.pawn == p.pawn) {
            if (g_cfg.soundEspGunshots && p.shotsFired > prev.shotsFired) {
                Event ev{};
                ev.pos = p.origin;
                ev.type = 1;
                ev.maxAge = 2.2f;
                ev.team = p.teamNum;
                m_events.push_back(ev);
            }

            if (g_cfg.soundEspFootsteps) {
                const Vec3 delta{
                    p.origin.x - prev.origin.x,
                    p.origin.y - prev.origin.y,
                    p.origin.z - prev.origin.z,
                };
                const float moveDist = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
                if (speed > 42.f && moveDist > 2.f && moveDist < 80.f) {
                    Event ev{};
                    ev.pos = p.origin;
                    ev.type = 0;
                    ev.maxAge = 1.4f;
                    ev.team = p.teamNum;
                    m_events.push_back(ev);
                }
            }
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

    for (const auto& ev : m_events) {
        Vec2 sc{};
        if (!vm.worldToScreen(ev.pos, sc, sw, sh))
            continue;

        const float t = 1.f - (ev.age / ev.maxAge);
        if (t <= 0.f) continue;

        const float radius = (ev.type == 1 ? 18.f : 12.f) + ev.age * (ev.type == 1 ? 48.f : 28.f);
        const unsigned int base = (ev.type == 1) ? 0xFFFF6644 : 0xFF88AAFF;
        const unsigned int col = (base & 0x00FFFFFFu) | (static_cast<unsigned int>(180.f * t) << 24);

        r.drawCircle(sc.x, sc.y, radius, col, 1.5f);
        r.drawFilledCircle(sc.x, sc.y, 3.f, col);
    }
}
