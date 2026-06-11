#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <cstddef>

bool loadTextureFromMemoryPng(ID3D11Device* device,
                              const unsigned char* data,
                              std::size_t size,
                              Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& outSrv,
                              int& outW,
                              int& outH);

bool loadBrandLogoFromResource(ID3D11Device* device,
                               Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& outSrv,
                               int& outW,
                               int& outH);
