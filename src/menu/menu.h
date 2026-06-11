#pragma once

#include "gui/gui.h"
#include "gui/font.h"
#include "cloud/cloud_api.h"
#include <array>
#include <cstdint>
#include <d3d11.h>
#include <string>
#include <vector>
#include <wrl/client.h>

class Renderer;
class EntityManager;
class Process;
struct AimGroupConfig;

class Menu {
public:
    bool init(Renderer& renderer);
    void render(Renderer& renderer, const EntityManager& em, const Process& proc, HWND hwnd);

    float menuAlpha() const { return m_menuAlpha; }
    const FontAtlas& font() const { return m_font; }
    ID3D11ShaderResourceView* brandLogoSrv() const { return m_brandLogoSrv.Get(); }
    int brandLogoW() const { return m_brandLogoW; }
    int brandLogoH() const { return m_brandLogoH; }

private:
    void drawSidebar();
    void drawUserPanel(float x, float y, float w);
    void tryLoadUserAvatar(ID3D11Device* device);
    void drawHeader();
    void drawPanelHeader(int tabIndex);
    void drawConfigsPanel();
    void drawAimbotPanel();
    void drawCalibrationPromptOverlay();
    void drawLeetifySetupPromptOverlay();
    void drawLeetifyAttribution(float x, float& y, float maxW);
    bool drawLeetifyProfileLink(float x, float y, std::uint64_t steamId);
    void drawAimHitboxWindow(AimGroupConfig& aimCfg);
    void drawEspPanel();
    void drawPlayersEspPreviewWindow(bool occluded);
    void drawWorldPanel();
    void drawNadesPanel(const EntityManager& em);
    void drawSystemPanel();
    void drawIntelPanel(const EntityManager& em);
    void drawPlayerInfoPanel(const EntityManager& em);
    void drawPlayerDetailOverlay();

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
    float m_winX = 0, m_winY = 0, m_winW = 1040, m_winH = 760;
    float m_headerH = 0;
    float m_sideW = 182;

    // Component preview state
    int   m_aimBoneIdx = 0;
    int   m_aimSubTab = 0; // 0=Global, 1..N=weapon groups
    std::array<float, 7> m_aimSubTabAnim{};
    int   m_aimWeaponGroupIdx = 0;
    float m_aimbotScroll = 0.f;
    float m_aimbotMaxScroll = 0.f;
    float m_aimbotScrollTarget = 0.f;

    // World tab
    int   m_worldSubTab = 0; // 0=Grenades, 1=Radar, 2=Web
    std::array<float, 3> m_worldSubTabAnim{};
    float m_worldScroll = 0.f;
    float m_worldMaxScroll = 0.f;
    float m_worldScrollTarget = 0.f;

    // System tab scrolling
    float m_systemScroll = 0.f;
    float m_systemMaxScroll = 0.f;
    float m_systemScrollTarget = 0.f;

    // Intel tab scrolling
    float m_intelScroll = 0.f;
    float m_intelMaxScroll = 0.f;
    float m_intelScrollTarget = 0.f;

    // Player Info tab scrolling
    float m_playerInfoScroll = 0.f;
    float m_playerInfoMaxScroll = 0.f;
    float m_playerInfoScrollTarget = 0.f;

    // ESP tab
    int   m_playerSubTab = 0; // 0=General, 1=Visible, 2=Occluded, 3=Style
    std::array<float, 4> m_playerSubTabAnim{};
    float m_playersScroll = 0.f;
    float m_playersMaxScroll = 0.f;
    float m_playersScrollTarget = 0.f;

    // Config tab scrolling
    float m_configsScroll = 0.f;
    float m_configsMaxScroll = 0.f;
    float m_configsScrollTarget = 0.f;

    // Nades tab scrolling
    float m_nadesScroll = 0.f;
    float m_nadesScrollTarget = 0.f;

    std::array<float, 8> m_tabHover{};
    std::array<float, 8> m_tabActive{};
    float m_tabIndicatorY = -1.f;
    float m_tabIndicatorAlpha = 0.f;

    // Config tab state
    std::vector<std::string> m_configFiles;
    int   m_selectedConfig = -1;
    bool  m_configsLoaded = false;
    std::string m_configStatus;
    float m_configStatusTimer = 0.f;
    std::vector<CloudConfigEntry> m_cloudConfigs;
    bool m_cloudConfigsLoaded = false;
    int m_selectedCloudConfig = -1;
    std::vector<CloudLineupEntry> m_cloudLineups;
    bool m_cloudLineupsLoaded = false;
    int m_selectedCloudLineup = -1;
    std::string m_nadesStatus;
    float m_nadesStatusTimer = 0.f;
    std::string m_aimCalibStatus;
    float m_aimCalibStatusTimer = 0.f;

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
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_leetifyBadgeSrv;
    int   m_leetifyBadgeW = 0;
    int   m_leetifyBadgeH = 0;
    char  m_leetifyKeyBuf[256]{};
    bool  m_leetifyKeyBufInit = false;
    std::string m_leetifyPromptStatus;
    float m_leetifyPromptStatusTimer = 0.f;
    bool  m_leetifyPromptDismissed = false;
    std::uint64_t m_playerDetailSteamId = 0;
    bool m_playerDetailOpenedThisFrame = false;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_zoomInIconSrv;
    int   m_zoomInIconW = 0;
    int   m_zoomInIconH = 0;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_userAvatarSrv;
    int   m_userAvatarW = 0;
    int   m_userAvatarH = 0;
    std::string m_userAvatarLoadedUrl;
    bool  m_userAvatarFetchAttempted = false;
    std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, 8> m_tabIconSrv{};
    std::array<int, 8> m_tabIconW{};
    std::array<int, 8> m_tabIconH{};
    std::array<bool, 8> m_tabIconFullColor{};
    bool  m_espPreviewDragging = false;
    int   m_espPreviewDragElement = -1;
    bool  m_espPreviewResetDone = false;

    const Process*       m_renderProc = nullptr;
    const EntityManager* m_renderEm = nullptr;
};
