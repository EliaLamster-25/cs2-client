#include "menu.h"
#include "esp/chams_mesh.h"
#include "overlay/renderer.h"
#include "game/entity_manager.h"
#include "game/game_sensitivity.h"
#include "memory/process.h"
#include "analytics/match_intel.h"
#include "analytics/player_scout.h"
#include "game/aim_style.h"
#include "game/weapon_group.h"
#include "gui/icons.h"
#include "config.h"
#include "profile/user_profile.h"
#include "cloud/cloud_api.h"
#include "game/grenade_lineup.h"
#include "json.hpp"
#include <algorithm>
#include <shellapi.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <ctime>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <wincodec.h>
#include <winhttp.h>
#include <imgui.h>

#pragma comment(lib, "winhttp.lib")

using json = nlohmann::json;

static const char* kTabs[] = {
    "Aim", "ESP", "World", "Nades", "System", "Intel", "Player Info", "Config"
};
static constexpr int kTabN = 8;
static constexpr float kTabH = 46.f;
static constexpr wchar_t kLeetifyHomeUrl[] = L"https://leetify.com/";
static constexpr wchar_t kLeetifyDeveloperUrl[] = L"https://leetify.com/app/developer";

static const char* kTabSubtitles[] = {
    "Assist, weapons & hitboxes",
    "Player overlay & live preview",
    "Grenades, radar & bomb timer",
    "Lineup helper & sound ESP",
    "Performance & overlay control",
    "Match intelligence & round analysis",
    "Leetify player stats & risk estimates",
    "Cloud sync, save & load profiles"
};

static void (*kTabIcons[])(Renderer&, float, float, float, unsigned int) = {
    Icons::aimbot, Icons::visuals, Icons::players, Icons::crosshair,
    Icons::misc, Icons::radar, Icons::players, Icons::configs
};

static float deltaS(int64_t& last, int64_t freq) {
    if (!freq) return 0.016f;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = (float)((now.QuadPart - last) / (double)freq);
    last = now.QuadPart;
    return (dt > 0.f && dt < 0.1f) ? dt : 0.016f;
}

struct MenuColumnLayout {
    float leftX = 0.f;
    float rightX = 0.f;
    float colW = 0.f;
    float leftY = 0.f;
    float rightY = 0.f;

    MenuColumnLayout(float contentX, float contentW, float startY, float gap) {
        colW = (contentW - gap) * 0.5f;
        leftX = contentX;
        rightX = contentX + colW + gap;
        leftY = rightY = startY;
    }

    void beginLeft(Gui& gui) const {
        gui.setCursor(leftX, leftY);
        gui.setItemWidth(colW);
    }

    void beginRight(Gui& gui) const {
        gui.setCursor(rightX, rightY);
        gui.setItemWidth(colW);
    }

    void end(Gui& gui) {
        const float bottom = (std::max)(leftY, rightY);
        gui.setCursor(leftX, bottom + 8.f);
        gui.setItemWidth(colW * 2.f + (rightX - leftX - colW));
        leftY = rightY = bottom;
    }

    void syncLeft(float y) { leftY = y; }
    void syncRight(float y) { rightY = y; }

    void syncRow() {
        const float bottom = (std::max)(leftY, rightY);
        leftY = rightY = bottom;
    }
};

static float textCenterY(const FontAtlas& font, float top, float boxH, float fontSize) {
    return top + (boxH - font.lineHeight(fontSize)) * 0.5f;
}

static bool utf8ToWide(const char* text, std::wstring& out) {
    out.clear();
    if (!text || !*text)
        return false;

    int len = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (len <= 1)
        return false;

    out.resize(len - 1);
    MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), len);
    return true;
}

static bool copyUtf8ToClipboard(const std::string& text) {
    if (text.empty())
        return false;

    std::wstring wide;
    if (!utf8ToWide(text.c_str(), wide))
        return false;

    if (!OpenClipboard(nullptr))
        return false;

    EmptyClipboard();
    const std::size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!hMem) {
        CloseClipboard();
        return false;
    }

    void* dst = GlobalLock(hMem);
    if (!dst) {
        GlobalFree(hMem);
        CloseClipboard();
        return false;
    }

    std::memcpy(dst, wide.c_str(), bytes);
    GlobalUnlock(hMem);

    if (!SetClipboardData(CF_UNICODETEXT, hMem)) {
        GlobalFree(hMem);
        CloseClipboard();
        return false;
    }

    CloseClipboard();
    return true;
}

static float textVisualCenterY(const FontAtlas& font, float top, float boxH, const wchar_t* text, float fontSize) {
    if (!text || !*text)
        return textCenterY(font, top, boxH, fontSize);

    float scale = fontSize / font.renderPx();
    float minY = (std::numeric_limits<float>::max)();
    float maxY = (std::numeric_limits<float>::lowest)();

    for (const wchar_t* p = text; *p; ++p) {
        if (*p == L' ')
            continue;

        const GlyphInfo* gi = font.glyph(*p);
        if (!gi || gi->width <= 0 || gi->height <= 0)
            continue;

        float glyphTop = gi->bearingY * scale;
        float glyphBottom = glyphTop + gi->height * scale;
        minY = (std::min)(minY, glyphTop);
        maxY = (std::max)(maxY, glyphBottom);
    }

    if (minY == (std::numeric_limits<float>::max)() || maxY == (std::numeric_limits<float>::lowest)())
        return textCenterY(font, top, boxH, fontSize);

    float boundsH = maxY - minY;
    return top + (boxH - boundsH) * 0.5f - minY;
}

static float textVisualCenterY(const FontAtlas& font, float top, float boxH, const char* text, float fontSize) {
    std::wstring wide;
    if (!utf8ToWide(text, wide))
        return textCenterY(font, top, boxH, fontSize);
    return textVisualCenterY(font, top, boxH, wide.c_str(), fontSize);
}

static float textControlCenterY(const FontAtlas& font, float top, float boxH, const char* text, float fontSize) {
    return textVisualCenterY(font, top, boxH, text, fontSize) + fontSize * 0.16f;
}

static unsigned int withAlpha(unsigned int color, unsigned int alpha) {
    return (color & 0x00FFFFFFu) | (alpha << 24);
}

static std::filesystem::path findTabIconAssetPath(const char* filename) {
    std::vector<std::filesystem::path> candidates;
    candidates.emplace_back(std::filesystem::path("assets") / filename);
    candidates.emplace_back(std::filesystem::path("../assets") / filename);
    candidates.emplace_back(std::filesystem::path("../../assets") / filename);
    candidates.emplace_back(filename);

    wchar_t exePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        const std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
        candidates.emplace_back(exeDir / "assets" / filename);
        candidates.emplace_back(exeDir / filename);
        candidates.emplace_back(exeDir.parent_path() / "assets" / filename);
        candidates.emplace_back(exeDir.parent_path() / filename);
    }

    for (const auto& p : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(p, ec))
            return p;
    }
    return {};
}

static unsigned int lerpColor(unsigned int c0, unsigned int c1, float t) {
    t = std::clamp(t, 0.f, 1.f);
    auto ch = [&](int shift) -> unsigned int {
        float a = (float)((c0 >> shift) & 0xFF);
        float b = (float)((c1 >> shift) & 0xFF);
        return (unsigned int)(a + (b - a) * t + 0.5f);
    };
    return (ch(24) << 24) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

static float easeOutCubic(float t) {
    t = std::clamp(t, 0.f, 1.f);
    float inv = 1.f - t;
    return 1.f - inv * inv * inv;
}

static float advanceSubTabAnim(float current, bool active, float dt) {
    const float target = active ? 1.f : 0.f;
    return current + (target - current) * (std::min)(dt * 7.5f, 1.f);
}

static float advanceTabAnim(float current, bool active, float dt) {
    const float target = active ? 1.f : 0.f;
    const float speed = active ? 11.f : 18.f;
    float next = current + (target - current) * (std::min)(dt * speed, 1.f);
    if (!active && next < 0.008f)
        next = 0.f;
    return next;
}

static void drawSubTabSurface(Renderer& r, float x, float y, float w, float h,
                              unsigned int inactiveBg, unsigned int activeBg,
                              float corner, float activeT, float scale,
                              bool fadeBackground = false,
                              unsigned int fadeBaseBg = 0) {
    const float t = std::clamp(activeT, 0.f, 1.f);
    const float barEase = easeOutCubic(t);

    if (fadeBackground) {
        if (t > 0.f) {
            const unsigned int baseBg = fadeBaseBg ? fadeBaseBg : inactiveBg;
            const unsigned int bg = lerpColor(baseBg, activeBg, t);
            r.drawRoundedFilledRect(x, y, w, h, bg, corner);
        }
    } else {
        const unsigned int bg = lerpColor(inactiveBg, activeBg, t);
        r.drawRoundedFilledRect(x, y, w, h, bg, corner);
    }

    if (barEase > 0.001f) {
        const float animH = 7.f * scale * barEase;
        const float barY = y + h - animH;
        const unsigned int barBg = withAlpha(Theme::ACCENT, (unsigned int)(240.f * barEase));
        r.drawRoundedFilledRectCorners(
            x, barY, w, animH, barBg, corner,
            ImDrawFlags_RoundCornersBottomLeft | ImDrawFlags_RoundCornersBottomRight);
    }
}

static uint32_t hashAnimId(const char* text, float x = 0.f, float y = 0.f) {
    uint32_t h = 5381u;
    if (text) {
        for (const char* p = text; *p; ++p)
            h = ((h << 5) + h) + (unsigned char)*p;
    }

    uint32_t xi = (uint32_t)std::lround(x * 10.f);
    uint32_t yi = (uint32_t)std::lround(y * 10.f);
    h ^= xi + 0x9E3779B9u + (h << 6) + (h >> 2);
    h ^= yi + 0x9E3779B9u + (h << 6) + (h >> 2);
    return h;
}

static float animBlend(float speed, float dt) {
    speed = std::clamp(speed * 1.35f, 0.01f, 0.98f);
    float frameScale = std::clamp(dt * 60.f, 0.25f, 4.f);
    return 1.f - std::pow(1.f - speed, frameScale);
}

static float smoothValue(float current, float target, float speed, float dt) {
    return current + (target - current) * animBlend(speed, dt);
}

static float animValue(uint32_t id, float target, float speed, float dt) {
    static std::unordered_map<uint32_t, float> s_anim;
    float& value = s_anim[id];
    value = smoothValue(value, target, speed, dt);
    return value;
}

static bool loadTextureFromWicFile(ID3D11Device* device,
                                   const std::filesystem::path& path,
                                   Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& outSrv,
                                   int& outW,
                                   int& outH) {
    outSrv.Reset();
    outW = 0;
    outH = 0;
    if (!device)
        return false;

    HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool needsUninit = SUCCEEDED(coHr);
    if (FAILED(coHr) && coHr != RPC_E_CHANGED_MODE)
        return false;

    Microsoft::WRL::ComPtr<IWICImagingFactory> wicFactory;
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;

    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wicFactory)
    );
    if (SUCCEEDED(hr)) {
        hr = wicFactory->CreateDecoderFromFilename(
            path.c_str(),
            nullptr,
            GENERIC_READ,
            WICDecodeMetadataCacheOnDemand,
            decoder.GetAddressOf()
        );
    }
    if (SUCCEEDED(hr))
        hr = decoder->GetFrame(0, frame.GetAddressOf());
    if (SUCCEEDED(hr))
        hr = wicFactory->CreateFormatConverter(converter.GetAddressOf());
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(
            frame.Get(),
            GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom
        );
    }

    UINT width = 0;
    UINT height = 0;
    if (SUCCEEDED(hr))
        hr = converter->GetSize(&width, &height);
    if (SUCCEEDED(hr) && (width == 0 || height == 0))
        hr = E_FAIL;

    std::vector<unsigned char> pixels;
    if (SUCCEEDED(hr)) {
        const UINT stride = width * 4;
        const UINT bytes = stride * height;
        pixels.resize(bytes);
        hr = converter->CopyPixels(nullptr, stride, bytes, pixels.data());
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    if (SUCCEEDED(hr)) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = width;
        td.Height = height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = pixels.data();
        init.SysMemPitch = width * 4;

        hr = device->CreateTexture2D(&td, &init, texture.GetAddressOf());
    }

    if (SUCCEEDED(hr))
        hr = device->CreateShaderResourceView(texture.Get(), nullptr, outSrv.GetAddressOf());

    if (needsUninit)
        CoUninitialize();

    if (FAILED(hr))
        return false;

    outW = static_cast<int>(width);
    outH = static_cast<int>(height);
    return true;
}

static bool loadTextureFromWicMemory(ID3D11Device* device,
                                     const std::uint8_t* data,
                                     size_t size,
                                     Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& outSrv,
                                     int& outW,
                                     int& outH) {
    outSrv.Reset();
    outW = 0;
    outH = 0;
    if (!device || !data || size == 0)
        return false;

    HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool needsUninit = SUCCEEDED(coHr);
    if (FAILED(coHr) && coHr != RPC_E_CHANGED_MODE)
        return false;

    Microsoft::WRL::ComPtr<IWICImagingFactory> wicFactory;
    Microsoft::WRL::ComPtr<IWICStream> stream;
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;

    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&wicFactory));
    if (SUCCEEDED(hr))
        hr = wicFactory->CreateStream(stream.GetAddressOf());
    if (SUCCEEDED(hr))
        hr = stream->InitializeFromMemory(const_cast<BYTE*>(data), static_cast<DWORD>(size));
    if (SUCCEEDED(hr))
        hr = wicFactory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand,
                                                 decoder.GetAddressOf());
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
    if (SUCCEEDED(hr) && (width == 0 || height == 0))
        hr = E_FAIL;

    std::vector<unsigned char> pixels;
    if (SUCCEEDED(hr)) {
        const UINT stride = width * 4;
        const UINT bytes = stride * height;
        pixels.resize(bytes);
        hr = converter->CopyPixels(nullptr, stride, bytes, pixels.data());
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    if (SUCCEEDED(hr)) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = width;
        td.Height = height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = pixels.data();
        init.SysMemPitch = width * 4;

        hr = device->CreateTexture2D(&td, &init, texture.GetAddressOf());
    }

    if (SUCCEEDED(hr))
        hr = device->CreateShaderResourceView(texture.Get(), nullptr, outSrv.GetAddressOf());

    if (needsUninit)
        CoUninitialize();

    if (FAILED(hr))
        return false;

    outW = static_cast<int>(width);
    outH = static_cast<int>(height);
    return true;
}

static bool httpDownloadUrl(const std::string& url, std::vector<std::uint8_t>& out) {
    out.clear();
    if (url.empty())
        return false;

    std::wstring wurl(url.begin(), url.end());
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256]{};
    wchar_t path[2048]{};
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts))
        return false;

    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    HINTERNET session = WinHttpOpen(L"CrymoreOverlay/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        return false;
    WinHttpSetTimeouts(session, 3000, 3000, 5000, 5000);

    HINTERNET connect = WinHttpConnect(session, host, parts.nPort, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return false;
    }

    const DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    bool ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
           && WinHttpReceiveResponse(request, nullptr);
    if (ok) {
        DWORD size = 0;
        do {
            if (!WinHttpQueryDataAvailable(request, &size))
                break;
            if (size == 0)
                break;
            const size_t offset = out.size();
            out.resize(offset + size);
            DWORD read = 0;
            if (!WinHttpReadData(request, out.data() + offset, size, &read)) {
                out.resize(offset);
                ok = false;
                break;
            }
            out.resize(offset + read);
        } while (size > 0);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return ok && !out.empty();
}

static void prepareTabIconPixels(std::vector<unsigned char>& pixels, bool& outFullColor) {
    outFullColor = false;
    bool sawOpaque = false;

    for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
        const unsigned char b = pixels[i + 0];
        const unsigned char g = pixels[i + 1];
        const unsigned char r = pixels[i + 2];
        const unsigned char a = pixels[i + 3];
        if (a < 8)
            continue;

        sawOpaque = true;
        if (r > 20 || g > 20 || b > 20)
            outFullColor = true;
    }

    if (outFullColor || !sawOpaque)
        return;

    // Legacy alpha-mask icons store shape in alpha with black RGB. ImGui tints
    // texture RGB by the draw color, so black * accent = black. Expand to white.
    for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
        if (pixels[i + 3] < 8) {
            pixels[i + 0] = pixels[i + 1] = pixels[i + 2] = 0;
            continue;
        }
        pixels[i + 0] = 255;
        pixels[i + 1] = 255;
        pixels[i + 2] = 255;
    }
}

static bool loadTabIconTexture(ID3D11Device* device,
                               const std::filesystem::path& path,
                               Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& outSrv,
                               int& outW,
                               int& outH,
                               bool& outFullColor) {
    outFullColor = false;
    outSrv.Reset();
    outW = 0;
    outH = 0;
    if (!device)
        return false;

    HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool needsUninit = SUCCEEDED(coHr);
    if (FAILED(coHr) && coHr != RPC_E_CHANGED_MODE)
        return false;

    Microsoft::WRL::ComPtr<IWICImagingFactory> wicFactory;
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;

    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wicFactory)
    );
    if (SUCCEEDED(hr)) {
        hr = wicFactory->CreateDecoderFromFilename(
            path.c_str(),
            nullptr,
            GENERIC_READ,
            WICDecodeMetadataCacheOnDemand,
            decoder.GetAddressOf()
        );
    }
    if (SUCCEEDED(hr))
        hr = decoder->GetFrame(0, frame.GetAddressOf());
    if (SUCCEEDED(hr))
        hr = wicFactory->CreateFormatConverter(converter.GetAddressOf());
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(
            frame.Get(),
            GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom
        );
    }

    UINT width = 0;
    UINT height = 0;
    if (SUCCEEDED(hr))
        hr = converter->GetSize(&width, &height);
    if (SUCCEEDED(hr) && (width == 0 || height == 0))
        hr = E_FAIL;

    std::vector<unsigned char> pixels;
    if (SUCCEEDED(hr)) {
        const UINT stride = width * 4;
        const UINT bytes = stride * height;
        pixels.resize(bytes);
        hr = converter->CopyPixels(nullptr, stride, bytes, pixels.data());
    }

    if (SUCCEEDED(hr))
        prepareTabIconPixels(pixels, outFullColor);

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    if (SUCCEEDED(hr)) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = width;
        td.Height = height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = pixels.data();
        init.SysMemPitch = width * 4;

        hr = device->CreateTexture2D(&td, &init, texture.GetAddressOf());
    }

    if (SUCCEEDED(hr))
        hr = device->CreateShaderResourceView(texture.Get(), nullptr, outSrv.GetAddressOf());

    if (needsUninit)
        CoUninitialize();

    if (FAILED(hr))
        return false;

    outW = static_cast<int>(width);
    outH = static_cast<int>(height);
    return true;
}

static std::filesystem::path findHitboxModelAssetPath() {
    std::vector<std::filesystem::path> candidates;
    candidates.emplace_back("assets/hitbox playermodel.png");
    candidates.emplace_back("../assets/hitbox playermodel.png");
    candidates.emplace_back("../../assets/hitbox playermodel.png");
    candidates.emplace_back("assets/playermodel.png");
    candidates.emplace_back("../assets/playermodel.png");
    candidates.emplace_back("../../assets/playermodel.png");
    candidates.emplace_back("assets/hitbox_operator.png");
    candidates.emplace_back("../assets/hitbox_operator.png");
    candidates.emplace_back("../../assets/hitbox_operator.png");

    wchar_t exePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
        candidates.emplace_back(exeDir / "assets" / "hitbox playermodel.png");
        candidates.emplace_back(exeDir.parent_path() / "assets" / "hitbox playermodel.png");
        candidates.emplace_back(exeDir / "assets" / "playermodel.png");
        candidates.emplace_back(exeDir.parent_path() / "assets" / "playermodel.png");
        candidates.emplace_back(exeDir / "assets" / "hitbox_operator.png");
        candidates.emplace_back(exeDir.parent_path() / "assets" / "hitbox_operator.png");
    }

    for (const auto& p : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(p, ec))
            return p;
    }
    return {};
}

static std::filesystem::path findEspPreviewModelAssetPath() {
    std::vector<std::filesystem::path> candidates;
    candidates.emplace_back("assets/esp playermodel.png");
    candidates.emplace_back("../assets/esp playermodel.png");
    candidates.emplace_back("../../assets/esp playermodel.png");

    wchar_t exePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
        candidates.emplace_back(exeDir / "assets" / "esp playermodel.png");
        candidates.emplace_back(exeDir.parent_path() / "assets" / "esp playermodel.png");
    }

    for (const auto& p : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(p, ec))
            return p;
    }
    return {};
}

static std::filesystem::path findBrandLogoAssetPath() {
    std::vector<std::filesystem::path> candidates;
    candidates.emplace_back("assets/brand_logo.png");
    candidates.emplace_back("assets/crymore logo.png");
    candidates.emplace_back("assets/crymore_logo.png");
    candidates.emplace_back("assets/logo.png");
    candidates.emplace_back("../assets/brand_logo.png");
    candidates.emplace_back("../assets/crymore logo.png");
    candidates.emplace_back("../assets/crymore_logo.png");
    candidates.emplace_back("../assets/logo.png");
    candidates.emplace_back("../../assets/brand_logo.png");
    candidates.emplace_back("../../assets/crymore logo.png");
    candidates.emplace_back("../../assets/crymore_logo.png");
    candidates.emplace_back("../../assets/logo.png");

    wchar_t exePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
        candidates.emplace_back(exeDir / "assets" / "brand_logo.png");
        candidates.emplace_back(exeDir / "assets" / "crymore logo.png");
        candidates.emplace_back(exeDir / "assets" / "crymore_logo.png");
        candidates.emplace_back(exeDir / "assets" / "logo.png");
        candidates.emplace_back(exeDir.parent_path() / "assets" / "brand_logo.png");
        candidates.emplace_back(exeDir.parent_path() / "assets" / "crymore logo.png");
        candidates.emplace_back(exeDir.parent_path() / "assets" / "crymore_logo.png");
        candidates.emplace_back(exeDir.parent_path() / "assets" / "logo.png");
    }

    for (const auto& p : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(p, ec))
            return p;
    }
    return {};
}

static std::filesystem::path findLeetifyBadgeAssetPath() {
    std::vector<std::filesystem::path> candidates;
    candidates.emplace_back("assets/leetify_badge.png");
    candidates.emplace_back("Leetify Badge Black Small.png");
    candidates.emplace_back("../assets/leetify_badge.png");
    candidates.emplace_back("../Leetify Badge Black Small.png");
    candidates.emplace_back("../../assets/leetify_badge.png");
    candidates.emplace_back("../../Leetify Badge Black Small.png");

    wchar_t exePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
        candidates.emplace_back(exeDir / "assets" / "leetify_badge.png");
        candidates.emplace_back(exeDir / "Leetify Badge Black Small.png");
        candidates.emplace_back(exeDir.parent_path() / "assets" / "leetify_badge.png");
        candidates.emplace_back(exeDir.parent_path() / "Leetify Badge Black Small.png");
    }

    for (const auto& p : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(p, ec))
            return p;
    }
    return {};
}

static std::filesystem::path findZoomInIconAssetPath() {
    std::vector<std::filesystem::path> candidates;
    candidates.emplace_back("assets/zoom-in.png");
    candidates.emplace_back("zoom-in.png");
    candidates.emplace_back("../assets/zoom-in.png");
    candidates.emplace_back("../../assets/zoom-in.png");

    wchar_t exePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
        candidates.emplace_back(exeDir / "assets" / "zoom-in.png");
        candidates.emplace_back(exeDir / "zoom-in.png");
        candidates.emplace_back(exeDir.parent_path() / "assets" / "zoom-in.png");
    }

    for (const auto& p : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(p, ec))
            return p;
    }
    return {};
}

static uint32_t hashUiId(const char* id) {
    uint32_t h = 2166136261u;
    if (!id)
        return h;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(id); *p; ++p) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

static bool drawIconAccentButton(Gui& gui, Renderer& r, const char* id,
                                 ID3D11ShaderResourceView* icon, int iconW, int iconH,
                                 float x, float y, float size, float s) {
    const uint32_t idh = hashUiId(id);
    const bool hovered = gui.mouseX() >= x && gui.mouseX() <= x + size
                      && gui.mouseY() >= y && gui.mouseY() <= y + size;
    const unsigned int bg = hovered ? 0xFF8B89F8 : Theme::ACCENT;
    r.drawRoundedFilledRect(x, y, size, size, bg, 10.f * s);
    (void)icon;
    (void)iconW;
    (void)iconH;
    const float cx = x + size * 0.47f;
    const float cy = y + size * 0.43f;
    const float rr = size * 0.19f;
    const unsigned int iconCol = 0xFFFFFFFFu;
    r.drawCircle(cx, cy, rr, iconCol, (std::max)(2.f, 2.3f * s));
    r.drawLine(cx + rr * 0.65f, cy + rr * 0.65f,
               x + size * 0.75f, y + size * 0.75f,
               iconCol, (std::max)(2.f, 2.8f * s));
    r.drawLine(cx - rr * 0.42f, cy, cx + rr * 0.42f, cy, iconCol, (std::max)(1.5f, 2.f * s));
    r.drawLine(cx, cy - rr * 0.42f, cx, cy + rr * 0.42f, iconCol, (std::max)(1.5f, 2.f * s));
    return hovered && gui.mouseClicked();
}

static const char* playerRiskGlance(const PlayerScout::Row& row) {
    switch (row.state) {
    case PlayerScout::FetchState::Queued:
        return "Waiting…";
    case PlayerScout::FetchState::Loading:
        return "Checking…";
    case PlayerScout::FetchState::NotOnLeetify:
        return "No Leetify profile";
    case PlayerScout::FetchState::Error:
        return row.status.empty() ? "Stats unavailable" : row.status.c_str();
    case PlayerScout::FetchState::Ready:
    default:
        break;
    }
    if (row.suspicionScore >= 75)
        return "High risk";
    if (row.suspicionScore >= 50)
        return "Potentially risky";
    if (row.suspicionScore >= 25)
        return "Some concerns";
    return "Clean";
}

static unsigned int playerRiskGlanceColor(const PlayerScout::Row& row) {
    switch (row.state) {
    case PlayerScout::FetchState::Queued:
    case PlayerScout::FetchState::Loading:
        return Theme::TEXT_MUTED;
    case PlayerScout::FetchState::NotOnLeetify:
        return Theme::TEXT_MUTED;
    case PlayerScout::FetchState::Error:
        return 0xFFFF8A65;
    case PlayerScout::FetchState::Ready:
    default:
        break;
    }
    if (row.suspicionScore >= 75)
        return Theme::DESTRUCTIVE;
    if (row.suspicionScore >= 50)
        return 0xFFFFD77A;
    if (row.suspicionScore >= 25)
        return Theme::TEXT;
    return Theme::TEXT_LINK;
}

static float measureMenuTextWidth(const FontAtlas& font, const char* text, float size);

static void drawHighlightedStat(Renderer& r, const FontAtlas& f, float x, float y,
                                const char* label, const char* value, float labelSize, float valueSize) {
    r.drawText(f, x, y, label, Theme::TEXT_MUTED, labelSize);
    const bool missing = !value || !value[0] || std::strcmp(value, "--") == 0;
    r.drawText(f, x, y + labelSize + 2.f, value && value[0] ? value : "--",
               missing ? Theme::TEXT_MUTED : Theme::ACCENT, valueSize);
}

static void drawStatMeter(Renderer& r, const FontAtlas& f, float x, float y, float w,
                          const char* label, float fill01, const char* valueText,
                          float fontSize, float s) {
    r.drawText(f, x, y, label, Theme::TEXT_MUTED, fontSize);
    const float barY = y + fontSize + 6.f * s;
    const float barH = 8.f * s;
    r.drawRoundedFilledRect(x, barY, w, barH, 0xFF1A1D28, 4.f * s);
    fill01 = std::clamp(fill01, 0.f, 1.f);
    if (fill01 > 0.001f)
        r.drawRoundedFilledRect(x, barY, w * fill01, barH, Theme::ACCENT, 4.f * s);
    r.drawText(f, x + w + 10.f * s, barY - 1.f * s, valueText, Theme::ACCENT, fontSize);
}

static unsigned int qualityColor(float score01) {
    score01 = std::clamp(score01, 0.f, 1.f);
    if (score01 >= 0.72f) return 0xFF58D6A5u;
    if (score01 >= 0.50f) return 0xFFFFC857u;
    return 0xFFFF766Fu;
}

static void drawQualityMeter(Renderer& r, const FontAtlas& f, float x, float y, float w,
                             const char* label, float fill01, const char* valueText,
                             float fontSize, float s, bool lowerIsBetter = false) {
    fill01 = std::clamp(fill01, 0.f, 1.f);
    const float quality = lowerIsBetter ? (1.f - fill01) : fill01;
    const unsigned int col = qualityColor(quality);
    r.drawText(f, x, y, label, Theme::TEXT_MUTED, fontSize);
    const float barY = y + fontSize + 6.f * s;
    const float barH = 7.f * s;
    r.drawRoundedFilledRect(x, barY, w, barH, 0xFF1A1D28, 4.f * s);
    if (fill01 > 0.001f)
        r.drawRoundedFilledRect(x, barY, w * fill01, barH, col, 4.f * s);
    r.drawText(f, x + w + 10.f * s, barY - 1.f * s, valueText, col, fontSize);
}

static void drawRiskMeter(Renderer& r, const FontAtlas& f, float x, float y, float w,
                          const char* label, int suspicionScore, const char* valueText,
                          float fontSize, float s) {
    const float fill01 = std::clamp(suspicionScore / 100.f, 0.f, 1.f);
    const unsigned int col = qualityColor(1.f - fill01);
    r.drawText(f, x, y, label, Theme::TEXT_MUTED, fontSize);
    const float barY = y + fontSize + 6.f * s;
    const float barH = 7.f * s;
    r.drawRoundedFilledRect(x, barY, w, barH, 0xFF1A1D28, 4.f * s);
    if (fill01 > 0.001f)
        r.drawRoundedFilledRect(x, barY, w * fill01, barH, col, 4.f * s);
    r.drawText(f, x + w + 10.f * s, barY - 1.f * s, valueText, col, fontSize);
}

static void drawQualityCircle(Renderer& r, const FontAtlas& f, float cx, float cy, float radius,
                              const char* label, const char* valueText, float score01,
                              float fontSize, float s, bool lowerIsBetter = false) {
    score01 = std::clamp(score01, 0.f, 1.f);
    const float quality = lowerIsBetter ? (1.f - score01) : score01;
    const unsigned int col = qualityColor(quality);
    const float trackThk = (std::max)(1.f, 1.5f * s);
    const float arcThk = (std::max)(3.f, 4.f * s);
    r.drawCircle(cx, cy, radius, 0xFF2A2E3Cu, trackThk);
    if (score01 > 0.005f) {
        const int steps = (std::max)(16, static_cast<int>(score01 * 40.f));
        const float start = -1.5707963f;
        const float sweep = score01 * 6.2831853f;
        for (int i = 0; i < steps; ++i) {
            const float t0 = start + sweep * (static_cast<float>(i) / static_cast<float>(steps));
            const float t1 = start + sweep * (static_cast<float>(i + 1) / static_cast<float>(steps));
            r.drawLine(cx + std::cosf(t0) * radius, cy + std::sinf(t0) * radius,
                       cx + std::cosf(t1) * radius, cy + std::sinf(t1) * radius,
                       col, arcThk);
        }
    }
    const float valueSize = fontSize * 1.08f;
    const float valueW = measureMenuTextWidth(f, valueText, valueSize);
    const float valueH = f.lineHeight(valueSize);
    r.drawText(f, cx - valueW * 0.5f, cy - valueH * 0.5f, valueText, Theme::TEXT, valueSize);
    const float labelSize = fontSize * 0.82f;
    const float labelW = measureMenuTextWidth(f, label, labelSize);
    r.drawText(f, cx - labelW * 0.5f, cy + radius + 10.f * s, label, Theme::TEXT_MUTED, labelSize);
}

static float measureMenuTextWidth(const FontAtlas& font, const char* text, float size) {
    if (!text || !*text)
        return 0.f;
    int len = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (len <= 1)
        return 0.f;

    std::wstring wtext(len - 1, L' ');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wtext.data(), len);

    const float scale = size / font.renderPx();
    float width = 0.f;
    float spaceAdv = 8.f;
    if (const GlyphInfo* sg = font.glyph(L' '))
        spaceAdv = static_cast<float>(sg->advanceX);

    for (wchar_t ch : wtext) {
        if (ch == L' ') {
            width += spaceAdv * scale;
            continue;
        }
        if (const GlyphInfo* gi = font.glyph(ch))
            width += gi->advanceX * scale;
        else
            width += 8.f * scale;
    }
    return width;
}

static bool openExternalUrl(const wchar_t* url) {
    if (!url || !url[0])
        return false;
    const INT_PTR result = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL));
    return result > 32;
}

static float measureRenderedTextWidth(Renderer& r, const FontAtlas& f, const char* text, float fontSize) {
    if (!text || !*text)
        return 0.f;

    int len = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (len <= 1)
        return 0.f;

    std::wstring wtext(static_cast<std::size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wtext.data(), len);

    float minX = 0.f, minY = 0.f, maxX = 0.f, maxY = 0.f;
    r.measureTextBoundsW(f, wtext.c_str(), fontSize, minX, minY, maxX, maxY);
    return (std::max)(0.f, maxX - minX);
}

static bool drawTextLink(Gui& gui, Renderer& r, const FontAtlas& f, float x, float y,
                         const char* text, const wchar_t* url, float fontSize,
                         unsigned int textColor = Theme::TEXT_MUTED, bool bold = false,
                         bool underline = true) {
    if (!text || !text[0])
        return false;

    const float textW = measureRenderedTextWidth(r, f, text, fontSize);
    const float hitH = fontSize * 1.45f;
    const bool hovered = gui.mouseX() >= x && gui.mouseX() <= x + textW
                      && gui.mouseY() >= y && gui.mouseY() <= y + hitH;
    unsigned int col = textColor;
    if (hovered)
        col = lerpColor(textColor, Theme::TEXT, 0.35f);

    if (bold) {
        r.drawText(f, x + 0.5f, y, text, col, fontSize);
        r.drawText(f, x, y, text, col, fontSize);
    } else {
        r.drawText(f, x, y, text, col, fontSize);
    }
    if (underline)
        r.drawFilledRect(x, y + fontSize + 1.5f, textW, 1.2f, col);

    if (hovered && gui.mouseClicked()) {
        if (url && url[0])
            return openExternalUrl(url);
        return true;
    }
    return false;
}

