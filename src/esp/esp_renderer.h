#pragma once

#include "overlay/renderer.h"
#include "game/entity_manager.h"
#include "gui/font.h"
#include "esp/grenade_helper.h"
#include "esp/sound_esp.h"
#include <d3d11.h>

class Process;

class EspRenderer {
public:
    void render(Renderer& r, const EntityManager& em, Process& proc);
    void drawGrenades(Renderer& r, const EntityManager::Snapshot& snap, const ViewMatrix& vm);
    void drawBomb(Renderer& r, const EntityManager::Snapshot& snap);
    void drawSpectators(Renderer& r, const EntityManager::Snapshot& snap);
    void drawRadar(Renderer& r, const EntityManager::Snapshot& snap);

    void setFont(const FontAtlas& f) { m_font = &f; }
    void setBrandLogo(ID3D11ShaderResourceView* srv, int w, int h) {
        m_brandLogoSrv = srv;
        m_brandLogoW = w;
        m_brandLogoH = h;
    }

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
                   const Vec2& feetSc,
                   const Vec2& headSc,
                   float sw, float sh,
                   const Vec3& predDelta);

    void drawPlayerInfo(Renderer& r,
                        const PlayerData& player,
                        float boxX, float boxY,
                        float boxW, float boxH,
                        bool occ);

    void drawFpsWatermark(Renderer& r, float sw, float sh, const EntityManager::Snapshot& snap);

    static unsigned int lerpColor(unsigned int a, unsigned int b, float t);

    const FontAtlas* m_font = nullptr;
    ID3D11ShaderResourceView* m_brandLogoSrv = nullptr;
    int m_brandLogoW = 0;
    int m_brandLogoH = 0;
    GrenadeHelper m_grenadeHelper;
    SoundEsp      m_soundEsp;
};
