#include "renderer.h"
#include "gui/font.h"
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <iostream>
#include <cstring>
#include <cmath>
#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx11.h>
#include "config.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")

// ─── HLSL flat-colour shaders ────────────────────────────────────────────────

static const char* kFlatVS = R"(
    struct VS_IN { float2 pos : POSITION; float4 col : COLOR; };
    struct PS_IN { float4 pos : SV_POSITION; float4 col : COLOR; };
    cbuffer CB0 : register(b0) { float2 screenSize; float2 _pad; };
    PS_IN VSMain(VS_IN i) {
        PS_IN o;
        o.pos = float4(i.pos.x/screenSize.x*2.0-1.0, i.pos.y/screenSize.y*-2.0+1.0, 0.0, 1.0);
        o.col = i.col; return o;
    }
    float4 PSMain(PS_IN i) : SV_TARGET { return i.col; }
)";

// ─── HLSL textured shaders (pos + uv + colour, R8_UNORM .r = alpha) ──────────

static const char* kTxVS = R"(
    struct VS_IN { float2 pos : POSITION; float2 uv : TEXCOORD; float4 col : COLOR; };
    struct PS_IN { float4 pos : SV_POSITION; float2 uv : TEXCOORD; float4 col : COLOR; };
    cbuffer CB0 : register(b0) { float2 screenSize; float2 _pad; };
    PS_IN VSMain(VS_IN i) {
        PS_IN o;
        o.pos = float4(i.pos.x/screenSize.x*2.0-1.0, i.pos.y/screenSize.y*-2.0+1.0, 0.0, 1.0);
        o.uv = i.uv; o.col = i.col; return o;
    }
    float4 PSMain(PS_IN i) : SV_TARGET {
        float alpha = tex.Sample(sam, i.uv).r;
        return float4(i.col.rgb, i.col.a * alpha);
    }
)";

// ⚠ NOTE: The PS above references `tex` and `sam`; they are declared as
//   Texture2D tex : register(t0);
//   SamplerState sam : register(s0);
// in the full concatenated shader string below (kTxSrc).

static const char* kTxRgbaSrc = R"(
    Texture2D tex : register(t0);
    SamplerState sam : register(s0);
    struct VS_IN { float2 pos : POSITION; float2 uv : TEXCOORD; float4 col : COLOR; };
    struct PS_IN { float4 pos : SV_POSITION; float2 uv : TEXCOORD; float4 col : COLOR; };
    cbuffer CB0 : register(b0) { float2 screenSize; float2 _pad; };
    PS_IN VSMain(VS_IN i) {
        PS_IN o;
        o.pos = float4(i.pos.x/screenSize.x*2.0-1.0, i.pos.y/screenSize.y*-2.0+1.0, 0.0, 1.0);
        o.uv = i.uv; o.col = i.col; return o;
    }
    float4 PSMain(PS_IN i) : SV_TARGET {
        float4 t = tex.Sample(sam, i.uv);
        return t * i.col;
    }
)";

static const char* kTxSrc = R"(
    Texture2D tex : register(t0);
    SamplerState sam : register(s0);
    struct VS_IN { float2 pos : POSITION; float2 uv : TEXCOORD; float4 col : COLOR; };
    struct PS_IN { float4 pos : SV_POSITION; float2 uv : TEXCOORD; float4 col : COLOR; };
    cbuffer CB0 : register(b0) { float2 screenSize; float2 _pad; };
    PS_IN VSMain(VS_IN i) {
        PS_IN o;
        o.pos = float4(i.pos.x/screenSize.x*2.0-1.0, i.pos.y/screenSize.y*-2.0+1.0, 0.0, 1.0);
        o.uv = i.uv; o.col = i.col; return o;
    }
    float4 PSMain(PS_IN i) : SV_TARGET {
        float alpha = tex.Sample(sam, i.uv).r;
        return float4(i.col.rgb, i.col.a * alpha);
    }
)";

// ─── Full-frame post AA shaders ─────────────────────────────────────────────

static const char* kPostSrc = R"(
    Texture2D sceneTex : register(t0);
    SamplerState sam : register(s0);
    cbuffer CB0 : register(b0) { float2 screenSize; float aaMode; float aaQuality; };

    struct VS_OUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };

    VS_OUT VSMain(uint id : SV_VertexID) {
        VS_OUT o;
        float2 p = float2((id << 1) & 2, id & 2);
        o.pos = float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
        o.uv = p;
        return o;
    }

    float luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
    float lumaSample(float4 s) { return luma(s.rgb * s.a); }
    float3 normalizePremul(float3 rgb, float a) {
        return (a > 0.0001) ? (rgb / a) : float3(0.0, 0.0, 0.0);
    }

    float4 PSMain(VS_OUT i) : SV_TARGET {
        static const float FXAA_SPAN_MAX = 8.0;
        static const float FXAA_REDUCE_MUL = 1.0 / 8.0;
        static const float FXAA_REDUCE_MIN = 1.0 / 128.0;
        static const float FXAA_EDGE_THRESHOLD = 1.0 / 8.0;
        static const float FXAA_EDGE_THRESHOLD_MIN = 1.0 / 32.0;

        float2 px = 1.0 / screenSize;
        float4 c = sceneTex.Sample(sam, i.uv);
        if (aaMode < 1.5)
            return c;
        if (c.a < 0.06)
            return c;

        float4 sNW = sceneTex.Sample(sam, i.uv + float2(-px.x, -px.y));
        float4 sNE = sceneTex.Sample(sam, i.uv + float2( px.x, -px.y));
        float4 sSW = sceneTex.Sample(sam, i.uv + float2(-px.x,  px.y));
        float4 sSE = sceneTex.Sample(sam, i.uv + float2( px.x,  px.y));
        float3 baseRgb = normalizePremul(c.rgb, c.a);

        float lumaNW = lumaSample(sNW);
        float lumaNE = lumaSample(sNE);
        float lumaSW = lumaSample(sSW);
        float lumaSE = lumaSample(sSE);
        float lumaM  = lumaSample(c);

        float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
        float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
        float lumaRange = lumaMax - lumaMin;
        bool comboHigh = aaMode > 4.5;
        float edgeThreshScale = comboHigh ? 0.74 : ((aaQuality > 0.8) ? 0.85 : ((aaQuality > 0.2) ? 0.94 : 1.0));
        float edgeThresh = max(FXAA_EDGE_THRESHOLD_MIN, lumaMax * FXAA_EDGE_THRESHOLD * edgeThreshScale);
        if (lumaRange < edgeThresh)
            return c;

        float2 dir;
        dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
        dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

        float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * FXAA_REDUCE_MUL), FXAA_REDUCE_MIN);
        float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
        float span = FXAA_SPAN_MAX;
        if (comboHigh) span *= 1.35;
        else if (aaQuality > 0.8) span *= 1.2;
        else if (aaQuality > 0.2) span *= 1.0;
        dir = clamp(dir * rcpDirMin, float2(-span, -span), float2(span, span)) * px;

        float4 sA0 = sceneTex.Sample(sam, i.uv + dir * (1.0 / 3.0 - 0.5));
        float4 sA1 = sceneTex.Sample(sam, i.uv + dir * (2.0 / 3.0 - 0.5));
        float3 premulA = 0.5 * (sA0.rgb * sA0.a + sA1.rgb * sA1.a);
        float alphaA = 0.5 * (sA0.a + sA1.a);
        float3 rgbA = normalizePremul(premulA, alphaA);

        float4 sB0 = sceneTex.Sample(sam, i.uv + dir * -0.5);
        float4 sB1 = sceneTex.Sample(sam, i.uv + dir *  0.5);
        float3 premulB = premulA * 0.5 + 0.25 * (sB0.rgb * sB0.a + sB1.rgb * sB1.a);
        float alphaB = alphaA * 0.5 + 0.25 * (sB0.a + sB1.a);
        float3 rgbB = normalizePremul(premulB, alphaB);

        float lumaB = luma(premulB);
        float3 aa = ((lumaB < lumaMin) || (lumaB > lumaMax)) ? rgbA : rgbB;

        // Keep UI/text readable by attenuating AA on lower-alpha pixels.
        float alphaFloor = comboHigh ? 0.48 : 0.35;
        float alphaAtten = lerp(alphaFloor, 1.0, saturate((c.a - 0.2) / 0.8));
        float mixAmt = alphaAtten;
        if (comboHigh) mixAmt *= 1.08;
        else if (aaQuality > 0.8) mixAmt *= 1.0;
        else if (aaQuality > 0.2) mixAmt *= 0.62;
        else mixAmt *= 0.82;
        float3 outRgb = lerp(baseRgb, aa, mixAmt);

        // Lite hybrid mode: apply a subtle local sharpen so AA stays clean but crisp.
        if (aaQuality > 0.2 && aaQuality < 0.8) {
            float3 nCol = normalizePremul(sceneTex.Sample(sam, i.uv + float2(0.0, -px.y)).rgb,
                                          sceneTex.Sample(sam, i.uv + float2(0.0, -px.y)).a);
            float3 sCol = normalizePremul(sceneTex.Sample(sam, i.uv + float2(0.0,  px.y)).rgb,
                                          sceneTex.Sample(sam, i.uv + float2(0.0,  px.y)).a);
            float3 eCol = normalizePremul(sceneTex.Sample(sam, i.uv + float2( px.x, 0.0)).rgb,
                                          sceneTex.Sample(sam, i.uv + float2( px.x, 0.0)).a);
            float3 wCol = normalizePremul(sceneTex.Sample(sam, i.uv + float2(-px.x, 0.0)).rgb,
                                          sceneTex.Sample(sam, i.uv + float2(-px.x, 0.0)).a);
            float3 blur = (nCol + sCol + eCol + wCol) * 0.25;
            float3 detail = clamp(baseRgb - blur, -0.08, 0.08);
            outRgb = saturate(outRgb + detail * (0.20 * alphaAtten));
        }
        return float4(outRgb, c.a);
    }
)";