static std::filesystem::path findAvatarPlaceholderPath() {
    std::vector<std::filesystem::path> candidates;
    candidates.emplace_back("assets/picture_placeholder.png");
    candidates.emplace_back("../assets/picture_placeholder.png");
    candidates.emplace_back("../../assets/picture_placeholder.png");

    wchar_t exePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
        candidates.emplace_back(exeDir / "assets" / "picture_placeholder.png");
        candidates.emplace_back(exeDir.parent_path() / "assets" / "picture_placeholder.png");
    }

    for (const auto& p : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(p, ec))
            return p;
    }
    return {};
}

static int previewAnchorOffsetSource(int anchor) {
    switch (anchor) {
    case 1: return 0;
    case 6: return 4;
    case 7: return 5;
    case 3: return 2;
    default: return anchor;
    }
}

static void applyPreviewAnchorOffset(int anchor, float& outX, float& outY) {
    const int src = previewAnchorOffsetSource(anchor);
    outX += g_cfg.espAnchorOffsetX[(size_t)src];
    const float oy = g_cfg.espAnchorOffsetY[(size_t)src];
    if (anchor == 1 || anchor == 6 || anchor == 7)
        outY -= oy;
    else
        outY += oy;
}

static int clampEspAnchor(int v) {
    return std::clamp(v, 0, 7);
}

static int clampEspBarAnchorStored(int v) {
    if (v < 0)
        return -1;
    switch (clampEspAnchor(v)) {
    case 0:
    case 1:
    case 2:
    case 3:
        return clampEspAnchor(v);
    case 4:
    case 6:
        return 2;
    case 5:
    case 7:
    default:
        return 3;
    }
}

static int clampEspInfoAnchorStored(int v) {
    if (v < 0)
        return -1;
    switch (clampEspAnchor(v)) {
    case 0:
    case 1:
    case 4:
    case 5:
    case 6:
    case 7:
        return clampEspAnchor(v);
    case 2:
        return 4;
    case 3:
    default:
        return 5;
    }
}

// Baked preview layout values chosen from tuning pass.
static constexpr float kPreviewPanelOffsetX = 0.f;
static constexpr float kPreviewPanelOffsetY = 0.f;
static constexpr float kPreviewPanelW = 380.f;
static constexpr float kPreviewPanelH = 760.f;
static constexpr float kPreviewBottomReserve = 223.f;
static constexpr float kPreviewModelOffsetX = 10.f;
static constexpr float kPreviewModelOffsetY = -20.f;
static constexpr float kPreviewModelScale = 0.90f;
static constexpr float kPreviewBoxX = 0.102f;
static constexpr float kPreviewBoxY = 0.078f;
static constexpr float kPreviewBoxW = 0.731f;
static constexpr float kPreviewBoxH = 0.921f;

static constexpr float kPreviewBarLenHorizontal = 1.80f;
static constexpr float kPreviewBarLenVertical = 2.00f;
static constexpr float kHitboxBottomReserve = 14.f;


static std::filesystem::path cfgDirPath() {
    return std::filesystem::path("configs");
}

static json colorToJson(const float c[4]) {
    return json::array({ c[0], c[1], c[2], c[3] });
}

static void jsonToColor(const json& j, float c[4]) {
    if (!j.is_array() || j.size() != 4) return;
    for (int i = 0; i < 4; ++i) c[i] = j[i].get<float>();
}

static json aimGroupCalibToJson(const AimGroupCalibProfile& p) {
    json j;
    j["valid"] = p.valid;
    j["accuracy"] = p.accuracy;
    j["sampleCount"] = p.sampleCount;
    j["measuredReactionMs"] = p.measuredReactionMs;
    j["measuredSmooth"] = p.measuredSmooth;
    j["measuredFlickSpeed"] = p.measuredFlickSpeed;
    j["measuredJitter"] = p.measuredJitter;
    j["measuredOvershoot"] = p.measuredOvershoot;
    j["measuredPunchPitch"] = p.measuredPunchPitch;
    j["measuredPunchYaw"] = p.measuredPunchYaw;
    j["assistReactionMs"] = p.assistReactionMs;
    j["assistSmooth"] = p.assistSmooth;
    j["assistFlickSpeed"] = p.assistFlickSpeed;
    j["assistJitter"] = p.assistJitter;
    j["assistOvershoot"] = p.assistOvershoot;
    j["rcsX"] = p.rcsX;
    j["rcsY"] = p.rcsY;
    j["rcsSmooth"] = p.rcsSmooth;
    return j;
}

static void jsonToAimGroupCalib(const json& j, AimGroupCalibProfile& p) {
    auto getF = [&](const char* k, float& v) { if (j.contains(k)) v = j[k].get<float>(); };
    auto getI = [&](const char* k, int& v) { if (j.contains(k)) v = j[k].get<int>(); };
    if (j.contains("valid")) p.valid = j["valid"].get<bool>();
    getF("accuracy", p.accuracy);
    getI("sampleCount", p.sampleCount);
    getF("measuredReactionMs", p.measuredReactionMs);
    getF("measuredSmooth", p.measuredSmooth);
    getF("measuredFlickSpeed", p.measuredFlickSpeed);
    getF("measuredJitter", p.measuredJitter);
    getF("measuredOvershoot", p.measuredOvershoot);
    getF("measuredPunchPitch", p.measuredPunchPitch);
    getF("measuredPunchYaw", p.measuredPunchYaw);
    getF("assistReactionMs", p.assistReactionMs);
    getF("assistSmooth", p.assistSmooth);
    getF("assistFlickSpeed", p.assistFlickSpeed);
    getF("assistJitter", p.assistJitter);
    getF("assistOvershoot", p.assistOvershoot);
    getF("rcsX", p.rcsX);
    getF("rcsY", p.rcsY);
    getF("rcsSmooth", p.rcsSmooth);
}

static json aimGroupToJson(const AimGroupConfig& cfg) {
    json j;
    j["aimBone"] = cfg.aimBone;
    j["hitboxHead"] = cfg.hitboxHead;
    j["hitboxStomach"] = cfg.hitboxStomach;
    j["hitboxChest"] = cfg.hitboxChest;
    j["hitboxPelvis"] = cfg.hitboxPelvis;
    j["hitboxArms"] = cfg.hitboxArms;
    j["hitboxLegs"] = cfg.hitboxLegs;
    j["aimFov"] = cfg.aimFov;
    j["aimSmooth"] = cfg.aimSmooth;
    j["rcsEnabled"] = cfg.rcsEnabled;
    j["rcsMode"] = cfg.rcsMode;
    j["rcsX"] = cfg.rcsX;
    j["rcsY"] = cfg.rcsY;
    j["rcsSmooth"] = cfg.rcsSmooth;
    j["triggerEnabled"] = cfg.triggerEnabled;
    j["triggerDelayMs"] = cfg.triggerDelayMs;
    j["triggerKey"] = cfg.triggerKey;
    return j;
}

static void jsonToAimGroup(const json& j, AimGroupConfig& cfg) {
    auto setInt = [&](const char* k, int& v) { if (j.contains(k)) v = j[k].get<int>(); };
    auto setBool = [&](const char* k, bool& v) { if (j.contains(k)) v = j[k].get<bool>(); };
    auto setFloat = [&](const char* k, float& v) { if (j.contains(k)) v = j[k].get<float>(); };

    setInt("aimBone", cfg.aimBone);
    setBool("hitboxHead", cfg.hitboxHead);
    setBool("hitboxStomach", cfg.hitboxStomach);
    setBool("hitboxChest", cfg.hitboxChest);
    setBool("hitboxPelvis", cfg.hitboxPelvis);
    setBool("hitboxArms", cfg.hitboxArms);
    setBool("hitboxLegs", cfg.hitboxLegs);
    setFloat("aimFov", cfg.aimFov);
    setFloat("aimSmooth", cfg.aimSmooth);
    cfg.aimSmooth = std::clamp(cfg.aimSmooth, 1.f, 20.f);
    setBool("rcsEnabled", cfg.rcsEnabled);
    setInt("rcsMode", cfg.rcsMode);
    setFloat("rcsX", cfg.rcsX);
    setFloat("rcsY", cfg.rcsY);
    setFloat("rcsSmooth", cfg.rcsSmooth);
    setBool("triggerEnabled", cfg.triggerEnabled);
    setInt("triggerDelayMs", cfg.triggerDelayMs);
    setInt("triggerKey", cfg.triggerKey);
    // Backward compatibility with previous single-slider configs.
    if (!j.contains("rcsX") && !j.contains("rcsY") && j.contains("rcsStrength")) {
        float legacy = j["rcsStrength"].get<float>();
        cfg.rcsX = legacy;
        cfg.rcsY = legacy;
    }

    const bool hasAnyHitboxKeys = j.contains("hitboxHead") || j.contains("hitboxStomach") || j.contains("hitboxChest")
        || j.contains("hitboxPelvis") || j.contains("hitboxArms") || j.contains("hitboxLegs");
    if (!hasAnyHitboxKeys) {
        cfg.hitboxHead = false;
        cfg.hitboxStomach = false;
        cfg.hitboxChest = false;
        cfg.hitboxPelvis = false;
        cfg.hitboxArms = false;
        cfg.hitboxLegs = false;
        if (cfg.aimBone == 6) cfg.hitboxHead = true;
        else if (cfg.aimBone == 2 || cfg.aimBone == 3) cfg.hitboxStomach = true;
        else if (cfg.aimBone == 4) cfg.hitboxChest = true;
        else cfg.hitboxHead = true;
    }

    cfg.rcsX = std::clamp(cfg.rcsX, 0.f, 1.25f);
    cfg.rcsY = std::clamp(cfg.rcsY, 0.f, 1.25f);
    if (cfg.rcsMode < 0) cfg.rcsMode = 0;
    if (cfg.rcsMode > 1) cfg.rcsMode = 1;
}

static json configToJson(const OverlayConfig& cfg) {
    json j;
    j["espEnabled"] = cfg.espEnabled;
    j["boxEnabled"] = cfg.boxEnabled;
    j["boxOccluded"] = cfg.boxOccluded;
    j["hpBarEnabled"] = cfg.hpBarEnabled;
    j["hpBarOccluded"] = cfg.hpBarOccluded;
    j["hpTextEnabled"] = cfg.hpTextEnabled;
    j["hpTextVisibleEnabled"] = cfg.hpTextVisibleEnabled;
    j["hpTextOccludedEnabled"] = cfg.hpTextOccludedEnabled;
    j["enemyOnly"] = cfg.enemyOnly;
    j["nameEspEnabled"] = cfg.nameEspEnabled;
    j["nameEspAvatarEnabled"] = cfg.nameEspAvatarEnabled;
    j["armorEspEnabled"] = cfg.armorEspEnabled;
    j["armorVisibleEnabled"] = cfg.armorVisibleEnabled;
    j["armorOccludedEnabled"] = cfg.armorOccludedEnabled;
    j["weaponEspEnabled"] = cfg.weaponEspEnabled;
    j["weaponVisibleEnabled"] = cfg.weaponVisibleEnabled;
    j["weaponOccludedEnabled"] = cfg.weaponOccludedEnabled;
    j["weaponTextEnabled"] = cfg.weaponTextEnabled;
    j["weaponIconEnabled"] = cfg.weaponIconEnabled;
    j["weaponEspMode"] = cfg.weaponEspMode;
    j["armorEspMode"] = cfg.armorEspMode;
    j["armorTextEnabled"] = cfg.armorTextEnabled;
    j["armorBarEnabled"] = cfg.armorBarEnabled;
    j["ammoEspEnabled"] = cfg.ammoEspEnabled;
    j["ammoVisibleEnabled"] = cfg.ammoVisibleEnabled;
    j["ammoOccludedEnabled"] = cfg.ammoOccludedEnabled;
    j["ammoTextEnabled"] = cfg.ammoTextEnabled;
    j["ammoBarEnabled"] = cfg.ammoBarEnabled;
    j["ammoEspMode"] = cfg.ammoEspMode;
    j["flagsEspEnabled"] = cfg.flagsEspEnabled;
    j["flagsVisibleEnabled"] = cfg.flagsVisibleEnabled;
    j["flagsOccludedEnabled"] = cfg.flagsOccludedEnabled;
    j["flagFlashedEnabled"] = cfg.flagFlashedEnabled;
    j["flagDefusingEnabled"] = cfg.flagDefusingEnabled;
    j["flagScopedEnabled"] = cfg.flagScopedEnabled;
    j["flagDefuseKitEnabled"] = cfg.flagDefuseKitEnabled;
    j["visibilityCheckEnabled"] = cfg.visibilityCheckEnabled;
    j["aimRequireVisibility"] = cfg.aimRequireVisibility;
    j["aimVisibilityMode"] = cfg.aimVisibilityMode;
    j["visibilityMode"] = cfg.visibilityMode;
    j["visibilityBackend"] = cfg.visibilityBackend;
    j["visibilityLatchFrames"] = cfg.visibilityLatchFrames;
    j["visMaxDistance"] = cfg.visMaxDistance;
    j["visBudgetMs"] = cfg.visBudgetMs;
    j["visEvalBase"] = cfg.visEvalBase;

    j["enemyColor"] = colorToJson(cfg.enemyColor);
    j["teamColor"] = colorToJson(cfg.teamColor);
    j["boxVisibleColor"] = colorToJson(cfg.boxVisibleColor);
    j["boxOccludedColor"] = colorToJson(cfg.boxOccludedColor);
    j["skeletonVisibleColor"] = colorToJson(cfg.skeletonVisibleColor);
    j["skeletonOccludedColor"] = colorToJson(cfg.skeletonOccludedColor);
    j["hpBarVisibleColor"] = colorToJson(cfg.hpBarVisibleColor);
    j["hpBarOccludedColor"] = colorToJson(cfg.hpBarOccludedColor);
    j["chamsVisibleColor"] = colorToJson(cfg.chamsVisibleColor);
    j["chamsOccludedColor"] = colorToJson(cfg.chamsOccludedColor);
    j["infoTextColor"] = colorToJson(cfg.infoTextColor);
    j["armorBarColor"] = colorToJson(cfg.armorBarColor);
    j["ammoBarColor"] = colorToJson(cfg.ammoBarColor);
    j["dormantColor"] = colorToJson(cfg.dormantColor);
    j["boxThickness"] = cfg.boxThickness;
    j["boxWidthScale"] = cfg.boxWidthScale;
    j["infoTextSize"] = cfg.infoTextSize;
    j["hpBarPosVisible"] = cfg.hpBarPosVisible;
    j["hpBarPosOccluded"] = cfg.hpBarPosOccluded;
    j["hpTextPosVisible"] = cfg.hpTextPosVisible;
    j["hpTextPosOccluded"] = cfg.hpTextPosOccluded;
    j["namePosVisible"] = cfg.namePosVisible;
    j["namePosOccluded"] = cfg.namePosOccluded;
    j["weaponPosVisible"] = cfg.weaponPosVisible;
    j["weaponPosOccluded"] = cfg.weaponPosOccluded;
    j["weaponTextPosVisible"] = cfg.weaponTextPosVisible;
    j["weaponTextPosOccluded"] = cfg.weaponTextPosOccluded;
    j["weaponIconPosVisible"] = cfg.weaponIconPosVisible;
    j["weaponIconPosOccluded"] = cfg.weaponIconPosOccluded;
    j["armorPosVisible"] = cfg.armorPosVisible;
    j["armorPosOccluded"] = cfg.armorPosOccluded;
    j["armorTextPosVisible"] = cfg.armorTextPosVisible;
    j["armorTextPosOccluded"] = cfg.armorTextPosOccluded;
    j["armorBarPosVisible"] = cfg.armorBarPosVisible;
    j["armorBarPosOccluded"] = cfg.armorBarPosOccluded;
    j["ammoPosVisible"] = cfg.ammoPosVisible;
    j["ammoPosOccluded"] = cfg.ammoPosOccluded;
    j["ammoTextPosVisible"] = cfg.ammoTextPosVisible;
    j["ammoTextPosOccluded"] = cfg.ammoTextPosOccluded;
    j["ammoBarPosVisible"] = cfg.ammoBarPosVisible;
    j["ammoBarPosOccluded"] = cfg.ammoBarPosOccluded;
    j["espItemOrderVisible"] = cfg.espItemOrderVisible;
    j["espItemOrderOccluded"] = cfg.espItemOrderOccluded;
    j["flagsPosVisible"] = cfg.flagsPosVisible;
    j["flagsPosOccluded"] = cfg.flagsPosOccluded;
    j["espAnchorOffsetX"] = cfg.espAnchorOffsetX;
    j["espAnchorOffsetY"] = cfg.espAnchorOffsetY;

    j["hpBarWidth"] = cfg.hpBarWidth;
    j["hpBarLowColor"] = colorToJson(cfg.hpBarLowColor);
    j["hpBarFullColor"] = colorToJson(cfg.hpBarFullColor);

    j["skeletonEnabled"] = cfg.skeletonEnabled;
    j["skeletonOccluded"] = cfg.skeletonOccluded;
    j["skeletonColor"] = colorToJson(cfg.skeletonColor);

    j["chamsEnabled"] = cfg.chamsEnabled;
    j["chamsOccluded"] = cfg.chamsOccluded;
    j["chamsStyle"] = cfg.chamsStyle;
    j["chamsAlpha"] = cfg.chamsAlpha;

    j["grenadeEnabled"] = cfg.grenadeEnabled;
    j["grenadeTrajectory"] = cfg.grenadeTrajectory;
    j["grenadeHelperEnabled"] = cfg.grenadeHelperEnabled;
    j["grenadeLineupPackPath"] = cfg.grenadeLineupPackPath;
    j["grenadeLineupCloudId"] = cfg.grenadeLineupCloudId;
    j["soundEspEnabled"] = cfg.soundEspEnabled;
    j["soundEspGunshots"] = cfg.soundEspGunshots;
    j["soundEspFootsteps"] = cfg.soundEspFootsteps;
    j["cloudConfigEnabled"] = cfg.cloudConfigEnabled;
    j["cloudActiveConfigId"] = cfg.cloudActiveConfigId;
    j["grenadeOffscreenInset"] = cfg.grenadeOffscreenInset;
    j["grenadeColors"] = json::array({
        colorToJson(cfg.grenadeColors[0]),
        colorToJson(cfg.grenadeColors[1]),
        colorToJson(cfg.grenadeColors[2]),
        colorToJson(cfg.grenadeColors[3]),
        colorToJson(cfg.grenadeColors[4]),
    });
    j["grenadePreThrowColor"] = colorToJson(cfg.grenadePreThrowColor);
    j["grenadeDangerColor"] = colorToJson(cfg.grenadeDangerColor);
    j["grenadeTrajectoryAlpha"] = cfg.grenadeTrajectoryAlpha;
    j["bombTimerEnabled"] = cfg.bombTimerEnabled;
    j["spectatorListEnabled"] = cfg.spectatorListEnabled;
    j["radarEnabled"] = cfg.radarEnabled;
    j["webRadarEnabled"] = cfg.webRadarEnabled;
    j["webRadarPublishMs"] = cfg.webRadarPublishMs;
    j["radarMode"] = cfg.radarMode;
    j["radarPosX"] = cfg.radarPosX;
    j["radarPosY"] = cfg.radarPosY;
    j["radarSize"] = cfg.radarSize;
    j["radarRange"] = cfg.radarRange;
    j["radarBlipSize"] = cfg.radarBlipSize;
    j["radarBgOpacity"] = cfg.radarBgOpacity;
    j["spectatorPosX"] = cfg.spectatorPosX;
    j["spectatorPosY"] = cfg.spectatorPosY;
    j["aaMode"] = cfg.aaMode;
    j["processPriority"] = cfg.processPriority;
    j["entityThreadPriority"] = cfg.entityThreadPriority;
    j["bgMode"] = cfg.bgMode;

    j["triggerbotEnabled"] = cfg.triggerbotEnabled;
    j["triggerbotDelayMs"] = cfg.triggerbotDelayMs;
    j["triggerbotKey"] = cfg.triggerbotKey;

    j["aimAssistEnabled"] = cfg.aimAssistEnabled;
    j["aimAssistKey"] = cfg.aimAssistKey;
    j["aimBone"] = cfg.aimBone;
    j["aimFov"] = cfg.aimFov;
    j["aimSmooth"] = cfg.aimSmooth;
    j["aimBoneOffsetZ"] = cfg.aimBoneOffsetZ;
    j["aimHeadForward"] = cfg.aimHeadForward;
    j["aimSensitivity"] = cfg.aimSensitivity;

    j["aimHumanizeEnabled"] = cfg.aimHumanizeEnabled;
    j["aimHumanizeMode"] = cfg.aimHumanizeMode;
    j["aimAssistStyle"] = cfg.aimAssistStyle;
    j["aimSupportStrength"] = cfg.aimSupportStrength;
    j["aimDebugConsole"] = cfg.aimDebugConsole;
    j["aimSupportAlwaysOn"] = cfg.aimSupportAlwaysOn;
    j["aimHumanizeUseProfile"] = cfg.aimHumanizeUseProfile;
    j["aimHumanizeStrength"] = cfg.aimHumanizeStrength;
    j["aimCalibAssistStrength"] = cfg.aimCalibAssistStrength;
    j["aimHumanizeReactionMs"] = cfg.aimHumanizeReactionMs;
    j["aimHumanizeJitter"] = cfg.aimHumanizeJitter;
    j["aimHumanizeOvershoot"] = cfg.aimHumanizeOvershoot;
    j["aimHumanizeSmoothExtra"] = cfg.aimHumanizeSmoothExtra;
    j["aimHumanizeSpeedScale"] = cfg.aimHumanizeSpeedScale;
    j["aimHumanizeMicroPause"] = cfg.aimHumanizeMicroPause;
    j["aimCalibrationTargets"] = cfg.aimCalibrationTargets;
    j["aimCalibrationTargetsPerGroup"] = cfg.aimCalibrationTargetsPerGroup;
    j["calibrationFrameworkComplete"] = cfg.calibrationFrameworkComplete;
    j["calibrationPromptDismissed"] = cfg.calibrationPromptDismissed;
    j["aimStyleProfileValid"] = cfg.aimStyleProfileValid;
    j["aimStyleReactionMs"] = cfg.aimStyleReactionMs;
    j["aimStyleSmooth"] = cfg.aimStyleSmooth;
    j["aimStyleOvershoot"] = cfg.aimStyleOvershoot;
    j["aimStyleFlickSpeed"] = cfg.aimStyleFlickSpeed;
    j["aimStyleJitter"] = cfg.aimStyleJitter;
    j["aimStyleAccuracy"] = cfg.aimStyleAccuracy;
    j["aimStyleMeasuredReactionMs"] = cfg.aimStyleMeasuredReactionMs;
    j["aimStyleMeasuredSmooth"] = cfg.aimStyleMeasuredSmooth;
    j["aimStyleMeasuredOvershoot"] = cfg.aimStyleMeasuredOvershoot;
    j["aimStyleMeasuredFlickSpeed"] = cfg.aimStyleMeasuredFlickSpeed;
    j["aimStyleMeasuredJitter"] = cfg.aimStyleMeasuredJitter;
    j["showFpsWatermark"] = cfg.showFpsWatermark;

    json aimCalibGroups = json::array();
    for (std::size_t i = 0; i < kAimWeaponGroupCount; ++i)
        aimCalibGroups.push_back(aimGroupCalibToJson(cfg.aimCalibByGroup[i]));
    j["aimCalibByGroup"] = std::move(aimCalibGroups);

    json aimGroups = json::array();
    for (std::size_t i = 0; i < kAimWeaponGroupCount; ++i)
        aimGroups.push_back(aimGroupToJson(cfg.aimByWeaponGroup[i]));
    j["aimByWeaponGroup"] = std::move(aimGroups);

    j["menuPosX"] = cfg.menuPosX;
    j["menuPosY"] = cfg.menuPosY;
    j["bombTimerPosX"] = cfg.bombTimerPosX;
    j["bombTimerPosY"] = cfg.bombTimerPosY;

    return j;
}

