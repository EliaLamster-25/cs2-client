#pragma once

#include "overlay/renderer.h"
#include "game/entity_manager.h"
#include "gui/font.h"
#include "math/vector.h"
#include <cstdint>
#include <vector>

class SoundEsp {
public:
    void update(const EntityManager::Snapshot& snap);
    void render(Renderer& r, const ViewMatrix& vm, const FontAtlas* font);

private:
    struct Event {
        Vec3     pos{};
        int      type = 0; // 0=footstep, 1=gunshot
        float    age = 0.f;
        float    maxAge = 1.8f;
        int      team = 0;
    };

    struct PrevPlayer {
        std::uintptr_t pawn = 0;
        Vec3           origin{};
        int            shotsFired = 0;
        float          horizSpeed = 0.f;
    };

    std::vector<Event> m_events;
    PrevPlayer m_prev[64]{};
};