static void argbToF4(unsigned int argb, float out[4]) {
    out[0] = ((argb >> 16) & 0xFF) / 255.f;
    out[1] = ((argb >>  8) & 0xFF) / 255.f;
    out[2] = ((argb >>  0) & 0xFF) / 255.f;
    out[3] = ((argb >> 24) & 0xFF) / 255.f;
}

static ImU32 argbToImU32(unsigned int argb) {
    return (argb & 0xFF000000u)
        | ((argb & 0x00FF0000u) >> 16)
        | (argb & 0x0000FF00u)
        | ((argb & 0x000000FFu) << 16);
}

static int curveSegmentCount(float radius, float fraction = 1.0f) {
    if (radius < 0.5f)
        return 8;

    float scaled = radius * 3.6f;
    if (scaled < 28.f) scaled = 28.f;
    if (scaled > 112.f) scaled = 112.f;

    int segs = static_cast<int>(std::ceil(scaled * fraction));
    int minSegs = (fraction >= 0.999f) ? 24 : 8;
    if (segs < minSegs)
        segs = minSegs;
    return segs;
}

// ═════════════════════════════════════════════════════════════════════════════
// Init
// ═════════════════════════════════════════════════════════════════════════════

bool Renderer::init(HWND hwnd, int width, int height) {
    m_width = width; m_height = height;

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount        = 2;
    sd.BufferDesc.Format  = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferDesc.Width   = width;
    sd.BufferDesc.Height  = height;
    sd.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow       = hwnd;
    sd.SampleDesc.Count   = 1;
    sd.Windowed           = TRUE;
    sd.SwapEffect         = DXGI_SWAP_EFFECT_DISCARD;
    sd.Flags              = 0;

    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &sd,
        m_swapChain.GetAddressOf(), m_device.GetAddressOf(),
        &fl, m_context.GetAddressOf());
    if (FAILED(hr)) { std::cerr << "[Renderer] D3D11 init failed 0x" << std::hex << hr << "\n"; return false; }

    // Keep composition queue depth low to minimize end-to-end overlay latency.
    {
        Microsoft::WRL::ComPtr<IDXGIDevice1> dxgiDevice1;
        if (SUCCEEDED(m_device.As(&dxgiDevice1)) && dxgiDevice1)
            dxgiDevice1->SetMaximumFrameLatency(1);
    }

    // Pick the highest supported MSAA level (16x -> 8x -> 4x -> 2x).
    m_msaaCount = 1;
    m_msaaQuality = 0;
    for (UINT sampleCount : { 16u, 8u, 4u, 2u }) {
        UINT q = 0;
        if (SUCCEEDED(m_device->CheckMultisampleQualityLevels(DXGI_FORMAT_B8G8R8A8_UNORM, sampleCount, &q)) && q > 0) {
            m_msaaCount = sampleCount;
            m_msaaQuality = q;
            break;
        }
    }

    if (!createRenderTarget()) return false;
    if (!initShaders())        return false;
    if (!initTexturedShaders()) return false;
    if (!initRgbaImageShader()) return false;
    if (!initPostProcessShaders()) return false;
    if (!initBlendState())     return false;
    if (!initRasterizerState()) return false;
    if (!initWhiteTexture())   return false;

    // Shared sampler (bilinear, clamp)
    D3D11_SAMPLER_DESC samp{};
    samp.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    samp.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    m_device->CreateSamplerState(&samp, m_sampler.GetAddressOf());

    m_batchBuf.resize(kBatchMaxVerts * 6);

    // ── ImGui ─────────────────────────────────────────────────────────────
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    {
        ImGuiIO& io = ImGui::GetIO();
        ImFontConfig cfg{};
        cfg.OversampleH = 4;
        cfg.OversampleV = 2;
        cfg.RasterizerMultiply = 1.15f;

        wchar_t windowsDir[MAX_PATH] = {};
        if (GetWindowsDirectoryW(windowsDir, MAX_PATH) > 0) {
            std::wstring fontPath = windowsDir;
            fontPath += L"\\Fonts\\segoeui.ttf";
            int utf8Len = WideCharToMultiByte(CP_UTF8, 0, fontPath.c_str(), -1, nullptr, 0, nullptr, nullptr);
            if (utf8Len > 1) {
                std::string utf8Path(utf8Len, '\0');
                WideCharToMultiByte(CP_UTF8, 0, fontPath.c_str(), -1, utf8Path.data(), utf8Len, nullptr, nullptr);
                m_imguiTextFont = io.Fonts->AddFontFromFileTTF(utf8Path.c_str(), 36.0f, &cfg);
            }
        }
        if (!m_imguiTextFont)
            m_imguiTextFont = io.Fonts->AddFontDefault();
    }
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(m_device.Get(), m_context.Get());

    std::cout << "[Renderer] D3D11 initialised.\n";
    return true;
}