static void applyJsonToConfig(const json& j, OverlayConfig& cfg) {
    auto setBool = [&](const char* k, bool& v) { if (j.contains(k)) v = j[k].get<bool>(); };
    auto setInt = [&](const char* k, int& v) { if (j.contains(k)) v = j[k].get<int>(); };
    auto setFloat = [&](const char* k, float& v) { if (j.contains(k)) v = j[k].get<float>(); };

    setBool("espEnabled", cfg.espEnabled);
    setBool("boxEnabled", cfg.boxEnabled);
    setBool("boxOccluded", cfg.boxOccluded);
    setBool("hpBarEnabled", cfg.hpBarEnabled);
    setBool("hpBarOccluded", cfg.hpBarOccluded);
    setBool("hpTextEnabled", cfg.hpTextEnabled);
    setBool("hpTextVisibleEnabled", cfg.hpTextVisibleEnabled);
    setBool("hpTextOccludedEnabled", cfg.hpTextOccludedEnabled);
    setBool("enemyOnly", cfg.enemyOnly);
    setBool("nameEspEnabled", cfg.nameEspEnabled);
    setBool("nameEspAvatarEnabled", cfg.nameEspAvatarEnabled);
    setBool("armorEspEnabled", cfg.armorEspEnabled);
    setBool("armorVisibleEnabled", cfg.armorVisibleEnabled);
    setBool("armorOccludedEnabled", cfg.armorOccludedEnabled);
    setBool("weaponEspEnabled", cfg.weaponEspEnabled);
    setBool("weaponVisibleEnabled", cfg.weaponVisibleEnabled);
    setBool("weaponOccludedEnabled", cfg.weaponOccludedEnabled);
    setBool("weaponTextEnabled", cfg.weaponTextEnabled);
    setBool("weaponIconEnabled", cfg.weaponIconEnabled);
    setInt("weaponEspMode", cfg.weaponEspMode);
    setInt("armorEspMode", cfg.armorEspMode);
    setBool("armorTextEnabled", cfg.armorTextEnabled);
    setBool("armorBarEnabled", cfg.armorBarEnabled);
    setBool("ammoEspEnabled", cfg.ammoEspEnabled);
    setBool("ammoVisibleEnabled", cfg.ammoVisibleEnabled);
    setBool("ammoOccludedEnabled", cfg.ammoOccludedEnabled);
    setBool("ammoTextEnabled", cfg.ammoTextEnabled);
    setBool("ammoBarEnabled", cfg.ammoBarEnabled);
    setInt("ammoEspMode", cfg.ammoEspMode);
    setBool("flagsEspEnabled", cfg.flagsEspEnabled);
    setBool("flagsVisibleEnabled", cfg.flagsVisibleEnabled);
    setBool("flagsOccludedEnabled", cfg.flagsOccludedEnabled);
    setBool("flagFlashedEnabled", cfg.flagFlashedEnabled);
    setBool("flagDefusingEnabled", cfg.flagDefusingEnabled);
    setBool("flagScopedEnabled", cfg.flagScopedEnabled);
    setBool("flagDefuseKitEnabled", cfg.flagDefuseKitEnabled);
    setBool("visibilityCheckEnabled", cfg.visibilityCheckEnabled);
    setBool("aimRequireVisibility", cfg.aimRequireVisibility);
    setInt("aimVisibilityMode", cfg.aimVisibilityMode);
    setInt("visibilityMode", cfg.visibilityMode);
    setInt("visibilityBackend", cfg.visibilityBackend);
    setInt("visibilityLatchFrames", cfg.visibilityLatchFrames);
    cfg.visibilityBackend = std::clamp(cfg.visibilityBackend, 0, 2);
    cfg.weaponEspMode = std::clamp(cfg.weaponEspMode, 0, 1);
    cfg.armorEspMode = std::clamp(cfg.armorEspMode, 0, 1);
    cfg.ammoEspMode = std::clamp(cfg.ammoEspMode, 0, 1);
    if (!j.contains("weaponTextEnabled") && !j.contains("weaponIconEnabled")) {
        cfg.weaponTextEnabled = (cfg.weaponEspMode == 0);
        cfg.weaponIconEnabled = (cfg.weaponEspMode == 1);
    }
    if (!j.contains("armorTextEnabled") && !j.contains("armorBarEnabled")) {
        cfg.armorTextEnabled = (cfg.armorEspMode == 0);
        cfg.armorBarEnabled = (cfg.armorEspMode == 1);
    }
    if (!j.contains("ammoTextEnabled") && !j.contains("ammoBarEnabled")) {
        cfg.ammoTextEnabled = (cfg.ammoEspMode == 0);
        cfg.ammoBarEnabled = (cfg.ammoEspMode == 1);
    }
    if (!cfg.weaponTextEnabled && !cfg.weaponIconEnabled)
        cfg.weaponTextEnabled = true;
    if (!cfg.armorTextEnabled && !cfg.armorBarEnabled)
        cfg.armorTextEnabled = true;
    if (!cfg.ammoTextEnabled && !cfg.ammoBarEnabled)
        cfg.ammoTextEnabled = true;
    cfg.aimVisibilityMode = std::clamp(cfg.aimVisibilityMode, 0, 1);
    cfg.visibilityMode = std::clamp(cfg.visibilityMode, 0, 2);
    cfg.visibilityLatchFrames = std::clamp(cfg.visibilityLatchFrames, 0, 5);
    setFloat("visMaxDistance", cfg.visMaxDistance);
    setInt("visBudgetMs", cfg.visBudgetMs);
    setInt("visEvalBase", cfg.visEvalBase);
    cfg.visMaxDistance = (std::max)(0.f, cfg.visMaxDistance);
    cfg.visBudgetMs = std::clamp(cfg.visBudgetMs, 3, 30);
    cfg.visEvalBase = std::clamp(cfg.visEvalBase, 0, 5);

    // Backward compat: old configs had single enemyColor/teamColor → map to visible
    if (j.contains("enemyColor") && !j.contains("enemyVisibleColor"))
        jsonToColor(j["enemyColor"], cfg.enemyVisibleColor);
    if (j.contains("teamColor") && !j.contains("teamVisibleColor"))
        jsonToColor(j["teamColor"], cfg.teamVisibleColor);
    if (j.contains("enemyColor")) jsonToColor(j["enemyColor"], cfg.enemyColor);
    if (j.contains("teamColor")) jsonToColor(j["teamColor"], cfg.teamColor);
    if (j.contains("boxVisibleColor")) {
        jsonToColor(j["boxVisibleColor"], cfg.boxVisibleColor);
    } else if (j.contains("enemyVisibleColor")) {
        jsonToColor(j["enemyVisibleColor"], cfg.boxVisibleColor);
    } else if (j.contains("enemyColor")) {
        jsonToColor(j["enemyColor"], cfg.boxVisibleColor);
    }
    if (j.contains("boxOccludedColor")) {
        jsonToColor(j["boxOccludedColor"], cfg.boxOccludedColor);
    } else if (j.contains("enemyOccludedColor")) {
        jsonToColor(j["enemyOccludedColor"], cfg.boxOccludedColor);
    } else {
        std::memcpy(cfg.boxOccludedColor, cfg.boxVisibleColor, sizeof(cfg.boxVisibleColor));
        cfg.boxOccludedColor[3] = (std::min)(cfg.boxOccludedColor[3], 0.45f);
    }

    if (j.contains("skeletonVisibleColor")) {
        jsonToColor(j["skeletonVisibleColor"], cfg.skeletonVisibleColor);
    } else if (j.contains("skeletonColor")) {
        jsonToColor(j["skeletonColor"], cfg.skeletonVisibleColor);
    }
    if (j.contains("skeletonOccludedColor")) {
        jsonToColor(j["skeletonOccludedColor"], cfg.skeletonOccludedColor);
    } else {
        std::memcpy(cfg.skeletonOccludedColor, cfg.skeletonVisibleColor, sizeof(cfg.skeletonVisibleColor));
        cfg.skeletonOccludedColor[3] = (std::min)(cfg.skeletonOccludedColor[3], 0.45f);
    }

    if (j.contains("hpBarVisibleColor")) jsonToColor(j["hpBarVisibleColor"], cfg.hpBarVisibleColor);
    if (j.contains("hpBarOccludedColor")) {
        jsonToColor(j["hpBarOccludedColor"], cfg.hpBarOccludedColor);
    } else {
        std::memcpy(cfg.hpBarOccludedColor, cfg.hpBarVisibleColor, sizeof(cfg.hpBarVisibleColor));
        cfg.hpBarOccludedColor[3] = (std::min)(cfg.hpBarOccludedColor[3], 0.55f);
    }

    if (j.contains("chamsVisibleColor")) {
        jsonToColor(j["chamsVisibleColor"], cfg.chamsVisibleColor);
    } else if (j.contains("enemyVisibleColor")) {
        jsonToColor(j["enemyVisibleColor"], cfg.chamsVisibleColor);
    } else if (j.contains("enemyColor")) {
        jsonToColor(j["enemyColor"], cfg.chamsVisibleColor);
    }
    if (j.contains("chamsOccludedColor")) {
        jsonToColor(j["chamsOccludedColor"], cfg.chamsOccludedColor);
    } else if (j.contains("enemyOccludedColor")) {
        jsonToColor(j["enemyOccludedColor"], cfg.chamsOccludedColor);
    } else {
        std::memcpy(cfg.chamsOccludedColor, cfg.chamsVisibleColor, sizeof(cfg.chamsVisibleColor));
        cfg.chamsOccludedColor[3] = (std::min)(cfg.chamsOccludedColor[3], 0.55f);
    }
    if (j.contains("infoTextColor")) jsonToColor(j["infoTextColor"], cfg.infoTextColor);
    if (j.contains("armorBarColor")) jsonToColor(j["armorBarColor"], cfg.armorBarColor);
    if (j.contains("ammoBarColor")) jsonToColor(j["ammoBarColor"], cfg.ammoBarColor);
    if (j.contains("dormantColor")) jsonToColor(j["dormantColor"], cfg.dormantColor);
    setFloat("boxThickness", cfg.boxThickness);
    setFloat("boxWidthScale", cfg.boxWidthScale);
    setFloat("infoTextSize", cfg.infoTextSize);
    setInt("hpBarPosVisible", cfg.hpBarPosVisible);
    setInt("hpBarPosOccluded", cfg.hpBarPosOccluded);
    setInt("hpTextPosVisible", cfg.hpTextPosVisible);
    setInt("hpTextPosOccluded", cfg.hpTextPosOccluded);
    setInt("namePosVisible", cfg.namePosVisible);
    setInt("namePosOccluded", cfg.namePosOccluded);
    setInt("weaponPosVisible", cfg.weaponPosVisible);
    setInt("weaponPosOccluded", cfg.weaponPosOccluded);
    setInt("weaponTextPosVisible", cfg.weaponTextPosVisible);
    setInt("weaponTextPosOccluded", cfg.weaponTextPosOccluded);
    setInt("weaponIconPosVisible", cfg.weaponIconPosVisible);
    setInt("weaponIconPosOccluded", cfg.weaponIconPosOccluded);
    setInt("armorPosVisible", cfg.armorPosVisible);
    setInt("armorPosOccluded", cfg.armorPosOccluded);
    setInt("armorTextPosVisible", cfg.armorTextPosVisible);
    setInt("armorTextPosOccluded", cfg.armorTextPosOccluded);
    setInt("armorBarPosVisible", cfg.armorBarPosVisible);
    setInt("armorBarPosOccluded", cfg.armorBarPosOccluded);
    setInt("ammoPosVisible", cfg.ammoPosVisible);
    setInt("ammoPosOccluded", cfg.ammoPosOccluded);
    setInt("ammoTextPosVisible", cfg.ammoTextPosVisible);
    setInt("ammoTextPosOccluded", cfg.ammoTextPosOccluded);
    setInt("ammoBarPosVisible", cfg.ammoBarPosVisible);
    setInt("ammoBarPosOccluded", cfg.ammoBarPosOccluded);
    setInt("flagsPosVisible", cfg.flagsPosVisible);
    setInt("flagsPosOccluded", cfg.flagsPosOccluded);

    if (j.contains("espAnchorOffsetX") && j["espAnchorOffsetX"].is_array()) {
        const auto& arr = j["espAnchorOffsetX"];
        for (int i = 0; i < 8 && i < static_cast<int>(arr.size()); ++i)
            cfg.espAnchorOffsetX[i] = arr[i].get<float>();
    }
    if (j.contains("espAnchorOffsetY") && j["espAnchorOffsetY"].is_array()) {
        const auto& arr = j["espAnchorOffsetY"];
        for (int i = 0; i < 8 && i < static_cast<int>(arr.size()); ++i)
            cfg.espAnchorOffsetY[i] = arr[i].get<float>();
    }
    for (int i = 0; i < 8; ++i) {
        cfg.espAnchorOffsetX[i] = std::clamp(cfg.espAnchorOffsetX[i], -60.f, 60.f);
        cfg.espAnchorOffsetY[i] = std::clamp(cfg.espAnchorOffsetY[i], -60.f, 60.f);
    }

    if (!j.contains("hpTextPosVisible")) cfg.hpTextPosVisible = cfg.hpBarPosVisible;
    if (!j.contains("hpTextPosOccluded")) cfg.hpTextPosOccluded = cfg.hpBarPosOccluded;
    if (!j.contains("weaponTextPosVisible")) cfg.weaponTextPosVisible = cfg.weaponPosVisible;
    if (!j.contains("weaponTextPosOccluded")) cfg.weaponTextPosOccluded = cfg.weaponPosOccluded;
    if (!j.contains("weaponIconPosVisible")) cfg.weaponIconPosVisible = cfg.weaponPosVisible;
    if (!j.contains("weaponIconPosOccluded")) cfg.weaponIconPosOccluded = cfg.weaponPosOccluded;
    if (!j.contains("armorTextPosVisible")) cfg.armorTextPosVisible = cfg.armorPosVisible;
    if (!j.contains("armorTextPosOccluded")) cfg.armorTextPosOccluded = cfg.armorPosOccluded;
    if (!j.contains("armorBarPosVisible")) cfg.armorBarPosVisible = cfg.armorPosVisible;
    if (!j.contains("armorBarPosOccluded")) cfg.armorBarPosOccluded = cfg.armorPosOccluded;
    if (!j.contains("ammoTextPosVisible")) cfg.ammoTextPosVisible = cfg.ammoPosVisible;
    if (!j.contains("ammoTextPosOccluded")) cfg.ammoTextPosOccluded = cfg.ammoPosOccluded;
    if (!j.contains("ammoBarPosVisible")) cfg.ammoBarPosVisible = cfg.ammoPosVisible;
    if (!j.contains("ammoBarPosOccluded")) cfg.ammoBarPosOccluded = cfg.ammoPosOccluded;
    cfg.infoTextSize = std::clamp(cfg.infoTextSize, 10.f, 24.f);
    cfg.hpBarPosVisible = clampEspBarAnchorStored(cfg.hpBarPosVisible);
    cfg.hpBarPosOccluded = clampEspBarAnchorStored(cfg.hpBarPosOccluded);
    cfg.hpTextPosVisible = clampEspInfoAnchorStored(cfg.hpTextPosVisible);
    cfg.hpTextPosOccluded = clampEspInfoAnchorStored(cfg.hpTextPosOccluded);
    cfg.namePosVisible = clampEspInfoAnchorStored(cfg.namePosVisible);
    cfg.namePosOccluded = clampEspInfoAnchorStored(cfg.namePosOccluded);
    cfg.weaponPosVisible = clampEspInfoAnchorStored(cfg.weaponPosVisible);
    cfg.weaponPosOccluded = clampEspInfoAnchorStored(cfg.weaponPosOccluded);
    cfg.weaponTextPosVisible = clampEspInfoAnchorStored(cfg.weaponTextPosVisible);
    cfg.weaponTextPosOccluded = clampEspInfoAnchorStored(cfg.weaponTextPosOccluded);
    cfg.weaponIconPosVisible = clampEspInfoAnchorStored(cfg.weaponIconPosVisible);
    cfg.weaponIconPosOccluded = clampEspInfoAnchorStored(cfg.weaponIconPosOccluded);
    cfg.armorPosVisible = clampEspInfoAnchorStored(cfg.armorPosVisible);
    cfg.armorPosOccluded = clampEspInfoAnchorStored(cfg.armorPosOccluded);
    cfg.armorTextPosVisible = clampEspInfoAnchorStored(cfg.armorTextPosVisible);
    cfg.armorTextPosOccluded = clampEspInfoAnchorStored(cfg.armorTextPosOccluded);
    cfg.armorBarPosVisible = clampEspBarAnchorStored(cfg.armorBarPosVisible);
    cfg.armorBarPosOccluded = clampEspBarAnchorStored(cfg.armorBarPosOccluded);
    cfg.ammoPosVisible = clampEspInfoAnchorStored(cfg.ammoPosVisible);
    cfg.ammoPosOccluded = clampEspInfoAnchorStored(cfg.ammoPosOccluded);
    cfg.ammoTextPosVisible = clampEspInfoAnchorStored(cfg.ammoTextPosVisible);
    cfg.ammoTextPosOccluded = clampEspInfoAnchorStored(cfg.ammoTextPosOccluded);
    cfg.ammoBarPosVisible = clampEspBarAnchorStored(cfg.ammoBarPosVisible);
    cfg.ammoBarPosOccluded = clampEspBarAnchorStored(cfg.ammoBarPosOccluded);
    cfg.flagsPosVisible = clampEspInfoAnchorStored(cfg.flagsPosVisible);
    cfg.flagsPosOccluded = clampEspInfoAnchorStored(cfg.flagsPosOccluded);
    if (j.contains("espItemOrderVisible")) { for (int i=0; i<10; ++i) cfg.espItemOrderVisible[i] = j["espItemOrderVisible"][i]; }
    if (j.contains("espItemOrderOccluded")) { for (int i=0; i<10; ++i) cfg.espItemOrderOccluded[i] = j["espItemOrderOccluded"][i]; }

    setFloat("hpBarWidth", cfg.hpBarWidth);
    if (j.contains("hpBarLowColor")) jsonToColor(j["hpBarLowColor"], cfg.hpBarLowColor);
    if (j.contains("hpBarFullColor")) jsonToColor(j["hpBarFullColor"], cfg.hpBarFullColor);

    setBool("skeletonEnabled", cfg.skeletonEnabled);
    setBool("skeletonOccluded", cfg.skeletonOccluded);
    if (j.contains("skeletonColor")) jsonToColor(j["skeletonColor"], cfg.skeletonColor);

    setBool("chamsEnabled", cfg.chamsEnabled);
    setBool("chamsOccluded", cfg.chamsOccluded);
    setInt("chamsStyle", cfg.chamsStyle);
    setFloat("chamsAlpha", cfg.chamsAlpha);

    setBool("grenadeEnabled", cfg.grenadeEnabled);
    setBool("grenadeTrajectory", cfg.grenadeTrajectory);
    setBool("grenadeHelperEnabled", cfg.grenadeHelperEnabled);
    if (j.contains("grenadeLineupPackPath")) cfg.grenadeLineupPackPath = j["grenadeLineupPackPath"].get<std::string>();
    if (j.contains("grenadeLineupCloudId")) cfg.grenadeLineupCloudId = j["grenadeLineupCloudId"].get<std::string>();
    setBool("soundEspEnabled", cfg.soundEspEnabled);
    setBool("soundEspGunshots", cfg.soundEspGunshots);
    setBool("soundEspFootsteps", cfg.soundEspFootsteps);
    setBool("cloudConfigEnabled", cfg.cloudConfigEnabled);
    if (j.contains("cloudActiveConfigId")) cfg.cloudActiveConfigId = j["cloudActiveConfigId"].get<std::string>();
    setFloat("grenadeOffscreenInset", cfg.grenadeOffscreenInset);
    if (j.contains("grenadeColors") && j["grenadeColors"].is_array()) {
        for (int i = 0; i < 5 && i < static_cast<int>(j["grenadeColors"].size()); ++i)
            jsonToColor(j["grenadeColors"][i], cfg.grenadeColors[i]);
    }
    if (j.contains("grenadePreThrowColor")) jsonToColor(j["grenadePreThrowColor"], cfg.grenadePreThrowColor);
    if (j.contains("grenadeDangerColor")) jsonToColor(j["grenadeDangerColor"], cfg.grenadeDangerColor);
    setFloat("grenadeTrajectoryAlpha", cfg.grenadeTrajectoryAlpha);
    cfg.grenadeOffscreenInset = std::clamp(cfg.grenadeOffscreenInset, 0.f, 320.f);
    cfg.grenadeTrajectoryAlpha = std::clamp(cfg.grenadeTrajectoryAlpha, 0.05f, 1.f);
    setBool("bombTimerEnabled", cfg.bombTimerEnabled);
    setBool("spectatorListEnabled", cfg.spectatorListEnabled);
    setBool("radarEnabled", cfg.radarEnabled);
    setBool("webRadarEnabled", cfg.webRadarEnabled);
    setInt("webRadarPublishMs", cfg.webRadarPublishMs);
    setInt("radarMode", cfg.radarMode);
    setFloat("radarPosX", cfg.radarPosX);
    setFloat("radarPosY", cfg.radarPosY);
    setFloat("radarSize", cfg.radarSize);
    setFloat("radarRange", cfg.radarRange);
    setFloat("radarBlipSize", cfg.radarBlipSize);
    setFloat("radarBgOpacity", cfg.radarBgOpacity);
    setFloat("spectatorPosX", cfg.spectatorPosX);
    setFloat("spectatorPosY", cfg.spectatorPosY);
    cfg.radarMode = std::clamp(cfg.radarMode, 0, 1);
    cfg.radarPosX = std::clamp(cfg.radarPosX, 0.f, 1.f);
    cfg.radarPosY = std::clamp(cfg.radarPosY, 0.f, 1.f);
    cfg.radarSize = std::clamp(cfg.radarSize, 120.f, 420.f);
    cfg.radarRange = std::clamp(cfg.radarRange, 400.f, 5000.f);
    cfg.radarBlipSize = std::clamp(cfg.radarBlipSize, 2.f, 8.f);
    cfg.radarBgOpacity = std::clamp(cfg.radarBgOpacity, 0.f, 1.f);
    cfg.webRadarPublishMs = std::clamp(cfg.webRadarPublishMs, 1, 2000);
    if (cfg.spectatorPosX < -1.f) cfg.spectatorPosX = -1.f;
    if (cfg.spectatorPosY < -1.f) cfg.spectatorPosY = -1.f;
    if (cfg.spectatorPosX > 1.f) cfg.spectatorPosX = 1.f;
    if (cfg.spectatorPosY > 1.f) cfg.spectatorPosY = 1.f;
    setInt("aaMode", cfg.aaMode);
    if (cfg.aaMode < 0) cfg.aaMode = 0;
    if (cfg.aaMode > 5) cfg.aaMode = 5;
    setInt("processPriority", cfg.processPriority);
    setInt("entityThreadPriority", cfg.entityThreadPriority);
    setInt("bgMode", cfg.bgMode);
    cfg.processPriority = std::clamp(cfg.processPriority, 0, 2);
    cfg.entityThreadPriority = std::clamp(cfg.entityThreadPriority, 0, 3);
    cfg.bgMode = std::clamp(cfg.bgMode, 0, 2);

    setBool("triggerbotEnabled", cfg.triggerbotEnabled);
    setInt("triggerbotDelayMs", cfg.triggerbotDelayMs);
    setInt("triggerbotKey", cfg.triggerbotKey);

    setBool("aimAssistEnabled", cfg.aimAssistEnabled);
    setInt("aimAssistKey", cfg.aimAssistKey);
    setInt("aimBone", cfg.aimBone);
    setFloat("aimFov", cfg.aimFov);
    setFloat("aimSmooth", cfg.aimSmooth);
    cfg.aimSmooth = std::clamp(cfg.aimSmooth, 1.f, 20.f);
    setFloat("aimBoneOffsetZ", cfg.aimBoneOffsetZ);
    setFloat("aimHeadForward", cfg.aimHeadForward);
    setFloat("aimSensitivity", cfg.aimSensitivity);

    setBool("aimHumanizeEnabled", cfg.aimHumanizeEnabled);
    setInt("aimHumanizeMode", cfg.aimHumanizeMode);
    setInt("aimAssistStyle", cfg.aimAssistStyle);
    if (cfg.aimAssistStyle < 0) cfg.aimAssistStyle = 0;
    if (cfg.aimAssistStyle > 1) cfg.aimAssistStyle = 1;
    setFloat("aimSupportStrength", cfg.aimSupportStrength);
    cfg.aimSupportStrength = std::clamp(cfg.aimSupportStrength, 0.f, 1.f);
    setBool("aimDebugConsole", cfg.aimDebugConsole);
    setBool("aimSupportAlwaysOn", cfg.aimSupportAlwaysOn);
    if (cfg.aimHumanizeMode < 0) cfg.aimHumanizeMode = 0;
    if (cfg.aimHumanizeMode > 1) cfg.aimHumanizeMode = 1;
    cfg.aimHumanizeUseProfile = (cfg.aimHumanizeMode == 0);
    setBool("aimHumanizeUseProfile", cfg.aimHumanizeUseProfile);
    setFloat("aimHumanizeStrength", cfg.aimHumanizeStrength);
    setFloat("aimCalibAssistStrength", cfg.aimCalibAssistStrength);
    setFloat("aimHumanizeReactionMs", cfg.aimHumanizeReactionMs);
    setFloat("aimHumanizeJitter", cfg.aimHumanizeJitter);
    setFloat("aimHumanizeOvershoot", cfg.aimHumanizeOvershoot);
    setFloat("aimHumanizeSmoothExtra", cfg.aimHumanizeSmoothExtra);
    setFloat("aimHumanizeSpeedScale", cfg.aimHumanizeSpeedScale);
    setBool("aimHumanizeMicroPause", cfg.aimHumanizeMicroPause);
    setInt("aimCalibrationTargets", cfg.aimCalibrationTargets);
    setInt("aimCalibrationTargetsPerGroup", cfg.aimCalibrationTargetsPerGroup);
    setBool("calibrationFrameworkComplete", cfg.calibrationFrameworkComplete);
    setBool("calibrationPromptDismissed", cfg.calibrationPromptDismissed);
    setBool("aimStyleProfileValid", cfg.aimStyleProfileValid);
    setFloat("aimStyleReactionMs", cfg.aimStyleReactionMs);
    setFloat("aimStyleSmooth", cfg.aimStyleSmooth);
    setFloat("aimStyleOvershoot", cfg.aimStyleOvershoot);
    setFloat("aimStyleFlickSpeed", cfg.aimStyleFlickSpeed);
    setFloat("aimStyleJitter", cfg.aimStyleJitter);
    setFloat("aimStyleAccuracy", cfg.aimStyleAccuracy);
    setFloat("aimStyleMeasuredReactionMs", cfg.aimStyleMeasuredReactionMs);
    setFloat("aimStyleMeasuredSmooth", cfg.aimStyleMeasuredSmooth);
    setFloat("aimStyleMeasuredOvershoot", cfg.aimStyleMeasuredOvershoot);
    setFloat("aimStyleMeasuredFlickSpeed", cfg.aimStyleMeasuredFlickSpeed);
    setFloat("aimStyleMeasuredJitter", cfg.aimStyleMeasuredJitter);
    setBool("showFpsWatermark", cfg.showFpsWatermark);

    if (j.contains("aimCalibByGroup") && j["aimCalibByGroup"].is_array()) {
        const auto& calib = j["aimCalibByGroup"];
        const std::size_t count = (std::min)(calib.size(), kAimWeaponGroupCount);
        for (std::size_t i = 0; i < count; ++i)
            jsonToAimGroupCalib(calib[i], cfg.aimCalibByGroup[i]);
    }

    bool loadedAimGroups = false;
    if (j.contains("aimByWeaponGroup") && j["aimByWeaponGroup"].is_array()) {
        const auto& groups = j["aimByWeaponGroup"];
        std::size_t count = (std::min)(groups.size(), kAimWeaponGroupCount);
        for (std::size_t i = 0; i < count; ++i)
            jsonToAimGroup(groups[i], cfg.aimByWeaponGroup[i]);
        loadedAimGroups = count == kAimWeaponGroupCount;
    }
    if (!loadedAimGroups) {
        AimGroupConfig legacy;
        legacy.aimBone = cfg.aimBone;
        legacy.hitboxHead = (cfg.aimBone == 6);
        legacy.hitboxStomach = (cfg.aimBone == 2 || cfg.aimBone == 3);
        legacy.hitboxChest = (cfg.aimBone == 4);
        legacy.hitboxPelvis = false;
        legacy.hitboxArms = false;
        legacy.hitboxLegs = false;
        if (!legacy.hitboxHead && !legacy.hitboxStomach && !legacy.hitboxChest
            && !legacy.hitboxPelvis && !legacy.hitboxArms && !legacy.hitboxLegs)
            legacy.hitboxHead = true;
        legacy.aimFov = cfg.aimFov;
        legacy.aimSmooth = cfg.aimSmooth;
        legacy.rcsMode = 1;
        legacy.rcsX = 1.0f;
        legacy.rcsY = 1.0f;
        legacy.rcsSmooth = 6.0f;
        legacy.triggerEnabled = cfg.triggerbotEnabled;
        legacy.triggerDelayMs = cfg.triggerbotDelayMs;
        legacy.triggerKey = cfg.triggerbotKey;
        for (std::size_t i = 0; i < kAimWeaponGroupCount; ++i) {
            cfg.aimByWeaponGroup[i] = legacy;
            cfg.aimByWeaponGroup[i].rcsEnabled =
                weaponGroupSupportsRcs(static_cast<AimWeaponGroup>(i));
        }
    }

    setFloat("menuPosX", cfg.menuPosX);
    setFloat("menuPosY", cfg.menuPosY);
    setFloat("bombTimerPosX", cfg.bombTimerPosX);
    setFloat("bombTimerPosY", cfg.bombTimerPosY);
}

static bool writeConfigFile(const std::filesystem::path& path, const OverlayConfig& cfg) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out << configToJson(cfg).dump(2);
    return out.good();
}

static bool readConfigFile(const std::filesystem::path& path, OverlayConfig& cfg) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    json j;
    in >> j;
    applyJsonToConfig(j, cfg);
    syncAimStyleFromConfig();
    return true;
}

static std::filesystem::path calibDirPath() {
    return cfgDirPath() / "calibration";
}

static std::filesystem::path defaultCalibrationPath() {
    return calibDirPath() / "aim_calibration.json";
}

static json calibrationToJson(const OverlayConfig& cfg) {
    json j;
    j["version"] = 1;
    j["aimCalibAssistStrength"] = cfg.aimCalibAssistStrength;
    j["aimCalibrationTargetsPerGroup"] = cfg.aimCalibrationTargetsPerGroup;
    j["calibrationFrameworkComplete"] = cfg.calibrationFrameworkComplete;
    j["calibrationPromptDismissed"] = cfg.calibrationPromptDismissed;
    json aimCalibGroups = json::array();
    for (std::size_t i = 0; i < kAimWeaponGroupCount; ++i)
        aimCalibGroups.push_back(aimGroupCalibToJson(cfg.aimCalibByGroup[i]));
    j["aimCalibByGroup"] = std::move(aimCalibGroups);
    return j;
}

static void applyCalibrationJson(const json& j, OverlayConfig& cfg) {
    auto setBool = [&](const char* k, bool& v) { if (j.contains(k)) v = j[k].get<bool>(); };
    auto setInt = [&](const char* k, int& v) { if (j.contains(k)) v = j[k].get<int>(); };
    auto setFloat = [&](const char* k, float& v) { if (j.contains(k)) v = j[k].get<float>(); };

    setFloat("aimCalibAssistStrength", cfg.aimCalibAssistStrength);
    setInt("aimCalibrationTargetsPerGroup", cfg.aimCalibrationTargetsPerGroup);
    cfg.aimCalibrationTargets = cfg.aimCalibrationTargetsPerGroup;
    setBool("calibrationFrameworkComplete", cfg.calibrationFrameworkComplete);
    setBool("calibrationPromptDismissed", cfg.calibrationPromptDismissed);
    if (j.contains("aimCalibByGroup") && j["aimCalibByGroup"].is_array()) {
        const auto& calib = j["aimCalibByGroup"];
        const std::size_t n = (std::min)(calib.size(), kAimWeaponGroupCount);
        for (std::size_t i = 0; i < n; ++i)
            jsonToAimGroupCalib(calib[i], cfg.aimCalibByGroup[i]);
    }
}

static bool writeCalibrationFile(const std::filesystem::path& path, const OverlayConfig& cfg) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out << calibrationToJson(cfg).dump(2);
    return out.good();
}

static bool readCalibrationFile(const std::filesystem::path& path, OverlayConfig& cfg) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    json j;
    in >> j;
    applyCalibrationJson(j, cfg);
    syncAimStyleFromConfig();
    return true;
}

static std::string makeDefaultCalibrationName() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "calib_%04d%02d%02d_%02d%02d%02d.json",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string(buf);
}

