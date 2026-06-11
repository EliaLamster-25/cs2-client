#include <imgui.h>
#include <d3d11.h>

// Mirrors ImGui_ImplDX11_Data from imgui_impl_dx11.cpp (v1.91) — sampler swap only.
struct LoaderDx11Backend {
    ID3D11Device* pd3dDevice;
    ID3D11DeviceContext* pd3dDeviceContext;
    IDXGIFactory* pFactory;
    ID3D11Buffer* pVB;
    ID3D11Buffer* pIB;
    ID3D11VertexShader* pVertexShader;
    ID3D11InputLayout* pInputLayout;
    ID3D11Buffer* pVertexConstantBuffer;
    ID3D11PixelShader* pPixelShader;
    ID3D11SamplerState* pFontSampler;
};

void loaderApplyPointSampling() {
    auto* bd = static_cast<LoaderDx11Backend*>(ImGui::GetIO().BackendRendererUserData);
    if (!bd || !bd->pd3dDevice)
        return;

    if (bd->pFontSampler) {
        bd->pFontSampler->Release();
        bd->pFontSampler = nullptr;
    }

    D3D11_SAMPLER_DESC desc{};
    desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    bd->pd3dDevice->CreateSamplerState(&desc, &bd->pFontSampler);
}