void Renderer::setImGuiDrawMode(bool enabled) {
    if (m_useImGuiDrawMode == enabled)
        return;

    if (!enabled && m_imguiClipPushed) {
        ImGui::GetBackgroundDrawList()->PopClipRect();
        m_imguiClipPushed = false;
    }

    m_useImGuiDrawMode = enabled;
    if (enabled) {
        ImDrawList* drawList = ImGui::GetBackgroundDrawList();
        drawList->Flags |= ImDrawListFlags_AntiAliasedLines;
        drawList->Flags |= ImDrawListFlags_AntiAliasedLinesUseTex;
        drawList->Flags |= ImDrawListFlags_AntiAliasedFill;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Pipeline initialisers
// ═════════════════════════════════════════════════════════════════════════════

bool Renderer::createRenderTarget() {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> bb;
    HRESULT hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&bb));
    if (FAILED(hr)) return false;
    if (FAILED(m_device->CreateRenderTargetView(bb.Get(), nullptr, m_rtv.GetAddressOf())))
        return false;

    // Single-sample scene color target used by the post-process AA pass.
    m_sceneColor.Reset();
    m_sceneRTV.Reset();
    m_sceneSRV.Reset();
    D3D11_TEXTURE2D_DESC sceneTd{};
    sceneTd.Width = (UINT)m_width;
    sceneTd.Height = (UINT)m_height;
    sceneTd.MipLevels = 1;
    sceneTd.ArraySize = 1;
    sceneTd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sceneTd.SampleDesc.Count = 1;
    sceneTd.Usage = D3D11_USAGE_DEFAULT;
    sceneTd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(m_device->CreateTexture2D(&sceneTd, nullptr, m_sceneColor.GetAddressOf())))
        return false;
    if (FAILED(m_device->CreateRenderTargetView(m_sceneColor.Get(), nullptr, m_sceneRTV.GetAddressOf())))
        return false;
    if (FAILED(m_device->CreateShaderResourceView(m_sceneColor.Get(), nullptr, m_sceneSRV.GetAddressOf())))
        return false;

    m_msaaColor.Reset();
    m_msaaRTV.Reset();
    if (m_msaaCount > 1) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = (UINT)m_width;
        td.Height = (UINT)m_height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = m_msaaCount;
        td.SampleDesc.Quality = (m_msaaQuality > 0) ? (m_msaaQuality - 1) : 0;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;

        hr = m_device->CreateTexture2D(&td, nullptr, m_msaaColor.GetAddressOf());
        if (FAILED(hr)) {
            m_msaaCount = 1;
            m_msaaQuality = 0;
            m_msaaColor.Reset();
            m_msaaRTV.Reset();
            return true;
        }
        hr = m_device->CreateRenderTargetView(m_msaaColor.Get(), nullptr, m_msaaRTV.GetAddressOf());
        if (FAILED(hr)) {
            m_msaaCount = 1;
            m_msaaQuality = 0;
            m_msaaColor.Reset();
            m_msaaRTV.Reset();
        }
    }
    return true;
}
void Renderer::releaseRenderTarget() {
    m_msaaRTV.Reset();
    m_msaaColor.Reset();
    m_sceneSRV.Reset();
    m_sceneRTV.Reset();
    m_sceneColor.Reset();
    m_rtv.Reset();
}

bool Renderer::initShaders() {
    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, psBlob, err;
    if (FAILED(D3DCompile(kFlatVS, strlen(kFlatVS), nullptr,
        nullptr, nullptr, "VSMain", "vs_4_0", 0, 0, &vsBlob, &err))) {
        if (err) std::cerr << (char*)err->GetBufferPointer(); return false;
    }
    if (FAILED(D3DCompile(kFlatVS, strlen(kFlatVS), nullptr,
        nullptr, nullptr, "PSMain", "ps_4_0", 0, 0, &psBlob, &err))) {
        if (err) std::cerr << (char*)err->GetBufferPointer(); return false;
    }
    m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_vs);
    m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_ps);

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT,  0,  8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    m_device->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &m_inputLayout);

    D3D11_BUFFER_DESC cbd{ 16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE };
    m_device->CreateBuffer(&cbd, nullptr, &m_cbScreen);
    D3D11_MAPPED_SUBRESOURCE mapped;
    m_context->Map(m_cbScreen.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    float cb[4] = { (float)m_width, (float)m_height, 0, 0 };
    memcpy(mapped.pData, cb, 16);
    m_context->Unmap(m_cbScreen.Get(), 0);

    D3D11_BUFFER_DESC vbd{ kBatchMaxVerts * 6 * sizeof(float), D3D11_USAGE_DYNAMIC,
                           D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE };
    m_device->CreateBuffer(&vbd, nullptr, &m_vertexBuffer);
    return true;
}

bool Renderer::initTexturedShaders() {
    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, psBlob, err;
    if (FAILED(D3DCompile(kTxSrc, strlen(kTxSrc), nullptr,
        nullptr, nullptr, "VSMain", "vs_4_0", 0, 0, &vsBlob, &err))) {
        if (err) std::cerr << (char*)err->GetBufferPointer(); return false;
    }
    if (FAILED(D3DCompile(kTxSrc, strlen(kTxSrc), nullptr,
        nullptr, nullptr, "PSMain", "ps_4_0", 0, 0, &psBlob, &err))) {
        if (err) std::cerr << (char*)err->GetBufferPointer(); return false;
    }
    m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_txVS);
    m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_txPS);

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0,  8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT,  0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    m_device->CreateInputLayout(layout, 3, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &m_txInputLayout);

    // Vertex buffer: 2048 vertices (512 quads) × 8 floats × 4 bytes
    D3D11_BUFFER_DESC vbd{ 2048 * 8 * sizeof(float), D3D11_USAGE_DYNAMIC,
                           D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE };
    m_device->CreateBuffer(&vbd, nullptr, &m_txVertexBuffer);
    return true;
}

bool Renderer::initRgbaImageShader() {
    Microsoft::WRL::ComPtr<ID3DBlob> psBlob, err;
    if (FAILED(D3DCompile(kTxRgbaSrc, strlen(kTxRgbaSrc), nullptr,
        nullptr, nullptr, "PSMain", "ps_4_0", 0, 0, &psBlob, &err))) {
        if (err) std::cerr << (char*)err->GetBufferPointer();
        return false;
    }
    return SUCCEEDED(m_device->CreatePixelShader(
        psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_txRgbaPS));
}

bool Renderer::initPostProcessShaders() {
    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, psBlob, err;
    if (FAILED(D3DCompile(kPostSrc, strlen(kPostSrc), nullptr,
        nullptr, nullptr, "VSMain", "vs_4_0", 0, 0, &vsBlob, &err))) {
        if (err) std::cerr << (char*)err->GetBufferPointer(); return false;
    }
    if (FAILED(D3DCompile(kPostSrc, strlen(kPostSrc), nullptr,
        nullptr, nullptr, "PSMain", "ps_4_0", 0, 0, &psBlob, &err))) {
        if (err) std::cerr << (char*)err->GetBufferPointer(); return false;
    }
    if (FAILED(m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_postVS)))
        return false;
    if (FAILED(m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_postPS)))
        return false;
    return true;
}

bool Renderer::initBlendState() {
    D3D11_BLEND_DESC bd{};
    bd.AlphaToCoverageEnable = FALSE;
    bd.RenderTarget[0] = {
        TRUE,
        D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD,
        D3D11_BLEND_ONE,       D3D11_BLEND_INV_SRC_ALPHA,  D3D11_BLEND_OP_ADD,
        D3D11_COLOR_WRITE_ENABLE_ALL
    };
    return SUCCEEDED(m_device->CreateBlendState(&bd, &m_blendState));
}

bool Renderer::initRasterizerState() {
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.FrontCounterClockwise = FALSE;
    rd.ScissorEnable = TRUE;
    rd.MultisampleEnable = TRUE;
    rd.AntialiasedLineEnable = TRUE;
    rd.DepthClipEnable = TRUE;
    HRESULT hr = m_device->CreateRasterizerState(&rd, m_rasterizerState.GetAddressOf());
    if (FAILED(hr)) return false;
    return true;
}

bool Renderer::initWhiteTexture() {
    unsigned char white = 255;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 1; td.Height = 1;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init{ &white, 1, 1 };
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    if (FAILED(m_device->CreateTexture2D(&td, &init, tex.GetAddressOf())))
        return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
    svd.Format = td.Format;
    svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    svd.Texture2D.MipLevels = 1;
    return SUCCEEDED(m_device->CreateShaderResourceView(tex.Get(), &svd, m_whiteSRV.GetAddressOf()));
}

// ═════════════════════════════════════════════════════════════════════════════
// Frame
// ═════════════════════════════════════════════════════════════════════════════