static std::vector<std::string> listConfigFiles() {
    std::vector<std::string> names;
    std::filesystem::create_directories(cfgDirPath());
    for (const auto& e : std::filesystem::directory_iterator(cfgDirPath())) {
        if (!e.is_regular_file()) continue;
        if (e.path().extension() != ".json") continue;
        names.push_back(e.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

static std::string makeDefaultConfigName() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "cfg_%04d%02d%02d_%02d%02d%02d.json",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string(buf);
}

static std::filesystem::path quickConfigPath() {
    return cfgDirPath() / "quick_save.json";
}

static void drawBrandMark(Renderer& r, float x, float y, float scale) {
    float size = 34.f * scale;
    float cx = x + size * 0.5f;
    float cy = y + size * 0.5f;

    float outer[18] = {
        cx, y + size * 0.02f,
        x + size * 0.82f, y + size * 0.16f,
        x + size * 0.98f, cy,
        x + size * 0.82f, y + size * 0.84f,
        cx, y + size * 0.98f,
        x + size * 0.18f, y + size * 0.84f,
        x + size * 0.02f, cy,
        x + size * 0.18f, y + size * 0.16f,
        cx, y + size * 0.02f,
    };
    r.drawFilledConvexPolygon(outer, 9, 0xFF4B1E7A);

    float ring[18] = {
        cx, y + size * 0.08f,
        x + size * 0.74f, y + size * 0.20f,
        x + size * 0.90f, cy,
        x + size * 0.74f, y + size * 0.80f,
        cx, y + size * 0.90f,
        x + size * 0.26f, y + size * 0.80f,
        x + size * 0.10f, cy,
        x + size * 0.26f, y + size * 0.20f,
        cx, y + size * 0.08f,
    };
    r.drawFilledConvexPolygon(ring, 9, 0xFF6A2EA6);

    float face[12] = {
        cx, y + size * 0.22f,
        x + size * 0.57f, cy,
        cx, y + size * 0.78f,
        x + size * 0.43f, cy,
        cx, y + size * 0.22f,
        cx, y + size * 0.22f,
    };
    r.drawFilledConvexPolygon(face, 4, 0xFFF4F4F8);

    r.drawFilledCircle(x + size * 0.39f, y + size * 0.50f, size * 0.065f, 0xFF1C102E);
    r.drawFilledCircle(x + size * 0.61f, y + size * 0.50f, size * 0.065f, 0xFF1C102E);
    r.drawFilledRect(x + size * 0.48f, y + size * 0.58f, size * 0.04f, size * 0.18f, 0xFF1C102E);
    r.drawLine(x + size * 0.27f, y + size * 0.77f, x + size * 0.73f, y + size * 0.77f, 0xFF1C102E, 1.5f * scale);
}

static bool setClipboardText(const std::string& text) {
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();

    HGLOBAL hglb = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
    if (!hglb) { CloseClipboard(); return false; }
    void* p = GlobalLock(hglb);
    if (!p) { GlobalFree(hglb); CloseClipboard(); return false; }
    memcpy(p, text.c_str(), text.size() + 1);
    GlobalUnlock(hglb);

    if (!SetClipboardData(CF_TEXT, hglb)) {
        GlobalFree(hglb);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

static bool getClipboardText(std::string& out) {
    out.clear();
    if (!OpenClipboard(nullptr)) return false;
    HANDLE h = GetClipboardData(CF_TEXT);
    if (!h) { CloseClipboard(); return false; }
    const char* p = static_cast<const char*>(GlobalLock(h));
    if (!p) { CloseClipboard(); return false; }
    out = p;
    GlobalUnlock(h);
    CloseClipboard();
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// Init
// ═════════════════════════════════════════════════════════════════════════════

bool Menu::init(Renderer& renderer) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    m_tickFreq = f.QuadPart;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    m_lastTick = now.QuadPart;

    if (!m_font.init(renderer.device(), L"Segoe UI", 64)) {
        return false;
    }

    const std::filesystem::path modelAssetPath = findHitboxModelAssetPath();
    if (!modelAssetPath.empty()) {
        loadTextureFromWicFile(renderer.device(), modelAssetPath, m_hitboxModelSrv, m_hitboxModelW, m_hitboxModelH);
    }

    const std::filesystem::path espPreviewPath = findEspPreviewModelAssetPath();
    if (!espPreviewPath.empty()) {
        loadTextureFromWicFile(renderer.device(), espPreviewPath, m_espPreviewModelSrv, m_espPreviewModelW, m_espPreviewModelH);
    }

    const std::filesystem::path brandLogoPath = findBrandLogoAssetPath();
    if (!brandLogoPath.empty()) {
        loadTextureFromWicFile(renderer.device(), brandLogoPath, m_brandLogoSrv, m_brandLogoW, m_brandLogoH);
    }

    const std::filesystem::path leetifyBadgePath = findLeetifyBadgeAssetPath();
    if (!leetifyBadgePath.empty()) {
        loadTextureFromWicFile(renderer.device(), leetifyBadgePath,
            m_leetifyBadgeSrv, m_leetifyBadgeW, m_leetifyBadgeH);
    }

    const std::filesystem::path zoomInPath = findZoomInIconAssetPath();
    if (!zoomInPath.empty()) {
        loadTextureFromWicFile(renderer.device(), zoomInPath,
            m_zoomInIconSrv, m_zoomInIconW, m_zoomInIconH);
    }

    const std::filesystem::path avatarPlaceholderPath = findAvatarPlaceholderPath();
    if (!avatarPlaceholderPath.empty()) {
        loadTextureFromWicFile(renderer.device(), avatarPlaceholderPath,
                               m_userAvatarSrv, m_userAvatarW, m_userAvatarH);
    }

    static const char* kTabIconFiles[kTabN] = {
        "tab0.png", "tab1.png", "tab2.png", "tab2.png", "tab3.png", "tab4.png", "tab2.png", "tab5.png"
    };
    for (int i = 0; i < kTabN; ++i) {
        const std::filesystem::path iconPath = findTabIconAssetPath(kTabIconFiles[i]);
        if (!iconPath.empty()) {
            loadTabIconTexture(renderer.device(), iconPath,
                               m_tabIconSrv[i], m_tabIconW[i], m_tabIconH[i],
                               m_tabIconFullColor[i]);
        }
    }

    m_aimSubTabAnim[m_aimSubTab] = 1.f;
    m_playerSubTabAnim[m_playerSubTab] = 1.f;
    m_worldSubTabAnim[m_worldSubTab] = 1.f;

    // Preview-first workflow: start with no player ESP items placed or enabled.
    g_cfg.boxEnabled = false;
    g_cfg.boxOccluded = false;
    g_cfg.skeletonEnabled = false;
    g_cfg.skeletonOccluded = false;
    g_cfg.chamsEnabled = false;
    g_cfg.chamsOccluded = false;
    g_cfg.hpBarEnabled = false;
    g_cfg.hpBarOccluded = false;
    g_cfg.hpTextEnabled = false;
    g_cfg.hpTextVisibleEnabled = false;
    g_cfg.hpTextOccludedEnabled = false;
    g_cfg.nameEspEnabled = false;
    g_cfg.weaponEspEnabled = false;
    g_cfg.weaponVisibleEnabled = false;
    g_cfg.weaponOccludedEnabled = false;
    g_cfg.weaponTextEnabled = false;
    g_cfg.weaponIconEnabled = true;
    g_cfg.armorEspEnabled = false;
    g_cfg.armorVisibleEnabled = false;
    g_cfg.armorOccludedEnabled = false;
    g_cfg.armorTextEnabled = false;
    g_cfg.armorBarEnabled = false;
    g_cfg.ammoEspEnabled = false;
    g_cfg.ammoVisibleEnabled = false;
    g_cfg.ammoOccludedEnabled = false;
    g_cfg.ammoTextEnabled = false;
    g_cfg.ammoBarEnabled = false;
    g_cfg.flagsEspEnabled = false;
    g_cfg.flagsVisibleEnabled = false;
    g_cfg.flagsOccludedEnabled = false;

    g_cfg.hpBarPosVisible = -1;
    g_cfg.hpBarPosOccluded = -1;
    g_cfg.hpTextPosVisible = -1;
    g_cfg.hpTextPosOccluded = -1;
    g_cfg.namePosVisible = -1;
    g_cfg.namePosOccluded = -1;
    g_cfg.weaponPosVisible = -1;
    g_cfg.weaponPosOccluded = -1;
    g_cfg.weaponTextPosVisible = -1;
    g_cfg.weaponTextPosOccluded = -1;
    g_cfg.weaponIconPosVisible = 2;
    g_cfg.weaponIconPosOccluded = 2;
    g_cfg.armorPosVisible = -1;
    g_cfg.armorPosOccluded = -1;
    g_cfg.armorTextPosVisible = -1;
    g_cfg.armorTextPosOccluded = -1;
    g_cfg.armorBarPosVisible = -1;
    g_cfg.armorBarPosOccluded = -1;
    g_cfg.ammoPosVisible = -1;
    g_cfg.ammoPosOccluded = -1;
    g_cfg.ammoTextPosVisible = -1;
    g_cfg.ammoTextPosOccluded = -1;
    g_cfg.ammoBarPosVisible = -1;
    g_cfg.ammoBarPosOccluded = -1;
    g_cfg.flagsPosVisible = -1;
    g_cfg.flagsPosOccluded = -1;

    m_espPreviewResetDone = true;

    if (!g_cfg.grenadeLineupPackPath.empty()) {
        std::string err;
        GrenadeLineupManager::instance().loadPackFromFile(g_cfg.grenadeLineupPackPath, err);
    }

    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// Render
// ═════════════════════════════════════════════════════════════════════════════

void Menu::render(Renderer& renderer, const EntityManager& em, const Process& proc, HWND hwnd) {
    m_renderProc = &proc;
    m_renderEm = &em;
    PlayerScout::instance().tick(em);
    float dt = deltaS(m_lastTick, m_tickFreq);
    m_frameDt = dt;
    m_uiTime += dt;
    if (m_leetifyPromptStatusTimer > 0.f)
        m_leetifyPromptStatusTimer = (std::max)(0.f, m_leetifyPromptStatusTimer - dt);

    if (g_cfg.menuVisible && !m_prevMenuVisible && !m_aimFirstOpenInitialized) {
        m_aimSubTab = 0;
        m_aimFirstOpenInitialized = true;
    }
    m_prevMenuVisible = g_cfg.menuVisible;

    if (g_cfg.menuVisible)
        m_menuAlpha = (std::min)(m_menuAlpha + dt * 6.f, 1.f);
    else
        m_menuAlpha = (std::max)(m_menuAlpha - dt * 10.f, 0.f);

    if (m_menuAlpha < 0.01f) return;

    renderer.setImGuiDrawMode(true);

    ImGuiStyle& imguiStyle = ImGui::GetStyle();
    const bool oldAALines = imguiStyle.AntiAliasedLines;
    const bool oldAALinesTex = imguiStyle.AntiAliasedLinesUseTex;
    const bool oldAAFill = imguiStyle.AntiAliasedFill;
    const float oldCurveTol = imguiStyle.CurveTessellationTol;
    const float oldCircleErr = imguiStyle.CircleTessellationMaxError;
    imguiStyle.AntiAliasedLines = true;
    imguiStyle.AntiAliasedLinesUseTex = true;
    imguiStyle.AntiAliasedFill = true;
    imguiStyle.CurveTessellationTol = 0.45f;
    imguiStyle.CircleTessellationMaxError = 0.08f;

    m_screenW = renderer.screenWidth();
    m_screenH = renderer.screenHeight();
    m_uiScale = std::clamp(m_screenH / 1280.f, 1.f, 1.22f);
    m_contentScale = std::clamp(m_screenH / 1180.f, 1.f, 1.28f);
    m_winW = 1040.f * m_uiScale;
    m_winH = 760.f * m_uiScale;
    m_sideW = 182.f * m_uiScale;

    float openEase = easeOutCubic(m_menuAlpha);
    float availX = (std::max)(0.f, m_screenW - m_winW);
    float availY = (std::max)(0.f, m_screenH - m_winH);
    float defaultX = (m_screenW - m_winW) * 0.5f;
    float defaultY = (m_screenH - m_winH) * 0.5f + (1.f - openEase) * (18.f * m_uiScale);
    if (g_cfg.menuPosX >= 0.f && g_cfg.menuPosY >= 0.f) {
        m_winX = availX > 0.f ? std::clamp(g_cfg.menuPosX, 0.f, 1.f) * availX : defaultX;
        m_winY = availY > 0.f ? std::clamp(g_cfg.menuPosY, 0.f, 1.f) * availY : defaultY;
    } else {
        m_winX = defaultX;
        m_winY = defaultY;
    }

    m_gui.beginFrame(renderer, m_font, hwnd);
    m_gui.setScale(m_uiScale, m_contentScale);
    m_gui.setModalInputBlocked(AimCalibration::instance().needsSetupPrompt());

    const float dragH = 62.f * m_uiScale;
    const bool dragHover = m_gui.mouseX() >= m_winX && m_gui.mouseX() <= m_winX + m_winW
        && m_gui.mouseY() >= m_winY && m_gui.mouseY() <= m_winY + dragH;

    if (!m_draggingMenu && dragHover && m_gui.mouseClicked()) {
        m_draggingMenu = true;
        m_dragOffsetX = m_gui.mouseX() - m_winX;
        m_dragOffsetY = m_gui.mouseY() - m_winY;
    }
    if (m_draggingMenu) {
        if (m_gui.mouseDown()) {
            m_winX = std::clamp(m_gui.mouseX() - m_dragOffsetX, 0.f, availX);
            m_winY = std::clamp(m_gui.mouseY() - m_dragOffsetY, 0.f, availY);
            g_cfg.menuPosX = availX > 0.f ? (m_winX / availX) : 0.5f;
            g_cfg.menuPosY = availY > 0.f ? (m_winY / availY) : 0.5f;
        } else {
            m_draggingMenu = false;
        }
    }

    for (int i = 0; i < 6; ++i) {
        float spread = (7.f + i * 7.5f) * m_uiScale;
        float shadowY = spread * 0.35f;
        unsigned int shadowAlpha = (unsigned int)((15.f - i * 2.1f) * m_menuAlpha);
        renderer.drawRoundedFilledRect(
            m_winX - spread,
            m_winY - shadowY,
            m_winW + spread * 2.f,
            m_winH + shadowY * 2.f + spread * 0.65f,
            withAlpha(0xFF000000, shadowAlpha),
            Theme::CORNER_RADIUS * m_uiScale + spread * 0.6f
        );
    }

    // Window
    renderer.drawRoundedFilledRect(m_winX, m_winY, m_winW, m_winH, Theme::BG, Theme::CORNER_RADIUS * m_uiScale);
    renderer.drawRoundedRect(m_winX, m_winY, m_winW, m_winH, Theme::BORDER, Theme::CORNER_RADIUS * m_uiScale, (std::max)(1.f, m_uiScale));
    drawSidebar();

    // Route each tab to its panel
    if (m_activeTab != 6)
        m_leetifyPromptDismissed = false;

    switch (m_activeTab) {
        case 0: drawAimbotPanel(); break;
        case 1: drawEspPanel(); break;
        case 2: drawWorldPanel(); break;
        case 3: drawNadesPanel(em); break;
        case 4: drawSystemPanel(); break;
        case 5: drawIntelPanel(em); break;
        case 6: drawPlayerInfoPanel(em); break;
        case 7: drawConfigsPanel(); break;
        default: {
            drawPanelHeader(m_activeTab);
            auto& r2 = m_gui.renderer();
            r2.drawText(m_gui.font(),
                m_winX + m_sideW + Theme::PADDING * m_uiScale,
                m_gui.cursorY(),
                "Coming soon...", Theme::TEXT_MUTED, 17.f * m_contentScale);
            break;
        }
    }

    drawCalibrationPromptOverlay();
    drawLeetifySetupPromptOverlay();
    drawPlayerDetailOverlay();

    m_playerDetailOpenedThisFrame = false;

    m_gui.endFrame();

    imguiStyle.AntiAliasedLines = oldAALines;
    imguiStyle.AntiAliasedLinesUseTex = oldAALinesTex;
    imguiStyle.AntiAliasedFill = oldAAFill;
    imguiStyle.CurveTessellationTol = oldCurveTol;
    imguiStyle.CircleTessellationMaxError = oldCircleErr;
    renderer.setImGuiDrawMode(false);
}

// ═════════════════════════════════════════════════════════════════════════════
// Header
// ═════════════════════════════════════════════════════════════════════════════

void Menu::drawHeader() {
}

void Menu::tryLoadUserAvatar(ID3D11Device* device) {
    const UserProfile& profile = userProfileGet();
    const std::string& url = profile.avatarUrl;
    if (url.empty() || url == m_userAvatarLoadedUrl || m_userAvatarFetchAttempted)
        return;

    m_userAvatarFetchAttempted = true;
    std::vector<std::uint8_t> bytes;
    if (!httpDownloadUrl(url, bytes))
        return;
    if (!loadTextureFromWicMemory(device, bytes.data(), bytes.size(),
                                  m_userAvatarSrv, m_userAvatarW, m_userAvatarH))
        return;
    m_userAvatarLoadedUrl = url;
}

void Menu::drawUserPanel(float x, float y, float w) {
    auto& r = m_gui.renderer();
    auto& f = m_gui.font();
    const float s = m_uiScale;
    const float c = m_contentScale;
    auto S = [s](float v) { return v * s; };
    auto C = [c](float v) { return v * c; };

    tryLoadUserAvatar(r.device());

    const UserProfile& profile = userProfileGet();
    const float pad = S(10.f);
    const float avatarSize = S(48.f);
    const float avatarRadius = S(10.f);
    const float panelH = avatarSize + pad * 2.f;
    const float cardX = x + S(8.f);
    const float cardW = w - S(16.f);

    r.drawRoundedFilledRect(cardX, y, cardW, panelH, 0xFF131522, S(10.f));
    r.drawRoundedRect(cardX, y, cardW, panelH, Theme::BORDER, S(10.f), (std::max)(1.f, s));

    const float avatarX = cardX + pad;
    const float avatarY = y + pad;
    if (m_userAvatarSrv && m_userAvatarW > 0 && m_userAvatarH > 0) {
        ImGui::GetBackgroundDrawList()->AddImageRounded(
            reinterpret_cast<ImTextureID>(m_userAvatarSrv.Get()),
            ImVec2(avatarX, avatarY),
            ImVec2(avatarX + avatarSize, avatarY + avatarSize),
            ImVec2(0.f, 0.f), ImVec2(1.f, 1.f),
            IM_COL32(255, 255, 255, 255), avatarRadius);
    } else {
        r.drawRoundedFilledRect(avatarX, avatarY, avatarSize, avatarSize, 0xFF1B1C28, avatarRadius);
    }

    const float textX = avatarX + avatarSize + S(10.f);
    const float nameSize = C(16.f);
    const float planSize = C(14.f);
    const float dateSize = C(12.f);
    const float lineGap = S(3.f);
    const float textBlockH = nameSize + lineGap + planSize + lineGap + dateSize;
    const float textY0 = y + pad + (avatarSize - textBlockH) * 0.5f;

    const char* username = profile.username.empty() ? "user" : profile.username.c_str();
    r.drawText(f, textX, textY0, username, Theme::TEXT, nameSize);

    const int days = userProfileDaysRemaining();
    std::string planLine = profile.plan.empty() ? "free" : profile.plan;
    if (days > 0)
        planLine += " - " + std::to_string(days) + "d left";
    r.drawText(f, textX, textY0 + nameSize + lineGap, planLine.c_str(), Theme::ACCENT, planSize);

    if (profile.subExpiresUnix > 0) {
        const std::time_t exp = static_cast<std::time_t>(profile.subExpiresUnix);
        std::tm tm{};
        localtime_s(&tm, &exp);
        char buf[32]{};
        std::strftime(buf, sizeof(buf), "%b %d, %Y", &tm);
        r.drawText(f, textX, textY0 + nameSize + lineGap + planSize + lineGap,
            buf, Theme::TEXT_MUTED, dateSize);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Sidebar
// ═════════════════════════════════════════════════════════════════════════════

void Menu::drawSidebar() {
    auto& r = m_gui.renderer();
    auto& f = m_gui.font();
    const float s = m_uiScale;
    const float c = m_contentScale;
    auto S = [s](float v) { return v * s; };
    auto C = [c](float v) { return v * c; };
    const float tabH = kTabH * s;
    const float corner = Theme::CORNER_RADIUS * s;

    float x = m_winX;
    float y = m_winY;
    float sideH = m_winH;
    r.drawRoundedFilledRect(x, y, m_sideW, sideH, 0xFF090A10, corner);
    r.drawFilledRect(x + m_sideW - corner, y, corner, sideH, 0xFF090A10);
    r.drawFilledRect(x + m_sideW, y, (std::max)(1.f, s), sideH, Theme::BORDER);

    if (m_brandLogoSrv && m_brandLogoW > 0 && m_brandLogoH > 0) {
        const float logoSize = S(42.f);
        r.drawImage(m_brandLogoSrv.Get(), x + S(14.f), y + S(14.f), logoSize, logoSize);
    } else {
        drawBrandMark(r, x + S(18.f), y + S(18.f), c);
    }
    r.drawText(f, x + S(66.f), y + S(17.f), "crymore.pw", Theme::TEXT, C(16.f));
    r.drawText(f, x + S(66.f), y + S(35.f), "rebirth", Theme::TEXT_MUTED, C(13.f));

    float startY = y + S(80.f);

    auto drawTab = [&](int index, float itemY) {
        bool active = index == m_activeTab;
        float itemX = x + S(8.f);
        float itemW = m_sideW - S(16.f);
        bool hovered = m_gui.mouseX() >= itemX && m_gui.mouseX() <= itemX + itemW
                    && m_gui.mouseY() >= itemY && m_gui.mouseY() <= itemY + tabH;

        float hoverT = m_tabHover[index] = m_tabHover[index] + ((hovered ? 1.f : 0.f) - m_tabHover[index]) * (std::min)(m_frameDt * 12.f, 1.f);
        m_tabActive[index] = advanceTabAnim(m_tabActive[index], active, m_frameDt);
        const float activeT = m_tabActive[index];

        {
            unsigned int inactiveBg = lerpColor(0xFF131522, 0xFF252242, hoverT * 0.18f);
            unsigned int activeBg = lerpColor(inactiveBg, 0xFF2A3148, 1.f);
            activeBg = lerpColor(activeBg, Theme::ACCENT, 0.08f);
            drawSubTabSurface(r, itemX, itemY, itemW, tabH, inactiveBg, activeBg, S(10.f), activeT, s, true, 0xFF090A10u);
        }

        unsigned int textColor = lerpColor(Theme::TEXT_MUTED, Theme::TEXT, activeT * 0.92f + hoverT * 0.12f);

        float iconSize = S(20.f);
        float iconX = itemX + S(16.f);
        float iconY = itemY + (tabH - iconSize) * 0.5f;
        float textX = itemX + S(50.f);
        if (m_tabIconSrv[index]) {
            unsigned int iconTint;
            if (m_tabIconFullColor[index]) {
                iconTint = withAlpha(0xFFFFFFFFu, (unsigned int)(150.f + 105.f * activeT));
            } else {
                const unsigned int iconRgb = lerpColor(0xFF67698A, Theme::ACCENT, activeT);
                iconTint = withAlpha(iconRgb, 255u);
            }
            r.drawImage(m_tabIconSrv[index].Get(), iconX, iconY, iconSize, iconSize, iconTint);
        } else {
            const unsigned int iconColor = lerpColor(0xFF67698A, Theme::ACCENT, activeT);
            kTabIcons[index](r, iconX, iconY, iconSize, iconColor);
        }
        r.drawText(f, textX, textControlCenterY(f, itemY, tabH, kTabs[index], C(16.f)), kTabs[index], textColor, C(16.f));

        if (hovered && m_gui.mouseClicked())
            m_activeTab = index;
    };

    const float tabGap = S(40.f);
    for (int i = 0; i < 7; ++i)
        drawTab(i, startY + i * tabGap);

    const float userPanelH = S(68.f);
    const float bottomPad = S(8.f);
    const float configTabH = tabH;
    const float configGap = S(8.f);
    const float userPanelY = y + sideH - userPanelH - bottomPad;
    const float configTabY = userPanelY - configGap - configTabH;

    drawTab(7, configTabY);
    drawUserPanel(x, userPanelY, m_sideW);
}

// ═════════════════════════════════════════════════════════════════════════════
// Panel header helper (title + separator, sets cursor for content)
// ═════════════════════════════════════════════════════════════════════════════

void Menu::drawPanelHeader(int tabIndex) {
    auto& r = m_gui.renderer();
    auto& f = m_gui.font();
    const float s = m_uiScale;
    const float c = m_contentScale;
    const float pad = Theme::PADDING * s;
    auto S = [s](float v) { return v * s; };
    auto C = [c](float v) { return v * c; };
    float x = m_winX + m_sideW + pad;
    float y = m_winY + S(24.f);
    float w = m_winW - m_sideW - pad * 2.f;

    r.drawText(f, x, y, kTabs[tabIndex], Theme::TEXT, C(22.f));
    if (tabIndex == 0) {
        const bool enabled = g_cfg.aimAssistEnabled;
        const char* badge = enabled ? "ON" : "OFF";
        float pillX = x + S(72.f);
        float pillW = enabled ? S(38.f) : S(42.f);
        unsigned int pillBg = enabled ? 0xFF2B2953 : 0xFF1B1C28;
        unsigned int pillTc = enabled ? Theme::ACCENT : Theme::TEXT_MUTED;
        r.drawRoundedFilledRect(pillX, y - S(1.f), pillW, S(24.f), pillBg, S(7.f));
        r.drawText(f, pillX + S(9.f), textControlCenterY(f, y - S(1.f), S(24.f), badge, C(13.f)), badge, pillTc, C(13.f));
    }
    r.drawText(f, x, y + S(28.f), kTabSubtitles[tabIndex], Theme::TEXT_MUTED, C(15.f));
    m_gui.setCursor(x, y + S(64.f));
    m_gui.setItemWidth(w);
}

// ═════════════════════════════════════════════════════════════════════════════
// Configs panel
// ═════════════════════════════════════════════════════════════════════════════

void Menu::drawConfigsPanel() {
    auto& r = m_gui.renderer();
    auto& f = m_gui.font();
    const float s = m_uiScale;
    const float c = m_contentScale;
    auto S = [s](float v) { return v * s; };
    auto C = [c](float v) { return v * c; };

    float x = m_winX + m_sideW, y = m_winY;
    float w = m_winW - m_sideW;
    float pad = Theme::PADDING * s;

    if (!m_configsLoaded) {
        m_configFiles = listConfigFiles();
        m_configsLoaded = true;
        if (m_selectedConfig >= (int)m_configFiles.size()) m_selectedConfig = (int)m_configFiles.size() - 1;
    }

    drawPanelHeader(7);
    float clipX = x + pad;
    float clipY = m_gui.cursorY();
    float clipW = w - pad * 2.f;
    float clipH = m_winH - (clipY - y) - pad;

    if (m_configStatusTimer > 0.f) {
        m_configStatusTimer = (std::max)(0.f, m_configStatusTimer - m_frameDt);
        if (!m_configStatus.empty())
            r.drawText(f, clipX, clipY - S(24.f), m_configStatus.c_str(), Theme::TEXT_MUTED, C(13.f));
    }

    bool clipHovered = m_gui.mouseX() >= clipX && m_gui.mouseX() <= clipX + clipW
                    && m_gui.mouseY() >= clipY && m_gui.mouseY() <= clipY + clipH;
    if (clipHovered) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.f)
            m_configsScrollTarget -= wheel * S(44.f);
    }
    m_configsScroll = smoothValue(m_configsScroll, m_configsScrollTarget, 0.22f, m_frameDt);

    r.setClipRect(clipX, clipY, clipW, clipH);
    float cursorY = clipY - m_configsScroll;
    const float gap = S(12.f);

    auto drawPillButton = [&](float bx, float by, float bw, float bh, const char* text,
                              unsigned int fill, unsigned int border, unsigned int textColor) {
        bool hovered = m_gui.mouseX() >= bx && m_gui.mouseX() <= bx + bw
                    && m_gui.mouseY() >= by && m_gui.mouseY() <= by + bh;
        uint32_t idh = hashAnimId(text, bx, by);
        float hoverT = animValue(idh ^ 0x610100u, hovered ? 1.f : 0.f, 0.22f, m_frameDt);
        float pressT = animValue(idh ^ 0x610101u, (hovered && m_gui.mouseDown()) ? 1.f : 0.f, 0.34f, m_frameDt);
        float drawY = by + pressT * S(1.25f);
        unsigned int bg = lerpColor(fill, 0xFFFFFFFF, hoverT * 0.05f + pressT * 0.04f);
        unsigned int br = lerpColor(border, Theme::ACCENT, hoverT * 0.08f);
        r.drawRoundedFilledRect(bx, drawY, bw, bh, bg, S(8.f));
        r.drawRoundedRect(bx, drawY, bw, bh, br, S(8.f), (std::max)(1.f, s));
        float tw = static_cast<float>(std::char_traits<char>::length(text)) * C(7.2f);
        r.drawText(f, bx + (bw - tw) * 0.5f, textControlCenterY(f, drawY, bh, text, C(13.f)), text, textColor, C(13.f));
        return hovered && m_gui.mouseClicked();
    };

    auto drawActionCard = [&](float bx, float by, float bw, float bh, const char* title,
                              const char* subtitle,
                              void(*iconFn)(Renderer&, float, float, float, unsigned int),
                              bool accent) {
        bool hovered = m_gui.mouseX() >= bx && m_gui.mouseX() <= bx + bw
                    && m_gui.mouseY() >= by && m_gui.mouseY() <= by + bh;
        uint32_t idh = hashAnimId(title, bx, by);
        float hoverT = animValue(idh ^ 0x620100u, hovered ? 1.f : 0.f, 0.22f, m_frameDt);
        float pressT = animValue(idh ^ 0x620101u, (hovered && m_gui.mouseDown()) ? 1.f : 0.f, 0.34f, m_frameDt);
        float drawY = by + pressT * S(1.8f);
        unsigned int bg = accent
            ? lerpColor(0xFF171927, 0xFF242140, 0.45f + hoverT * 0.2f)
            : lerpColor(Theme::SURFACE, 0xFF171928, hoverT * 0.45f);
        unsigned int border = accent
            ? lerpColor(0xFF312D57, Theme::ACCENT, 0.55f + hoverT * 0.25f)
            : lerpColor(Theme::BORDER, 0xFF2A2D42, hoverT * 0.65f);
        unsigned int titleColor = accent ? 0xFFFFFFFF : Theme::TEXT;
        unsigned int subtitleColor = accent ? 0xFFC5C5FF : Theme::TEXT_MUTED;

        r.drawRoundedFilledRect(bx, drawY, bw, bh, bg, S(12.f));
        r.drawRoundedRect(bx, drawY, bw, bh, border, S(12.f), (std::max)(1.f, s));
        r.drawRoundedFilledRect(bx + S(16.f), drawY + S(16.f), S(34.f), S(34.f),
                                accent ? withAlpha(0xFFFFFFFF, 28) : withAlpha(Theme::ACCENT, 26), S(10.f));
        iconFn(r, bx + S(25.f), drawY + S(25.f), S(16.f), accent ? 0xFFFFFFFF : Theme::ACCENT);
        r.drawText(f, bx + S(16.f), drawY + S(58.f), title, titleColor, C(15.f));
        r.drawText(f, bx + S(16.f), drawY + S(78.f), subtitle, subtitleColor, C(12.f));
        return hovered && m_gui.mouseClicked();
    };

    float actionY = cursorY;
    float actionW = (clipW - gap * 2.f) / 3.f;
    float actionH = S(108.f);

    bool refreshClicked = drawActionCard(clipX, actionY, actionW, actionH,
                                         "Refresh", "Reload configs from disk",
                                         Icons::refresh, false);
    bool createClicked = drawActionCard(clipX + actionW + gap, actionY, actionW, actionH,
                                        "Create Config", "Save current settings as a new file",
                                        Icons::save, true);
    bool pasteClicked = drawActionCard(clipX + (actionW + gap) * 2.f, actionY, actionW, actionH,
                                       "Paste JSON", "Apply a config directly from clipboard",
                                       Icons::page, false);

    if (refreshClicked) {
        m_configFiles = listConfigFiles();
        if (m_selectedConfig >= (int)m_configFiles.size()) m_selectedConfig = (int)m_configFiles.size() - 1;
        m_configStatus = "Config list refreshed";
        m_configStatusTimer = 2.5f;
    }
    if (createClicked) {
        std::filesystem::create_directories(cfgDirPath());
        const std::string name = makeDefaultConfigName();
        if (writeConfigFile(cfgDirPath() / name, g_cfg)) {
            m_configFiles = listConfigFiles();
            for (int i = 0; i < (int)m_configFiles.size(); ++i)
                if (m_configFiles[i] == name) m_selectedConfig = i;
            m_configStatus = "Created " + name;
        } else {
            m_configStatus = "Failed to create config";
        }
        m_configStatusTimer = 2.5f;
    }
    if (pasteClicked) {
        std::string text;
        if (getClipboardText(text)) {
            try {
                json j = json::parse(text);
                applyJsonToConfig(j, g_cfg);
                m_configStatus = "Applied config from clipboard";
            } catch (...) {
                m_configStatus = "Clipboard does not contain valid config JSON";
            }
        } else {
            m_configStatus = "Clipboard unavailable";
        }
        m_configStatusTimer = 2.5f;
    }

    cursorY += actionH + S(18.f);

    r.drawText(f, clipX, cursorY, "Cloud configs", Theme::TEXT, C(18.f));
    cursorY += S(28.f);
    if (!cloud_api::hasToken()) {
        r.drawText(f, clipX, cursorY, "Launch via loader to sync configs with crymore.pw.", Theme::TEXT_MUTED, C(12.f));
        cursorY += S(22.f);
    } else {
        const float cloudBtnW = S(120.f);
        const float cloudBtnH = S(30.f);
        auto cloudBtn = [&](float bx, const char* text, bool accent) -> bool {
            bool hovered = m_gui.mouseX() >= bx && m_gui.mouseX() <= bx + cloudBtnW
                        && m_gui.mouseY() >= cursorY && m_gui.mouseY() <= cursorY + cloudBtnH;
            unsigned int bg = accent ? 0xFF2A2848 : 0xFF151822;
            if (hovered) bg = lerpColor(bg, Theme::ACCENT, 0.25f);
            r.drawRoundedFilledRect(bx, cursorY, cloudBtnW, cloudBtnH, bg, S(8.f));
            r.drawText(f, bx + S(12.f), textControlCenterY(f, cursorY, cloudBtnH, text, C(12.f)),
                       text, Theme::TEXT, C(12.f));
            return hovered && m_gui.mouseClicked();
        };

        if (cloudBtn(clipX, "Refresh cloud", false)) {
            std::string err;
            m_cloudConfigs.clear();
            if (cloud_api::listConfigs(m_cloudConfigs, err)) {
                m_cloudConfigsLoaded = true;
                m_configStatus = "Cloud configs refreshed";
            } else {
                m_configStatus = err;
            }
            m_configStatusTimer = 2.5f;
        }
        if (cloudBtn(clipX + cloudBtnW + S(8.f), "Upload current", true)) {
            std::string err, id;
            const std::string payload = configToJson(g_cfg).dump(2);
            if (cloud_api::uploadConfig("Overlay config", "Uploaded from overlay", payload, false, id, err)) {
                g_cfg.cloudActiveConfigId = id;
                m_configStatus = "Uploaded to cloud";
            } else {
                m_configStatus = err;
            }
            m_configStatusTimer = 2.5f;
        }
        cursorY += cloudBtnH + S(10.f);

        for (int i = 0; i < static_cast<int>(m_cloudConfigs.size()); ++i) {
            const auto& cc = m_cloudConfigs[i];
            const float rowH = S(52.f);
            bool rowHover = m_gui.mouseX() >= clipX && m_gui.mouseX() <= clipX + clipW
                         && m_gui.mouseY() >= cursorY && m_gui.mouseY() <= cursorY + rowH;
            if (i == m_selectedCloudConfig || rowHover)
                r.drawRoundedFilledRect(clipX, cursorY, clipW, rowH, 0xFF151822, S(8.f));
            r.drawText(f, clipX + S(12.f), cursorY + S(8.f), cc.name.c_str(), Theme::TEXT, C(14.f));
            r.drawText(f, clipX + S(12.f), cursorY + S(26.f), cc.description.c_str(), Theme::TEXT_MUTED, C(11.f));

            const float loadW2 = S(52.f);
            const float loadX = clipX + clipW - loadW2 - S(12.f);
            if (drawPillButton(loadX, cursorY + S(10.f), loadW2, S(28.f), "Load",
                               0xFF242140, Theme::BORDER, Theme::TEXT)) {
                std::string jsonText, err;
                if (cloud_api::downloadConfig(cc.id, jsonText, err)) {
                    try {
                        applyJsonToConfig(json::parse(jsonText), g_cfg);
                        g_cfg.cloudActiveConfigId = cc.id;
                        m_configStatus = "Loaded cloud config";
                    } catch (...) {
                        m_configStatus = "Invalid cloud config JSON";
                    }
                } else {
                    m_configStatus = err;
                }
                m_configStatusTimer = 2.5f;
            }
            if (rowHover && m_gui.mouseClicked())
                m_selectedCloudConfig = i;
            cursorY += rowH + S(6.f);
        }
        cursorY += S(8.f);
    }

    r.drawText(f, clipX, cursorY, "Local configs", Theme::TEXT, C(18.f));
    char countBuf[32];
    std::snprintf(countBuf, sizeof(countBuf), "%d files", (int)m_configFiles.size());
    float countW = static_cast<float>(std::char_traits<char>::length(countBuf)) * C(7.2f) + S(18.f);
    float countX = clipX + clipW - countW;
    r.drawRoundedFilledRect(countX, cursorY - S(2.f), countW, S(24.f), 0xFF1A1D2A, S(7.f));
    r.drawText(f, countX + S(9.f), textControlCenterY(f, cursorY - S(2.f), S(24.f), countBuf, C(12.f)), countBuf, Theme::TEXT_MUTED, C(12.f));

    cursorY += S(34.f);

    if (m_configFiles.empty()) {
        float emptyH = S(130.f);
        r.drawRoundedFilledRect(clipX, cursorY, clipW, emptyH, 0xFF101216, S(12.f));
        r.drawRoundedRect(clipX, cursorY, clipW, emptyH, Theme::BORDER, S(12.f), (std::max)(1.f, s));
        r.drawRoundedFilledRect(clipX + S(18.f), cursorY + S(22.f), S(42.f), S(42.f), withAlpha(Theme::ACCENT, 28), S(11.f));
        Icons::page(r, clipX + S(31.f), cursorY + S(35.f), S(16.f), Theme::ACCENT);
        r.drawText(f, clipX + S(74.f), cursorY + S(24.f), "No configs yet", Theme::TEXT, C(19.f));
        r.drawText(f, clipX + S(74.f), cursorY + S(48.f), "Create a config from the cards above or paste JSON from the clipboard.", Theme::TEXT_MUTED, C(12.f));
        cursorY += emptyH;
    } else {
        const float rowH = S(68.f);
        const float actionH2 = S(30.f);
        const float loadW = S(62.f);
        const float copyW = S(58.f);
        const float deleteW = S(68.f);

        for (int i = 0; i < (int)m_configFiles.size(); ++i) {
            float rowY = cursorY;
            float rowX = clipX;
            bool rowSelected = (i == m_selectedConfig);
            bool rowHover = m_gui.mouseX() >= rowX && m_gui.mouseX() <= rowX + clipW
                         && m_gui.mouseY() >= rowY && m_gui.mouseY() <= rowY + rowH;

            const std::string& name = m_configFiles[i];
            uint32_t rowId = hashAnimId(name.c_str(), rowX, rowY);
            float hoverT = animValue(rowId ^ 0x630100u, rowHover ? 1.f : 0.f, 0.22f, m_frameDt);
            float selectT = animValue(rowId ^ 0x630101u, rowSelected ? 1.f : 0.f, 0.18f, m_frameDt);
            unsigned int rowBg = lerpColor(0xFF11131C, 0xFF171A27, hoverT * 0.5f);
            rowBg = lerpColor(rowBg, 0xFF17192A, selectT * 0.9f);

            r.drawRoundedFilledRect(rowX, rowY, clipW, rowH, rowBg, S(12.f));
            if (selectT > 0.01f) {
                r.drawRoundedFilledRect(rowX, rowY, clipW, rowH,
                                        withAlpha(Theme::ACCENT, (unsigned int)(22.f * selectT)), S(12.f));
                r.drawRoundedFilledRect(rowX + S(4.f), rowY + S(12.f), S(3.f), rowH - S(24.f),
                                        withAlpha(Theme::ACCENT, (unsigned int)(255.f * selectT)), S(1.5f));
            }

            r.drawText(f, rowX + S(18.f), rowY + S(16.f), name.c_str(), Theme::TEXT, C(16.f));
            r.drawText(f, rowX + S(18.f), rowY + S(38.f), "Local JSON file", Theme::TEXT_MUTED, C(12.f));

            float actionY2 = rowY + (rowH - actionH2) * 0.5f;
            float deleteX = rowX + clipW - deleteW - S(16.f);
            float copyX = deleteX - gap - copyW;
            float loadX2 = copyX - gap - loadW;

            bool loadClicked = drawPillButton(loadX2, actionY2, loadW, actionH2,
                                              "Load", Theme::ACCENT, Theme::ACCENT, 0xFFFFFFFF);
            bool copyClicked = drawPillButton(copyX, actionY2, copyW, actionH2,
                                              "Copy", 0xFF161925, 0xFF2B2F44, Theme::TEXT);
            bool deleteClicked = drawPillButton(deleteX, actionY2, deleteW, actionH2,
                                                "Delete", 0xFF25141B, 0xFF5B2432, 0xFFF0A4B7);

            if (rowHover && m_gui.mouseClicked() && !loadClicked && !copyClicked && !deleteClicked)
                m_selectedConfig = i;

            if (loadClicked) {
                const auto path = cfgDirPath() / name;
                if (readConfigFile(path, g_cfg)) m_configStatus = "Loaded " + name;
                else m_configStatus = "Failed to load " + name;
                m_selectedConfig = i;
                m_configStatusTimer = 2.5f;
            }
            if (copyClicked) {
                const auto path = cfgDirPath() / name;
                std::ifstream in(path, std::ios::binary);
                if (in.is_open()) {
                    std::string txt((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                    if (setClipboardText(txt)) m_configStatus = "Copied JSON to clipboard";
                    else m_configStatus = "Clipboard unavailable";
                } else {
                    m_configStatus = "Failed to read config file";
                }
                m_selectedConfig = i;
                m_configStatusTimer = 2.5f;
            }
            if (deleteClicked) {
                const auto path = cfgDirPath() / name;
                std::error_code ec;
                std::filesystem::remove(path, ec);
                if (!ec) {
                    m_configStatus = "Deleted " + name;
                    m_configFiles = listConfigFiles();
                    if (m_selectedConfig >= (int)m_configFiles.size())
                        m_selectedConfig = (int)m_configFiles.size() - 1;
                } else {
                    m_configStatus = "Failed to delete " + name;
                }
                m_configStatusTimer = 2.5f;
                break;
            }

            cursorY += rowH + gap;
        }
    }

    float contentHeight = cursorY - (clipY - m_configsScroll);
    r.clearClipRect();

    m_configsMaxScroll = (std::max)(0.f, contentHeight - clipH);
    if (m_configsScrollTarget < 0.f) m_configsScrollTarget = 0.f;
    if (m_configsScrollTarget > m_configsMaxScroll) m_configsScrollTarget = m_configsMaxScroll;
    if (m_configsScroll < 0.f) m_configsScroll = 0.f;
    if (m_configsScroll > m_configsMaxScroll) m_configsScroll = m_configsMaxScroll;

    if (m_configsMaxScroll > 0.5f) {
        float trackX = clipX + clipW - S(5.f);
        r.drawRoundedFilledRect(trackX, clipY, S(3.f), clipH, 0xFF101015, S(2.f));
        float thumbH = (std::max)(S(36.f), clipH * (clipH / (contentHeight + 0.001f)));
        float t = m_configsScroll / m_configsMaxScroll;
        float thumbY = clipY + (clipH - thumbH) * t;
        r.drawRoundedFilledRect(trackX, thumbY, S(3.f), thumbH, Theme::ACCENT, S(2.f));
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Aimbot panel
// ═════════════════════════════════════════════════════════════════════════════

void Menu::drawAimbotPanel() {
    m_gui.setModalInputBlocked(AimCalibration::instance().needsSetupPrompt());

    if (m_aimCalibStatusTimer > 0.f)
        m_aimCalibStatusTimer = (std::max)(0.f, m_aimCalibStatusTimer - m_frameDt);
    auto& r = m_gui.renderer();
    const float s = m_uiScale;
    auto S = [s](float v) { return v * s; };
    const float pad = Theme::PADDING * s;

    drawPanelHeader(0);

    float contentX = m_winX + m_sideW + pad;
    float clipY = m_gui.cursorY();
    float contentW = m_winW - m_sideW - pad * 2.f;
    float clipH = m_winH - (clipY - m_winY) - S(18.f);

    bool clipHovered = m_gui.mouseX() >= contentX && m_gui.mouseX() <= contentX + contentW
                    && m_gui.mouseY() >= clipY && m_gui.mouseY() <= clipY + clipH;
    if (clipHovered) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.f)
            m_aimbotScrollTarget -= wheel * S(46.f);
    }
    m_aimbotScroll = smoothValue(m_aimbotScroll, m_aimbotScrollTarget, 0.22f, m_frameDt);

    m_gui.setCursor(contentX, clipY - m_aimbotScroll);
    m_gui.setItemWidth(contentW);
    r.setClipRect(contentX, clipY, contentW, clipH);

    static const wchar_t kSettingsIcon[] = { 0x2699, 0 };
    static const wchar_t kGroupIcons[] = {
        0xE001, // Pistols (deagle)
        0xE002, // Heavy (shotgun)
        0xE022, // SMG (mp9)
        0xE007, // Rifles (ak-47)
        0xE009, // Snipers (awp)
        0xE03D, // Other (usp-s)
    };

    if (m_aimSubTab < 0 || m_aimSubTab > static_cast<int>(kAimWeaponGroupCount))
        m_aimSubTab = 0;
    if (m_aimWeaponGroupIdx < 0 || m_aimWeaponGroupIdx >= static_cast<int>(kAimWeaponGroupCount))
        m_aimWeaponGroupIdx = 0;

    AimGroupConfig* sideModelCfg = nullptr;

    float tabsY = m_gui.cursorY();
    float tabH = S(58.f);
    float chipGap = S(6.f);
    float globalW = S(62.f);
    float iconW = S(104.f);

    auto measureWide = [&](const wchar_t* text, float fontSize) {
        if (!text || !*text) return 0.f;
        float scale = fontSize / m_gui.font().renderPx();
        float width = 0.f;
        float spaceAdv = 8.f;
        if (const GlyphInfo* sg = m_gui.font().glyph(L' '))
            spaceAdv = static_cast<float>(sg->advanceX);
        for (const wchar_t* p = text; *p; ++p) {
            if (*p == L' ') { width += spaceAdv * scale; continue; }
            if (const GlyphInfo* gi = m_gui.font().glyph(*p))
                width += static_cast<float>(gi->advanceX) * scale;
            else
                width += 8.f * scale;
        }
        return width;
    };

    auto drawChip = [&](const char* id, float x, float y, float w, float h, int chipIndex, bool active,
                        const wchar_t* wlabel, bool iconOnly, bool isSettingsIcon) {
        bool hovered = m_gui.mouseX() >= x && m_gui.mouseX() <= x + w
                    && m_gui.mouseY() >= y && m_gui.mouseY() <= y + h;
        uint32_t idh = hashAnimId(id, x, y);
        float hoverT = animValue(idh ^ 0x650100u, hovered ? 1.f : 0.f, 0.22f, m_frameDt);
        m_aimSubTabAnim[(size_t)chipIndex] = advanceSubTabAnim(
            m_aimSubTabAnim[(size_t)chipIndex], active, m_frameDt);
        const float activeT = m_aimSubTabAnim[(size_t)chipIndex];
        float pressT = animValue(idh ^ 0x650102u, (hovered && m_gui.mouseDown()) ? 1.f : 0.f, 0.34f, m_frameDt);

        unsigned int inactiveBg = lerpColor(0xFF131522, 0xFF252242, hoverT * 0.18f);
        inactiveBg = lerpColor(inactiveBg, 0xFF0E1018, pressT * 0.35f);
        unsigned int activeBg = lerpColor(inactiveBg, 0xFF2A3148, 1.f);
        activeBg = lerpColor(activeBg, withAlpha(Theme::ACCENT, 26), 1.f);
        unsigned int textCol = lerpColor(Theme::TEXT_MUTED, Theme::ACCENT, activeT * 0.95f + hoverT * 0.18f);
        drawSubTabSurface(r, x, y, w, h, inactiveBg, activeBg, S(9.f), activeT, s);

        if (wlabel) {
            float fontSize = iconOnly ? S(27.f) : S(13.f);
            float tx = x + (w - measureWide(wlabel, fontSize)) * 0.5f + (isSettingsIcon ? S(0.5f) : 0.f);
            float ty = iconOnly
                ? (textVisualCenterY(m_gui.font(), y, h, wlabel, fontSize) + (isSettingsIcon ? S(0.4f) : S(1.4f)))
                : textVisualCenterY(m_gui.font(), y, h, wlabel, fontSize);
            r.drawTextW(m_gui.font(), tx, ty, wlabel, textCol, fontSize);
        }

        return hovered && m_gui.mouseClicked();
    };

    float chipX = contentX;
    if (drawChip("aimChipGlobal", chipX, tabsY, globalW, tabH, 0, m_aimSubTab == 0, kSettingsIcon, true, true))
        m_aimSubTab = 0;
    chipX += globalW + chipGap;

    for (int i = 0; i < static_cast<int>(kAimWeaponGroupCount); ++i) {
        wchar_t glyph[2] = { kGroupIcons[i], 0 };
        const char* cid = (i == 0) ? "aimChipPistol"
                        : (i == 1) ? "aimChipHeavy"
                        : (i == 2) ? "aimChipSmg"
                        : (i == 3) ? "aimChipRifle"
                        : (i == 4) ? "aimChipSniper"
                                   : "aimChipOther";
        bool active = m_aimSubTab == (i + 1);
        if (drawChip(cid, chipX, tabsY, iconW, tabH, i + 1, active, glyph, true, false)) {
            m_aimSubTab = i + 1;
            m_aimWeaponGroupIdx = i;
        }
        chipX += iconW + chipGap;
    }

    m_gui.advanceY(tabH + S(10.f));

    if (m_aimSubTab == 0) {
        const float sectionY = m_gui.cursorY();
        MenuColumnLayout cols(contentX, contentW, sectionY, S(14.f));
        static const char* kAssistStyles[] = { "Normal", "Support (brake & guide)" };
        static const char* kHumModes[] = { "Auto (calibrated)", "Manual" };
        int assistStyle = std::clamp(g_cfg.aimAssistStyle, 0, 1);
        int humMode = std::clamp(g_cfg.aimHumanizeMode, 0, 1);

        cols.beginLeft(m_gui);
        m_gui.label("CORE", Theme::TEXT_MUTED, 16.f);
        m_gui.dummy(8.f);
        m_gui.toggleCheckbox("aimEnabled", "Enable aimbot", &g_cfg.aimAssistEnabled, 44.f);
        m_gui.toggleCheckbox("aimNeedVis", "Visibility check", &g_cfg.aimRequireVisibility, 44.f);
        if (g_cfg.aimRequireVisibility) {
            static const char* kAimVisModes[] = { "Performance", "Accuracy" };
            int aimVisMode = std::clamp(g_cfg.aimVisibilityMode, 0, 1);
            m_gui.label("Vis mode", Theme::TEXT_MUTED, 15.f);
            if (m_gui.comboBox("##aimVisMode", kAimVisModes, 2, &aimVisMode))
                g_cfg.aimVisibilityMode = aimVisMode;
        }
        m_gui.keybindCard("aimGlobalKey", "Aim key", &g_cfg.aimAssistKey, 58.f);
        if (!g_cfg.aimSensitivityManual && m_renderProc && m_renderEm) {
            float liveSens = 0.f;
            if (readGameSensitivity(*m_renderProc, m_renderEm->clientBase(), liveSens, m_renderEm->localPawn()))
                g_cfg.aimSensitivity = liveSens;
        }
        if (m_gui.sliderFloatValue("aimGlobalSensitivity", "CS2 sensitivity", &g_cfg.aimSensitivity, 0.1f, 10.f, "%.2f", 74.f))
            g_cfg.aimSensitivityManual = true;
        if (!g_cfg.aimSensitivityManual) {
            float liveSens = 0.f;
            const bool synced = m_renderProc && m_renderEm
                && readGameSensitivity(*m_renderProc, m_renderEm->clientBase(), liveSens, m_renderEm->localPawn());
            if (synced)
                m_gui.label("Auto-synced from CS2", Theme::TEXT_MUTED, 13.f);
            else
                m_gui.label("Could not read game sensitivity — set manually", Theme::TEXT_MUTED, 13.f);
        }
        m_gui.toggleCheckbox("aimDbgCon", "Debug console", &g_cfg.aimDebugConsole, 44.f);
        if (g_cfg.aimDebugConsole)
            m_gui.label("Live log window + %LOCALAPPDATA%\\crymore\\overlay.log", Theme::TEXT_MUTED, 13.f);
        m_gui.sliderFloatValue("aimOffsetZ", "Bone offset Z", &g_cfg.aimBoneOffsetZ, -8.f, 20.f, "%.1f", 74.f);
        m_gui.sliderFloatValue("aimHeadForward", "Head forward", &g_cfg.aimHeadForward, -8.f, 20.f, "%.1f", 74.f);
        cols.syncLeft(m_gui.cursorY());

        cols.beginRight(m_gui);
        m_gui.label("HUMANIZATION", Theme::TEXT_MUTED, 16.f);
        m_gui.dummy(8.f);
        m_gui.toggleCheckbox("aimHumEn", "Humanize aim", &g_cfg.aimHumanizeEnabled, 44.f);
        m_gui.label("Assist mode", Theme::TEXT_MUTED, 15.f);
        if (m_gui.comboBox("aimAssistStyle", kAssistStyles, 2, &assistStyle))
            g_cfg.aimAssistStyle = assistStyle;
        if (assistStyle == 0) {
            m_gui.label("Classic aim — uses FOV & smoothing below.", Theme::TEXT_MUTED, 14.f);
        } else {
            m_gui.label("Assists your flicks — brakes overshoots, no lock-on.", Theme::TEXT_MUTED, 14.f);
            m_gui.label("Lower smoothing = stronger support. 0 = fastest.", Theme::TEXT_MUTED, 13.f);
        }
        if (assistStyle == 1) {
            m_gui.sliderFloatValue("aimSupportStr", "Support strength",
                &g_cfg.aimSupportStrength, 0.f, 1.f, "%.2f", 74.f);
            m_gui.toggleCheckbox("aimSupportAlways", "Always active (no aim key)",
                &g_cfg.aimSupportAlwaysOn, 44.f);
        }
        m_gui.label("Humanize mode", Theme::TEXT_MUTED, 15.f);
        if (m_gui.comboBox("aimHumMode", kHumModes, 2, &humMode)) {
            g_cfg.aimHumanizeMode = humMode;
            g_cfg.aimHumanizeUseProfile = (humMode == 0);
        }
        if (humMode == 0 && assistStyle == 0) {
            m_gui.sliderFloatValue("aimCalibStr", "Assist intensity",
                &g_cfg.aimCalibAssistStrength, 0.f, 1.f, "%.2f", 74.f);
        } else if (humMode == 1) {
            m_gui.sliderFloatValue("aimHumStr", "Strength", &g_cfg.aimHumanizeStrength, 0.f, 1.f, "%.2f", 74.f);
            m_gui.sliderFloatValue("aimHumReact", "Reaction delay", &g_cfg.aimHumanizeReactionMs, 20.f, 400.f, "%.0f", 74.f);
            m_gui.sliderFloatValue("aimHumJit", "Jitter", &g_cfg.aimHumanizeJitter, 0.f, 3.f, "%.2f", 74.f);
        }
        cols.syncRight(m_gui.cursorY());
        cols.end(m_gui);

        m_gui.separator();
        m_gui.label("CALIBRATION", Theme::TEXT_MUTED, 16.f);
        m_gui.dummy(8.f);
        m_gui.label("Per-weapon-class setup: flicks, smoothing, humanization. RCS for SMG / rifles / LMG only.", Theme::TEXT_MUTED, 13.f);
        {
            float tgt = static_cast<float>(g_cfg.aimCalibrationTargetsPerGroup);
            m_gui.sliderFloatValue("aimCalTargetsGrp", "Targets per weapon class", &tgt, 5.f, 30.f, "%.0f", 74.f);
            g_cfg.aimCalibrationTargetsPerGroup = static_cast<int>(tgt + 0.5f);
            g_cfg.aimCalibrationTargets = g_cfg.aimCalibrationTargetsPerGroup;
        }
        if (AimCalibration::instance().isActive()) {
            const auto& cal = AimCalibration::instance();
            char status[256];
            std::snprintf(status, sizeof(status), "Running: %s — %s (%d/%d classes)",
                aimWeaponGroupLabel(cal.currentGroup()),
                cal.phase() == CalibPhase::Rcs ? "RCS spray" :
                cal.phase() == CalibPhase::Flick ? "Flick targets" : "Equip weapon",
                cal.groupIndex() + 1, cal.groupCount());
            m_gui.label(status, Theme::ACCENT, 14.f);
            if (m_gui.accentButton("aimCalStop", "Cancel calibration"))
                AimCalibration::instance().cancelCalibration();
        } else {
            if (m_gui.accentButton("aimCalStart", g_cfg.calibrationFrameworkComplete
                    ? "Re-run full calibration" : "Start full calibration"))
                AimCalibration::instance().startFullCalibration();
            if (g_cfg.calibrationFrameworkComplete)
                m_gui.label("All weapon classes calibrated. Settings applied per group.", Theme::TEXT_MUTED, 13.f);
            m_gui.dummy(6.f);
            if (m_gui.accentButton("aimCalSave", "Save calibration")) {
                std::filesystem::create_directories(calibDirPath());
                const std::string stampName = makeDefaultCalibrationName();
                const auto stampPath = calibDirPath() / stampName;
                const auto defaultPath = defaultCalibrationPath();
                if (writeCalibrationFile(stampPath, g_cfg) && writeCalibrationFile(defaultPath, g_cfg)) {
                    m_aimCalibStatus = "Saved " + stampName + " + aim_calibration.json";
                } else {
                    m_aimCalibStatus = "Failed to save calibration";
                }
                m_aimCalibStatusTimer = 3.f;
            }
            if (m_gui.accentButton("aimCalLoad", "Load calibration")) {
                if (readCalibrationFile(defaultCalibrationPath(), g_cfg)) {
                    m_aimCalibStatus = "Loaded aim_calibration.json";
                } else {
                    m_aimCalibStatus = "No calibration file found (configs/calibration/)";
                }
                m_aimCalibStatusTimer = 3.f;
            }
            if (m_aimCalibStatusTimer > 0.f)
                m_gui.label(m_aimCalibStatus.c_str(), Theme::ACCENT, 13.f);
        }
        m_gui.dummy(6.f);
        for (int i = 0; i < kCalibrationGroupCount; ++i) {
            const auto group = kCalibrationGroups[i];
            const auto& gp = g_cfg.aimCalibByGroup[aimGroupIndex(group)];
            if (!gp.valid) {
                char line[96];
                std::snprintf(line, sizeof(line), "%s — not calibrated", aimWeaponGroupLabel(group));
                m_gui.label(line, Theme::TEXT_MUTED, 13.f);
                continue;
            }
            char line[320];
            if (weaponGroupSupportsRcs(group)) {
                std::snprintf(line, sizeof(line),
                    "%s: smooth %.1f  RT %.0fms  RCS %.2f/%.2f  acc %.0f%%",
                    aimWeaponGroupLabel(group), gp.assistSmooth, gp.assistReactionMs,
                    gp.rcsX, gp.rcsY, gp.accuracy * 100.f);
            } else {
                std::snprintf(line, sizeof(line),
                    "%s: smooth %.1f  RT %.0fms  RCS off  acc %.0f%%",
                    aimWeaponGroupLabel(group), gp.assistSmooth, gp.assistReactionMs,
                    gp.accuracy * 100.f);
            }
            m_gui.label(line, Theme::TEXT_MUTED, 13.f);
        }
        if (!g_cfg.calibrationFrameworkComplete && !AimCalibration::instance().isActive()) {
            m_gui.dummy(4.f);
            m_gui.label("Complete calibration once to auto-tune all weapon groups.", Theme::TEXT_MUTED, 13.f);
        }
        m_gui.dummy(8.f);
    } else {
        m_aimWeaponGroupIdx = m_aimSubTab - 1;
        AimGroupConfig& aimCfg = g_cfg.aimByWeaponGroup[static_cast<std::size_t>(m_aimWeaponGroupIdx)];
        sideModelCfg = &aimCfg;

        m_gui.label("TARGETING", Theme::TEXT_MUTED, 16.f);
        m_gui.dummy(8.f);
        m_gui.label("Use the hitbox panel to toggle aim points.", Theme::TEXT_MUTED, 14.f);
        m_gui.dummy(6.f);

        const std::string groupSuffix = std::to_string(m_aimWeaponGroupIdx);
        const float rowY = m_gui.cursorY();
        MenuColumnLayout aimCols(contentX, contentW, rowY, S(14.f));
        aimCols.beginLeft(m_gui);
        m_gui.sliderFloatValue("aimFov", "FOV radius", &aimCfg.aimFov, 0.5f, 30.f, "%.0f°", 74.f);
        aimCols.syncLeft(m_gui.cursorY());
        aimCols.beginRight(m_gui);
        m_gui.sliderFloatValue("aimSmooth", "Smoothing", &aimCfg.aimSmooth, 1.f, 20.f, "%.1f", 74.f);
        aimCols.syncRight(m_gui.cursorY());
        aimCols.end(m_gui);

        const auto weaponGroup = static_cast<AimWeaponGroup>(m_aimWeaponGroupIdx);
        if (weaponGroupSupportsRcs(weaponGroup)) {
            m_gui.dummy(6.f);
            m_gui.label("RCS", Theme::TEXT_MUTED, 16.f);
            m_gui.dummy(6.f);
            static const char* kRcsModes[] = { "Aim only", "Standalone" };
            m_gui.toggleCheckbox(("aimRcsEnabled_" + groupSuffix).c_str(), "RCS", &aimCfg.rcsEnabled, 44.f);
            int rcsMode = aimCfg.rcsMode;
            if (rcsMode < 0) rcsMode = 0;
            if (rcsMode > 1) rcsMode = 1;
            if (m_gui.comboBox(("aimRcsMode_" + groupSuffix).c_str(), kRcsModes, 2, &rcsMode))
                aimCfg.rcsMode = rcsMode;

            const float rcsY = m_gui.cursorY();
            MenuColumnLayout rcsCols(contentX, contentW, rcsY, S(14.f));
            rcsCols.beginLeft(m_gui);
            m_gui.sliderFloatValue(("aimRcsX_" + groupSuffix).c_str(), "RCS X", &aimCfg.rcsX, 0.f, 1.25f, "%.2f", 74.f);
            m_gui.sliderFloatValue(("aimRcsY_" + groupSuffix).c_str(), "RCS Y", &aimCfg.rcsY, 0.f, 1.25f, "%.2f", 74.f);
            rcsCols.syncLeft(m_gui.cursorY());
            rcsCols.beginRight(m_gui);
            m_gui.sliderFloatValue(("aimRcsSmooth_" + groupSuffix).c_str(), "RCS smooth", &aimCfg.rcsSmooth, 0.f, 20.f, "%.1f", 74.f);
            rcsCols.syncRight(m_gui.cursorY());
            rcsCols.end(m_gui);
        } else {
            aimCfg.rcsEnabled = false;
        }

        m_gui.separator();
        m_gui.label("TRIGGER", Theme::TEXT_MUTED, 16.f);
        m_gui.dummy(8.f);
        m_gui.toggleCheckbox(("aimTrigEnabled_" + groupSuffix).c_str(), "Triggerbot", &aimCfg.triggerEnabled, 44.f);
        float trigDelay = static_cast<float>(aimCfg.triggerDelayMs);
        m_gui.sliderFloatValue(("aimTrigDelay_" + groupSuffix).c_str(), "Trigger delay (ms)", &trigDelay, 0.f, 100.f, "%.0f", 74.f);
        aimCfg.triggerDelayMs = static_cast<int>(trigDelay + 0.5f);
        m_gui.keybindCard(("aimTrigKey_" + groupSuffix).c_str(), "Trigger key", &aimCfg.triggerKey, 58.f);

        m_gui.dummy(8.f);
        m_gui.label("Bone offset and head-forward are in Global settings.", Theme::TEXT_MUTED, 14.f);
    }

    m_gui.dummy(12.f);

    float contentHeight = m_gui.cursorY() - (clipY - m_aimbotScroll);
    r.clearClipRect();

    m_aimbotMaxScroll = (std::max)(0.f, contentHeight - clipH);
    if (m_aimbotScrollTarget < 0.f) m_aimbotScrollTarget = 0.f;
    if (m_aimbotScrollTarget > m_aimbotMaxScroll) m_aimbotScrollTarget = m_aimbotMaxScroll;
    if (m_aimbotScroll < 0.f) m_aimbotScroll = 0.f;
    if (m_aimbotScroll > m_aimbotMaxScroll) m_aimbotScroll = m_aimbotMaxScroll;

    if (m_aimbotMaxScroll > 0.5f) {
        float trackX = m_winX + m_winW - S(5.f);
        r.drawRoundedFilledRect(trackX, clipY, S(2.f), clipH, 0xFF1B1C28, S(1.f));
        float thumbH = (std::max)(S(80.f), clipH * (clipH / (contentHeight + 0.001f)));
        float t = m_aimbotScroll / m_aimbotMaxScroll;
        float thumbY = clipY + (clipH - thumbH) * t;
        r.drawRoundedFilledRect(trackX, thumbY, S(2.f), thumbH, Theme::ACCENT, S(1.f));
    }

    if (sideModelCfg)
        drawAimHitboxWindow(*sideModelCfg);
}

void Menu::drawCalibrationPromptOverlay() {
    if (!AimCalibration::instance().needsSetupPrompt())
        return;

    m_gui.setModalInputBlocked(false);

    auto& r = m_gui.renderer();
    auto& f = m_gui.font();
    const float s = m_uiScale;
    const float c = m_contentScale;
    auto S = [s](float v) { return v * s; };

    const float corner = Theme::CORNER_RADIUS * s;

    r.drawRoundedFilledRect(m_winX, m_winY, m_winW, m_winH, 0xE8050508, corner);

    const float modalW = S(480.f);
    const float pad = S(24.f);
    const float btnW = modalW - pad * 2.f;
    const float titleSize = 21.f * c;
    const float bodySize = 14.5f * c;
    const float lineH = S(20.f);
    const float btnH1 = S(42.f);
    const float skipGap = S(18.f);
    const float textBtnGap = S(44.f);
    const float bodyLines = 4.f;
    const float modalH = pad + S(28.f) + S(12.f) + bodyLines * lineH + textBtnGap
        + btnH1 + skipGap + bodySize * 1.2f + pad * 0.75f;
    const float mx = m_winX + (m_winW - modalW) * 0.5f;
    const float my = m_winY + (m_winH - modalH) * 0.5f;

    r.drawRoundedFilledRect(mx, my, modalW, modalH, 0xFF12141C, S(14.f));
    r.drawRoundedRect(mx, my, modalW, modalH, Theme::BORDER, S(14.f), (std::max)(1.f, s));

    float ty = my + pad;
    r.drawText(f, mx + pad, ty, "Aim calibration recommended", Theme::TEXT, titleSize);
    ty += S(28.f) + S(12.f);

    r.drawText(f, mx + pad, ty,
        "Calibrate once for all weapon classes (pistol, rifle, sniper, SMG, heavy).",
        Theme::TEXT_MUTED, bodySize);
    ty += lineH;
    r.drawText(f, mx + pad, ty,
        "Flicks tune smoothing and humanization for tap fire and flicks.",
        Theme::TEXT_MUTED, bodySize);
    ty += lineH;
    r.drawText(f, mx + pad, ty,
        "Spray phase tunes RCS for SMG, rifles, and LMG.",
        Theme::TEXT_MUTED, bodySize);
    ty += lineH;
    r.drawText(f, mx + pad, ty,
        "Use a private server or offline map. Combat pauses during calibration.",
        Theme::TEXT_MUTED, bodySize);

    ty += textBtnGap;
    m_gui.setCursor(mx + pad, ty);
    m_gui.setItemWidth(btnW);
    if (m_gui.accentButton("calPromptStart", "Start calibration now", 0.f, btnH1)) {
        AimCalibration::instance().startFullCalibration();
        m_gui.consumeMouseClick();
    }

    ty += btnH1 + skipGap;
    const float skipW = measureRenderedTextWidth(r, f, "skip for now", bodySize);
    const float skipX = mx + (modalW - skipW) * 0.5f;
    if (drawTextLink(m_gui, r, f, skipX, ty, "skip for now", nullptr, bodySize, 0xFFFFFFFFu, false, false)) {
        AimCalibration::instance().dismissSetupPrompt();
        m_gui.consumeMouseClick();
    }

    m_gui.setModalInputBlocked(AimCalibration::instance().needsSetupPrompt());
}

void Menu::drawLeetifyAttribution(float x, float& y, float /*maxW*/) {
    auto& r = m_gui.renderer();
    auto& f = m_gui.font();
    const float s = m_uiScale;
    const float c = m_contentScale;
    auto S = [s](float v) { return v * s; };

    if (m_leetifyBadgeSrv && m_leetifyBadgeW > 0 && m_leetifyBadgeH > 0) {
        const float badgeH = S(30.f);
        const float badgeW = badgeH * static_cast<float>(m_leetifyBadgeW) / static_cast<float>(m_leetifyBadgeH);
        const bool badgeHovered = m_gui.mouseX() >= x && m_gui.mouseX() <= x + badgeW
                               && m_gui.mouseY() >= y && m_gui.mouseY() <= y + badgeH;
        if (badgeHovered && m_gui.mouseClicked())
            openExternalUrl(kLeetifyHomeUrl);
        r.drawImage(m_leetifyBadgeSrv.Get(), x, y, badgeW, badgeH);
        y += badgeH + S(4.f);
    } else {
        drawTextLink(m_gui, r, f, x, y, "Data Provided by Leetify", kLeetifyHomeUrl, 13.f * c, Theme::TEXT_MUTED);
        y += S(20.f);
    }
}

bool Menu::drawLeetifyProfileLink(float x, float y, std::uint64_t steamId) {
    if (steamId == 0)
        return false;

    wchar_t url[128];
    std::swprintf(url, std::size(url), L"https://leetify.com/app/profile/%llu",
        static_cast<unsigned long long>(steamId));

    auto& r = m_gui.renderer();
    auto& f = m_gui.font();
    const float fontSize = 12.f * m_contentScale;
    return drawTextLink(m_gui, r, f, x, y, "View on Leetify", url, fontSize);
}

void Menu::drawLeetifySetupPromptOverlay() {
    if (m_activeTab != 5 || !PlayerScout::instance().needsSetupPrompt() || m_leetifyPromptDismissed)
        return;

    m_gui.setModalInputBlocked(false);

    if (!m_leetifyKeyBufInit) {
        PlayerScout::instance().ensureApiKeyLoaded();
        const std::string stored = PlayerScout::instance().storedApiKey();
        std::snprintf(m_leetifyKeyBuf, sizeof(m_leetifyKeyBuf), "%s", stored.c_str());
        m_leetifyKeyBufInit = true;
    }

    auto& r = m_gui.renderer();
    auto& f = m_gui.font();
    const float s = m_uiScale;
    const float c = m_contentScale;
    auto S = [s](float v) { return v * s; };

    const float corner = Theme::CORNER_RADIUS * s;
    r.drawRoundedFilledRect(m_winX, m_winY, m_winW, m_winH, 0xE8050508, corner);

    const float modalW = S(520.f);
    const float pad = S(24.f);
    const float btnW = modalW - pad * 2.f;
    const float titleSize = 21.f * c;
    const float bodySize = 14.f * c;
    const float lineH = S(20.f);
    const float fieldH = S(40.f);
    const float btnH = S(42.f);
    const float textBtnGap = S(24.f);
    const float skipGap = S(14.f);
    const float modalH = pad + S(28.f) + S(12.f) + lineH * 4.f + S(10.f) + fieldH + textBtnGap
        + btnH + skipGap + S(22.f) + pad;
    const float mx = m_winX + (m_winW - modalW) * 0.5f;
    const float my = m_winY + (m_winH - modalH) * 0.5f;

    r.drawRoundedFilledRect(mx, my, modalW, modalH, 0xFF12141C, S(14.f));
    r.drawRoundedRect(mx, my, modalW, modalH, Theme::BORDER, S(14.f), (std::max)(1.f, s));

    float ty = my + pad;
    r.drawText(f, mx + pad, ty, "Leetify API key required", Theme::TEXT, titleSize);
    ty += S(28.f) + S(12.f);

    r.drawText(f, mx + pad, ty,
        "Player stats on this tab use the free Leetify public API.",
        Theme::TEXT_MUTED, bodySize);
    ty += lineH;

    r.drawText(f, mx + pad, ty,
        "Developers need to sign their requests with an API key which can be obtained at",
        Theme::TEXT_MUTED, bodySize);
    ty += lineH;

    const float linkX = mx + pad;
    const float linkY = ty;
    drawTextLink(m_gui, r, f, linkX, linkY, "leetify.com/app/developer", kLeetifyDeveloperUrl, bodySize, Theme::TEXT_MUTED);
    ty += lineH;

    r.drawText(f, mx + pad, ty,
        "Register for free, then paste your key below.",
        Theme::TEXT_MUTED, bodySize);
    ty += lineH + S(10.f);

    m_gui.setCursor(mx + pad, ty);
    m_gui.setItemWidth(btnW);
    m_gui.textField("leetifyPromptKey", m_leetifyKeyBuf, sizeof(m_leetifyKeyBuf), fieldH, true);

    ty += fieldH + textBtnGap;
    m_gui.setCursor(mx + pad, ty);
    m_gui.setItemWidth(btnW);
    if (m_gui.accentButton("leetifyPromptSave", "Save & validate key", 0.f, btnH)) {
        if (m_leetifyKeyBuf[0] == '\0') {
            m_leetifyPromptStatus = "Paste your API key first";
            m_leetifyPromptStatusTimer = 4.f;
        } else if (PlayerScout::instance().applyKeyBuffer(m_leetifyKeyBuf, sizeof(m_leetifyKeyBuf))) {
            m_leetifyPromptStatus = "API key saved — player stats enabled";
            m_leetifyPromptStatusTimer = 4.f;
        } else if (PlayerScout::instance().apiKeyStatus() == PlayerScout::ApiKeyStatus::Invalid) {
            m_leetifyPromptStatus = "Invalid API key — check the value and try again";
            m_leetifyPromptStatusTimer = 4.f;
        } else {
            m_leetifyPromptStatus = "Could not reach Leetify — check your connection and try again";
            m_leetifyPromptStatusTimer = 4.f;
        }
    }

    ty += btnH + skipGap;
    const float skipW = measureRenderedTextWidth(r, f, "skip for now", bodySize);
    const float skipX = mx + (modalW - skipW) * 0.5f;
    if (drawTextLink(m_gui, r, f, skipX, ty, "skip for now", nullptr, bodySize, 0xFFFFFFFFu, false, false)) {
        m_leetifyPromptDismissed = true;
        m_gui.consumeMouseClick();
    }

    if (m_leetifyPromptStatusTimer > 0.f && !m_leetifyPromptStatus.empty()) {
        ty += S(22.f);
        const unsigned int statusCol = PlayerScout::instance().hasValidApiKey()
            ? Theme::TEXT_LINK : Theme::DESTRUCTIVE;
        r.drawText(f, mx + pad, ty, m_leetifyPromptStatus.c_str(), statusCol, 13.f * c);
    }
}

void Menu::drawAimHitboxWindow(AimGroupConfig& aimCfg) {
    auto& r = m_gui.renderer();
    const float s = m_uiScale;
    auto S = [s](float v) { return v * s; };

    float panelW = S(kPreviewPanelW);
    float panelH = (std::min)(S(kPreviewPanelH), m_winH - S(24.f));
    float panelGap = S(16.f);
    float panelX = m_winX + m_winW + panelGap;
    float panelY = m_winY + S(12.f);

    if (panelX + panelW > m_screenW - S(8.f))
        panelX = m_winX - panelGap - panelW;
    panelX = std::clamp(panelX, S(8.f), m_screenW - panelW - S(8.f));
    panelY = std::clamp(panelY, S(8.f), m_screenH - panelH - S(8.f));

    for (int i = 0; i < 5; ++i) {
        float spread = S(4.f + i * 4.2f);
        unsigned int alpha = (unsigned int)(25.f - i * 4.2f);
        r.drawRoundedFilledRect(
            panelX - spread,
            panelY - spread * 0.9f,
            panelW + spread * 2.f,
            panelH + spread * 2.05f,
            withAlpha(0xFF000000, alpha),
            S(12.f) + spread * 0.64f
        );
    }
    r.drawRoundedFilledRect(panelX, panelY, panelW, panelH, 0xF10A0D15, S(12.f));
    r.drawRoundedRect(panelX, panelY, panelW, panelH, 0xFF2B3041, S(12.f), (std::max)(1.f, s));
    r.drawText(m_gui.font(), panelX + S(14.f), panelY + S(12.f), "Hitbox selector", 0xFFE9ECFF, S(18.f));
    r.drawText(m_gui.font(), panelX + S(14.f), panelY + S(34.f), "Click zones to toggle", 0xFF9EA5C4, S(13.f));

    const float previewPad = S(8.f);
    const float previewX = panelX + previewPad;
    const float previewY = panelY + S(52.f);
    const float previewW = panelW - previewPad * 2.f;
    const float previewH = panelH - S(52.f) - S(kHitboxBottomReserve);
    r.drawRoundedFilledRect(previewX, previewY, previewW, previewH, 0xFF04070E, S(8.f));
    r.drawRoundedRect(previewX, previewY, previewW, previewH, 0xFF1E2434, S(8.f), (std::max)(1.f, s));

    float drawX = previewX + S(4.f);
    float drawY = previewY + S(4.f);
    float drawW = previewW - S(8.f);
    float drawH = previewH - S(8.f);
    r.setClipRect(previewX, previewY, previewW, previewH);
    if (m_hitboxModelSrv && m_hitboxModelW > 0 && m_hitboxModelH > 0) {
        float fit = (std::min)(drawW / (float)m_hitboxModelW, drawH / (float)m_hitboxModelH);
        fit *= 2.0f;
        float imgW = (float)m_hitboxModelW * fit;
        float imgH = (float)m_hitboxModelH * fit;
        drawX += (drawW - imgW) * 0.5f;
        drawY += (drawH - imgH) * 0.5f;
        drawW = imgW;
        drawH = imgH;
        r.drawImage(m_hitboxModelSrv.Get(), drawX, drawY, drawW, drawH);
    }

    struct HitNode {
        bool* value;
        float nx;
        float ny;
    };
    HitNode nodes[] = {
        { &aimCfg.hitboxHead, 0.50f, 0.15f },
        { &aimCfg.hitboxChest, 0.50f, 0.35f },
        { &aimCfg.hitboxStomach, 0.50f, 0.46f },
        { &aimCfg.hitboxPelvis, 0.50f, 0.56f },
        { &aimCfg.hitboxArms, 0.40f, 0.30f },
        { &aimCfg.hitboxArms, 0.60f, 0.30f },
        { &aimCfg.hitboxLegs, 0.37f, 0.79f },
        { &aimCfg.hitboxLegs, 0.63f, 0.79f },
    };

    static const char* kNodeLabels[] = { "Head", "Chest", "Stomach", "Pelvis", "L Arm", "R Arm", "L Leg", "R Leg" };
    for (int ni = 0; ni < static_cast<int>(sizeof(nodes) / sizeof(nodes[0])); ++ni) {
        const HitNode& node = nodes[ni];
        float x = drawX + drawW * node.nx;
        float y = drawY + drawH * node.ny;
        float dx = m_gui.mouseX() - x;
        float dy = m_gui.mouseY() - y;
        const float hitR = S(13.f);
        bool hovered = (dx * dx + dy * dy) <= (hitR * hitR);
        if (hovered && m_gui.mouseClicked())
            *node.value = !*node.value;

        const float activeT = animValue(hashAnimId("hitNode", x, y) ^ static_cast<uint32_t>(ni),
                                        *node.value ? 1.f : 0.f, 0.24f, m_frameDt);
        const float hoverT = animValue(hashAnimId("hitNodeH", x, y) ^ static_cast<uint32_t>(ni),
                                       hovered ? 1.f : 0.f, 0.28f, m_frameDt);

        const float radius = S(10.f) + hoverT * S(1.5f);
        unsigned int fill = lerpColor(0x332A3144, withAlpha(Theme::ACCENT, 210), activeT);
        unsigned int ring = lerpColor(0xFF78829A, 0xFFE9ECFF, activeT * 0.85f + hoverT * 0.15f);
        r.drawFilledCircle(x, y, radius, fill);
        r.drawCircle(x, y, radius, ring, (std::max)(1.f, s * 1.1f));

        const float plus = radius * 0.42f;
        const float thick = (std::max)(1.2f, s * 1.15f);
        unsigned int plusCol = lerpColor(0xDDE9ECFF, 0xFF10131C, activeT * 0.55f);
        r.drawLine(x - plus, y, x + plus, y, plusCol, thick);
        r.drawLine(x, y - plus, x, y + plus, plusCol, thick);

        if (hovered || activeT > 0.5f) {
            const float labelW = std::strlen(kNodeLabels[ni]) * S(7.2f);
            r.drawRoundedFilledRect(x - labelW * 0.5f - S(4.f), y - radius - S(24.f), labelW + S(8.f), S(18.f),
                                    0xDD10131C, S(4.f));
            r.drawText(m_gui.font(), x - labelW * 0.5f, y - radius - S(22.f), kNodeLabels[ni], 0xFFE9ECFF, S(13.f));
        }
    }

    if (!aimCfg.hitboxHead && !aimCfg.hitboxChest && !aimCfg.hitboxStomach
        && !aimCfg.hitboxPelvis && !aimCfg.hitboxArms && !aimCfg.hitboxLegs)
        aimCfg.hitboxHead = true;

    r.clearClipRect();
}

void Menu::drawEspPanel() {
    auto& r = m_gui.renderer();
    const float s = m_uiScale;
    auto S = [s](float v) { return v * s; };

    float x = m_winX + m_sideW, y = m_winY;
    float w = m_winW - m_sideW;
    float pad = Theme::PADDING * s;

    drawPanelHeader(1);

    // ── Sub-tabs ──────────────────────────────────────────────────────────────
    constexpr int kPlayerTabCount = 4;
    const char* kPlayerTabs[kPlayerTabCount] = { "General", "Visible", "Occluded", "Style" };

    const float tabBarY = m_gui.cursorY();
    const float tabItemH = S(42.f);
    const float tabGap = S(8.f);
    const float tabBarX = x + pad;
    const float tabBarW = w - pad * 2.f;
    const float tabItemW = (tabBarW - tabGap * (kPlayerTabCount - 1)) / kPlayerTabCount;

    for (int i = 0; i < kPlayerTabCount; ++i) {
        const float tx = tabBarX + i * (tabItemW + tabGap);
        const bool active = (i == m_playerSubTab);
        m_playerSubTabAnim[(size_t)i] = advanceSubTabAnim(m_playerSubTabAnim[(size_t)i], active, m_frameDt);
        const float activeT = m_playerSubTabAnim[(size_t)i];
        const bool hovered = m_gui.mouseX() >= tx && m_gui.mouseX() <= tx + tabItemW
                          && m_gui.mouseY() >= tabBarY && m_gui.mouseY() <= tabBarY + tabItemH;
        const float hoverT = animValue(hashAnimId("espSubHover") ^ static_cast<uint32_t>(i),
                                     hovered ? 1.f : 0.f, 0.22f, m_frameDt);
        const float pressT = animValue(hashAnimId("espSubPress") ^ static_cast<uint32_t>(i),
                                     (hovered && m_gui.mouseDown()) ? 1.f : 0.f, 0.34f, m_frameDt);

        unsigned int inactiveBg = lerpColor(0xFF151822, 0xFF2A3148, hoverT * 0.12f);
        inactiveBg = lerpColor(inactiveBg, 0xFF0E1018, pressT * 0.35f);
        unsigned int activeBg = lerpColor(inactiveBg, 0xFF2A3148, 1.f);
        activeBg = lerpColor(activeBg, withAlpha(Theme::ACCENT, 26), 1.f);
        const unsigned int fg = lerpColor(0xFF9EA5C4, 0xFFE9ECFF, activeT * 0.95f + hoverT * 0.15f);

        drawSubTabSurface(r, tx, tabBarY, tabItemW, tabItemH, inactiveBg, activeBg, S(8.f), activeT, s);
        const float fontSize = S(15.f);
        float txtX = tx + (tabItemW - (std::strlen(kPlayerTabs[i]) * 7.8f * s)) * 0.5f;
        float txtY = textControlCenterY(m_gui.font(), tabBarY, tabItemH, kPlayerTabs[i], fontSize);
        r.drawText(m_gui.font(), txtX, txtY, kPlayerTabs[i], fg, fontSize);

        if (hovered && m_gui.mouseClicked())
            m_playerSubTab = i;
    }

    // ── Scroll region ────────────────────────────────────────────────────────
    float scrollY = tabBarY + tabItemH + 10.f * s;
    float clipX = x + pad;
    float clipY = scrollY;
    float clipW = w - pad * 2;
    float clipH = m_winH - (clipY - y) - pad;

    bool clipHovered = m_gui.mouseX() >= clipX && m_gui.mouseX() <= clipX + clipW
                    && m_gui.mouseY() >= clipY && m_gui.mouseY() <= clipY + clipH;

    if (clipHovered) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.f)
            m_playersScrollTarget -= wheel * S(42.f);
    }

    m_playersScroll = smoothValue(m_playersScroll, m_playersScrollTarget, 0.22f, m_frameDt);

    m_gui.setCursor(clipX, clipY - m_playersScroll);
    m_gui.setItemWidth(clipW - S(10.f));

    r.setClipRect(clipX, clipY, clipW, clipH);

    const bool isSettings = (m_playerSubTab == 0);
    const bool isVisible = (m_playerSubTab == 1);
    const bool isOcc = (m_playerSubTab == 2);
    const bool isSizes = (m_playerSubTab == 3);

    if (isSettings) {
        const float sectionY = m_gui.cursorY();
        MenuColumnLayout cols(clipX, clipW - S(10.f), sectionY, S(14.f));

        cols.beginLeft(m_gui);
        m_gui.label("General", Theme::TEXT, 20.f);
        m_gui.dummy(8.f);
        m_gui.toggleCheckbox("plEspEn", "ESP enabled", &g_cfg.espEnabled);
        m_gui.toggleCheckbox("plEnemyOnly", "Enemy only", &g_cfg.enemyOnly);
        m_gui.toggleCheckbox("plNameAvatar", "Steam avatar on name ESP", &g_cfg.nameEspAvatarEnabled);
        m_gui.toggleCheckbox("plVisCheck", "Visibility check", &g_cfg.visibilityCheckEnabled);
        static const char* kVisModes[] = { "Fast", "Balanced", "Strict" };
        int visMode = std::clamp(g_cfg.visibilityMode, 0, 2);
        m_gui.label("Visibility mode", Theme::TEXT_MUTED, 15.f);
        if (m_gui.comboBox("##plVisMode", kVisModes, 3, &visMode))
            g_cfg.visibilityMode = visMode;
        static const char* kVisBackends[] = { "BSP map (safe)", "Game TraceShape", "Auto" };
        int visBackend = std::clamp(g_cfg.visibilityBackend, 0, 2);
        m_gui.label("Visibility backend", Theme::TEXT_MUTED, 15.f);
        if (m_gui.comboBox("##plVisBackend", kVisBackends, 3, &visBackend))
            g_cfg.visibilityBackend = visBackend;
        cols.syncLeft(m_gui.cursorY());

        cols.beginRight(m_gui);
        m_gui.label("Team colors", Theme::TEXT, 20.f);
        m_gui.dummy(8.f);
        m_gui.colorEdit4("Enemy Color", g_cfg.enemyColor);
        m_gui.colorEdit4("Team Color", g_cfg.teamColor);
        m_gui.colorEdit4("Skeleton Color", g_cfg.skeletonColor);
        m_gui.colorEdit4("HP Low", g_cfg.hpBarLowColor);
        m_gui.colorEdit4("HP Full", g_cfg.hpBarFullColor);
        cols.syncRight(m_gui.cursorY());
        cols.end(m_gui);
    } else if (isVisible) {
        m_gui.label("Use the preview panel to place visible elements.", Theme::TEXT_MUTED, 15.f);
        m_gui.label("Adjust sizes and offsets in the Style tab.", Theme::TEXT_MUTED, 15.f);
        m_gui.separator();
        m_gui.label("Flags", Theme::TEXT, 20.f);
        m_gui.dummy(8.f);
        m_gui.toggleCheckbox("plFlagFlashedVis", "Flashed", &g_cfg.flagFlashedEnabled);
        m_gui.toggleCheckbox("plFlagDefusingVis", "Defusing", &g_cfg.flagDefusingEnabled);
        m_gui.toggleCheckbox("plFlagScopedVis", "Scoped", &g_cfg.flagScopedEnabled);
        m_gui.toggleCheckbox("plFlagKitVis", "Defuse kit", &g_cfg.flagDefuseKitEnabled);
    } else if (isOcc) {
        m_gui.label("Use the preview panel to place occluded elements.", Theme::TEXT_MUTED, 15.f);
        m_gui.label("Adjust sizes and offsets in the Style tab.", Theme::TEXT_MUTED, 15.f);
        m_gui.separator();
        m_gui.label("Flags", Theme::TEXT, 20.f);
        m_gui.dummy(8.f);
        m_gui.toggleCheckbox("plFlagFlashedOcc", "Flashed", &g_cfg.flagFlashedEnabled);
        m_gui.toggleCheckbox("plFlagDefusingOcc", "Defusing", &g_cfg.flagDefusingEnabled);
        m_gui.toggleCheckbox("plFlagScopedOcc", "Scoped", &g_cfg.flagScopedEnabled);
        m_gui.toggleCheckbox("plFlagKitOcc", "Defuse kit", &g_cfg.flagDefuseKitEnabled);
    } else if (isSizes) {
        const float sectionY = m_gui.cursorY();
        MenuColumnLayout cols(clipX, clipW - S(10.f), sectionY, S(14.f));

        cols.beginLeft(m_gui);
        m_gui.label("Style & sizing", Theme::TEXT, 20.f);
        m_gui.dummy(8.f);
        m_gui.sliderFloatValue("plBoxThk", "Box thickness", &g_cfg.boxThickness, 0.5f, 5.f, "%.1f");
        m_gui.sliderFloatValue("plBoxW", "Box width", &g_cfg.boxWidthScale, 0.35f, 2.5f, "%.2fx");
        m_gui.sliderFloatValue("plHpW", "HP bar width", &g_cfg.hpBarWidth, 1.f, 10.f, "%.0f");
        m_gui.sliderFloatValue("plChamsA", "Chams alpha", &g_cfg.chamsAlpha, 0.f, 1.f, "%.2f");
        static const char* kChamsStyles[] = { "Bone capsules", "Mesh (GLB)", "Silhouette (2D)" };
        m_gui.comboField("plChamsStyle", "Chams style", kChamsStyles, 3, &g_cfg.chamsStyle, 72.f);
        if (g_cfg.chamsStyle == 1) {
            static ChamsMeshLibrary s_chamsStatus;
            s_chamsStatus.initOnce();
            if (s_chamsStatus.ready())
                m_gui.label("Mesh models loaded.", Theme::TEXT_MUTED, 14.f);
            else {
                const std::string& msg = s_chamsStatus.statusMessage();
                m_gui.label(msg.empty() ? "Failed to load .glb meshes." : msg.c_str(), 0xFFFF8A65, 14.f);
            }
        }
        m_gui.sliderFloatValue("plInfoTextSz", "Info text size", &g_cfg.infoTextSize, 10.f, 24.f, "%.1f");
        cols.syncLeft(m_gui.cursorY());
        cols.end(m_gui);
    }

    if (isOcc) {
        m_gui.separator();
        m_gui.label("Occluded colors", Theme::TEXT, 20.f);
        m_gui.dummy(8.f);
        m_gui.colorEdit4("Box Occluded", g_cfg.boxOccludedColor);
        m_gui.colorEdit4("Skeleton Occluded", g_cfg.skeletonOccludedColor);
        m_gui.colorEdit4("HP Occluded", g_cfg.hpBarOccludedColor);
        m_gui.colorEdit4("Chams Occluded", g_cfg.chamsOccludedColor);
    } else if (isVisible) {
        m_gui.separator();
        m_gui.label("Visible colors", Theme::TEXT, 20.f);
        m_gui.dummy(8.f);
        m_gui.colorEdit4("Box Visible", g_cfg.boxVisibleColor);
        m_gui.colorEdit4("Skeleton Visible", g_cfg.skeletonVisibleColor);
        m_gui.colorEdit4("HP Visible", g_cfg.hpBarVisibleColor);
        m_gui.colorEdit4("Chams Visible", g_cfg.chamsVisibleColor);
    }

    float contentHeight = m_gui.cursorY() - (clipY - m_playersScroll);
    r.clearClipRect();

    m_playersMaxScroll = (std::max)(0.f, contentHeight - clipH);
    if (m_playersScrollTarget < 0.f) m_playersScrollTarget = 0.f;
    if (m_playersScrollTarget > m_playersMaxScroll) m_playersScrollTarget = m_playersMaxScroll;
    if (m_playersScroll < 0.f) m_playersScroll = 0.f;
    if (m_playersScroll > m_playersMaxScroll) m_playersScroll = m_playersMaxScroll;

    if (m_playersMaxScroll > 0.5f) {
        float trackX = clipX + clipW - S(5.f);
        r.drawRoundedFilledRect(trackX, clipY, S(3.f), clipH, 0xFF101015, S(2.f));
        float thumbH = (std::max)(S(36.f), clipH * (clipH / (contentHeight + 0.001f)));
        float t = m_playersScroll / m_playersMaxScroll;
        float thumbY = clipY + (clipH - thumbH) * t;
        r.drawRoundedFilledRect(trackX, thumbY, S(3.f), thumbH, 0xFF8E95E8, S(2.f));
    }

    if (isVisible || isOcc)
        drawPlayersEspPreviewWindow(isOcc);
}

void Menu::drawPlayersEspPreviewWindow(bool occluded) {
    auto& r = m_gui.renderer();
    const float s = m_uiScale;
    auto S = [s](float v) { return v * s; };

    float panelW = S(kPreviewPanelW);
    float panelH = (std::min)(S(kPreviewPanelH), m_winH - S(24.f));
    float panelGap = S(16.f);
    float panelX = m_winX + m_winW + panelGap;
    float panelY = m_winY + S(12.f);

    if (panelX + panelW > m_screenW - S(8.f))
        panelX = m_winX - panelGap - panelW;
    panelX = std::clamp(panelX, S(8.f), m_screenW - panelW - S(8.f));
    panelY = std::clamp(panelY, S(8.f), m_screenH - panelH - S(8.f));

    for (int i = 0; i < 5; ++i) {
        float spread = S(4.f + i * 4.2f);
        unsigned int alpha = (unsigned int)(25.f - i * 4.2f);
        r.drawRoundedFilledRect(
            panelX - spread,
            panelY - spread * 0.9f,
            panelW + spread * 2.f,
            panelH + spread * 2.05f,
            withAlpha(0xFF000000, alpha),
            S(12.f) + spread * 0.64f
        );
    }
    r.drawRoundedFilledRect(panelX, panelY, panelW, panelH, 0xF10A0D15, S(12.f));
    r.drawRoundedRect(panelX, panelY, panelW, panelH, 0xFF2B3041, S(12.f), (std::max)(1.f, s));

    const char* modeText = occluded ? "Occluded Preview" : "Visible Preview";
    r.drawText(m_gui.font(), panelX + S(14.f), panelY + S(12.f), "ESP preview", 0xFFE9ECFF, S(18.f));
    r.drawText(m_gui.font(), panelX + S(14.f), panelY + S(36.f), modeText, 0xFF9EA5C4, S(14.f));

    const float previewPad = S(10.f);
    const float previewX = panelX + previewPad;
    const float previewY = panelY + S(52.f);
    const float previewW = panelW - previewPad * 2.f;
    const float previewH = panelH - S(kPreviewBottomReserve);
    r.drawRoundedFilledRect(previewX, previewY, previewW, previewH, 0xFF04070E, S(8.f));
    r.drawRoundedRect(previewX, previewY, previewW, previewH, 0xFF1E2434, S(8.f), (std::max)(1.f, s));

    float drawX = previewX + S(6.f);
    float drawY = previewY + S(8.f);
    float drawW = previewW - S(12.f);
    float drawH = previewH - S(20.f);
    if (m_espPreviewModelSrv && m_espPreviewModelW > 0 && m_espPreviewModelH > 0) {
        float fit = (std::min)(drawW / (float)m_espPreviewModelW, drawH / (float)m_espPreviewModelH);
        fit *= 0.98f * kPreviewModelScale;
        float imgW = (float)m_espPreviewModelW * fit;
        float imgH = (float)m_espPreviewModelH * fit;
        drawX += (drawW - imgW) * 0.5f;
        drawY += (drawH - imgH) * 0.5f + S(2.f);
        drawX += S(kPreviewModelOffsetX);
        drawY += S(kPreviewModelOffsetY);
        drawW = imgW;
        drawH = imgH;
        r.drawImage(m_espPreviewModelSrv.Get(), drawX, drawY, drawW, drawH);
    }

    const float modelX = drawX;
    const float modelY = drawY;
    const float modelW = drawW;
    const float modelH = drawH;

    // Box frame tuned to the preview playermodel silhouette.
    const float simBoxX = modelX + modelW * kPreviewBoxX;
    const float simBoxY = modelY + modelH * kPreviewBoxY;
    const float widthScale = std::clamp(g_cfg.boxWidthScale, 0.35f, 2.5f);
    const float simBoxW = modelW * kPreviewBoxW * widthScale;
    const float simBoxH = modelH * kPreviewBoxH;

    r.setClipRect(previewX, previewY, previewW, previewH);

    struct SlotDef { int anchor; float x; float y; };
    SlotDef slots[] = {
        { 0, simBoxX + simBoxW * 0.5f, simBoxY - S(10.f) },
        { 1, simBoxX + simBoxW * 0.5f, simBoxY + simBoxH + S(6.f) },
        { 2, simBoxX - S(8.f), simBoxY + simBoxH * 0.5f },
        { 3, simBoxX + simBoxW + S(8.f), simBoxY + simBoxH * 0.5f },
        { 4, simBoxX - S(8.f), simBoxY - S(8.f) },
        { 5, simBoxX + simBoxW + S(8.f), simBoxY - S(8.f) },
        { 6, simBoxX - S(8.f), simBoxY + simBoxH + S(6.f) },
        { 7, simBoxX + simBoxW + S(8.f), simBoxY + simBoxH + S(6.f) },
    };

    enum EspPreviewElement : int {
        kPreviewBox = 0,
        kPreviewSkeleton,
        kPreviewChams,
        kPreviewHpBar,
        kPreviewHpText,
        kPreviewName,
        kPreviewWeaponText,
        kPreviewWeaponIcon,
        kPreviewArmorBar,
        kPreviewArmorText,
        kPreviewAmmoBar,
        kPreviewAmmoText,
        kPreviewFlags,
        kPreviewCount
    };

    enum EspPreviewPlaceGroup : int {
        kPlaceAny = 0,
        kPlaceBar4,
        kPlaceInfo6
    };

    struct ElemDef {
        const char* label;
        int* visAnchor;
        int* occAnchor;
        bool* visToggle;
        bool* occToggle;
        bool* globalToggle;
        bool* featureToggle;
        bool* masterToggle; // per-category enable (weaponEspEnabled etc.)
        bool bar;
        unsigned int col;
        int placeGroup;
    };

    ElemDef defs[kPreviewCount] = {
        { "Box", nullptr, nullptr, &g_cfg.boxEnabled, &g_cfg.boxOccluded, &g_cfg.espEnabled, nullptr, nullptr, false, 0xFFD36B6B, kPlaceAny },
        { "Skeleton", nullptr, nullptr, &g_cfg.skeletonEnabled, &g_cfg.skeletonOccluded, &g_cfg.espEnabled, nullptr, nullptr, false, 0xFFF1E66A, kPlaceAny },
        { "Chams", nullptr, nullptr, &g_cfg.chamsEnabled, &g_cfg.chamsOccluded, &g_cfg.espEnabled, nullptr, nullptr, false, 0xFFD36B6B, kPlaceAny },
        { "HP Bar", &g_cfg.hpBarPosVisible, &g_cfg.hpBarPosOccluded, &g_cfg.hpBarEnabled, &g_cfg.hpBarOccluded, &g_cfg.espEnabled, nullptr, nullptr, true, 0xFF34FF6A, kPlaceBar4 },
        { "HP Text", &g_cfg.hpTextPosVisible, &g_cfg.hpTextPosOccluded, &g_cfg.hpTextVisibleEnabled, &g_cfg.hpTextOccludedEnabled, &g_cfg.espEnabled, &g_cfg.hpTextEnabled, nullptr, false, 0xFF9DFBB7, kPlaceInfo6 },
        { "Name", &g_cfg.namePosVisible, &g_cfg.namePosOccluded, nullptr, nullptr, &g_cfg.espEnabled, &g_cfg.nameEspEnabled, nullptr, false, 0xFFE9ECFF, kPlaceInfo6 },
        { "Weapon Text", &g_cfg.weaponTextPosVisible, &g_cfg.weaponTextPosOccluded, &g_cfg.weaponVisibleEnabled, &g_cfg.weaponOccludedEnabled, &g_cfg.espEnabled, &g_cfg.weaponTextEnabled, &g_cfg.weaponEspEnabled, false, 0xFFE9ECFF, kPlaceInfo6 },
        { "Weapon Icon", &g_cfg.weaponIconPosVisible, &g_cfg.weaponIconPosOccluded, &g_cfg.weaponVisibleEnabled, &g_cfg.weaponOccludedEnabled, &g_cfg.espEnabled, &g_cfg.weaponIconEnabled, &g_cfg.weaponEspEnabled, false, 0xFFC5D5FF, kPlaceInfo6 },
        { "Armor Bar", &g_cfg.armorBarPosVisible, &g_cfg.armorBarPosOccluded, &g_cfg.armorVisibleEnabled, &g_cfg.armorOccludedEnabled, &g_cfg.espEnabled, &g_cfg.armorBarEnabled, &g_cfg.armorEspEnabled, true, 0xFF63BBFF, kPlaceBar4 },
        { "Armor Text", &g_cfg.armorTextPosVisible, &g_cfg.armorTextPosOccluded, &g_cfg.armorVisibleEnabled, &g_cfg.armorOccludedEnabled, &g_cfg.espEnabled, &g_cfg.armorTextEnabled, &g_cfg.armorEspEnabled, false, 0xFFA9D8FF, kPlaceInfo6 },
        { "Ammo Bar", &g_cfg.ammoBarPosVisible, &g_cfg.ammoBarPosOccluded, &g_cfg.ammoVisibleEnabled, &g_cfg.ammoOccludedEnabled, &g_cfg.espEnabled, &g_cfg.ammoBarEnabled, &g_cfg.ammoEspEnabled, true, 0xFFFFD563, kPlaceBar4 },
        { "Ammo Text", &g_cfg.ammoTextPosVisible, &g_cfg.ammoTextPosOccluded, &g_cfg.ammoVisibleEnabled, &g_cfg.ammoOccludedEnabled, &g_cfg.espEnabled, &g_cfg.ammoTextEnabled, &g_cfg.ammoEspEnabled, false, 0xFFFFE8A8, kPlaceInfo6 },
        { "Flags", &g_cfg.flagsPosVisible, &g_cfg.flagsPosOccluded, &g_cfg.flagsVisibleEnabled, &g_cfg.flagsOccludedEnabled, &g_cfg.espEnabled, &g_cfg.flagsEspEnabled, nullptr, false, 0xFFE9ECFF, kPlaceInfo6 },
    };

    if (!m_espPreviewResetDone) {
        m_espPreviewResetDone = true;
    }

    auto clampAnchorForDef = [&](const ElemDef& def, int anchor) {
        int a = clampEspAnchor(anchor);
        if (def.placeGroup == kPlaceBar4) {
            if (a >= 4)
                a = (a == 4 || a == 6) ? 2 : 3;
        } else if (def.placeGroup == kPlaceInfo6) {
            if (a == 2) a = 4;
            else if (a == 3) a = 5;
        }
        return a;
    };

    static bool s_animInit = false;
    static float s_animPos[2][kPreviewCount][2]{};
    static bool s_dragFromPreview = false;
    const int modeIndex = occluded ? 1 : 0;
    if (!s_animInit) {
        for (int m = 0; m < 2; ++m) {
            for (int i = 0; i < kPreviewCount; ++i) {
                if (defs[i].visAnchor && defs[i].occAnchor) {
                    int stored = (m == 0 ? *defs[i].visAnchor : *defs[i].occAnchor);
                    int anchor = clampAnchorForDef(defs[i], stored < 0 ? 0 : stored);
                    s_animPos[m][i][0] = slots[anchor].x;
                    s_animPos[m][i][1] = slots[anchor].y;
                } else {
                    s_animPos[m][i][0] = modelX + modelW * 0.5f;
                    s_animPos[m][i][1] = modelY + modelH * 0.45f;
                }
            }
        }
        s_animInit = true;
    }

    const bool previewHovered = m_gui.mouseX() >= previewX && m_gui.mouseX() <= previewX + previewW
        && m_gui.mouseY() >= previewY && m_gui.mouseY() <= previewY + previewH;

    auto isModeEnabled = [&](int idx, int mode) -> bool {
        const bool modeEnabled = (mode == 0)
            ? (defs[idx].visToggle ? *defs[idx].visToggle : true)
            : (defs[idx].occToggle ? *defs[idx].occToggle : true);
        const bool globalEnabled = defs[idx].globalToggle ? *defs[idx].globalToggle : true;
        const bool featureEnabled = defs[idx].featureToggle ? *defs[idx].featureToggle : true;
        const bool placed = (!defs[idx].visAnchor || !defs[idx].occAnchor)
            ? true
            : (((mode == 0 ? *defs[idx].visAnchor : *defs[idx].occAnchor) >= 0));
        return modeEnabled && globalEnabled && featureEnabled && placed;
    };

    auto setPlaced = [&](int idx, int mode, int anchorOrMinusOne) {
        if (defs[idx].visAnchor && defs[idx].occAnchor) {
            int* outAnchor = (mode == 0) ? defs[idx].visAnchor : defs[idx].occAnchor;
            *outAnchor = (anchorOrMinusOne < 0) ? -1 : clampAnchorForDef(defs[idx], anchorOrMinusOne);
        }
        if (anchorOrMinusOne >= 0) {
            if (mode == 0) {
                if (defs[idx].visToggle) *defs[idx].visToggle = true;
            } else {
                if (defs[idx].occToggle) *defs[idx].occToggle = true;
            }
            if (defs[idx].globalToggle)
                *defs[idx].globalToggle = true;
            if (defs[idx].featureToggle)
                *defs[idx].featureToggle = true;
            if (defs[idx].masterToggle)
                *defs[idx].masterToggle = true;
        } else {
            // For fixed elements (box/skeleton/chams), removal from preview should
            // disable that mode because there is no anchor sentinel.
            if (!defs[idx].visAnchor || !defs[idx].occAnchor) {
                if (mode == 0) {
                    if (defs[idx].visToggle) *defs[idx].visToggle = false;
                } else {
                    if (defs[idx].occToggle) *defs[idx].occToggle = false;
                }
            }
        }
    };

    auto placeRect = [&](int anchor, float w, float h, float stack[8], float& outX, float& outY) {
        const float gap = S(3.f);
        const float sideInset = S(6.f);
        switch (anchor) {
        case 0:
            outX = simBoxX + simBoxW * 0.5f - w * 0.5f;
            outY = simBoxY - h - gap - stack[0];
            stack[0] += h + S(1.f);
            break;
        case 1:
            outX = simBoxX + simBoxW * 0.5f - w * 0.5f;
            outY = simBoxY + simBoxH + gap + stack[1];
            stack[1] += h + S(1.f);
            break;
        case 2:
            outX = simBoxX - w - sideInset - stack[2];
            outY = simBoxY;
            stack[2] += w + S(2.f);
            break;
        case 3:
            outX = simBoxX + simBoxW + sideInset + stack[3];
            outY = simBoxY;
            stack[3] += w + S(2.f);
            break;
        case 4:
            outX = simBoxX - w - sideInset;
            outY = simBoxY - h - gap - stack[4];
            stack[4] += h + S(1.f);
            break;
        case 5:
            outX = simBoxX + simBoxW + sideInset;
            outY = simBoxY - h - gap - stack[5];
            stack[5] += h + S(1.f);
            break;
        case 6:
            outX = simBoxX - w - sideInset;
            outY = simBoxY + simBoxH + gap + stack[6];
            stack[6] += h + S(1.f);
            break;
        case 7:
        default:
            outX = simBoxX + simBoxW + sideInset;
            outY = simBoxY + simBoxH + gap + stack[7];
            stack[7] += h + S(1.f);
            break;
        }

        applyPreviewAnchorOffset(anchor, outX, outY);
    };

    // Fixed element visuals to make placement state obvious.
    if (isModeEnabled(kPreviewChams, modeIndex)) {
        unsigned int base = modeIndex == 0 ? rgbaToArgb(g_cfg.chamsVisibleColor) : rgbaToArgb(g_cfg.chamsOccludedColor);
        float a = std::clamp(g_cfg.chamsAlpha, 0.f, 1.f);
        unsigned int chCol = withAlpha(base, (unsigned int)(((base >> 24) & 0xFF) * a));
        r.drawRoundedFilledRect(simBoxX + S(2.f), simBoxY + S(2.f), simBoxW - S(4.f), simBoxH - S(4.f), chCol, S(4.f));
    }
    if (isModeEnabled(kPreviewBox, modeIndex)) {
        const float* boxCol = modeIndex == 0 ? g_cfg.boxVisibleColor : g_cfg.boxOccludedColor;
        r.drawRect(simBoxX, simBoxY, simBoxW, simBoxH, rgbaToArgb(boxCol), (std::max)(1.f, g_cfg.boxThickness));
    }
    float skeletonMinX = (std::numeric_limits<float>::max)();
    float skeletonMinY = (std::numeric_limits<float>::max)();
    float skeletonMaxX = (std::numeric_limits<float>::lowest)();
    float skeletonMaxY = (std::numeric_limits<float>::lowest)();
    bool skeletonBoundsValid = false;

    if (isModeEnabled(kPreviewSkeleton, modeIndex)) {
        const float* skCol = modeIndex == 0 ? g_cfg.skeletonVisibleColor : g_cfg.skeletonOccludedColor;
        unsigned int col = rgbaToArgb(skCol);
        float px = modelX;
        float py = modelY;
        float pw = modelW;
        float ph = modelH;

        Vec2 head{ px + pw * 0.50f, py + ph * 0.15f };
        Vec2 neck{ px + pw * 0.50f, py + ph * 0.23f };
        Vec2 chest{ px + pw * 0.50f, py + ph * 0.35f };
        Vec2 pelvis{ px + pw * 0.50f, py + ph * 0.56f };

        Vec2 lShoulder{ px + pw * 0.40f, py + ph * 0.30f };
        Vec2 lElbow{ px + pw * 0.30f, py + ph * 0.41f };
        Vec2 lHand{ px + pw * 0.24f, py + ph * 0.54f };

        Vec2 rShoulder{ px + pw * 0.60f, py + ph * 0.30f };
        Vec2 rElbow{ px + pw * 0.72f, py + ph * 0.41f };
        Vec2 rHand{ px + pw * 0.76f, py + ph * 0.54f };

        Vec2 lHip{ px + pw * 0.44f, py + ph * 0.58f };
        Vec2 lKnee{ px + pw * 0.37f, py + ph * 0.79f };
        Vec2 lFoot{ px + pw * 0.35f, py + ph * 0.96f };

        Vec2 rHip{ px + pw * 0.56f, py + ph * 0.58f };
        Vec2 rKnee{ px + pw * 0.63f, py + ph * 0.79f };
        Vec2 rFoot{ px + pw * 0.65f, py + ph * 0.96f };

        auto addPt = [&](const Vec2& p) {
            skeletonMinX = (std::min)(skeletonMinX, p.x);
            skeletonMinY = (std::min)(skeletonMinY, p.y);
            skeletonMaxX = (std::max)(skeletonMaxX, p.x);
            skeletonMaxY = (std::max)(skeletonMaxY, p.y);
        };
        for (const Vec2& p : { head, neck, chest, pelvis, lShoulder, lElbow, lHand, rShoulder, rElbow, rHand, lHip, lKnee, lFoot, rHip, rKnee, rFoot })
            addPt(p);
        skeletonBoundsValid = true;

        auto seg = [&](float ax, float ay, float bx, float by) {
            r.drawLine(ax, ay, bx, by, col, S(1.4f));
        };
        seg(head.x, head.y, neck.x, neck.y);
        seg(neck.x, neck.y, chest.x, chest.y);
        seg(chest.x, chest.y, pelvis.x, pelvis.y);

        seg(chest.x, chest.y, lShoulder.x, lShoulder.y);
        seg(lShoulder.x, lShoulder.y, lElbow.x, lElbow.y);
        seg(lElbow.x, lElbow.y, lHand.x, lHand.y);

        seg(chest.x, chest.y, rShoulder.x, rShoulder.y);
        seg(rShoulder.x, rShoulder.y, rElbow.x, rElbow.y);
        seg(rElbow.x, rElbow.y, rHand.x, rHand.y);

        seg(pelvis.x, pelvis.y, lHip.x, lHip.y);
        seg(lHip.x, lHip.y, lKnee.x, lKnee.y);
        seg(lKnee.x, lKnee.y, lFoot.x, lFoot.y);

        seg(pelvis.x, pelvis.y, rHip.x, rHip.y);
        seg(rHip.x, rHip.y, rKnee.x, rKnee.y);
        seg(rKnee.x, rKnee.y, rFoot.x, rFoot.y);
    }

    float anchorStack[8]{};
    struct DrawRect { bool valid = false; float x = 0.f, y = 0.f, w = 0.f, h = 0.f; };
    DrawRect dragRects[kPreviewCount];

    // Fixed items: drag directly from their rendered geometry.
    dragRects[kPreviewBox] = { true, simBoxX, simBoxY, simBoxW, simBoxH };
    dragRects[kPreviewChams] = { true, simBoxX + S(2.f), simBoxY + S(2.f), simBoxW - S(4.f), simBoxH - S(4.f) };
    if (skeletonBoundsValid) {
        const float pad = S(5.f);
        dragRects[kPreviewSkeleton] = {
            true,
            skeletonMinX - pad,
            skeletonMinY - pad,
            (skeletonMaxX - skeletonMinX) + pad * 2.f,
            (skeletonMaxY - skeletonMinY) + pad * 2.f
        };
    }

    auto pointInRect = [&](const DrawRect& rc, float mx, float my) {
        return rc.valid && mx >= rc.x && mx <= (rc.x + rc.w) && my >= rc.y && my <= (rc.y + rc.h);
    };

    const float mx = m_gui.mouseX();
    const float my = m_gui.mouseY();
    const bool hoverSk = isModeEnabled(kPreviewSkeleton, modeIndex) && pointInRect(dragRects[kPreviewSkeleton], mx, my);
    const bool hoverCh = isModeEnabled(kPreviewChams, modeIndex) && pointInRect(dragRects[kPreviewChams], mx, my);
    const bool insideBox = isModeEnabled(kPreviewBox, modeIndex) && pointInRect(dragRects[kPreviewBox], mx, my);
    const float edgeBand = S(6.f);
    const bool insideInnerBox = insideBox
        && mx >= (simBoxX + edgeBand) && mx <= (simBoxX + simBoxW - edgeBand)
        && my >= (simBoxY + edgeBand) && my <= (simBoxY + simBoxH - edgeBand);
    const bool hoverBox = insideBox && !insideInnerBox;

    if (hoverSk || (m_espPreviewDragging && m_espPreviewDragElement == kPreviewSkeleton)) {
        r.drawRoundedRect(dragRects[kPreviewSkeleton].x, dragRects[kPreviewSkeleton].y,
            dragRects[kPreviewSkeleton].w, dragRects[kPreviewSkeleton].h,
            0xFFAAC3FF, S(4.f), (std::max)(1.f, s));
    }
    if (hoverCh || (m_espPreviewDragging && m_espPreviewDragElement == kPreviewChams)) {
        r.drawRoundedRect(dragRects[kPreviewChams].x, dragRects[kPreviewChams].y,
            dragRects[kPreviewChams].w, dragRects[kPreviewChams].h,
            0xFFAAC3FF, S(4.f), (std::max)(1.f, s));
    }
    if (hoverBox || (m_espPreviewDragging && m_espPreviewDragElement == kPreviewBox)) {
        r.drawRect(simBoxX - S(1.f), simBoxY - S(1.f), simBoxW + S(2.f), simBoxH + S(2.f), 0xFFAAC3FF, (std::max)(1.f, s));
    }

    std::vector<int> drawIndices;
    for (int i = 0; i < kPreviewCount; ++i) drawIndices.push_back(i);
    const auto& orderArr = modeIndex == 0 ? g_cfg.espItemOrderVisible : g_cfg.espItemOrderOccluded;
    std::sort(drawIndices.begin(), drawIndices.end(), [&](int a, int b) {
        bool aFixed = (a < 3);
        bool bFixed = (b < 3);
        if (aFixed && !bFixed) return true;
        if (!aFixed && bFixed) return false;
        if (aFixed && bFixed) return a < b;
        
        int anchorA = (defs[a].visAnchor && defs[a].occAnchor) ? (modeIndex == 0 ? *defs[a].visAnchor : *defs[a].occAnchor) : -1;
        int anchorB = (defs[b].visAnchor && defs[b].occAnchor) ? (modeIndex == 0 ? *defs[b].visAnchor : *defs[b].occAnchor) : -1;
        int aNorm = anchorA < 0 ? 0 : clampAnchorForDef(defs[a], anchorA);
        int bNorm = anchorB < 0 ? 0 : clampAnchorForDef(defs[b], anchorB);
        if (aNorm != bNorm) return aNorm < bNorm;
        return orderArr[a - 3] < orderArr[b - 3];
    });

    for (int i : drawIndices) {
        if (defs[i].visAnchor && defs[i].occAnchor) {
            int stored = modeIndex == 0 ? *defs[i].visAnchor : *defs[i].occAnchor;
            const bool placed = (stored >= 0) && isModeEnabled(i, modeIndex);
            const float fade = animValue(hashAnimId(defs[i].label, panelX + i * 3.f, panelY + 11.f), placed ? 1.f : 0.f, 0.24f, m_frameDt);
            if (fade <= 0.01f)
                continue;

            int anchor = clampAnchorForDef(defs[i], stored < 0 ? 0 : stored);
            float tx = slots[anchor].x;
            float ty = slots[anchor].y;
            s_animPos[modeIndex][i][0] = smoothValue(s_animPos[modeIndex][i][0], tx, 0.28f, m_frameDt);
            s_animPos[modeIndex][i][1] = smoothValue(s_animPos[modeIndex][i][1], ty, 0.28f, m_frameDt);

            float px = 0.f;
            float py = 0.f;

            if (defs[i].bar) {
                const bool horizontal = (anchor == 0 || anchor == 1);
                float bw = horizontal ? S(104.f) : S(7.f);
                float bh = horizontal ? S(7.f) : S(208.f);
                if (horizontal) bw *= kPreviewBarLenHorizontal;
                else bh *= kPreviewBarLenVertical;
                placeRect(anchor, bw, bh, anchorStack, px, py);
                if (horizontal)
                    px = simBoxX + simBoxW * 0.5f - bw * 0.5f;

                px += (s_animPos[modeIndex][i][0] - tx);
                py += (s_animPos[modeIndex][i][1] - ty);

                unsigned int bg = withAlpha(0xAA000000u, (unsigned int)(120.f * fade));
                unsigned int fg = withAlpha(defs[i].col, (unsigned int)(255.f * fade));
                r.drawRoundedFilledRect(px, py, bw, bh, bg, S(2.5f));
                r.drawRoundedFilledRect(px + S(1.f), py + S(1.f), bw - S(2.f), bh - S(2.f), fg, S(2.5f));
                dragRects[i] = { true, px, py, bw, bh };
            } else {
                float txtSz = S(13.f);
                float txtW = std::strlen(defs[i].label) * (txtSz * 0.56f);
                placeRect(anchor, txtW, txtSz + S(1.f), anchorStack, px, py);

                px += (s_animPos[modeIndex][i][0] - tx);
                py += (s_animPos[modeIndex][i][1] - ty);

                unsigned int shadow = withAlpha(0xAA000000u, (unsigned int)(190.f * fade));
                unsigned int fg = withAlpha(defs[i].col, (unsigned int)(255.f * fade));
                r.drawText(m_gui.font(), px + S(1.f), py + S(1.f), defs[i].label, shadow, txtSz);
                r.drawText(m_gui.font(), px, py, defs[i].label, fg, txtSz);
                dragRects[i] = { true, px, py, txtW + S(3.f), txtSz + S(2.f) };
            }
        }
    }

    if (!m_espPreviewDragging && m_gui.mouseClicked() && previewHovered) {
        if (hoverSk) {
            m_espPreviewDragging = true;
            m_espPreviewDragElement = kPreviewSkeleton;
            s_dragFromPreview = true;
        } else if (hoverBox) {
            m_espPreviewDragging = true;
            m_espPreviewDragElement = kPreviewBox;
            s_dragFromPreview = true;
        } else if (hoverCh) {
            m_espPreviewDragging = true;
            m_espPreviewDragElement = kPreviewChams;
            s_dragFromPreview = true;
        }

        if (!m_espPreviewDragging) {
        for (int i = 0; i < kPreviewCount; ++i) {
            if (i == kPreviewBox || i == kPreviewSkeleton || i == kPreviewChams)
                continue;
            if (!dragRects[i].valid)
                continue;
            const bool inside = m_gui.mouseX() >= dragRects[i].x && m_gui.mouseX() <= dragRects[i].x + dragRects[i].w
                && m_gui.mouseY() >= dragRects[i].y && m_gui.mouseY() <= dragRects[i].y + dragRects[i].h;
            if (inside) {
                m_espPreviewDragging = true;
                m_espPreviewDragElement = i;
                s_dragFromPreview = true;
                break;
            }
        }
        }
    }

    r.clearClipRect();

    const float listY = previewY + previewH + S(10.f);
    r.drawText(m_gui.font(), panelX + S(12.f), listY, "Drag & Drop Elements", 0xFFE9ECFF, S(13.f));
    r.drawText(m_gui.font(), panelX + S(12.f), listY + S(16.f), "Drop in preview to place, drop in list to remove", 0xFF8D95B2, S(11.f));

    float chipX = panelX + S(12.f);
    float chipY = listY + S(34.f);
    const float chipH = S(24.f);
    const float chipRight = panelX + panelW - S(12.f);
    float listBottom = chipY;

    for (int i = 0; i < kPreviewCount; ++i) {
        const bool placed = isModeEnabled(i, modeIndex);
        const bool draggingThis = m_espPreviewDragging && m_espPreviewDragElement == i;
        if (placed) {
            if (!(draggingThis && s_dragFromPreview))
                continue;
        } else if (draggingThis && !s_dragFromPreview) {
            continue;
        }

        float chipW = S(20.f + std::strlen(defs[i].label) * 7.0f);
        if (chipX + chipW > chipRight) {
            chipX = panelX + S(12.f);
            chipY += chipH + S(6.f);
        }

        bool hover = m_gui.mouseX() >= chipX && m_gui.mouseX() <= chipX + chipW
            && m_gui.mouseY() >= chipY && m_gui.mouseY() <= chipY + chipH;
        unsigned int chipCol = hover ? 0xFF2E3650 : 0xFF1A1F2E;
        if (m_espPreviewDragging && m_espPreviewDragElement == i)
            chipCol = 0xFF3E4A73;
        r.drawRoundedFilledRect(chipX, chipY, chipW, chipH, chipCol, S(5.f));
        r.drawRoundedRect(chipX, chipY, chipW, chipH, 0xFF3D4560, S(5.f), (std::max)(1.f, s));
        r.drawText(m_gui.font(), chipX + S(8.f), chipY + S(5.f), defs[i].label, 0xFFE1E6FF, S(14.f));

        if (hover && m_gui.mouseClicked()) {
            m_espPreviewDragging = true;
            m_espPreviewDragElement = i;
            s_dragFromPreview = false;
        }

        chipX += chipW + S(6.f);
        listBottom = (std::max)(listBottom, chipY + chipH);
    }

    const bool listHovered = m_gui.mouseX() >= panelX + S(8.f) && m_gui.mouseX() <= panelX + panelW - S(8.f)
        && m_gui.mouseY() >= listY + S(30.f) && m_gui.mouseY() <= listBottom + S(8.f);

    if (m_espPreviewDragging && m_espPreviewDragElement >= 0 && m_espPreviewDragElement < kPreviewCount) {
        const ElemDef& def = defs[m_espPreviewDragElement];

        int bestAnchor = 0;

        float bestD2 = (std::numeric_limits<float>::max)();
        for (const SlotDef& slot : slots) {
            float dx = m_gui.mouseX() - slot.x;
            float dy = m_gui.mouseY() - slot.y;
            float d2 = dx * dx + dy * dy;
            if (d2 < bestD2) {
                bestD2 = d2;
                bestAnchor = slot.anchor;
            }
        }

        if (previewHovered && def.visAnchor && def.occAnchor) {
            const int previewAnchor = clampAnchorForDef(def, bestAnchor);
            const bool horizontal = (previewAnchor == 0 || previewAnchor == 1);
            float bw = horizontal ? S(104.f) : S(7.f);
            float bh = horizontal ? S(7.f) : S(208.f);
            if (def.bar) {
                if (horizontal) bw *= kPreviewBarLenHorizontal;
                else bh *= kPreviewBarLenVertical;
            }
            float px = m_gui.mouseX() - bw * 0.5f;
            float py = m_gui.mouseY() - bh * 0.5f;

            unsigned int pulse = withAlpha(0xFF9AB1FF, (unsigned int)(140.f + std::sin(m_uiTime * 8.f) * 55.f + 55.f));
            if (def.bar) {
                r.drawRoundedFilledRect(px, py, bw, bh, 0xA2000000, S(2.5f));
                r.drawRoundedFilledRect(px + S(1.f), py + S(1.f), bw - S(2.f), bh - S(2.f), def.col, S(2.5f));
                r.drawRoundedRect(px - S(2.f), py - S(2.f), bw + S(4.f), bh + S(4.f), pulse, S(4.f), (std::max)(1.f, s));
            } else {
                float txtSz = S(13.f);
                float txtW = std::strlen(def.label) * (txtSz * 0.56f);
                px = m_gui.mouseX() - txtW * 0.5f;
                py = m_gui.mouseY() - txtSz * 0.5f;
                r.drawText(m_gui.font(), px + S(1.f), py + S(1.f), def.label, 0xAA000000, txtSz);
                r.drawText(m_gui.font(), px, py, def.label, def.col, txtSz);
                r.drawRoundedRect(px - S(3.f), py - S(2.f), txtW + S(6.f), txtSz + S(5.f), pulse, S(4.f), (std::max)(1.f, s));
            }
        } else {
            const char* dragLabel = def.label;
            float w = S(20.f + std::strlen(dragLabel) * 7.0f);
            float h = chipH;
            float x = m_gui.mouseX() - w * 0.5f;
            float y = m_gui.mouseY() - h * 0.5f;
            r.drawRoundedFilledRect(x, y, w, h, 0xD03E4A73, S(5.f));
            r.drawRoundedRect(x, y, w, h, 0xFF7E8CD7, S(5.f), (std::max)(1.f, s));
            r.drawText(m_gui.font(), x + S(8.f), y + S(5.f), dragLabel, 0xFFFFFFFF, S(14.f));
        }

        if (!m_gui.mouseDown()) {
            if (listHovered) {
                setPlaced(m_espPreviewDragElement, modeIndex, -1);
            } else if (previewHovered) {
                if (defs[m_espPreviewDragElement].visAnchor && defs[m_espPreviewDragElement].occAnchor) {
                    s_animPos[modeIndex][m_espPreviewDragElement][0] = m_gui.mouseX();
                    s_animPos[modeIndex][m_espPreviewDragElement][1] = m_gui.mouseY();

                    int oldAnchor = (modeIndex == 0 ? *defs[m_espPreviewDragElement].visAnchor : *defs[m_espPreviewDragElement].occAnchor);
                    int newAnchor = clampAnchorForDef(defs[m_espPreviewDragElement], bestAnchor);
                    
                    auto& ord = modeIndex == 0 ? g_cfg.espItemOrderVisible : g_cfg.espItemOrderOccluded;
                    if (oldAnchor >= 0 && clampAnchorForDef(defs[m_espPreviewDragElement], oldAnchor) == newAnchor) {
                        std::vector<int> siblingIndices;
                        for(int idx = 3; idx < kPreviewCount; ++idx) {
                            if (idx == m_espPreviewDragElement) continue;
                            if (!isModeEnabled(idx, modeIndex)) continue;
                            int a = (modeIndex == 0 ? *defs[idx].visAnchor : *defs[idx].occAnchor);
                            if (a >= 0 && clampAnchorForDef(defs[idx], a) == newAnchor) {
                                siblingIndices.push_back(idx);
                            }
                        }
                        std::sort(siblingIndices.begin(), siblingIndices.end(), [&](int a, int b) {
                            return ord[a-3] < ord[b-3];
                        });
                        
                        int insertPos = 0;
                        float dropCoord = (newAnchor == 0 || newAnchor == 1 || newAnchor == 4 || newAnchor == 5 || newAnchor == 6 || newAnchor == 7) ? m_gui.mouseY() : m_gui.mouseX();
                        for(int sib : siblingIndices) {
                            float sibCoord = (newAnchor == 0 || newAnchor == 1 || newAnchor == 4 || newAnchor == 5 || newAnchor == 6 || newAnchor == 7) ? dragRects[sib].y + dragRects[sib].h*0.5f : dragRects[sib].x + dragRects[sib].w*0.5f;
                            if (dropCoord > sibCoord) insertPos++;
                        }
                        
                        siblingIndices.insert(siblingIndices.begin() + insertPos, m_espPreviewDragElement);
                        for(int k=0; k<siblingIndices.size(); ++k) {
                            ord[siblingIndices[k]-3] = k;
                        }
                    } else {
                        ord[m_espPreviewDragElement-3] = 99; 
                    }
                    
                    std::vector<int> allIndices;
                    for(int idx = 3; idx < kPreviewCount; ++idx) allIndices.push_back(idx);
                    std::sort(allIndices.begin(), allIndices.end(), [&](int a, int b) {
                        return ord[a-3] < ord[b-3];
                    });
                    for(int k=0; k<allIndices.size(); ++k) ord[allIndices[k]-3] = k;
                }

                setPlaced(m_espPreviewDragElement, modeIndex, bestAnchor);
            } else if (!s_dragFromPreview) {
                // Dragged from list but released elsewhere: no change.
            }

            m_espPreviewDragging = false;
            m_espPreviewDragElement = -1;
            s_dragFromPreview = false;
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Nades panel — lineup helper & sound ESP
// ═════════════════════════════════════════════════════════════════════════════

void Menu::drawNadesPanel(const EntityManager& em) {
    auto& r = m_gui.renderer();
    auto& f = m_gui.font();
    const float s = m_uiScale;
    const float c = m_contentScale;
    auto S = [s](float v) { return v * s; };
    auto C = [c](float v) { return v * c; };

    float x = m_winX + m_sideW, y = m_winY;
    float w = m_winW - m_sideW;
    float pad = Theme::PADDING * s;

    drawPanelHeader(3);

    float clipX = x + pad;
    float clipY = m_gui.cursorY();
    float clipW = w - pad * 2.f;
    float clipH = m_winH - (clipY - y) - pad;

    if (m_nadesStatusTimer > 0.f) {
        m_nadesStatusTimer = (std::max)(0.f, m_nadesStatusTimer - m_frameDt);
        if (!m_nadesStatus.empty())
            r.drawText(f, clipX, clipY - S(22.f), m_nadesStatus.c_str(), Theme::TEXT_MUTED, C(13.f));
    }

    bool clipHovered = m_gui.mouseX() >= clipX && m_gui.mouseX() <= clipX + clipW
                    && m_gui.mouseY() >= clipY && m_gui.mouseY() <= clipY + clipH;
    if (clipHovered) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.f)
            m_nadesScrollTarget -= wheel * S(36.f);
    }
    m_nadesScroll = smoothValue(m_nadesScroll, m_nadesScrollTarget, 0.22f, m_frameDt);

    r.setClipRect(clipX, clipY, clipW, clipH);
    m_gui.setCursor(clipX, clipY - m_nadesScroll);
    m_gui.setItemWidth(clipW);

    r.drawText(f, clipX, m_gui.cursorY(), "Lineup helper", Theme::TEXT, C(18.f));
    m_gui.advanceY(S(28.f));

    m_gui.toggleCheckbox("nadeHelp", "Grenade lineup helper", &g_cfg.grenadeHelperEnabled);
    m_gui.label("Stand markers, aim dots, and throw instructions in-game.", Theme::TEXT_MUTED, C(12.f));
    m_gui.dummy(S(8.f));

    const auto snap = em.publishedFrame();
    char mapBuf[96];
    std::snprintf(mapBuf, sizeof(mapBuf), "Current map: %s",
        (snap && !snap->currentMapName.empty()) ? snap->currentMapName.c_str() : "unknown");
    m_gui.label(mapBuf, Theme::TEXT_MUTED, C(13.f));

    const auto* pack = GrenadeLineupManager::instance().activePack();
    if (pack) {
        char packBuf[160];
        std::snprintf(packBuf, sizeof(packBuf), "Active pack: %s (%d spots)",
            pack->name.c_str(), static_cast<int>(pack->spots.size()));
        m_gui.label(packBuf, Theme::TEXT, C(14.f));
    } else {
        m_gui.label("No lineup pack loaded.", Theme::TEXT_MUTED, C(13.f));
    }

    m_gui.dummy(S(6.f));
    if (m_gui.accentButton("nadeSample", "Load local sample pack", clipW * 0.55f, S(36.f))) {
        const std::filesystem::path sample = std::filesystem::path("lineups") / "de_mirage_sample.json";
        std::string err;
        if (GrenadeLineupManager::instance().loadPackFromFile(sample.string(), err)) {
            g_cfg.grenadeLineupPackPath = sample.string();
            m_nadesStatus = "Loaded sample Mirage pack";
        } else {
            m_nadesStatus = err.empty() ? "Failed to load sample pack" : err;
        }
        m_nadesStatusTimer = 3.f;
    }

    m_gui.dummy(S(14.f));
    r.drawText(f, clipX, m_gui.cursorY(), "Cloud lineup packs", Theme::TEXT, C(18.f));
    m_gui.advanceY(S(28.f));

    if (!cloud_api::hasToken())
        m_gui.label("Launch via loader to browse and import cloud packs.", Theme::TEXT_MUTED, C(12.f));

    if (m_gui.accentButton("nadeCloudRefresh", "Refresh cloud packs", clipW * 0.5f, S(36.f))) {
        std::string err;
        m_cloudLineups.clear();
        if (cloud_api::listLineupPacks("", m_cloudLineups, err)) {
            m_cloudLineupsLoaded = true;
            m_nadesStatus = "Cloud packs refreshed";
        } else {
            m_nadesStatus = err;
        }
        m_nadesStatusTimer = 3.f;
    }

    m_gui.dummy(S(8.f));
    for (int i = 0; i < static_cast<int>(m_cloudLineups.size()); ++i) {
        const auto& lp = m_cloudLineups[i];
        char row[256];
        std::snprintf(row, sizeof(row), "%s — %s (%d spots)", lp.map.c_str(), lp.title.c_str(), lp.spotCount);
        const bool sel = (i == m_selectedCloudLineup);
        const float rowH = S(34.f);
        const float rowY = m_gui.cursorY();
        const bool rowHover = m_gui.mouseX() >= clipX && m_gui.mouseX() <= clipX + clipW
                           && m_gui.mouseY() >= rowY && m_gui.mouseY() <= rowY + rowH;
        if (sel || rowHover)
            r.drawRoundedFilledRect(clipX, rowY, clipW, rowH,
                sel ? 0xFF1E2238 : 0xFF151822, S(8.f));
        r.drawText(f, clipX + S(10.f), rowY + S(8.f), row, sel ? Theme::TEXT : Theme::TEXT_MUTED, C(13.f));
        if (rowHover && m_gui.mouseClicked()) {
            m_selectedCloudLineup = i;
            std::string jsonText, err;
            if (cloud_api::downloadLineupPack(lp.id, jsonText, err)
                && GrenadeLineupManager::instance().loadPackFromJson(jsonText, err)) {
                g_cfg.grenadeLineupCloudId = lp.id;
                m_nadesStatus = "Imported " + lp.title;
            } else {
                m_nadesStatus = err.empty() ? "Import failed" : err;
            }
            m_nadesStatusTimer = 3.f;
        }
        m_gui.advanceY(rowH + S(4.f));
    }

    m_gui.dummy(S(14.f));
    r.drawText(f, clipX, m_gui.cursorY(), "Sound ESP", Theme::TEXT, C(18.f));
    m_gui.advanceY(S(28.f));
    m_gui.toggleCheckbox("sndEsp", "Sound ESP", &g_cfg.soundEspEnabled);
    m_gui.toggleCheckbox("sndGun", "Gunshot rings", &g_cfg.soundEspGunshots);
    m_gui.toggleCheckbox("sndStep", "Footstep rings", &g_cfg.soundEspFootsteps);
    m_gui.label("Expanding rings at estimated gunshot and footstep locations.", Theme::TEXT_MUTED, C(12.f));

    r.setClipRect(0, 0, 0, 0);
}

// ═════════════════════════════════════════════════════════════════════════════
// Visuals panel
// ═════════════════════════════════════════════════════════════════════════════

void Menu::drawWorldPanel() {
    auto& r = m_gui.renderer();
    const float s = m_uiScale;
    auto S = [s](float v) { return v * s; };

    float x = m_winX + m_sideW, y = m_winY;
    float w = m_winW - m_sideW;
    float pad = Theme::PADDING * s;

    drawPanelHeader(2);

    constexpr int kWorldTabCount = 3;
    const char* kWorldTabs[kWorldTabCount] = { "Grenades", "Radar", "Web radar" };

    const float tabBarY = m_gui.cursorY();
    const float tabItemH = S(42.f);
    const float tabGap = S(8.f);
    const float tabBarX = x + pad;
    const float tabBarW = w - pad * 2.f;
    const float tabItemW = (tabBarW - tabGap * (kWorldTabCount - 1)) / kWorldTabCount;

    for (int i = 0; i < kWorldTabCount; ++i) {
        const float tx = tabBarX + i * (tabItemW + tabGap);
        const bool active = (i == m_worldSubTab);
        m_worldSubTabAnim[(size_t)i] = advanceSubTabAnim(m_worldSubTabAnim[(size_t)i], active, m_frameDt);
        const float activeT = m_worldSubTabAnim[(size_t)i];
        const bool hovered = m_gui.mouseX() >= tx && m_gui.mouseX() <= tx + tabItemW
                          && m_gui.mouseY() >= tabBarY && m_gui.mouseY() <= tabBarY + tabItemH;
        const float hoverT = animValue(hashAnimId("worldSubHover") ^ static_cast<uint32_t>(i),
                                     hovered ? 1.f : 0.f, 0.22f, m_frameDt);
        const float pressT = animValue(hashAnimId("worldSubPress") ^ static_cast<uint32_t>(i),
                                     (hovered && m_gui.mouseDown()) ? 1.f : 0.f, 0.34f, m_frameDt);

        unsigned int inactiveBg = lerpColor(0xFF151822, 0xFF2A3148, hoverT * 0.12f);
        inactiveBg = lerpColor(inactiveBg, 0xFF0E1018, pressT * 0.35f);
        unsigned int activeBg = lerpColor(inactiveBg, 0xFF2A3148, 1.f);
        activeBg = lerpColor(activeBg, withAlpha(Theme::ACCENT, 26), 1.f);
        const unsigned int fg = lerpColor(0xFF9EA5C4, 0xFFE9ECFF, activeT * 0.95f + hoverT * 0.15f);

        drawSubTabSurface(r, tx, tabBarY, tabItemW, tabItemH, inactiveBg, activeBg, S(8.f), activeT, s);
        const float fontSize = S(15.f);
        float txtX = tx + (tabItemW - (std::strlen(kWorldTabs[i]) * 7.8f * s)) * 0.5f;
        float txtY = textControlCenterY(m_gui.font(), tabBarY, tabItemH, kWorldTabs[i], fontSize);
        r.drawText(m_gui.font(), txtX, txtY, kWorldTabs[i], fg, fontSize);

        if (hovered && m_gui.mouseClicked())
            m_worldSubTab = i;
    }

    float scrollY = tabBarY + tabItemH + S(10.f);
    float clipX = x + pad;
    float clipY = scrollY;
    float clipW = w - pad * 2;
    float clipH = m_winH - (clipY - y) - pad;

    bool clipHovered = m_gui.mouseX() >= clipX && m_gui.mouseX() <= clipX + clipW
                    && m_gui.mouseY() >= clipY && m_gui.mouseY() <= clipY + clipH;

    if (clipHovered) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.f)
            m_worldScrollTarget -= wheel * S(42.f);
    }

    m_worldScroll = smoothValue(m_worldScroll, m_worldScrollTarget, 0.22f, m_frameDt);

    m_gui.setCursor(clipX, clipY - m_worldScroll);
    m_gui.setItemWidth(clipW - S(10.f));

    r.setClipRect(clipX, clipY, clipW, clipH);

    if (m_worldSubTab == 0) {
        m_gui.label("Grenades & bomb", Theme::TEXT, 20.f);
        m_gui.dummy(8.f);
        m_gui.toggleCheckbox("grenEn", "Grenade ESP", &g_cfg.grenadeEnabled);
        m_gui.toggleCheckbox("grenTraj", "Grenade trajectory", &g_cfg.grenadeTrajectory);
        m_gui.sliderFloatValue("grenOffInset", "Off-screen indicator inset", &g_cfg.grenadeOffscreenInset,
                               0.f, 320.f, "%.0f px");
        m_gui.dummy(8.f);
        m_gui.separator();
        m_gui.label("Prediction colors", Theme::TEXT, 20.f);
        m_gui.dummy(6.f);
        static const char* kGrenadeColorLabels[] = { "HE", "Smoke", "Flash", "Molotov", "Decoy" };
        for (int i = 0; i < 5; ++i)
            m_gui.colorEdit4(kGrenadeColorLabels[i], g_cfg.grenadeColors[i]);
        m_gui.colorEdit4("Pre-throw", g_cfg.grenadePreThrowColor);
        m_gui.colorEdit4("Danger (in range)", g_cfg.grenadeDangerColor);
        m_gui.sliderFloatValue("grenTrajAlpha", "Trajectory opacity", &g_cfg.grenadeTrajectoryAlpha,
                               0.05f, 1.f, "%.2f");
        m_gui.dummy(8.f);
        m_gui.separator();
        m_gui.label("Bomb & spectators", Theme::TEXT, 20.f);
        m_gui.dummy(8.f);
        m_gui.toggleCheckbox("bombTimer", "Bomb timer", &g_cfg.bombTimerEnabled);
        m_gui.toggleCheckbox("specList", "Spectator list", &g_cfg.spectatorListEnabled);
    } else if (m_worldSubTab == 1) {
        m_gui.label("In-game radar", Theme::TEXT, 20.f);
        m_gui.dummy(8.f);
        m_gui.toggleCheckbox("radarEn", "Radar", &g_cfg.radarEnabled);
        static const char* kRadarModes[] = { "Manual", "In-game overlay" };
        int radarMode = std::clamp(g_cfg.radarMode, 0, 1);
        m_gui.label("Radar mode", Theme::TEXT_MUTED, 15.f);
        if (m_gui.comboBox("##radarMode", kRadarModes, 2, &radarMode))
            g_cfg.radarMode = radarMode;
        if (g_cfg.radarMode == 0)
            m_gui.sliderFloatValue("radarOpacity", "Radar opacity", &g_cfg.radarBgOpacity, 0.f, 1.f, "%.2f");
        else
            g_cfg.radarBgOpacity = 0.f;
        m_gui.sliderFloatValue("radarSize", "Radar size", &g_cfg.radarSize, 120.f, 420.f, "%.0f");
        m_gui.sliderFloatValue("radarRange", "Radar zoom", &g_cfg.radarRange, 400.f, 5000.f, "%.0f");
        m_gui.sliderFloatValue("radarBlip", "Blip size", &g_cfg.radarBlipSize, 2.f, 8.f, "%.1f");
    } else {
        m_gui.label("Web radar sharing", Theme::TEXT, 20.f);
        m_gui.dummy(8.f);
        m_gui.toggleCheckbox("webRadarEnabled", "Share web radar", &g_cfg.webRadarEnabled);
        float webPublishMs = static_cast<float>(g_cfg.webRadarPublishMs);
        m_gui.sliderFloatValue("webRadarPublishMs", "Publish interval (ms)", &webPublishMs, 1.f, 2000.f, "%.0f");
        g_cfg.webRadarPublishMs = static_cast<int>(webPublishMs + 0.5f);
        if (!g_cfg.webRadarSessionId.empty()) {
            if (m_gui.accentButton("copyWebRadarLink", "Copy share link")) {
                m_configStatus = copyUtf8ToClipboard(g_cfg.webRadarShareUrl)
                    ? "Web radar link copied" : "Failed to copy link";
                m_configStatusTimer = 2.0f;
            }
            m_gui.label(g_cfg.webRadarShareUrl.c_str(), Theme::TEXT_MUTED, 14.f);
        } else {
            m_gui.label("Enable sharing to generate a link.", Theme::TEXT_MUTED, 15.f);
        }
    }

    float contentHeight = m_gui.cursorY() - (clipY - m_worldScroll);
    r.clearClipRect();

    m_worldMaxScroll = (std::max)(0.f, contentHeight - clipH);
    m_worldScrollTarget = std::clamp(m_worldScrollTarget, 0.f, m_worldMaxScroll);
    m_worldScroll = std::clamp(m_worldScroll, 0.f, m_worldMaxScroll);

    if (m_worldMaxScroll > 0.5f) {
        float trackX = clipX + clipW - S(5.f);
        r.drawRoundedFilledRect(trackX, clipY, S(3.f), clipH, 0xFF101015, S(2.f));
        float thumbH = (std::max)(S(36.f), clipH * (clipH / (contentHeight + 0.001f)));
        float t = m_worldScroll / m_worldMaxScroll;
        float thumbY = clipY + (clipH - thumbH) * t;
        r.drawRoundedFilledRect(trackX, thumbY, S(3.f), thumbH, 0xFF8E95E8, S(2.f));
    }
}

void Menu::drawSystemPanel() {
    auto& r = m_gui.renderer();
    const float s = m_uiScale;
    auto S = [s](float v) { return v * s; };

    float x = m_winX + m_sideW, y = m_winY;
    float w = m_winW - m_sideW;
    float pad = Theme::PADDING * s;

    drawPanelHeader(4);

    float clipX = x + pad;
    float clipY = m_gui.cursorY();
    float clipW = w - pad * 2.f;
    float clipH = m_winH - (clipY - y) - pad;

    bool clipHovered = m_gui.mouseX() >= clipX && m_gui.mouseX() <= clipX + clipW
                    && m_gui.mouseY() >= clipY && m_gui.mouseY() <= clipY + clipH;
    if (clipHovered) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.f)
            m_systemScrollTarget -= wheel * S(42.f);
    }

    m_systemScroll = smoothValue(m_systemScroll, m_systemScrollTarget, 0.22f, m_frameDt);
    m_gui.setCursor(clipX, clipY - m_systemScroll);
    m_gui.setItemWidth(clipW - S(10.f));
    r.setClipRect(clipX, clipY, clipW, clipH);

    m_gui.label("Overlay", Theme::TEXT, 20.f);
    m_gui.dummy(8.f);
    m_gui.toggleCheckbox("fpsWm", "FPS watermark", &g_cfg.showFpsWatermark, 44.f);
    static const char* kAAModes[] = {
        "Off", "MSAA", "FXAA Balanced", "FXAA High", "MSAA + FXAA Lite", "MSAA + FXAA High"
    };
    int aaIdx = std::clamp(g_cfg.aaMode, 0, 5);
    m_gui.label("Anti-aliasing", Theme::TEXT_MUTED, 15.f);
    if (m_gui.comboBox("##aaMode", kAAModes, 6, &aaIdx))
        g_cfg.aaMode = aaIdx;

    m_gui.separator();
    m_gui.label("Process", Theme::TEXT, 20.f);
    m_gui.dummy(8.f);

    const float procY = m_gui.cursorY();
    MenuColumnLayout procCols(clipX, clipW - S(10.f), procY, S(14.f));
    procCols.beginLeft(m_gui);
    {
        static const char* kProcPrios[] = { "Normal", "Above Normal", "High" };
        int prio = std::clamp(g_cfg.processPriority, 0, 2);
        m_gui.label("Process priority", Theme::TEXT_MUTED, 15.f);
        if (m_gui.comboBox("##procPrio", kProcPrios, 3, &prio))
            g_cfg.processPriority = prio;
    }
    procCols.syncLeft(m_gui.cursorY());

    procCols.beginRight(m_gui);
    {
        static const char* kThrPrios[] = { "Normal", "Above Normal", "Highest", "Time Critical" };
        int prio = std::clamp(g_cfg.entityThreadPriority, 0, 3);
        m_gui.label("Entity thread priority", Theme::TEXT_MUTED, 15.f);
        if (m_gui.comboBox("##thrPrio", kThrPrios, 4, &prio))
            g_cfg.entityThreadPriority = prio;
    }
    procCols.syncRight(m_gui.cursorY());
    procCols.end(m_gui);

    {
        static const char* kBgModes[] = { "Full", "Reduced", "Minimal" };
        int bg = std::clamp(g_cfg.bgMode, 0, 2);
        m_gui.label("Background mode (unfocused)", Theme::TEXT_MUTED, 15.f);
        if (m_gui.comboBox("##bgMode", kBgModes, 3, &bg))
            g_cfg.bgMode = bg;
    }
    m_gui.label("Time Critical can improve responsiveness but uses more CPU.", Theme::TEXT_MUTED, 13.f);

    m_gui.separator();
    m_gui.label("Overlay control", Theme::TEXT, 20.f);
    m_gui.dummy(8.f);
    m_gui.label("Fully closes this overlay process from memory.", Theme::TEXT_MUTED, 13.f);
    if (m_gui.accentButton("unloadOverlay", "Unload Overlay")) {
        g_requestShutdown.store(true, std::memory_order_relaxed);
    }

    float contentHeight = m_gui.cursorY() - (clipY - m_systemScroll);
    r.clearClipRect();

    m_systemMaxScroll = (std::max)(0.f, contentHeight - clipH);
    m_systemScrollTarget = std::clamp(m_systemScrollTarget, 0.f, m_systemMaxScroll);
    m_systemScroll = std::clamp(m_systemScroll, 0.f, m_systemMaxScroll);

    if (m_systemMaxScroll > 0.5f) {
        float trackX = clipX + clipW - S(5.f);
        r.drawRoundedFilledRect(trackX, clipY, S(3.f), clipH, 0xFF101015, S(2.f));
        float thumbH = (std::max)(S(36.f), clipH * (clipH / (contentHeight + 0.001f)));
        float t = m_systemScroll / m_systemMaxScroll;
        float thumbY = clipY + (clipH - thumbH) * t;
        r.drawRoundedFilledRect(trackX, thumbY, S(3.f), thumbH, 0xFF8E95E8, S(2.f));
    }
}

void Menu::drawPlayerInfoPanel(const EntityManager& em) {
    (void)em;
    const bool leetifyPromptActive = !m_leetifyPromptDismissed
                                    && PlayerScout::instance().needsSetupPrompt();
    m_gui.setModalInputBlocked(leetifyPromptActive);

    auto& r = m_gui.renderer();
    auto& f = m_gui.font();
    const float s = m_uiScale;
    const float c = m_contentScale;
    auto S = [s](float v) { return v * s; };
    const float listTitleSize = 24.f * c;
    const float hintSize = 15.f * c;
    const float nameSize = 18.f * c;
    const float riskSize = 16.f * c;

    float x = m_winX + m_sideW, y = m_winY;
    float w = m_winW - m_sideW;
    float pad = Theme::PADDING * s;

    drawPanelHeader(6);

    float clipX = x + pad;
    float clipY = m_gui.cursorY();
    float clipW = w - pad * 2.f;
    float clipH = m_winH - (clipY - y) - pad;

    bool clipHovered = m_gui.mouseX() >= clipX && m_gui.mouseX() <= clipX + clipW
                    && m_gui.mouseY() >= clipY && m_gui.mouseY() <= clipY + clipH;
    if (clipHovered) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.f)
            m_playerInfoScrollTarget -= wheel * S(42.f);
    }

    m_playerInfoScroll = smoothValue(m_playerInfoScroll, m_playerInfoScrollTarget, 0.22f, m_frameDt);

    m_gui.setCursor(clipX, clipY - m_playerInfoScroll);
    m_gui.setItemWidth(clipW - S(10.f));
    r.setClipRect(clipX, clipY, clipW, clipH);

    if (PlayerScout::instance().needsSetupPrompt() && m_leetifyPromptDismissed) {
        m_gui.label("Leetify API key is not configured.", Theme::TEXT_MUTED, hintSize);
        m_gui.dummy(S(6.f));
        if (m_gui.accentButton("leetifyRestartSetup", "Set up Leetify API key", 0.f, S(44.f)))
            m_leetifyPromptDismissed = false;
        m_gui.separator();
    }

    if (PlayerScout::instance().hasValidApiKey()) {
        float attrY = m_gui.cursorY();
        drawLeetifyAttribution(clipX, attrY, clipW);
        m_gui.setCursor(clipX, attrY + S(8.f));
        m_gui.separator();
    }

    r.drawText(f, clipX, m_gui.cursorY(), "Player List", Theme::TEXT, listTitleSize);
    m_gui.setCursor(clipX, m_gui.cursorY() + listTitleSize + S(8.f));

    if (!PlayerScout::instance().hasValidApiKey()) {
        r.drawText(f, clipX, m_gui.cursorY(),
            "Enter your Leetify API key to load player stats.", Theme::TEXT_MUTED, hintSize);
        m_gui.setCursor(clipX, m_gui.cursorY() + hintSize + S(10.f));
    } else {
        m_gui.setCursor(clipX, m_gui.cursorY() + S(4.f));
    }

    const auto scoutRows = PlayerScout::instance().rows();
    if (!PlayerScout::instance().hasValidApiKey()) {
        r.drawText(f, clipX, m_gui.cursorY(), "Waiting for API key…", Theme::TEXT_MUTED, hintSize);
        m_gui.setCursor(clipX, m_gui.cursorY() + hintSize + S(8.f));
    } else if (scoutRows.empty()) {
        r.drawText(f, clipX, m_gui.cursorY(), "Waiting for players in match…", Theme::TEXT_MUTED, hintSize);
        m_gui.setCursor(clipX, m_gui.cursorY() + hintSize + S(8.f));
    } else {
        const float rowH = S(58.f);
        const float rowGap = S(10.f);
        const float btnSize = S(40.f);

        for (const auto& row : scoutRows) {
            const float rowY = m_gui.cursorY();
            r.drawRoundedFilledRect(clipX, rowY, clipW, rowH, 0xFF12141E, S(12.f));
            r.drawRoundedRect(clipX, rowY, clipW, rowH, 0xFF232636, S(12.f), (std::max)(1.f, s));

            const char* teamTag = row.teamNum == 2 ? "T" : (row.teamNum == 3 ? "CT" : "?");
            char header[192];
            std::snprintf(header, sizeof(header), "%s%s  [%s]",
                row.isLocal ? "* " : "",
                row.name.empty() ? "Unknown" : row.name.c_str(),
                teamTag);

            const float textX = clipX + S(16.f);
            const float nameY = rowY + S(12.f);
            r.drawText(f, textX, nameY, header, row.isLocal ? Theme::TEXT_LINK : Theme::TEXT, nameSize);

            const char* glance = playerRiskGlance(row);
            const unsigned int glanceCol = playerRiskGlanceColor(row);
            r.drawText(f, textX, nameY + nameSize + S(4.f), glance, glanceCol, riskSize);

            const float btnX = clipX + clipW - btnSize - S(12.f);
            const float btnY = rowY + (rowH - btnSize) * 0.5f;
            const bool rowVisible = (rowY + rowH >= clipY) && (rowY <= clipY + clipH);
            char btnId[64];
            std::snprintf(btnId, sizeof(btnId), "plyrZoom_%llu",
                static_cast<unsigned long long>(row.steamId));
            if (rowVisible && drawIconAccentButton(m_gui, r, btnId, m_zoomInIconSrv.Get(),
                    m_zoomInIconW, m_zoomInIconH, btnX, btnY, btnSize, s)) {
                m_playerDetailSteamId = row.steamId;
                m_playerDetailOpenedThisFrame = true;
            }

            m_gui.setCursor(clipX, rowY + rowH + rowGap);
        }

        m_gui.separator();
        float footerY = m_gui.cursorY();
        drawLeetifyAttribution(clipX, footerY, clipW);
        m_gui.setCursor(clipX, footerY + S(6.f));
    }

    float contentHeight = m_gui.cursorY() - (clipY - m_playerInfoScroll);
    m_playerInfoMaxScroll = (std::max)(0.f, contentHeight - clipH);
    if (m_playerInfoScrollTarget < 0.f) m_playerInfoScrollTarget = 0.f;
    if (m_playerInfoScrollTarget > m_playerInfoMaxScroll) m_playerInfoScrollTarget = m_playerInfoMaxScroll;
    if (m_playerInfoScroll < 0.f) m_playerInfoScroll = 0.f;
    if (m_playerInfoScroll > m_playerInfoMaxScroll) m_playerInfoScroll = m_playerInfoMaxScroll;

    if (m_playerInfoMaxScroll > 0.5f) {
        float trackX = clipX + clipW - S(5.f);
        r.drawRoundedFilledRect(trackX, clipY, S(3.f), clipH, 0xFF101015, S(2.f));
        float thumbH = (std::max)(S(36.f), clipH * (clipH / (contentHeight + 0.001f)));
        float t = m_playerInfoScroll / m_playerInfoMaxScroll;
        float thumbY = clipY + (clipH - thumbH) * t;
        r.drawRoundedFilledRect(trackX, thumbY, S(3.f), thumbH, 0xFF8E95E8, S(2.f));
    }

    r.setClipRect(0.f, 0.f, (float)m_screenW, (float)m_screenH);
}

