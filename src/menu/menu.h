#pragma once

#include "gui/gui.h"
#include "gui/font.h"
#include <array>
#include <cstdint>
#include <d3d11.h>
#include <string>
#include <vector>
#include <wrl/client.h>

class Renderer;
class EntityManager;
struct AimGroupConfig;

class Menu {
public:
    bool init(Renderer& renderer);
    void render(Renderer& renderer, const EntityManager& em, HWND hwnd);

    float menuAlpha() const { return m_menuAlpha; }
    const FontAtlas& font() const { return m_font; }

private:
    void drawSidebar();
    void drawHeader();
    void drawPanelHeader(int tabIndex);
    void drawConfigsPanel();
    void drawAimbotPanel();
    void drawAimHitboxWindow(AimGroupConfig& aimCfg);
    void drawPlayersPanel();
    void drawPlayersEspPreviewWindow(bool occluded);
    void drawVisualsPanel();
    void drawExtraPanel();
    void drawIntelPanel();

    Gui       m_gui;
    FontAtlas m_font;
    int       m_activeTab  = 0;
    float     m_menuAlpha  = 0.f;
    float     m_frameDt    = 0.f;
    float     m_uiTime     = 0.f;
    int64_t   m_lastTick   = 0;
    int64_t   m_tickFreq   = 0;
    int       m_screenW    = 0;
    int       m_screenH    = 0;
    float     m_uiScale    = 1.f;
    float     m_contentScale = 1.f;

    // Layout constants (computed each frame)
    float m_winX = 0, m_winY = 0, m_winW = 970, m_winH = 752;
    float m_headerH = 0;
    float m_sideW = 182;

    // Component preview state
    int   m_aimBoneIdx = 0;
    int   m_aimSubTab = 0; // 0=Global, 1..N=weapon groups
    int   m_aimWeaponGroupIdx = 0;
    float m_aimbotScroll = 0.f;
    float m_aimbotMaxScroll = 0.f;
    float m_aimbotScrollTarget = 0.f;

    // Visuals tab scrolling
    float m_visualsScroll = 0.f;
    float m_visualsMaxScroll = 0.f;
    float m_visualsScrollTarget = 0.f;

    // Intel tab scrolling
    float m_intelScroll = 0.f;
    float m_intelMaxScroll = 0.f;
    float m_intelScrollTarget = 0.f;

    // Players tab
    int   m_playerSubTab = 0; // 0=Settings, 1=Visible, 2=Occluded, 3=Sizes/Offsets
    float m_playersScroll = 0.f;
    float m_playersMaxScroll = 0.f;
    float m_playersScrollTarget = 0.f;

    // Config tab scrolling
    float m_configsScroll = 0.f;
    float m_configsMaxScroll = 0.f;
    float m_configsScrollTarget = 0.f;

    std::array<float, 6> m_tabHover{};
    std::array<float, 6> m_tabActive{};
    float m_tabIndicatorY = -1.f;
    float m_tabIndicatorAlpha = 0.f;

    // Config tab state
    std::vector<std::string> m_configFiles;
    int   m_selectedConfig = -1;
    bool  m_configsLoaded = false;
    std::string m_configStatus;
    float m_configStatusTimer = 0.f;

    int   m_openConfigPopup = -1;

    bool  m_draggingMenu = false;
    float m_dragOffsetX = 0.f;
    float m_dragOffsetY = 0.f;
    bool  m_prevMenuVisible = false;
    bool  m_aimFirstOpenInitialized = false;

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_hitboxModelSrv;
    int   m_hitboxModelW = 0;
    int   m_hitboxModelH = 0;

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_espPreviewModelSrv;
    int   m_espPreviewModelW = 0;
    int   m_espPreviewModelH = 0;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_brandLogoSrv;
    int   m_brandLogoW = 0;
    int   m_brandLogoH = 0;
    bool  m_espPreviewDragging = false;
    int   m_espPreviewDragElement = -1;
    bool  m_espPreviewResetDone = false;
};
