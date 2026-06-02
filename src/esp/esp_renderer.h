#pragma once

#include "overlay/renderer.h"
#include "game/entity_manager.h"
#include "gui/font.h"

class EspRenderer {
public:
    void render(Renderer& r, const EntityManager& em);
    void drawGrenades(Renderer& r, const EntityManager::Snapshot& snap);
    void drawBomb(Renderer& r, const EntityManager::Snapshot& snap);
    void drawSpectators(Renderer& r, const EntityManager::Snapshot& snap);
    void drawRadar(Renderer& r, const EntityManager::Snapshot& snap);
    void drawVoteRevealer(Renderer& r);

    void setFont(const FontAtlas& f) { m_font = &f; }

private:
    void drawPlayerBox(Renderer& r,
                       const PlayerData& player,
                       const ViewMatrix& vm,
                              int localTeam,
                       bool occ,
                       const Vec2& feetSc,
                       const Vec2& headSc,
                       float sw, float sh,
                       const Vec3& predDelta);

    void drawSkeleton(Renderer& r,
                      const PlayerData& player,
                             const ViewMatrix& vm,
                             bool occ,
                             const Vec2& feetSc,
                             const Vec2& headSc,
                             const Vec3& predDelta);

    void drawChams(Renderer& r,
                   const PlayerData& player,
                   const ViewMatrix& vm,
                          bool occ,
                          const Vec2& feetSc,
                          const Vec2& headSc,
                          float sw, float sh,
                          const Vec3& predDelta);

    void drawPlayerInfo(Renderer& r,
                        const PlayerData& player,
                        float boxX, float boxY,
                        float boxW, float boxH,
                        bool occ);

    static unsigned int lerpColor(unsigned int a, unsigned int b, float t);

    const FontAtlas* m_font = nullptr;
};