void Menu::drawPlayerDetailOverlay() {
    if (m_playerDetailSteamId == 0)
        return;

    const auto detailRows = PlayerScout::instance().rows();
    PlayerScout::Row detailCopy;
    bool foundDetail = false;
    for (const auto& row : detailRows) {
        if (row.steamId == m_playerDetailSteamId) {
            detailCopy = row;
            foundDetail = true;
            break;
        }
    }
    if (!foundDetail) {
        m_playerDetailSteamId = 0;
        return;
    }
    const PlayerScout::Row* detailRow = &detailCopy;

    auto& r = m_gui.renderer();
    auto& f = m_gui.font();
    const float s = m_uiScale;
    const float c = m_contentScale;
    auto S = [s](float v) { return v * s; };

    const float corner = Theme::CORNER_RADIUS * s;
    r.drawRoundedFilledRect(m_winX, m_winY, m_winW, m_winH, 0xE8050508, corner);

    const float modalW = S(620.f);
    const float pad = S(24.f);
    const float titleSize = 22.f * c;
    const float bodySize = 15.f * c;
    const float statSize = 13.f * c;
    const float valueSize = 18.f * c;
    const float modalH = S(650.f);
    const float mx = m_winX + (m_winW - modalW) * 0.5f;
    const float my = m_winY + (m_winH - modalH) * 0.5f;

    const bool clickOutside = m_gui.mouseClicked()
        && !(m_gui.mouseX() >= mx && m_gui.mouseX() <= mx + modalW
          && m_gui.mouseY() >= my && m_gui.mouseY() <= my + modalH);
    if (clickOutside && !m_playerDetailOpenedThisFrame) {
        m_playerDetailSteamId = 0;
        return;
    }

    r.drawRoundedFilledRect(mx, my, modalW, modalH, 0xFF12141C, S(14.f));
    r.drawRoundedRect(mx, my, modalW, modalH, Theme::BORDER, S(14.f), (std::max)(1.f, s));

    float ty = my + pad;
    const char* displayName = detailRow->name.empty() ? "Unknown player" : detailRow->name.c_str();
    r.drawText(f, mx + pad, ty, displayName, Theme::TEXT, titleSize);
    ty += titleSize + S(6.f);

    const char* teamTag = detailRow->teamNum == 2 ? "Terrorist" : (detailRow->teamNum == 3 ? "Counter-Terrorist" : "Unknown team");
    char subtitle[128];
    std::snprintf(subtitle, sizeof(subtitle), "%s  •  %s", teamTag, playerRiskGlance(*detailRow));
    r.drawText(f, mx + pad, ty, subtitle, playerRiskGlanceColor(*detailRow), bodySize);
    ty += bodySize + S(18.f);

    const float meterW = modalW - pad * 2.f - S(82.f);
    char riskValue[32];
    if (detailRow->state == PlayerScout::FetchState::Ready) {
        std::snprintf(riskValue, sizeof(riskValue), "%d / 100", detailRow->suspicionScore);
        drawRiskMeter(r, f, mx + pad, ty, meterW, "Risk estimate",
            detailRow->suspicionScore, riskValue, statSize, s);
    } else {
        drawStatMeter(r, f, mx + pad, ty, meterW, "Risk estimate", 0.f,
            detailRow->state == PlayerScout::FetchState::Loading ? "Loading…" : "N/A", statSize, s);
    }
    ty += statSize + S(26.f);

    if (detailRow->state == PlayerScout::FetchState::Ready) {
        auto pct01 = [](float pct) { return pct >= 0.f ? std::clamp(pct / 100.f, 0.f, 1.f) : 0.f; };
        auto rating01 = [](float v) { return v >= 0.f ? std::clamp(v / 100.f, 0.f, 1.f) : 0.f; };
        auto signedRating01 = [](float v) { return v > -900.f ? std::clamp((v + 0.08f) / 0.16f, 0.f, 1.f) : 0.f; };
        auto dateShort = [](const std::string& iso) -> std::string {
            return iso.size() >= 10 ? iso.substr(0, 10) : (iso.empty() ? "--" : iso);
        };

        char aimBuf[32], posBuf[32], utilBuf[32], hsBuf[32], wrBuf[32], matchBuf[32];
        std::snprintf(aimBuf, sizeof(aimBuf), detailRow->aimRating >= 0.f ? "%.0f" : "--",
            detailRow->aimRating);
        std::snprintf(posBuf, sizeof(posBuf), detailRow->positioningRating >= 0.f ? "%.0f" : "--",
            detailRow->positioningRating);
        std::snprintf(utilBuf, sizeof(utilBuf), detailRow->utilityRating >= 0.f ? "%.0f" : "--",
            detailRow->utilityRating);
        std::snprintf(hsBuf, sizeof(hsBuf), detailRow->headAccPct >= 0.f ? "%.1f%%" : "--",
            detailRow->headAccPct);
        std::snprintf(wrBuf, sizeof(wrBuf), detailRow->winrate >= 0.f ? "%.0f%%" : "--",
            detailRow->winrate * 100.f);
        std::snprintf(matchBuf, sizeof(matchBuf), detailRow->totalMatches >= 0 ? "%d" : "--",
            detailRow->totalMatches);

        const float circleY = ty + S(48.f);
        const float circleR = S(34.f);
        drawQualityCircle(r, f, mx + modalW * (1.f / 6.f), circleY, circleR, "Win rate",
            wrBuf, detailRow->winrate >= 0.f ? detailRow->winrate : 0.f, statSize, s);
        char leetifyBuf[32];
        std::snprintf(leetifyBuf, sizeof(leetifyBuf),
            detailRow->leetifyRating > -900.f ? "%+.2f" : "--", detailRow->leetifyRating);
        drawQualityCircle(r, f, mx + modalW * (3.f / 6.f), circleY, circleR, "Leetify",
            leetifyBuf, signedRating01(detailRow->leetifyRating), statSize, s);
        char reactBuf[32];
        std::snprintf(reactBuf, sizeof(reactBuf), detailRow->reactionTimeMs >= 0.f ? "%.0fms" : "--",
            detailRow->reactionTimeMs);
        drawQualityCircle(r, f, mx + modalW * (5.f / 6.f), circleY, circleR, "Reaction",
            reactBuf, detailRow->reactionTimeMs >= 0.f ? std::clamp(detailRow->reactionTimeMs / 900.f, 0.f, 1.f) : 0.f,
            statSize, s, true);
        ty += S(108.f);

        const float colGap = S(22.f);
        const float colW = (modalW - pad * 2.f - colGap - S(82.f)) * 0.5f;
        drawQualityMeter(r, f, mx + pad, ty, colW, "Aim", rating01(detailRow->aimRating), aimBuf, statSize, s);
        drawQualityMeter(r, f, mx + pad + colW + colGap + S(41.f), ty, colW, "Utility", rating01(detailRow->utilityRating), utilBuf, statSize, s);
        ty += statSize + S(25.f);
        drawQualityMeter(r, f, mx + pad, ty, colW, "Positioning", rating01(detailRow->positioningRating), posBuf, statSize, s);
        drawQualityMeter(r, f, mx + pad + colW + colGap + S(41.f), ty, colW, "Head accuracy", pct01(detailRow->headAccPct), hsBuf, statSize, s);
        ty += statSize + S(25.f);

        char accBuf[32], sprayBuf[32];
        std::snprintf(accBuf, sizeof(accBuf), detailRow->accuracyEnemySpotted >= 0.f ? "%.1f%%" : "--",
            detailRow->accuracyEnemySpotted);
        std::snprintf(sprayBuf, sizeof(sprayBuf), detailRow->sprayAccuracy >= 0.f ? "%.1f%%" : "--",
            detailRow->sprayAccuracy);
        drawQualityMeter(r, f, mx + pad, ty, colW, "Accuracy spotted", pct01(detailRow->accuracyEnemySpotted), accBuf, statSize, s);
        drawQualityMeter(r, f, mx + pad + colW + colGap + S(41.f), ty, colW, "Spray", pct01(detailRow->sprayAccuracy), sprayBuf, statSize, s);
        ty += statSize + S(30.f);

        char premBuf[32], faceBuf[32];
        if (detailRow->premier >= 0)
            std::snprintf(premBuf, sizeof(premBuf), "%d", detailRow->premier);
        else
            std::snprintf(premBuf, sizeof(premBuf), "--");
        if (detailRow->faceitElo > 0)
            std::snprintf(faceBuf, sizeof(faceBuf), "Lv%d  %d ELO", detailRow->faceitLevel, detailRow->faceitElo);
        else if (detailRow->faceitLevel > 0)
            std::snprintf(faceBuf, sizeof(faceBuf), "Lv%d", detailRow->faceitLevel);
        else
            std::snprintf(faceBuf, sizeof(faceBuf), "--");

        drawHighlightedStat(r, f, mx + pad, ty, "Premier rating", premBuf, statSize, valueSize);
        drawHighlightedStat(r, f, mx + pad + S(170.f), ty, "FACEIT", faceBuf, statSize, valueSize);
        char rankBuf[48];
        if (detailRow->renown >= 0)
            std::snprintf(rankBuf, sizeof(rankBuf), "%d", detailRow->renown);
        else if (detailRow->wingman >= 0)
            std::snprintf(rankBuf, sizeof(rankBuf), "Wingman %d", detailRow->wingman);
        else
            std::snprintf(rankBuf, sizeof(rankBuf), "--");
        drawHighlightedStat(r, f, mx + pad + S(350.f), ty, "Renown / Wingman", rankBuf, statSize, valueSize);
        ty += statSize + valueSize + S(16.f);

        drawHighlightedStat(r, f, mx + pad, ty, "Matches tracked", matchBuf, statSize, valueSize);
        char recentBuf[64];
        if (detailRow->recentMatches > 0) {
            const float recentWr = static_cast<float>(detailRow->recentWins) / static_cast<float>(detailRow->recentMatches) * 100.f;
            std::snprintf(recentBuf, sizeof(recentBuf), "%d recent  %.0f%% WR", detailRow->recentMatches, recentWr);
        } else {
            std::snprintf(recentBuf, sizeof(recentBuf), "--");
        }
        drawHighlightedStat(r, f, mx + pad + S(170.f), ty, "Recent form", recentBuf, statSize, valueSize);
        char profileBuf[96];
        const std::string first = dateShort(detailRow->firstMatchDate);
        std::snprintf(profileBuf, sizeof(profileBuf), "%s  %s",
            detailRow->privacyMode.empty() ? "public?" : detailRow->privacyMode.c_str(),
            first.c_str());
        drawHighlightedStat(r, f, mx + pad + S(350.f), ty, "Profile", profileBuf, statSize, valueSize);
        ty += statSize + valueSize + S(10.f);

        char utilBuf2[96];
        std::snprintf(utilBuf2, sizeof(utilBuf2), "Flash %.2f / %.1fs   HE %.1f   UD %.0f",
            detailRow->flashFoePerFlash >= 0.f ? detailRow->flashFoePerFlash : 0.f,
            detailRow->flashDuration >= 0.f ? detailRow->flashDuration : 0.f,
            detailRow->heDamageAvg >= 0.f ? detailRow->heDamageAvg : 0.f,
            detailRow->utilityOnDeath >= 0.f ? detailRow->utilityOnDeath : 0.f);
        r.drawText(f, mx + pad, ty, utilBuf2, Theme::TEXT_MUTED, statSize);
        ty += statSize + S(8.f);
        if (detailRow->banCount > 0) {
            char banBuf[160];
            std::snprintf(banBuf, sizeof(banBuf), "Bans: %d  %s", detailRow->banCount,
                detailRow->banSummary.empty() ? "" : detailRow->banSummary.c_str());
            r.drawText(f, mx + pad, ty, banBuf, 0xFFFF766Fu, statSize);
            ty += statSize + S(8.f);
        }
        if (!detailRow->lastMap.empty()) {
            char lastBuf[160];
            std::snprintf(lastBuf, sizeof(lastBuf), "Latest: %s  %s",
                detailRow->lastMap.c_str(), detailRow->lastOutcome.empty() ? "" : detailRow->lastOutcome.c_str());
            r.drawText(f, mx + pad, ty, lastBuf, Theme::TEXT_MUTED, statSize);
            ty += statSize + S(8.f);
        }
    } else if (detailRow->state == PlayerScout::FetchState::NotOnLeetify) {
        r.drawText(f, mx + pad, ty,
            "This player is not registered on Leetify or has a private profile.",
            Theme::TEXT_MUTED, bodySize);
        ty += bodySize + S(12.f);
    } else if (detailRow->state == PlayerScout::FetchState::Error) {
        r.drawText(f, mx + pad, ty, detailRow->status.c_str(), 0xFFFF8A65, bodySize);
        ty += bodySize + S(12.f);
    } else {
        r.drawText(f, mx + pad, ty, "Loading player stats from Leetify…", Theme::TEXT_MUTED, bodySize);
        ty += bodySize + S(12.f);
    }

    drawLeetifyProfileLink(mx + pad, ty, detailRow->steamId);
    ty += S(28.f);

    m_gui.setCursor(mx + pad, my + modalH - pad - S(42.f));
    m_gui.setItemWidth(modalW - pad * 2.f);
    if (m_gui.accentButton("playerDetailClose", "Close", 0.f, S(42.f)))
        m_playerDetailSteamId = 0;
}

