#pragma once

#include "loader_app.h"

#include <d3d11.h>
#include <wrl/client.h>
#include <Windows.h>

class Renderer;
class FontAtlas;
class Gui;

class LoaderUi {
public:
    bool init(Renderer& renderer, HWND hwnd);
    void shutdown();
    void render(LoaderApp& app, float dt);

    void onChar(wchar_t ch);
    void onKeyDown(WPARAM vk);

    bool closeRequested() const { return m_closeRequested; }

private:
    void drawTitlebar();
    void drawLoginPage(LoaderApp& app, float alpha, float slideY);
    void drawStatusPage(LoaderApp& app, float alpha, float slideY);
    void drawLogo(float cx, float cy, float frameSize, float logoSize, float alpha);
    void drawStatTile(float x, float y, float w, const char* label, const char* value,
                      unsigned int valueCol, unsigned int dotCol, float dotPulse, float alpha);
    bool drawGhostButton(const char* id, const char* label, float x, float y, float w, float h);
    bool drawPrimaryButton(const char* id, const char* label, float x, float y, float w, float h,
                           bool enabled, float alpha);

    FontAtlas* m_font = nullptr;
    Gui* m_gui = nullptr;
    Renderer* m_renderer = nullptr;
    HWND m_hwnd = nullptr;

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_logoSrv;
    int m_logoW = 0;
    int m_logoH = 0;

    char m_username[96]{};
    char m_password[96]{};
    bool m_showPassword = false;
    bool m_closeRequested = false;

    float m_pageAnim = 0.f;
    float m_time = 0.f;
};

bool loaderUiInit(Renderer& renderer, HWND hwnd);
void loaderUiShutdown();
void loaderUiRender(LoaderApp& app, float dt);
void loaderUiOnChar(wchar_t ch);
void loaderUiOnKeyDown(WPARAM vk);
bool loaderUiCloseRequested();