void Renderer::beginFrame() {
    m_useImGuiDrawMode = false;
    m_imguiClipPushed = false;
    const bool directEspPath = g_cfg.espEnabled || g_cfg.menuVisible;

    static int s_lastLatencyKey = -1;
    const int latencyKey = directEspPath ? 1 : 0;
    if (latencyKey != s_lastLatencyKey) {
        s_lastLatencyKey = latencyKey;
        Microsoft::WRL::ComPtr<IDXGIDevice1> dxgiDevice1;
        if (SUCCEEDED(m_device.As(&dxgiDevice1)) && dxgiDevice1)
            dxgiDevice1->SetMaximumFrameLatency(1u);
    }

    int aaMode = g_cfg.aaMode;
    if (aaMode < 0) aaMode = 0;
    if (aaMode > 5) aaMode = 5;
    if (directEspPath)
        aaMode = 0;
    float aaQuality = 0.0f;
    if (aaMode == 3 || aaMode == 5) aaQuality = 1.0f;
    else if (aaMode == 4) aaQuality = 0.35f;

    D3D11_MAPPED_SUBRESOURCE mapped;
    m_context->Map(m_cbScreen.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    float cb[4] = { (float)m_width, (float)m_height, (float)aaMode, aaQuality };
    memcpy(mapped.pData, cb, 16);
    m_context->Unmap(m_cbScreen.Get(), 0);

    float clear[4] = { 0, 0, 0, 0 };
    ID3D11RenderTargetView* activeRTV = m_rtv.Get();
    if (!directEspPath && aaMode == 1) {
        if (m_msaaRTV) activeRTV = m_msaaRTV.Get();
    } else if (!directEspPath && aaMode >= 2) {
        if (m_msaaRTV) activeRTV = m_msaaRTV.Get();
        else if (m_sceneRTV) activeRTV = m_sceneRTV.Get();
    }
    m_context->ClearRenderTargetView(activeRTV, clear);
    m_context->OMSetRenderTargets(1, &activeRTV, nullptr);
    float bf[4]{};
    m_context->OMSetBlendState(m_blendState.Get(), bf, 0xFFFFFFFF);
    D3D11_VIEWPORT vp{ 0, 0, (float)m_width, (float)m_height, 0, 1 };
    m_context->RSSetViewports(1, &vp);
    clearClipRect();
    m_context->RSSetState(m_rasterizerState.Get());
    // Constant buffer applies to both pipelines.
    m_context->VSSetConstantBuffers(0, 1, m_cbScreen.GetAddressOf());

    // Default to flat-colour pipeline and clear batch.
    m_batchCount = 0;
    useFlatPipeline();

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void Renderer::runPostProcessAA() {
    if (!m_rtv || !m_sceneSRV || !m_postVS || !m_postPS)
        return;

    ID3D11RenderTargetView* backbufferRTV = m_rtv.Get();
    m_context->OMSetRenderTargets(1, &backbufferRTV, nullptr);
    D3D11_VIEWPORT vp{ 0, 0, (float)m_width, (float)m_height, 0, 1 };
    m_context->RSSetViewports(1, &vp);
    D3D11_RECT rect{ 0, 0, m_width, m_height };
    m_context->RSSetScissorRects(1, &rect);

    m_context->IASetInputLayout(nullptr);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->VSSetShader(m_postVS.Get(), nullptr, 0);
    m_context->PSSetShader(m_postPS.Get(), nullptr, 0);
    m_context->VSSetConstantBuffers(0, 1, m_cbScreen.GetAddressOf());
    m_context->PSSetConstantBuffers(0, 1, m_cbScreen.GetAddressOf());
    m_context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
    m_context->PSSetShaderResources(0, 1, m_sceneSRV.GetAddressOf());

    // Replace target contents with post-processed scene.
    m_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    m_context->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrv = nullptr;
    m_context->PSSetShaderResources(0, 1, &nullSrv);
}

void Renderer::endFrame() {
    const bool directEspPath = g_cfg.espEnabled || g_cfg.menuVisible;

    int aaMode = g_cfg.aaMode;
    if (aaMode < 0) aaMode = 0;
    if (aaMode > 5) aaMode = 5;
    if (directEspPath)
        aaMode = 0;

    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    flushBatch();

    if (!directEspPath && aaMode == 1) {
        if (m_msaaColor && m_msaaRTV) {
            Microsoft::WRL::ComPtr<ID3D11Texture2D> bb;
            if (SUCCEEDED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&bb)))) {
                m_context->ResolveSubresource(bb.Get(), 0, m_msaaColor.Get(), 0, DXGI_FORMAT_B8G8R8A8_UNORM);
            }
        }
    } else if (!directEspPath && aaMode >= 2) {
        if (m_msaaColor && m_msaaRTV && m_sceneColor) {
            m_context->ResolveSubresource(m_sceneColor.Get(), 0, m_msaaColor.Get(), 0, DXGI_FORMAT_B8G8R8A8_UNORM);
        }
        runPostProcessAA();
    }

    m_swapChain->Present(0, 0);
}

void Renderer::resize(int width, int height) {
    if (!m_swapChain)
        return;
    if (width == m_width && height == m_height) return;
    m_width = width; m_height = height;
    releaseRenderTarget();
    m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    createRenderTarget();

    D3D11_MAPPED_SUBRESOURCE mapped;
    m_context->Map(m_cbScreen.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    float cb[4] = { (float)m_width, (float)m_height, 0, 0 };
    memcpy(mapped.pData, cb, 16);
    m_context->Unmap(m_cbScreen.Get(), 0);
}

// ═════════════════════════════════════════════════════════════════════════════
// Pipeline switching
// ═════════════════════════════════════════════════════════════════════════════

void Renderer::useFlatPipeline() {
    const UINT stride = 6 * sizeof(float), offset = 0;
    m_context->IASetInputLayout(m_inputLayout.Get());
    m_context->IASetVertexBuffers(0, 1, m_vertexBuffer.GetAddressOf(), &stride, &offset);
    m_context->VSSetShader(m_vs.Get(), nullptr, 0);
    m_context->PSSetShader(m_ps.Get(), nullptr, 0);
    m_context->OMSetBlendState(m_blendState.Get(), nullptr, 0xFFFFFFFF);
    m_context->RSSetState(m_rasterizerState.Get());
}

void Renderer::useTexturedPipeline() {
    flushBatch();
    const UINT stride = 8 * sizeof(float), offset = 0;
    m_context->IASetInputLayout(m_txInputLayout.Get());
    m_context->IASetVertexBuffers(0, 1, m_txVertexBuffer.GetAddressOf(), &stride, &offset);
    m_context->VSSetShader(m_txVS.Get(), nullptr, 0);
    m_context->PSSetShader(m_txPS.Get(), nullptr, 0);
    m_context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
    m_context->OMSetBlendState(m_blendState.Get(), nullptr, 0xFFFFFFFF);
    m_context->RSSetState(m_rasterizerState.Get());
}

void Renderer::setClipRect(float x, float y, float w, float h) {
    if (m_useImGuiDrawMode) {
        ImDrawList* drawList = ImGui::GetBackgroundDrawList();
        if (m_imguiClipPushed)
            drawList->PopClipRect();
        drawList->PushClipRect(ImVec2(x, y), ImVec2(x + w, y + h), true);
        m_imguiClipPushed = true;
        return;
    }

    LONG l = (LONG)floorf(x);
    LONG t = (LONG)floorf(y);
    LONG r = (LONG)ceilf(x + w);
    LONG b = (LONG)ceilf(y + h);

    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (r > m_width) r = m_width;
    if (b > m_height) b = m_height;

    m_clipRect = D3D11_RECT{ l, t, r, b };
    D3D11_RECT rect{ l, t, r, b };
    m_context->RSSetScissorRects(1, &rect);
}

void Renderer::clearClipRect() {
    if (m_useImGuiDrawMode) {
        if (m_imguiClipPushed) {
            ImGui::GetBackgroundDrawList()->PopClipRect();
            m_imguiClipPushed = false;
        }
        return;
    }

    m_clipRect = D3D11_RECT{ 0, 0, m_width, m_height };
    D3D11_RECT rect{ 0, 0, m_width, m_height };
    m_context->RSSetScissorRects(1, &rect);
}

void Renderer::getClipRect(float& x, float& y, float& w, float& h) const {
    x = (float)m_clipRect.left;
    y = (float)m_clipRect.top;
    w = (float)(m_clipRect.right - m_clipRect.left);
    h = (float)(m_clipRect.bottom - m_clipRect.top);
}

// ═════════════════════════════════════════════════════════════════════════════
// Flat-colour primitives
// ═════════════════════════════════════════════════════════════════════════════

void Renderer::flushBatch() {
    if (m_batchCount == 0) return;
    useFlatPipeline();
    const UINT stride = 6 * sizeof(float), offset = 0;
    D3D11_MAPPED_SUBRESOURCE m;
    m_context->Map(m_vertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    memcpy(m.pData, m_batchBuf.data(), m_batchCount * stride);
    m_context->Unmap(m_vertexBuffer.Get(), 0);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->Draw(m_batchCount, 0);
    m_batchCount = 0;
}

void Renderer::drawVertices(const float* v, int n, D3D11_PRIMITIVE_TOPOLOGY topo) {
    // Only TRIANGLELIST can be batched; everything else flushes first.
    if (topo != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {
        flushBatch();
        const UINT stride = 6 * sizeof(float), offset = 0;
        D3D11_MAPPED_SUBRESOURCE m;
        m_context->Map(m_vertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
        memcpy(m.pData, v, n * stride);
        m_context->Unmap(m_vertexBuffer.Get(), 0);
        m_context->IASetPrimitiveTopology(topo);
        m_context->Draw(n, 0);
        return;
    }
    if (m_batchCount + n > kBatchMaxVerts)
        flushBatch();
    memcpy(&m_batchBuf[m_batchCount * 6], v, n * 6 * sizeof(float));
    m_batchCount += n;
}

void Renderer::drawLine(float x1, float y1, float x2, float y2,
                         unsigned int color, float thickness) {
    if (m_useImGuiDrawMode) {
        ImGui::GetBackgroundDrawList()->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), argbToImU32(color), thickness);
        return;
    }

    float c[4]; argbToF4(color, c);
    float dx = x2 - x1, dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) return;

    auto drawLineQuad = [&](float halfW, const float col[4]) {
        float px = -dy / len * halfW;
        float py =  dx / len * halfW;
        float v[] = {
            x1 - px, y1 - py, col[0],col[1],col[2],col[3],
            x2 - px, y2 - py, col[0],col[1],col[2],col[3],
            x1 + px, y1 + py, col[0],col[1],col[2],col[3],
            x2 - px, y2 - py, col[0],col[1],col[2],col[3],
            x2 + px, y2 + py, col[0],col[1],col[2],col[3],
            x1 + px, y1 + py, col[0],col[1],col[2],col[3],
        };
        drawVertices(v, 6, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    };

    // Soft fringe + crisp core improves perceived edge smoothness on tiny icons.
    if (thickness <= 2.25f) {
        float outer[4] = { c[0], c[1], c[2], c[3] * 0.33f };
        drawLineQuad(thickness * 0.5f + 0.85f, outer);
        drawLineQuad((std::max)(0.45f, thickness * 0.5f - 0.15f), c);
    } else {
        drawLineQuad(thickness * 0.5f, c);
    }
}

void Renderer::drawRect(float x, float y, float w, float h,
                         unsigned int color, float t) {
    if (m_useImGuiDrawMode) {
        ImGui::GetBackgroundDrawList()->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), argbToImU32(color), 0.f, 0, t);
        return;
    }

    drawLine(x,   y,   x+w, y,   color, t);
    drawLine(x+w, y,   x+w, y+h, color, t);
    drawLine(x+w, y+h, x,   y+h, color, t);
    drawLine(x,   y+h, x,   y,   color, t);
}

