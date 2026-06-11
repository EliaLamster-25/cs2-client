#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <string>
#include <vector>

struct LoaderUiState {
    char username[96]{};
    char password[96]{};
    bool rememberMe = false;
    bool showPassword = false;
    bool busy = false;
    bool loggedIn = false;
    bool envOk = false;
    std::string statusLine;
    std::vector<std::string> logLines;
};

class LoaderApp;

void loaderUiSetWindow(HWND hwnd);

bool loaderUiInit(ID3D11Device* device);
void loaderUiShutdown();
void loaderUiFrame(LoaderApp& app, LoaderUiState& state, float dt);

Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> loaderBrandLogoSrv();
int loaderBrandLogoW();
int loaderBrandLogoH();