void Menu::drawIntelPanel(const EntityManager& em) {
    (void)em;
    const float s = m_uiScale;
    auto S = [s](float v) { return v * s; };

    float x = m_winX + m_sideW, y = m_winY;
    float w = m_winW - m_sideW;
    float pad = Theme::PADDING * s;

    auto intelView = MatchIntel::instance().view();

    drawPanelHeader(5);

    float clipX = x + pad;
    float clipY = m_gui.cursorY();
    float clipW = w - pad * 2.f;
    float clipH = m_winH - (clipY - y) - pad;

    bool clipHovered = m_gui.mouseX() >= clipX && m_gui.mouseX() <= clipX + clipW
                    && m_gui.mouseY() >= clipY && m_gui.mouseY() <= clipY + clipH;
    if (clipHovered) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.f)
            m_intelScrollTarget -= wheel * S(42.f);
    }

    m_intelScroll = smoothValue(m_intelScroll, m_intelScrollTarget, 0.22f, m_frameDt);

    m_gui.setCursor(clipX, clipY - m_intelScroll);
    m_gui.setItemWidth(clipW - S(10.f));
    auto& r = m_gui.renderer();
    r.setClipRect(clipX, clipY, clipW, clipH);

    m_gui.label("Match-State Intelligence", Theme::TEXT, 22.f);
    m_gui.dummy(10.f);

    if (intelView.cues.empty()) {
        m_gui.label("No high-impact cues right now.", Theme::TEXT_MUTED, 13.f);
    } else {
        for (const auto& cue : intelView.cues) {
            unsigned int col = Theme::TEXT_MUTED;
            if (cue.severity == 1) col = 0xFFFFD77A;
            if (cue.severity >= 2) col = 0xFFFF8A8A;
            m_gui.label(cue.text.c_str(), col, 13.f);
        }
    }

    m_gui.separator();

    m_gui.label("Threat Board", Theme::TEXT, 22.f);
    m_gui.dummy(8.f);
    if (intelView.threats.empty()) {
        m_gui.label("No enemy threat data yet this match.", Theme::TEXT_MUTED, 13.f);
    } else {
        for (const auto& t : intelView.threats) {
            char line[256];
            std::snprintf(line, sizeof(line),
                "%s  K:%d  entry:%.0f%%  clutch:%.0f%%  score:%.0f",
                t.name.c_str(), t.kills, t.entrySuccess * 100.f, t.clutchRate * 100.f, t.score);
            m_gui.label(line, Theme::TEXT, 13.f);
        }
    }

    m_gui.separator();

    m_gui.label("Session Summary", Theme::TEXT, 22.f);
    m_gui.dummy(8.f);
    {
        char sum[256];
        std::snprintf(sum, sizeof(sum), "Map: %s   Round: %d%s   Deaths: %d   Stored rounds: %d",
            intelView.mapName.empty() ? "unknown" : intelView.mapName.c_str(),
            intelView.currentRound, intelView.roundLive ? " (live)" : "",
            intelView.localDeaths, intelView.storedRoundCount);
        m_gui.label(sum, Theme::TEXT_MUTED, 13.f);
    }

    m_gui.separator();

    m_gui.label("Hot Zones", Theme::TEXT, 22.f);
    m_gui.dummy(8.f);
    auto drawHeat = [&](const char* title, const std::vector<MatchIntel::HeatPoint>& heat) {
        m_gui.label(title, Theme::TEXT_MUTED, 14.f);
        if (heat.empty()) {
            m_gui.label("  (none yet)", Theme::TEXT_MUTED, 12.f);
            return;
        }
        for (const auto& h : heat) {
            char hz[128];
            std::snprintf(hz, sizeof(hz), "  cell (%d,%d)  x%.0f y%.0f  hits:%d",
                h.cellX, h.cellY, h.cellX * 512.f, h.cellY * 512.f, h.count);
            m_gui.label(hz, Theme::TEXT, 12.f);
        }
    };
    drawHeat("Death locations", intelView.deathHeat);
    drawHeat("Failed entries", intelView.failedEntryHeat);

    m_gui.separator();

    m_gui.label("Round Replay Timeline", Theme::TEXT, 22.f);
    m_gui.dummy(10.f);
    m_gui.label((std::string("Round #") + std::to_string(intelView.currentRound) + (intelView.roundLive ? " (live)" : "")).c_str(), Theme::TEXT_MUTED, 13.f);
    if (intelView.replayRoundMax > 0) {
        float roundIdx = static_cast<float>(intelView.replayRoundIndex);
        if (m_gui.sliderFloatValue("intelReplayRound", "Stored Round", &roundIdx, 0.f, static_cast<float>(intelView.replayRoundMax), "%.0f"))
            MatchIntel::instance().setReplayRoundIndex(static_cast<int>(roundIdx + 0.5f));
    }
    const float replayMax = static_cast<float>((std::max)(0, intelView.replayEventMax));
    if (replayMax > 0.f) {
        float replayIdx = static_cast<float>(intelView.replayEventIndex);
        if (m_gui.sliderFloatValue("replayEventIdx", "Event Scrubber", &replayIdx, 0.f, replayMax, "%.0f"))
            MatchIntel::instance().setReplayEventIndex(static_cast<int>(replayIdx + 0.5f));
    } else {
        m_gui.label("No replay events captured yet.", Theme::TEXT_MUTED, 13.f);
    }

    if (!intelView.replayEvents.empty()) {
        const int idx = std::clamp(intelView.replayEventIndex, 0, static_cast<int>(intelView.replayEvents.size()) - 1);
        const auto& ev = intelView.replayEvents[idx];
        m_gui.label((std::string("[") + ev.type + "] " + ev.text).c_str(), Theme::TEXT, 13.f);
        if (ev.pos.lengthSq() > 1.f) {
            char pos[96];
            std::snprintf(pos, sizeof(pos), "  @ (%.0f, %.0f, %.0f)", ev.pos.x, ev.pos.y, ev.pos.z);
            m_gui.label(pos, Theme::TEXT_MUTED, 12.f);
        }
    } else {
        m_gui.label("Timeline is empty for this round yet.", Theme::TEXT_MUTED, 13.f);
    }

    m_gui.separator();
    if (m_gui.accentButton("intelReset", "Reset session intel"))
        MatchIntel::instance().resetSession();

    float contentHeight = m_gui.cursorY() - (clipY - m_intelScroll);
    r.clearClipRect();

    m_intelMaxScroll = (std::max)(0.f, contentHeight - clipH);
    if (m_intelScrollTarget < 0.f) m_intelScrollTarget = 0.f;
    if (m_intelScrollTarget > m_intelMaxScroll) m_intelScrollTarget = m_intelMaxScroll;
    if (m_intelScroll < 0.f) m_intelScroll = 0.f;
    if (m_intelScroll > m_intelMaxScroll) m_intelScroll = m_intelMaxScroll;

    if (m_intelMaxScroll > 0.5f) {
        float trackX = clipX + clipW - S(5.f);
        r.drawRoundedFilledRect(trackX, clipY, S(3.f), clipH, 0xFF101015, S(2.f));
        float thumbH = (std::max)(S(36.f), clipH * (clipH / (contentHeight + 0.001f)));
        float t = m_intelScroll / m_intelMaxScroll;
        float thumbY = clipY + (clipH - thumbH) * t;
        r.drawRoundedFilledRect(trackX, thumbY, S(3.f), thumbH, 0xFF8E95E8, S(2.f));
    }
}