void Renderer::drawFilledRect(float x, float y, float w, float h,
                               unsigned int color) {
    if (m_useImGuiDrawMode) {
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), argbToImU32(color));
        return;
    }

    float c[4]; argbToF4(color, c);
    float v[] = {
        x,   y,   c[0],c[1],c[2],c[3],
        x+w, y,   c[0],c[1],c[2],c[3],
        x,   y+h, c[0],c[1],c[2],c[3],
        x+w, y,   c[0],c[1],c[2],c[3],
        x+w, y+h, c[0],c[1],c[2],c[3],
        x,   y+h, c[0],c[1],c[2],c[3],
    };
    drawVertices(v, 6, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void Renderer::drawGradientRect(float x, float y, float w, float h,
                                 unsigned int topColor, unsigned int botColor) {
    if (m_useImGuiDrawMode) {
        ImDrawList* drawList = ImGui::GetBackgroundDrawList();
        ImU32 top = argbToImU32(topColor);
        ImU32 bot = argbToImU32(botColor);
        drawList->AddRectFilledMultiColor(ImVec2(x, y), ImVec2(x + w, y + h), top, top, bot, bot);
        return;
    }

    float tc[4], bc[4];
    argbToF4(topColor, tc);
    argbToF4(botColor, bc);
    float v[] = {
        x,   y,   tc[0],tc[1],tc[2],tc[3],
        x+w, y,   tc[0],tc[1],tc[2],tc[3],
        x,   y+h, bc[0],bc[1],bc[2],bc[3],
        x+w, y,   tc[0],tc[1],tc[2],tc[3],
        x+w, y+h, bc[0],bc[1],bc[2],bc[3],
        x,   y+h, bc[0],bc[1],bc[2],bc[3],
    };
    drawVertices(v, 6, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void Renderer::drawGradientRectH(float x, float y, float w, float h,
                                  unsigned int leftColor, unsigned int rightColor) {
    if (m_useImGuiDrawMode) {
        ImDrawList* drawList = ImGui::GetBackgroundDrawList();
        ImU32 left = argbToImU32(leftColor);
        ImU32 right = argbToImU32(rightColor);
        drawList->AddRectFilledMultiColor(ImVec2(x, y), ImVec2(x + w, y + h), left, right, right, left);
        return;
    }

    float lc[4], rc[4];
    argbToF4(leftColor, lc);
    argbToF4(rightColor, rc);
    float v[] = {
        x,   y,   lc[0],lc[1],lc[2],lc[3],
        x+w, y,   rc[0],rc[1],rc[2],rc[3],
        x,   y+h, lc[0],lc[1],lc[2],lc[3],
        x+w, y,   rc[0],rc[1],rc[2],rc[3],
        x+w, y+h, rc[0],rc[1],rc[2],rc[3],
        x,   y+h, lc[0],lc[1],lc[2],lc[3],
    };
    drawVertices(v, 6, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

// ═════════════════════════════════════════════════════════════════════════════
// Rounded rectangle helpers
// ═════════════════════════════════════════════════════════════════════════════

static void addCornerTri(float*& v, float cx, float cy, float r,
                          float startAngle, int seg, const float c[4]) {
    for (int i = 0; i < seg; ++i) {
        float a0 = startAngle + (3.14159265f * 0.5f) * i / seg;
        float a1 = startAngle + (3.14159265f * 0.5f) * (i + 1) / seg;
        *v++ = cx; *v++ = cy; *v++ = c[0]; *v++ = c[1]; *v++ = c[2]; *v++ = c[3];
        *v++ = cx + cosf(a0) * r; *v++ = cy + sinf(a0) * r; *v++ = c[0]; *v++ = c[1]; *v++ = c[2]; *v++ = c[3];
        *v++ = cx + cosf(a1) * r; *v++ = cy + sinf(a1) * r; *v++ = c[0]; *v++ = c[1]; *v++ = c[2]; *v++ = c[3];
    }
}

void Renderer::drawRoundedFilledRect(float x, float y, float w, float h,
                                      unsigned int color, float r) {
    if (m_useImGuiDrawMode) {
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), argbToImU32(color), r);
        return;
    }

    float c[4]; argbToF4(color, c);
    if (r < 0.5f) { drawFilledRect(x, y, w, h, color); return; }
    if (r > w * 0.5f) r = w * 0.5f;
    if (r > h * 0.5f) r = h * 0.5f;

    const int seg = curveSegmentCount(r, 0.25f);
    // 5 rects × 6 verts + 4 corners × seg × 3 verts
    int totalVerts = 30 + 4 * seg * 3;
    std::vector<float> buf(totalVerts * 6);
    float* vp = buf.data();
    float x0 = x + r, y0 = y + r, x1 = x + w - r, y1 = y + h - r;

    auto tri = [&](float ax, float ay, float bx, float by, float cx, float cy) {
        *vp++ = ax; *vp++ = ay; *vp++ = c[0]; *vp++ = c[1]; *vp++ = c[2]; *vp++ = c[3];
        *vp++ = bx; *vp++ = by; *vp++ = c[0]; *vp++ = c[1]; *vp++ = c[2]; *vp++ = c[3];
        *vp++ = cx; *vp++ = cy; *vp++ = c[0]; *vp++ = c[1]; *vp++ = c[2]; *vp++ = c[3];
    };

    // Centre
    tri(x0, y0, x1, y0, x0, y1);
    tri(x1, y0, x1, y1, x0, y1);
    // Top
    tri(x0, y,  x1, y,  x0, y0);
    tri(x1, y,  x1, y0, x0, y0);
    // Bottom
    tri(x0, y1, x1, y1, x0, y+h);
    tri(x1, y1, x1, y+h, x0, y+h);
    // Left
    tri(x,  y0, x0, y0, x,  y1);
    tri(x0, y0, x0, y1, x,  y1);
    // Right
    tri(x1, y0, x+w, y0, x1, y1);
    tri(x1, y1, x+w, y1, x+w, y0);
    // Corners
    addCornerTri(vp, x0, y0, r, 3.14159265f, seg, c);
    addCornerTri(vp, x1, y0, r, 4.71238898f, seg, c);
    addCornerTri(vp, x1, y1, r, 0.0f, seg, c);
    addCornerTri(vp, x0, y1, r, 1.57079633f, seg, c);

    int n = (int)((vp - buf.data()) / 6);
    drawVertices(buf.data(), n, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void Renderer::drawRoundedFilledRectCorners(float x, float y, float w, float h,
                                             unsigned int color, float r, int corners) {
    if (w <= 0.f || h <= 0.f)
        return;

    if (m_useImGuiDrawMode) {
        ImGui::GetBackgroundDrawList()->AddRectFilled(
            ImVec2(x, y), ImVec2(x + w, y + h), argbToImU32(color), r, corners);
        return;
    }

    drawRoundedFilledRect(x, y, w, h, color, r);
}

void Renderer::drawRoundedRect(float x, float y, float w, float h,
                                unsigned int color, float r, float thickness) {
    if (m_useImGuiDrawMode) {
        ImGui::GetBackgroundDrawList()->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), argbToImU32(color), r, 0, thickness);
        return;
    }

    // Outline using 4 straight edges + 4 corner arcs via line segments
    float x0 = x + r, x1 = x + w - r;
    float y0 = y + r, y1 = y + h - r;
    int segs = curveSegmentCount(r, 0.25f);
    float c[4]; argbToF4(color, c);
    // (crx, cry) = center of each corner, start angle:
    struct { float cx, cy, start; } corners[4] = {
        {x1, y0, 4.7123890f},   // top-right:   from up  (3π/2) → right (2π)
        {x1, y1, 0.f},           // bottom-right: from right (0) → down (π/2)
        {x0, y1, 1.5707963f},   // bottom-left:  from down (π/2) → left (π)
        {x0, y0, 3.1415927f},   // top-left:    from left (π) → up (3π/2)
    };
    // Straight edges: top, right, bottom, left
    drawLine(x0, y, x1, y, color, thickness);
    drawLine(x + w, y0, x + w, y1, color, thickness);
    drawLine(x1, y + h, x0, y + h, color, thickness);
    drawLine(x, y1, x, y0, color, thickness);
    // Corner arcs
    for (auto& cr : corners) {
        for (int i = 0; i < segs; ++i) {
            float a0 = cr.start + 1.5707963f * i / segs;
            float a1 = cr.start + 1.5707963f * (i + 1) / segs;
            drawLine(cr.cx + cosf(a0) * r, cr.cy + sinf(a0) * r,
                     cr.cx + cosf(a1) * r, cr.cy + sinf(a1) * r, color, thickness);
        }
    }
}

void Renderer::drawCircle(float cx, float cy, float r,
                           unsigned int color, float thickness) {
    if (m_useImGuiDrawMode) {
        ImGui::GetBackgroundDrawList()->AddCircle(ImVec2(cx, cy), r, argbToImU32(color), 0, thickness);
        return;
    }

    if (r < 0.5f) return;
    float c[4]; argbToF4(color, c);
    int segs = curveSegmentCount(r);
    float* buf = (float*)alloca(segs * 6 * 6 * sizeof(float));
    float* vp = buf;
    for (int i = 0; i < segs; ++i) {
        float a0 = 6.2831853f * i / segs;
        float a1 = 6.2831853f * (i + 1) / segs;
        float x1 = cx + cosf(a0) * r, y1 = cy + sinf(a0) * r;
        float x2 = cx + cosf(a1) * r, y2 = cy + sinf(a1) * r;
        float dx = x2 - x1, dy = y2 - y1;
        float len = sqrtf(dx * dx + dy * dy);
        if (len < 0.001f) continue;
        float px = -dy / len * (thickness * 0.5f);
        float py =  dx / len * (thickness * 0.5f);
        *vp++ = x1 - px; *vp++ = y1 - py; *vp++ = c[0]; *vp++ = c[1]; *vp++ = c[2]; *vp++ = c[3];
        *vp++ = x2 - px; *vp++ = y2 - py; *vp++ = c[0]; *vp++ = c[1]; *vp++ = c[2]; *vp++ = c[3];
        *vp++ = x1 + px; *vp++ = y1 + py; *vp++ = c[0]; *vp++ = c[1]; *vp++ = c[2]; *vp++ = c[3];
        *vp++ = x2 - px; *vp++ = y2 - py; *vp++ = c[0]; *vp++ = c[1]; *vp++ = c[2]; *vp++ = c[3];
        *vp++ = x2 + px; *vp++ = y2 + py; *vp++ = c[0]; *vp++ = c[1]; *vp++ = c[2]; *vp++ = c[3];
        *vp++ = x1 + px; *vp++ = y1 + py; *vp++ = c[0]; *vp++ = c[1]; *vp++ = c[2]; *vp++ = c[3];
    }
    int n = (int)((vp - buf) / 6);
    drawVertices(buf, n, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void Renderer::drawFilledCircle(float cx, float cy, float r,
                                 unsigned int color) {
    if (m_useImGuiDrawMode) {
        ImGui::GetBackgroundDrawList()->AddCircleFilled(ImVec2(cx, cy), r, argbToImU32(color));
        return;
    }

    if (r < 0.5f) return;
    float c[4]; argbToF4(color, c);
    int segs = curveSegmentCount(r);
    float* buf = (float*)alloca(segs * 3 * 6 * sizeof(float));
    float* vp = buf;
    for (int i = 0; i < segs; ++i) {
        float a0 = 6.2831853f * i / segs;
        float a1 = 6.2831853f * (i + 1) / segs;
        *vp++ = cx;             *vp++ = cy;             *vp++ = c[0]; *vp++ = c[1]; *vp++ = c[2]; *vp++ = c[3];
        *vp++ = cx + cosf(a0)*r; *vp++ = cy + sinf(a0)*r; *vp++ = c[0]; *vp++ = c[1]; *vp++ = c[2]; *vp++ = c[3];
        *vp++ = cx + cosf(a1)*r; *vp++ = cy + sinf(a1)*r; *vp++ = c[0]; *vp++ = c[1]; *vp++ = c[2]; *vp++ = c[3];
    }
    int n = (int)((vp - buf) / 6);
    drawVertices(buf, n, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void Renderer::drawFilledTriangle(float x1, float y1,
                                   float x2, float y2,
                                   float x3, float y3,
                                   unsigned int color) {
    if (m_useImGuiDrawMode) {
        ImGui::GetBackgroundDrawList()->AddTriangleFilled(ImVec2(x1, y1), ImVec2(x2, y2), ImVec2(x3, y3), argbToImU32(color));
        return;
    }

    float c[4]; argbToF4(color, c);
    float v[] = {
        x1, y1, c[0],c[1],c[2],c[3],
        x2, y2, c[0],c[1],c[2],c[3],
        x3, y3, c[0],c[1],c[2],c[3],
    };
    drawVertices(v, 3, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void Renderer::drawFilledConvexPolygon(const float* pts, int count,
                                        unsigned int color) {
    if (m_useImGuiDrawMode) {
        if (count < 3)
            return;
        std::vector<ImVec2> points(count);
        for (int i = 0; i < count; ++i)
            points[i] = ImVec2(pts[i * 2], pts[i * 2 + 1]);
        ImGui::GetBackgroundDrawList()->AddConvexPolyFilled(points.data(), count, argbToImU32(color));
        return;
    }

    float c[4]; argbToF4(color, c);
    if (count < 3) return;

    float cx = 0.f, cy = 0.f;
    for (int i = 0; i < count; ++i) { cx += pts[i*2]; cy += pts[i*2+1]; }
    cx /= (float)count; cy /= (float)count;

    float* buf = (float*)alloca(count * 3 * 6 * sizeof(float));
    float* vp = buf;
    for (int i = 0; i < count; ++i) {
        int ni = (i + 1 < count) ? i + 1 : 0;
        *vp++ = cx;        *vp++ = cy;        *vp++ = c[0];*vp++ = c[1];*vp++ = c[2];*vp++ = c[3];
        *vp++ = pts[i*2];  *vp++ = pts[i*2+1]; *vp++ = c[0];*vp++ = c[1];*vp++ = c[2];*vp++ = c[3];
        *vp++ = pts[ni*2]; *vp++ = pts[ni*2+1]; *vp++ = c[0];*vp++ = c[1];*vp++ = c[2];*vp++ = c[3];
    }
    int n = (int)((vp - buf) / 6);
    drawVertices(buf, n, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void Renderer::drawFilledPie(float cx, float cy, float r,
                              float startAngle, float endAngle,
                              unsigned int color) {
    if (m_useImGuiDrawMode) {
        if (r < 0.5f)
            return;
        float range = endAngle - startAngle;
        if (range < 0.001f)
            return;
        int segs = curveSegmentCount(r);
        int steps = (int)std::ceil(segs * range / 6.2831853f);
        if (steps < 3) steps = 3;
        ImDrawList* drawList = ImGui::GetBackgroundDrawList();
        drawList->PathClear();
        drawList->PathLineTo(ImVec2(cx, cy));
        drawList->PathArcTo(ImVec2(cx, cy), r, startAngle, endAngle, steps);
        drawList->PathLineTo(ImVec2(cx, cy));
        drawList->PathFillConvex(argbToImU32(color));
        return;
    }

    if (r < 0.5f) return;
    float c[4]; argbToF4(color, c);
    int segs = curveSegmentCount(r);
    float range = endAngle - startAngle;
    if (range < 0.001f) return;
    int steps = (int)std::ceil(segs * range / 6.2831853f);
    if (steps < 3) steps = 3;

    float* buf = (float*)alloca(steps * 3 * 6 * sizeof(float));
    float* vp = buf;
    float step = range / steps;
    for (int i = 0; i < steps; ++i) {
        float a0 = startAngle + step * i;
        float a1 = startAngle + step * (i + 1);
        *vp++ = cx;               *vp++ = cy;               *vp++ = c[0];*vp++ = c[1];*vp++ = c[2];*vp++ = c[3];
        *vp++ = cx + cosf(a0) * r; *vp++ = cy + sinf(a0) * r; *vp++ = c[0];*vp++ = c[1];*vp++ = c[2];*vp++ = c[3];
        *vp++ = cx + cosf(a1) * r; *vp++ = cy + sinf(a1) * r; *vp++ = c[0];*vp++ = c[1];*vp++ = c[2];*vp++ = c[3];
    }
    int n = (int)((vp - buf) / 6);
    drawVertices(buf, n, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

float Renderer::centerY(float boxH, float fontSize) const {
    // Actual line height at display size = font.m_fontHeight * fontSize / renderPx
    return (boxH - fontSize) * 0.5f;
}

// ═════════════════════════════════════════════════════════════════════════════
// Textured rendering
// ═════════════════════════════════════════════════════════════════════════════

void Renderer::drawTexturedQuads(const float* verts, int quadCount,
                                  ID3D11ShaderResourceView* tex,
                                  ID3D11PixelShader* psOverride) {
    int vertexCount = quadCount * 6;
    if (vertexCount == 0) return;

    useTexturedPipeline();
    m_context->PSSetShader(psOverride ? psOverride : m_txPS.Get(), nullptr, 0);
    m_context->PSSetShaderResources(0, 1, &tex);

    const UINT stride = 8 * sizeof(float), offset = 0;
    D3D11_MAPPED_SUBRESOURCE m;
    m_context->Map(m_txVertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    memcpy(m.pData, verts, vertexCount * stride);
    m_context->Unmap(m_txVertexBuffer.Get(), 0);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->Draw(vertexCount, 0);
}

void Renderer::drawImage(ID3D11ShaderResourceView* texture, float x, float y, float w, float h,
                         unsigned int color) {
    if (!texture || w <= 0.f || h <= 0.f)
        return;

    if (m_useImGuiDrawMode) {
        ImGui::GetBackgroundDrawList()->AddImage(
            reinterpret_cast<ImTextureID>(texture),
            ImVec2(x, y),
            ImVec2(x + w, y + h),
            ImVec2(0.f, 0.f),
            ImVec2(1.f, 1.f),
            argbToImU32(color)
        );
        return;
    }

    float c[4];
    argbToF4(color, c);
    float verts[] = {
        x,     y,     0.f, 0.f, c[0], c[1], c[2], c[3],
        x + w, y,     1.f, 0.f, c[0], c[1], c[2], c[3],
        x,     y + h, 0.f, 1.f, c[0], c[1], c[2], c[3],
        x + w, y,     1.f, 0.f, c[0], c[1], c[2], c[3],
        x + w, y + h, 1.f, 1.f, c[0], c[1], c[2], c[3],
        x,     y + h, 0.f, 1.f, c[0], c[1], c[2], c[3],
    };
    drawTexturedQuads(verts, 1, texture);
}

void Renderer::drawImageRgba(ID3D11ShaderResourceView* texture, float x, float y, float w, float h,
                             unsigned int color) {
    if (!texture || w <= 0.f || h <= 0.f || !m_txRgbaPS)
        return;

    if (m_useImGuiDrawMode) {
        ImGui::GetBackgroundDrawList()->AddImage(
            reinterpret_cast<ImTextureID>(texture),
            ImVec2(x, y),
            ImVec2(x + w, y + h),
            ImVec2(0.f, 0.f),
            ImVec2(1.f, 1.f),
            argbToImU32(color));
        return;
    }

    float c[4];
    argbToF4(color, c);
    float verts[] = {
        x,     y,     0.f, 0.f, c[0], c[1], c[2], c[3],
        x + w, y,     1.f, 0.f, c[0], c[1], c[2], c[3],
        x,     y + h, 0.f, 1.f, c[0], c[1], c[2], c[3],
        x + w, y,     1.f, 0.f, c[0], c[1], c[2], c[3],
        x + w, y + h, 1.f, 1.f, c[0], c[1], c[2], c[3],
        x,     y + h, 0.f, 1.f, c[0], c[1], c[2], c[3],
    };
    drawTexturedQuads(verts, 1, texture, m_txRgbaPS.Get());
}

void Renderer::drawText(const FontAtlas& font, float x, float y,
                         const char* text, unsigned int color, float size) {
    // Convert to wide and delegate.
    int len = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (len <= 1) return;
    std::wstring wtext(len - 1, L' ');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, &wtext[0], len);
    drawTextW(font, x, y, wtext.c_str(), color, size);
}

void Renderer::measureTextBoundsW(const FontAtlas& font, const wchar_t* text, float size,
                                  float& minX, float& minY, float& maxX, float& maxY) const {
    minX = 0.f;
    minY = 0.f;
    maxX = 0.f;
    maxY = 0.f;

    if (!text || !*text)
        return;

    if (m_useImGuiDrawMode && m_imguiTextFont) {
        bool canUseImGuiFont = true;
        for (const wchar_t* p = text; *p; ++p) {
            if (*p == L' ')
                continue;
            if (!m_imguiTextFont->FindGlyphNoFallback(static_cast<ImWchar>(*p))) {
                canUseImGuiFont = false;
                break;
            }
        }

        if (canUseImGuiFont) {
            int utf8Len = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
            if (utf8Len > 1) {
                std::string utf8Text(utf8Len, '\0');
                WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8Text.data(), utf8Len, nullptr, nullptr);
                ImVec2 bounds = m_imguiTextFont->CalcTextSizeA(size, FLT_MAX, 0.0f, utf8Text.c_str());
                maxX = bounds.x;
                maxY = bounds.y;
                return;
            }
        }
    }

    float scale = size / font.renderPx();
    float cursorX = 0.f;
    float spaceAdv = 8.f;
    if (const GlyphInfo* sg = font.glyph(L' '))
        spaceAdv = static_cast<float>(sg->advanceX);

    float boundsMinX = (std::numeric_limits<float>::max)();
    float boundsMinY = (std::numeric_limits<float>::max)();
    float boundsMaxX = (std::numeric_limits<float>::lowest)();
    float boundsMaxY = (std::numeric_limits<float>::lowest)();

    for (const wchar_t* p = text; *p; ++p) {
        if (*p == L' ') {
            cursorX += spaceAdv * scale;
            continue;
        }

        const GlyphInfo* gi = font.glyph(*p);
        if (!gi) {
            cursorX += 8.f * scale;
            continue;
        }

        if (gi->width > 0 && gi->height > 0) {
            float glyphLeft = cursorX + gi->bearingX * scale;
            float glyphTop = gi->bearingY * scale;
            float glyphRight = glyphLeft + gi->width * scale;
            float glyphBottom = glyphTop + gi->height * scale;
            boundsMinX = (std::min)(boundsMinX, glyphLeft);
            boundsMinY = (std::min)(boundsMinY, glyphTop);
            boundsMaxX = (std::max)(boundsMaxX, glyphRight);
            boundsMaxY = (std::max)(boundsMaxY, glyphBottom);
        }

        cursorX += gi->advanceX * scale;
    }

    if (boundsMinX == (std::numeric_limits<float>::max)()
        || boundsMinY == (std::numeric_limits<float>::max)()
        || boundsMaxX == (std::numeric_limits<float>::lowest)()
        || boundsMaxY == (std::numeric_limits<float>::lowest)()) {
        maxX = cursorX;
        maxY = font.lineHeight(size);
        return;
    }

    minX = boundsMinX;
    minY = boundsMinY;
    maxX = boundsMaxX;
    maxY = boundsMaxY;
}

void Renderer::drawTextW(const FontAtlas& font, float x, float y,
                          const wchar_t* text, unsigned int color, float size) {
    if (m_useImGuiDrawMode) {
        if (m_imguiTextFont && text && *text) {
            bool canUseImGuiFont = true;
            for (const wchar_t* p = text; *p; ++p) {
                if (*p == L' ')
                    continue;
                if (!m_imguiTextFont->FindGlyphNoFallback(static_cast<ImWchar>(*p))) {
                    canUseImGuiFont = false;
                    break;
                }
            }

            if (canUseImGuiFont) {
                int utf8Len = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
                if (utf8Len > 1) {
                    std::string utf8Text(utf8Len, '\0');
                    WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8Text.data(), utf8Len, nullptr, nullptr);
                    ImGui::GetBackgroundDrawList()->AddText(m_imguiTextFont, size, ImVec2(x, y), argbToImU32(color), utf8Text.c_str());
                    return;
                }
            }
        }

        float scale = size / font.renderPx();
        ImU32 col = argbToImU32(color);
        float cursorX = x;
        float spaceAdv = 8.f;
        if (const GlyphInfo* sg = font.glyph(L' '))
            spaceAdv = (float)sg->advanceX;

        ImDrawList* drawList = ImGui::GetBackgroundDrawList();
        ImTextureID tex = (ImTextureID)font.texture();
        int len = (int)wcslen(text);
        for (int i = 0; i < len; ++i) {
            wchar_t ch = text[i];
            if (ch == L' ') {
                cursorX += spaceAdv * scale;
                continue;
            }

            const GlyphInfo* gi = font.glyph(ch);
            if (!gi) {
                cursorX += 8.f * scale;
                continue;
            }
            if (gi->width <= 0 || gi->height <= 0) {
                cursorX += gi->advanceX * scale;
                continue;
            }

            float gx = cursorX + gi->bearingX * scale;
            float gy = y + gi->bearingY * scale;
            float gw = gi->width * scale;
            float gh = gi->height * scale;
            drawList->AddImage(tex,
                               ImVec2(gx, gy),
                               ImVec2(gx + gw, gy + gh),
                               ImVec2(gi->u0, gi->v0),
                               ImVec2(gi->u1, gi->v1),
                               col);
            cursorX += gi->advanceX * scale;
        }
        return;
    }

    float scale = size / font.renderPx();
    float c[4]; argbToF4(color, c);

    // Pre-allocate buffer (worst case: each char = 1 quad = 2 triangles = 6 vertices)
    int len = (int)wcslen(text);
    std::vector<float> verts(len * 6 * 8); // 8 floats per vertex
    float* v = verts.data();
    int quadCount = 0;

    float cursorX = x;
    float spaceAdv = 8.f;
    if (const GlyphInfo* sg = font.glyph(L' '))
        spaceAdv = (float)sg->advanceX;

    for (int i = 0; i < len; ++i) {
        wchar_t ch = text[i];
        if (ch == L' ') {
            cursorX += spaceAdv * scale;
            continue;
        }

        const GlyphInfo* gi = font.glyph(ch);
        if (!gi) { cursorX += 8.f * scale; continue; }
        if (gi->width <= 0 || gi->height <= 0) {
            cursorX += gi->advanceX * scale;
            continue;
        }

        float gx = cursorX + gi->bearingX * scale;
        float gy = y + gi->bearingY * scale;
        float gw = gi->width * scale;
        float gh = gi->height * scale;

        // Quad as two triangles (triangle list): tl-tr-bl, tr-bl-br
        float x0 = gx, x1 = gx+gw, y0 = gy, y1 = gy+gh;
        float u0 = gi->u0, u1 = gi->u1, v0 = gi->v0, v1 = gi->v1;
        // Triangle 1: tl, tr, bl
        *v++ = x0; *v++ = y0; *v++ = u0; *v++ = v0; *v++ = c[0]; *v++ = c[1]; *v++ = c[2]; *v++ = c[3];
        *v++ = x1; *v++ = y0; *v++ = u1; *v++ = v0; *v++ = c[0]; *v++ = c[1]; *v++ = c[2]; *v++ = c[3];
        *v++ = x0; *v++ = y1; *v++ = u0; *v++ = v1; *v++ = c[0]; *v++ = c[1]; *v++ = c[2]; *v++ = c[3];
        // Triangle 2: tr, bl, br
        *v++ = x1; *v++ = y0; *v++ = u1; *v++ = v0; *v++ = c[0]; *v++ = c[1]; *v++ = c[2]; *v++ = c[3];
        *v++ = x0; *v++ = y1; *v++ = u0; *v++ = v1; *v++ = c[0]; *v++ = c[1]; *v++ = c[2]; *v++ = c[3];
        *v++ = x1; *v++ = y1; *v++ = u1; *v++ = v1; *v++ = c[0]; *v++ = c[1]; *v++ = c[2]; *v++ = c[3];
        ++quadCount;
        cursorX += gi->advanceX * scale;
    }

    drawTexturedQuads(verts.data(), quadCount, font.texture());
}
