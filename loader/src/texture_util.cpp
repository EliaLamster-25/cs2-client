#include "texture_util.h"

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <vector>

#ifndef IDR_BRAND_LOGO
#define IDR_BRAND_LOGO 102
#endif

using Microsoft::WRL::ComPtr;

bool loadTextureFromMemoryPng(ID3D11Device* device,
                              const unsigned char* data,
                              std::size_t size,
                              ComPtr<ID3D11ShaderResourceView>& outSrv,
                              int& outW,
                              int& outH) {
    outSrv.Reset();
    outW = 0;
    outH = 0;
    if (!device || !data || size == 0)
        return false;

    // Only balance CoUninitialize when this call actually incremented the apartment ref
    // count (S_OK). S_FALSE means COM was already initialized on this thread — uninit
    // here would tear down the loader's apartment and crash later in the message loop.
    HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool needsUninit = (coHr == S_OK);
    if (FAILED(coHr) && coHr != RPC_E_CHANGED_MODE)
        return false;

    ComPtr<IWICImagingFactory> wicFactory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;

    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&wicFactory));
    if (SUCCEEDED(hr))
        hr = wicFactory->CreateStream(stream.GetAddressOf());
    if (SUCCEEDED(hr))
        hr = stream->InitializeFromMemory(const_cast<BYTE*>(data), static_cast<DWORD>(size));
    if (SUCCEEDED(hr))
        hr = wicFactory->CreateDecoderFromStream(stream.Get(), nullptr,
            WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf());
    if (SUCCEEDED(hr))
        hr = decoder->GetFrame(0, frame.GetAddressOf());
    if (SUCCEEDED(hr))
        hr = wicFactory->CreateFormatConverter(converter.GetAddressOf());
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    }

    UINT width = 0;
    UINT height = 0;
    if (SUCCEEDED(hr))
        hr = converter->GetSize(&width, &height);
    if (FAILED(hr) || width == 0 || height == 0) {
        if (needsUninit)
            CoUninitialize();
        return false;
    }

    const UINT stride = width * 4;
    std::vector<unsigned char> pixels(static_cast<std::size_t>(stride) * height);
    hr = converter->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data());
    if (FAILED(hr)) {
        if (needsUninit)
            CoUninitialize();
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA sub{};
    sub.pSysMem = pixels.data();
    sub.SysMemPitch = stride;

    ComPtr<ID3D11Texture2D> tex;
    hr = device->CreateTexture2D(&desc, &sub, tex.GetAddressOf());
    if (SUCCEEDED(hr)) {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = desc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        hr = device->CreateShaderResourceView(tex.Get(), &srvDesc, outSrv.GetAddressOf());
    }

    if (SUCCEEDED(hr)) {
        outW = static_cast<int>(width);
        outH = static_cast<int>(height);
    }

    if (needsUninit)
        CoUninitialize();
    return SUCCEEDED(hr);
}

bool loadBrandLogoFromResource(ID3D11Device* device,
                               ComPtr<ID3D11ShaderResourceView>& outSrv,
                               int& outW,
                               int& outH) {
    HMODULE mod = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(mod, MAKEINTRESOURCEW(IDR_BRAND_LOGO), MAKEINTRESOURCEW(10));
    if (!res)
        return false;
    HGLOBAL mem = LoadResource(mod, res);
    if (!mem)
        return false;
    const auto* data = static_cast<const unsigned char*>(LockResource(mem));
    const DWORD size = SizeofResource(mod, res);
    if (!data || size == 0)
        return false;
    return loadTextureFromMemoryPng(device, data, size, outSrv, outW, outH);
}
