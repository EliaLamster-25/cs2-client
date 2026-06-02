#include "menu.h"
#include "overlay/renderer.h"
#include "game/entity_manager.h"
#include "analytics/match_intel.h"
#include "gui/icons.h"
#include "config.h"
#include "json.hpp"
#include <algorithm>
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
#include <imgui.h>

using json = nlohmann::json;

static const char* kTabs[] = {
    "Aimbot", "Visuals", "Players", "Misc", "Intel", "Config"
};
static constexpr int kTabN = 6;
static constexpr float kTabH = 46.f;

static const char* kTabSubtitles[] = {
    "Mouse assistance & targeting",
    "Overlay visuals & rendering",
    "Player overlays & filters",
    "Hotkeys & extra utilities",
    "Match intelligence & analysis",
    "Load, save, and manage configs"
};

static void (*kTabIcons[])(Renderer&, float, float, float, unsigned int) = {
    Icons::aimbot, Icons::visuals, Icons::players,
    Icons::misc, Icons::radar, Icons::configs
};

static float deltaS(int64_t& last, int64_t freq) {
    if (!freq) return 0.016f;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = (float)((now.QuadPart - last) / (double)freq);
    last = now.QuadPart;
    return (dt > 0.f && dt < 0.1f) ? dt : 0.016f;
}

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
    candidates.emplace_back("assets/crymore logo.png");
    candidates.emplace_back("assets/crymore_logo.png");
    candidates.emplace_back("assets/logo.png");
    candidates.emplace_back("../assets/crymore logo.png");
    candidates.emplace_back("../assets/crymore_logo.png");
    candidates.emplace_back("../assets/logo.png");
    candidates.emplace_back("../../assets/crymore logo.png");
    candidates.emplace_back("../../assets/crymore_logo.png");
    candidates.emplace_back("../../assets/logo.png");

    wchar_t exePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
        candidates.emplace_back(exeDir / "assets" / "crymore logo.png");
        candidates.emplace_back(exeDir / "assets" / "crymore_logo.png");
        candidates.emplace_back(exeDir / "assets" / "logo.png");
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
static constexpr float kPreviewPanelOffsetY = -74.f;
static constexpr float kPreviewPanelW = 360.f;
static constexpr float kPreviewPanelH = 753.f;
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

    cfg.rcsX = std::clamp(cfg.rcsX, 0.f, 1.f);
    cfg.rcsY = std::clamp(cfg.rcsY, 0.f, 1.f);
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
    j["showDormant"] = cfg.showDormant;
    j["enemyOnly"] = cfg.enemyOnly;
    j["nameEspEnabled"] = cfg.nameEspEnabled;
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
    j["chamsAlpha"] = cfg.chamsAlpha;

    j["grenadeEnabled"] = cfg.grenadeEnabled;
    j["grenadeTrajectory"] = cfg.grenadeTrajectory;
    j["bombTimerEnabled"] = cfg.bombTimerEnabled;
    j["spectatorListEnabled"] = cfg.spectatorListEnabled;
    j["radarEnabled"] = cfg.radarEnabled;
    j["voteRevealerEnabled"] = cfg.voteRevealerEnabled;
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
    j["latencyMode"] = cfg.latencyMode;
    j["processPriority"] = cfg.processPriority;
    j["entityThreadPriority"] = cfg.entityThreadPriority;
    j["bgMode"] = cfg.bgMode;
    j["performanceProfile"] = cfg.performanceProfile;

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

    json aimGroups = json::array();
    for (std::size_t i = 0; i < kAimWeaponGroupCount; ++i)
        aimGroups.push_back(aimGroupToJson(cfg.aimByWeaponGroup[i]));
    j["aimByWeaponGroup"] = std::move(aimGroups);

    j["menuPosX"] = cfg.menuPosX;
    j["menuPosY"] = cfg.menuPosY;
    j["bombTimerPosX"] = cfg.bombTimerPosX;
    j["bombTimerPosY"] = cfg.bombTimerPosY;

    j["debugConsole"] = cfg.debugConsole;
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
    setBool("showDormant", cfg.showDormant);
    setBool("enemyOnly", cfg.enemyOnly);
    setBool("nameEspEnabled", cfg.nameEspEnabled);
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
    setInt("visibilityLatchFrames", cfg.visibilityLatchFrames);
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
    setFloat("chamsAlpha", cfg.chamsAlpha);

    setBool("grenadeEnabled", cfg.grenadeEnabled);
    setBool("grenadeTrajectory", cfg.grenadeTrajectory);
    setBool("bombTimerEnabled", cfg.bombTimerEnabled);
    setBool("spectatorListEnabled", cfg.spectatorListEnabled);
    setBool("radarEnabled", cfg.radarEnabled);
    setBool("voteRevealerEnabled", cfg.voteRevealerEnabled);
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
    setInt("latencyMode", cfg.latencyMode);
    if (cfg.latencyMode < 0) cfg.latencyMode = 0;
    if (cfg.latencyMode > 2) cfg.latencyMode = 2;
    setInt("processPriority", cfg.processPriority);
    setInt("entityThreadPriority", cfg.entityThreadPriority);
    setInt("bgMode", cfg.bgMode);
    setInt("performanceProfile", cfg.performanceProfile);
    cfg.processPriority = std::clamp(cfg.processPriority, 0, 2);
    cfg.entityThreadPriority = std::clamp(cfg.entityThreadPriority, 0, 3);
    cfg.bgMode = std::clamp(cfg.bgMode, 0, 2);
    cfg.performanceProfile = std::clamp(cfg.performanceProfile, 0, 2);

    setBool("triggerbotEnabled", cfg.triggerbotEnabled);
    setInt("triggerbotDelayMs", cfg.triggerbotDelayMs);
    setInt("triggerbotKey", cfg.triggerbotKey);

    setBool("aimAssistEnabled", cfg.aimAssistEnabled);
    setInt("aimAssistKey", cfg.aimAssistKey);
    setInt("aimBone", cfg.aimBone);
    setFloat("aimFov", cfg.aimFov);
    setFloat("aimSmooth", cfg.aimSmooth);
    setFloat("aimBoneOffsetZ", cfg.aimBoneOffsetZ);
    setFloat("aimHeadForward", cfg.aimHeadForward);
    setFloat("aimSensitivity", cfg.aimSensitivity);

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
        legacy.rcsEnabled = true;
        legacy.rcsMode = 1;
        legacy.rcsX = 1.0f;
        legacy.rcsY = 1.0f;
        legacy.rcsSmooth = 6.0f;
        legacy.triggerEnabled = cfg.triggerbotEnabled;
        legacy.triggerDelayMs = cfg.triggerbotDelayMs;
        legacy.triggerKey = cfg.triggerbotKey;
        for (auto& groupCfg : cfg.aimByWeaponGroup)
            groupCfg = legacy;
    }

    setFloat("menuPosX", cfg.menuPosX);
    setFloat("menuPosY", cfg.menuPosY);
    setFloat("bombTimerPosX", cfg.bombTimerPosX);
    setFloat("bombTimerPosY", cfg.bombTimerPosY);

    setBool("debugConsole", cfg.debugConsole);
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
    return true;
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
    g_cfg.weaponIconEnabled = false;
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
    g_cfg.weaponIconPosVisible = -1;
    g_cfg.weaponIconPosOccluded = -1;
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
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// Render
// ═════════════════════════════════════════════════════════════════════════════

void Menu::render(Renderer& renderer, const EntityManager& em, HWND hwnd) {
    float dt = deltaS(m_lastTick, m_tickFreq);
    m_frameDt = dt;
    m_uiTime += dt;

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
    m_winW = 970.f * m_uiScale;
    m_winH = 752.f * m_uiScale;
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
    switch (m_activeTab) {
        case 0: drawAimbotPanel(); break;
        case 1: drawVisualsPanel(); break;
        case 2: drawPlayersPanel(); break;
        case 3: drawExtraPanel(); break;
        case 4: drawIntelPanel(); break;
        case 5: drawConfigsPanel(); break;
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
    auto tabYForIndex = [&](int index) {
        return (index < 5) ? (startY + index * S(48.f)) : (y + sideH - S(54.f));
    };
    float indicatorY = tabYForIndex(m_activeTab) + S(10.f);
    if (m_tabIndicatorY < 0.f)
        m_tabIndicatorY = indicatorY;
    m_tabIndicatorY += (indicatorY - m_tabIndicatorY) * (std::min)(m_frameDt * 13.f, 1.f);
    m_tabIndicatorAlpha += (1.f - m_tabIndicatorAlpha) * (std::min)(m_frameDt * 9.f, 1.f);

    r.drawRoundedFilledRect(x + S(5.f), m_tabIndicatorY, S(3.f), tabH - S(20.f),
                            withAlpha(Theme::ACCENT, (unsigned int)(255.f * m_tabIndicatorAlpha)), S(1.5f));

    auto drawTab = [&](int index, float itemY) {
        bool active = index == m_activeTab;
        float itemX = x + S(8.f);
        float itemW = m_sideW - S(16.f);
        bool hovered = m_gui.mouseX() >= itemX && m_gui.mouseX() <= itemX + itemW
                    && m_gui.mouseY() >= itemY && m_gui.mouseY() <= itemY + tabH;

        float hoverT = m_tabHover[index] = m_tabHover[index] + ((hovered ? 1.f : 0.f) - m_tabHover[index]) * (std::min)(m_frameDt * 12.f, 1.f);
        float activeT = m_tabActive[index] = m_tabActive[index] + ((active ? 1.f : 0.f) - m_tabActive[index]) * (std::min)(m_frameDt * 10.f, 1.f);

        float cardT = hoverT * 0.32f + activeT;
        if (cardT > 0.01f) {
            r.drawRoundedFilledRect(itemX, itemY, itemW, tabH,
                                    withAlpha(active ? 0xFF242140 : 0xFF17182A,
                                              (unsigned int)(105.f * hoverT + 170.f * activeT)),
                                    S(10.f));
            if (activeT > 0.01f) {
                r.drawRoundedFilledRect(itemX, itemY, itemW, tabH,
                                        withAlpha(Theme::ACCENT, (unsigned int)(20.f * activeT)),
                                        S(10.f));
            }
        }

        unsigned int iconColor = lerpColor(0xFF67698A, Theme::ACCENT, activeT * 0.95f + hoverT * 0.18f);
        unsigned int textColor = lerpColor(0xFF7B7D9E, Theme::TEXT, hoverT * 0.22f);
        textColor = lerpColor(textColor, Theme::ACCENT, activeT * 0.95f);

        float iconSize = C(16.f) + activeT * S(1.5f);
        float iconX = itemX + S(18.f) + activeT * S(2.5f);
        float textX = itemX + S(50.f) + activeT * S(4.f);
        kTabIcons[index](r, iconX, itemY + (tabH - iconSize) * 0.5f, iconSize, iconColor);
        r.drawText(f, textX, textControlCenterY(f, itemY, tabH, kTabs[index], C(16.f)), kTabs[index], textColor, C(16.f));

        if (hovered && m_gui.mouseClicked())
            m_activeTab = index;
    };

    for (int i = 0; i < 5; ++i)
        drawTab(i, startY + i * S(48.f));

    drawTab(5, y + sideH - S(54.f));
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

    drawPanelHeader(5);
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

    r.drawText(f, clipX, cursorY, "Saved Configs", Theme::TEXT, C(18.f));
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
            unsigned int rowBorder = lerpColor(0xFF24283A, Theme::ACCENT, selectT * 0.92f + hoverT * 0.1f);

            r.drawRoundedFilledRect(rowX, rowY, clipW, rowH, rowBg, S(12.f));
            r.drawRoundedRect(rowX, rowY, clipW, rowH, rowBorder, S(12.f), (std::max)(1.f, s));
            if (selectT > 0.01f) {
                r.drawRoundedFilledRect(rowX + S(4.f), rowY + S(12.f), S(3.f), rowH - S(24.f),
                                        withAlpha(Theme::ACCENT, (unsigned int)(255.f * selectT)), S(1.5f));
            }

            r.drawText(f, rowX + S(18.f), rowY + S(16.f), name.c_str(), Theme::TEXT, C(16.f));
            r.drawText(f, rowX + S(18.f), rowY + S(38.f), "Local JSON config", Theme::TEXT_MUTED, C(12.f));

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

    auto drawChip = [&](const char* id, float x, float y, float w, float h, bool active, const wchar_t* wlabel, bool iconOnly, bool isSettingsIcon) {
        bool hovered = m_gui.mouseX() >= x && m_gui.mouseX() <= x + w
                    && m_gui.mouseY() >= y && m_gui.mouseY() <= y + h;
        uint32_t idh = hashAnimId(id, x, y);
        float hoverT = animValue(idh ^ 0x650100u, hovered ? 1.f : 0.f, 0.22f, m_frameDt);
        float activeT = animValue(idh ^ 0x650101u, active ? 1.f : 0.f, 0.22f, m_frameDt);
        float pressT = animValue(idh ^ 0x650102u, (hovered && m_gui.mouseDown()) ? 1.f : 0.f, 0.34f, m_frameDt);
        float drawY = y + pressT * S(1.2f);

        unsigned int fill = lerpColor(0xFF131522, 0xFF252242, activeT * 0.9f + hoverT * 0.18f);
        unsigned int border = lerpColor(0xFF282A3F, Theme::ACCENT, activeT * 0.95f + hoverT * 0.15f);
        unsigned int textCol = lerpColor(Theme::TEXT_MUTED, Theme::ACCENT, activeT * 0.95f + hoverT * 0.18f);
        r.drawRoundedFilledRect(x, drawY, w, h, fill, S(9.f));
        r.drawRoundedRect(x, drawY, w, h, border, S(9.f), (std::max)(1.f, s));

        if (wlabel) {
            float fontSize = iconOnly ? S(27.f) : S(13.f);
            float tx = x + (w - measureWide(wlabel, fontSize)) * 0.5f + (isSettingsIcon ? S(0.5f) : 0.f);
            float ty = iconOnly
                ? (textVisualCenterY(m_gui.font(), drawY, h, wlabel, fontSize) + (isSettingsIcon ? S(0.4f) : S(1.4f)))
                : textVisualCenterY(m_gui.font(), drawY, h, wlabel, fontSize);
            r.drawTextW(m_gui.font(), tx, ty, wlabel, textCol, fontSize);
        }

        return hovered && m_gui.mouseClicked();
    };

    float chipX = contentX;
    if (drawChip("aimChipGlobal", chipX, tabsY, globalW, tabH, m_aimSubTab == 0, kSettingsIcon, true, true))
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
        if (drawChip(cid, chipX, tabsY, iconW, tabH, active, glyph, true, false)) {
            m_aimSubTab = i + 1;
            m_aimWeaponGroupIdx = i;
        }
        chipX += iconW + chipGap;
    }

    m_gui.advanceY(tabH + S(10.f));

    if (m_aimSubTab == 0) {
        m_gui.label("SETTINGS", Theme::TEXT_MUTED, 15.f);
        m_gui.dummy(10.f);
        m_gui.toggleCheckbox("aimEnabled", "Enable aimbot", &g_cfg.aimAssistEnabled, 44.f);
        m_gui.toggleCheckbox("aimNeedVis", "Aim visibility check", &g_cfg.aimRequireVisibility, 44.f);
        if (g_cfg.aimRequireVisibility) {
            static const char* kAimVisModes[] = { "Performance", "Accuracy" };
            int aimVisMode = std::clamp(g_cfg.aimVisibilityMode, 0, 1);
            m_gui.label("Aim Visibility Mode", Theme::TEXT_MUTED, 14.f);
            if (m_gui.comboBox("##aimVisMode", kAimVisModes, 2, &aimVisMode))
                g_cfg.aimVisibilityMode = aimVisMode;
        }
        m_gui.keybindCard("aimGlobalKey", "Aim key", &g_cfg.aimAssistKey, 58.f);
        m_gui.sliderFloatValue("aimGlobalSensitivity", "CS2 sensitivity", &g_cfg.aimSensitivity, 0.1f, 10.f, "%.2f", 74.f);
        m_gui.sliderFloatValue("aimOffsetZ", "Bone offset", &g_cfg.aimBoneOffsetZ, -8.f, 20.f, "%.1f", 74.f);
        m_gui.sliderFloatValue("aimHeadForward", "Head forward", &g_cfg.aimHeadForward, -8.f, 20.f, "%.1f", 74.f);
        m_gui.dummy(8.f);
        m_gui.label("These values apply to all weapon groups.", Theme::TEXT_MUTED, 15.f);
    } else {
        m_aimWeaponGroupIdx = m_aimSubTab - 1;
        AimGroupConfig& aimCfg = g_cfg.aimByWeaponGroup[static_cast<std::size_t>(m_aimWeaponGroupIdx)];
        sideModelCfg = &aimCfg;

        m_gui.label("TARGETING", Theme::TEXT_MUTED, 15.f);
        m_gui.dummy(10.f);
        m_gui.label("AIM", Theme::TEXT_MUTED, 14.f);
        m_gui.dummy(6.f);
        const std::string groupSuffix = std::to_string(m_aimWeaponGroupIdx);
        m_gui.label("Use the model panel on the right to toggle hit points.", Theme::TEXT_MUTED, 14.f);
        m_gui.dummy(6.f);
        m_gui.sliderFloatValue("aimFov", "FOV radius", &aimCfg.aimFov, 0.5f, 30.f, "%.0f°", 74.f);
        m_gui.sliderFloatValue("aimSmooth", "Smoothing", &aimCfg.aimSmooth, 1.f, 20.f, "%.1f", 74.f);
        m_gui.dummy(8.f);

        m_gui.label("RCS", Theme::TEXT_MUTED, 14.f);
        m_gui.dummy(6.f);
        static const char* kRcsModes[] = { "Aim only", "Standalone" };
        m_gui.toggleCheckbox(("aimRcsEnabled_" + groupSuffix).c_str(), "RCS", &aimCfg.rcsEnabled, 44.f);
        int rcsMode = aimCfg.rcsMode;
        if (rcsMode < 0) rcsMode = 0;
        if (rcsMode > 1) rcsMode = 1;
        if (m_gui.comboBox(("aimRcsMode_" + groupSuffix).c_str(), kRcsModes, 2, &rcsMode))
            aimCfg.rcsMode = rcsMode;
        m_gui.sliderFloatValue(("aimRcsX_" + groupSuffix).c_str(), "RCS X", &aimCfg.rcsX, 0.f, 1.f, "%.2f", 74.f);
        m_gui.sliderFloatValue(("aimRcsY_" + groupSuffix).c_str(), "RCS Y", &aimCfg.rcsY, 0.f, 1.f, "%.2f", 74.f);
        m_gui.sliderFloatValue(("aimRcsSmooth_" + groupSuffix).c_str(), "RCS Smooth", &aimCfg.rcsSmooth, 0.f, 20.f, "%.1f", 74.f);

        m_gui.dummy(8.f);
        m_gui.label("TRIGGER", Theme::TEXT_MUTED, 14.f);
        m_gui.dummy(6.f);
        m_gui.toggleCheckbox(("aimTrigEnabled_" + groupSuffix).c_str(), "Triggerbot", &aimCfg.triggerEnabled, 44.f);
        float trigDelay = static_cast<float>(aimCfg.triggerDelayMs);
        m_gui.sliderFloatValue(("aimTrigDelay_" + groupSuffix).c_str(), "Trigger delay (ms)", &trigDelay, 0.f, 100.f, "%.0f", 74.f);
        aimCfg.triggerDelayMs = static_cast<int>(trigDelay + 0.5f);
        m_gui.keybindCard(("aimTrigKey_" + groupSuffix).c_str(), "Trigger key", &aimCfg.triggerKey, 58.f);

        m_gui.dummy(10.f);
        m_gui.label("Bone offset and head-forward are configured in SETTINGS.", Theme::TEXT_MUTED, 15.f);
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

void Menu::drawAimHitboxWindow(AimGroupConfig& aimCfg) {
    auto& r = m_gui.renderer();
    const float s = m_uiScale;
    auto S = [s](float v) { return v * s; };

    float panelW = S(340.f);
    float panelH = S(548.f);
    float panelGap = S(14.f);
    float panelX = m_winX + m_winW + panelGap;
    float panelY = m_winY + S(74.f);

    if (panelX + panelW > m_screenW - S(8.f))
        panelX = m_winX - panelGap - panelW;
    panelX = std::clamp(panelX, S(8.f), m_screenW - panelW - S(8.f));
    panelY = std::clamp(panelY, S(8.f), m_screenH - panelH - S(8.f));

    for (int i = 0; i < 5; ++i) {
        float spread = S(4.f + i * 4.5f);
        unsigned int alpha = (unsigned int)(26.f - i * 4.5f);
        r.drawRoundedFilledRect(
            panelX - spread,
            panelY - spread * 0.9f,
            panelW + spread * 2.f,
            panelH + spread * 2.05f,
            withAlpha(0xFF000000, alpha),
            S(12.f) + spread * 0.65f
        );
    }
    r.drawRoundedFilledRect(panelX, panelY, panelW, panelH, 0xF10A0D15, S(12.f));
    r.drawRoundedRect(panelX, panelY, panelW, panelH, 0xFF2B3041, S(12.f), (std::max)(1.f, s));

    float imagePadX = S(5.f);
    float imageTop = panelY + S(8.f);
    float imageW = panelW - imagePadX * 2.f;
    float imageH = panelH - S(12.f);

    float drawX = panelX + imagePadX;
    float drawY = imageTop;
    float drawW = imageW;
    float drawH = imageH;

    if (m_hitboxModelSrv && m_hitboxModelW > 0 && m_hitboxModelH > 0) {
        float fit = (std::min)(drawW / (float)m_hitboxModelW, drawH / (float)m_hitboxModelH);
        fit *= 1.75f;
        float imgW = (float)m_hitboxModelW * fit;
        float imgH = (float)m_hitboxModelH * fit;
        drawX += (drawW - imgW) * 0.5f;
        drawY += (drawH - imgH) * 0.5f - S(14.f);
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
        { &aimCfg.hitboxHead, 0.50f, 0.17f },
        { &aimCfg.hitboxChest, 0.50f, 0.38f },
        { &aimCfg.hitboxStomach, 0.50f, 0.53f },
        { &aimCfg.hitboxPelvis, 0.50f, 0.61f },
        { &aimCfg.hitboxArms, 0.33f, 0.42f },
        { &aimCfg.hitboxArms, 0.67f, 0.42f },
        { &aimCfg.hitboxLegs, 0.43f, 0.77f },
        { &aimCfg.hitboxLegs, 0.57f, 0.77f },
    };

    for (const HitNode& node : nodes) {
        float x = drawX + drawW * node.nx;
        float y = drawY + drawH * node.ny;
        float dx = m_gui.mouseX() - x;
        float dy = m_gui.mouseY() - y;
        float hitR = S(14.f);
        bool hovered = (dx * dx + dy * dy) <= (hitR * hitR);
        if (hovered && m_gui.mouseClicked())
            *node.value = !*node.value;

        unsigned int fill = *node.value ? 0xFF2CB7FF : 0xA06E778D;
        unsigned int ring = *node.value ? 0xFFC6EEFF : 0xFF96A0BA;
        r.drawFilledCircle(x, y, S(7.8f), fill);
        r.drawCircle(x, y, S(10.3f), ring, (std::max)(1.f, s));
        if (hovered)
            r.drawCircle(x, y, S(14.f), withAlpha(Theme::ACCENT, 0x93), (std::max)(1.f, s));
    }

    if (!aimCfg.hitboxHead && !aimCfg.hitboxChest && !aimCfg.hitboxStomach
        && !aimCfg.hitboxPelvis && !aimCfg.hitboxArms && !aimCfg.hitboxLegs)
        aimCfg.hitboxHead = true;
}

void Menu::drawPlayersPanel() {
    auto& r = m_gui.renderer();
    const float s = m_uiScale;
    auto S = [s](float v) { return v * s; };

    float x = m_winX + m_sideW, y = m_winY;
    float w = m_winW - m_sideW;
    float pad = Theme::PADDING * s;

    drawPanelHeader(2);

    // ── Sub-tabs ──────────────────────────────────────────────────────────────
    constexpr int kPlayerTabCount = 4;
    const char* kPlayerTabs[kPlayerTabCount] = { "Settings", "Visible", "Occluded", "Sizes/Offsets" };

    const float tabBarY = m_gui.cursorY();
    const float tabItemW = 120.f * s;
    const float tabItemH = 40.f * s;
    const float tabBarX = x + pad;
    for (int i = 0; i < kPlayerTabCount; ++i) {
        const float tx = tabBarX + i * (tabItemW + 6.f * s);
        unsigned int bg = (i == m_playerSubTab) ? 0xFF2F364A : 0xFF1A1F2E;
        unsigned int fg = (i == m_playerSubTab) ? 0xFFE9ECFF : 0xFF9EA5C4;
        r.drawRoundedFilledRect(tx, tabBarY, tabItemW, tabItemH, bg, 6.f * s);
        float txtX = tx + (tabItemW - (std::strlen(kPlayerTabs[i]) * 7.5f * s)) * 0.5f;
        float txtY = tabBarY + (tabItemH - 14.f * s) * 0.5f;
        r.drawText(m_gui.font(), txtX, txtY, kPlayerTabs[i], fg, 14.f * s);
        bool hovered = m_gui.mouseX() >= tx && m_gui.mouseX() <= tx + tabItemW
                    && m_gui.mouseY() >= tabBarY && m_gui.mouseY() <= tabBarY + tabItemH;
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

    // ── General ──────────────────────────────────────────────────────────────
    m_gui.label("General", Theme::TEXT, 22.f);
    m_gui.dummy(10.f);
    if (isSettings) {
        m_gui.toggleCheckbox("plEspEn", "ESP Enabled", &g_cfg.espEnabled);
        m_gui.toggleCheckbox("plEnemyOnly", "Enemy Only", &g_cfg.enemyOnly);
        m_gui.label("Visible/Occluded preview controls now handle item toggles and placement.", Theme::TEXT_MUTED, 14.f);
        m_gui.toggleCheckbox("plFlagFlashed", "Flag: Flashed", &g_cfg.flagFlashedEnabled);
        m_gui.toggleCheckbox("plFlagDefusing", "Flag: Defusing", &g_cfg.flagDefusingEnabled);
        m_gui.toggleCheckbox("plFlagScoped", "Flag: Scoped", &g_cfg.flagScopedEnabled);
        m_gui.toggleCheckbox("plFlagKit", "Flag: Defuse Kit", &g_cfg.flagDefuseKitEnabled);
        m_gui.toggleCheckbox("plVisCheck", "Visibility Check", &g_cfg.visibilityCheckEnabled);
        static const char* kVisModes[] = { "Fast", "Balanced", "Strict" };
        int visMode = std::clamp(g_cfg.visibilityMode, 0, 2);
        m_gui.label("Visibility Mode", Theme::TEXT_MUTED, 14.f);
        if (m_gui.comboBox("##plVisMode", kVisModes, 3, &visMode))
            g_cfg.visibilityMode = visMode;
        {
            static float s_latchF = 2.f;
            s_latchF = static_cast<float>(g_cfg.visibilityLatchFrames);
            s_latchF = std::clamp(s_latchF, 0.f, 5.f);
            if (m_gui.sliderFloatValue("plVisLatch", "Latch Frames", &s_latchF, 0.f, 5.f, "%.0f"))
                g_cfg.visibilityLatchFrames = static_cast<int>(s_latchF + 0.5f);
        }
        m_gui.dummy(6.f);
        m_gui.label("Vis Tuning", Theme::TEXT, 16.f);
        {
            float maxDistK = g_cfg.visMaxDistance > 0.f ? (g_cfg.visMaxDistance / 1000.f) : 12.f;
            maxDistK = std::clamp(maxDistK, 0.5f, 20.f);
            m_gui.sliderFloatValue("plVisDist", "Max Dist (k)", &maxDistK, 0.5f, 20.f, "%.1f");
            g_cfg.visMaxDistance = maxDistK * 1000.f;
        }
        {
            float budgetMs = std::clamp(static_cast<float>(g_cfg.visBudgetMs), 3.f, 30.f);
            m_gui.sliderFloatValue("plVisBudget", "Budget (ms)", &budgetMs, 3.f, 30.f, "%.0f");
            g_cfg.visBudgetMs = static_cast<int>(budgetMs + 0.5f);
        }
        {
            static const char* kEvalRates[] = { "Auto", "1", "2", "3", "4", "5" };
            int er = std::clamp(g_cfg.visEvalBase, 0, 5);
            m_gui.label("Eval Rate", Theme::TEXT_MUTED, 14.f);
            m_gui.comboBox("##plVisRate", kEvalRates, 6, &er);
            g_cfg.visEvalBase = std::clamp(er, 0, 5);
        }
    }

    m_gui.separator();

    // ── Style ────────────────────────────────────────────────────────────────
    m_gui.label("Style", Theme::TEXT, 22.f);
    m_gui.dummy(10.f);

    if (isSizes) {
        m_gui.sliderFloatValue("plBoxThk", "Box Thickness", &g_cfg.boxThickness, 0.5f, 5.f, "%.1f");
        m_gui.sliderFloatValue("plHpW", "HP Bar Width", &g_cfg.hpBarWidth, 1.f, 10.f, "%.0f");
        m_gui.sliderFloatValue("plChamsA", "Chams Alpha", &g_cfg.chamsAlpha, 0.f, 1.f, "%.2f");
        m_gui.sliderFloatValue("plInfoTextSz", "Info Text Size", &g_cfg.infoTextSize, 10.f, 24.f, "%.1f");
        m_gui.dummy(8.f);
        m_gui.label("Anchor Offsets (px)", Theme::TEXT_MUTED, 14.f);
        m_gui.sliderFloatValue("plOffTopX", "Top X", &g_cfg.espAnchorOffsetX[0], -60.f, 60.f, "%.0f");
        m_gui.sliderFloatValue("plOffTopY", "Top Y", &g_cfg.espAnchorOffsetY[0], -60.f, 60.f, "%.0f");
        m_gui.sliderFloatValue("plOffBottomX", "Bottom X", &g_cfg.espAnchorOffsetX[1], -60.f, 60.f, "%.0f");
        m_gui.sliderFloatValue("plOffBottomY", "Bottom Y", &g_cfg.espAnchorOffsetY[1], -60.f, 60.f, "%.0f");
        m_gui.sliderFloatValue("plOffLeftX", "Left X", &g_cfg.espAnchorOffsetX[2], -60.f, 60.f, "%.0f");
        m_gui.sliderFloatValue("plOffLeftY", "Left Y", &g_cfg.espAnchorOffsetY[2], -60.f, 60.f, "%.0f");
        m_gui.sliderFloatValue("plOffRightX", "Right X", &g_cfg.espAnchorOffsetX[3], -60.f, 60.f, "%.0f");
        m_gui.sliderFloatValue("plOffRightY", "Right Y", &g_cfg.espAnchorOffsetY[3], -60.f, 60.f, "%.0f");
        m_gui.sliderFloatValue("plOffTopLeftX", "Top Left X", &g_cfg.espAnchorOffsetX[4], -60.f, 60.f, "%.0f");
        m_gui.sliderFloatValue("plOffTopLeftY", "Top Left Y", &g_cfg.espAnchorOffsetY[4], -60.f, 60.f, "%.0f");
        m_gui.sliderFloatValue("plOffTopRightX", "Top Right X", &g_cfg.espAnchorOffsetX[5], -60.f, 60.f, "%.0f");
        m_gui.sliderFloatValue("plOffTopRightY", "Top Right Y", &g_cfg.espAnchorOffsetY[5], -60.f, 60.f, "%.0f");
        m_gui.sliderFloatValue("plOffBottomLeftX", "Bottom Left X", &g_cfg.espAnchorOffsetX[6], -60.f, 60.f, "%.0f");
        m_gui.sliderFloatValue("plOffBottomLeftY", "Bottom Left Y", &g_cfg.espAnchorOffsetY[6], -60.f, 60.f, "%.0f");
        m_gui.sliderFloatValue("plOffBottomRightX", "Bottom Right X", &g_cfg.espAnchorOffsetX[7], -60.f, 60.f, "%.0f");
        m_gui.sliderFloatValue("plOffBottomRightY", "Bottom Right Y", &g_cfg.espAnchorOffsetY[7], -60.f, 60.f, "%.0f");
    } else if (isVisible || isOcc) {
        m_gui.label("Use the preview window to enable/disable and place visible/occluded ESP items.", Theme::TEXT_MUTED, 14.f);
        m_gui.label("Use Sizes/Offsets subtab for component size and anchor offset tuning.", Theme::TEXT_MUTED, 14.f);
    }

    m_gui.separator();

    // ── Colors ───────────────────────────────────────────────────────────────
    m_gui.label("Colors", Theme::TEXT, 22.f);
    m_gui.dummy(10.f);

    if (isSettings) {
        m_gui.colorEdit4("HP Low", g_cfg.hpBarLowColor);
        m_gui.colorEdit4("HP Full", g_cfg.hpBarFullColor);
        m_gui.colorEdit4("Info Text", g_cfg.infoTextColor);
        m_gui.colorEdit4("Armor Bar", g_cfg.armorBarColor);
        m_gui.colorEdit4("Ammo Bar", g_cfg.ammoBarColor);
    } else if (isOcc) {
        m_gui.colorEdit4("Box Occluded", g_cfg.boxOccludedColor);
        m_gui.colorEdit4("Skeleton Occluded", g_cfg.skeletonOccludedColor);
        m_gui.colorEdit4("HP Occluded", g_cfg.hpBarOccludedColor);
        m_gui.colorEdit4("Chams Occluded", g_cfg.chamsOccludedColor);
    } else {
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
    float panelH = S(kPreviewPanelH);
    float panelGap = S(14.f);
    float panelX = m_winX + m_winW + panelGap;
    float panelY = m_winY + S(74.f);
    panelX += S(kPreviewPanelOffsetX);
    panelY += S(kPreviewPanelOffsetY);

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
    r.drawText(m_gui.font(), panelX + S(12.f), panelY + S(10.f), "Interactive ESP Preview", 0xFFE9ECFF, S(15.f));
    r.drawText(m_gui.font(), panelX + S(12.f), panelY + S(31.f), modeText, 0xFF9EA5C4, S(12.5f));

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
    const float simBoxW = modelW * kPreviewBoxW;
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
        const float sidePad = S(6.f);
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
            outX = simBoxX - w - sidePad - stack[2];
            outY = simBoxY;
            stack[2] += w + S(2.f);
            break;
        case 3:
            outX = simBoxX + simBoxW + sidePad + stack[3];
            outY = simBoxY;
            stack[3] += w + S(2.f);
            break;
        case 4:
            outX = simBoxX - w - S(4.f);
            outY = simBoxY - h - gap - stack[4];
            stack[4] += h + S(1.f);
            break;
        case 5:
            outX = simBoxX + simBoxW + S(4.f);
            outY = simBoxY - h - gap - stack[5];
            stack[5] += h + S(1.f);
            break;
        case 6:
            outX = simBoxX - w - S(4.f);
            outY = simBoxY + simBoxH + gap + stack[6];
            stack[6] += h + S(1.f);
            break;
        case 7:
        default:
            outX = simBoxX + simBoxW + S(4.f);
            outY = simBoxY + simBoxH + gap + stack[7];
            stack[7] += h + S(1.f);
            break;
        }

        if (anchor >= 0 && anchor < 8) {
            outX += S(g_cfg.espAnchorOffsetX[anchor]);
            outY += S(g_cfg.espAnchorOffsetY[anchor]);
        }
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
// Visuals panel
// ═════════════════════════════════════════════════════════════════════════════

void Menu::drawVisualsPanel() {
    auto& r = m_gui.renderer();
    const float s = m_uiScale;
    auto S = [s](float v) { return v * s; };

    float x = m_winX + m_sideW, y = m_winY;
    float w = m_winW - m_sideW;
    float pad = Theme::PADDING * s;

    drawPanelHeader(1);

    float clipX = x + pad;
    float clipY = m_gui.cursorY();
    float clipW = w - pad * 2;
    float clipH = m_winH - (clipY - y) - pad;

    bool clipHovered = m_gui.mouseX() >= clipX && m_gui.mouseX() <= clipX + clipW
                    && m_gui.mouseY() >= clipY && m_gui.mouseY() <= clipY + clipH;

    if (clipHovered) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.f)
            m_visualsScrollTarget -= wheel * S(42.f);
    }

    m_visualsScroll = smoothValue(m_visualsScroll, m_visualsScrollTarget, 0.22f, m_frameDt);

    m_gui.setCursor(clipX, clipY - m_visualsScroll);
    m_gui.setItemWidth(clipW - S(10.f));

    r.setClipRect(clipX, clipY, clipW, clipH);

    // General
    m_gui.label("General", Theme::TEXT, 22.f);
    m_gui.dummy(10.f);
    m_gui.toggleCheckbox("espEn", "ESP Enabled", &g_cfg.espEnabled);
    m_gui.toggleCheckbox("enemyOnly", "Enemy Only", &g_cfg.enemyOnly);

    m_gui.separator();

    // Players / boxes
    m_gui.label("Players", Theme::TEXT, 22.f);
    m_gui.dummy(10.f);
    m_gui.toggleCheckbox("boxEn", "Box Enabled", &g_cfg.boxEnabled);
    m_gui.sliderFloatValue("boxThk", "Box Thickness", &g_cfg.boxThickness, 0.5f, 5.f, "%.1f");

    m_gui.toggleCheckbox("hpEn", "HP Bar Enabled", &g_cfg.hpBarEnabled);
    m_gui.sliderFloatValue("hpW", "HP Bar Width", &g_cfg.hpBarWidth, 1.f, 10.f, "%.0f");

    m_gui.toggleCheckbox("skel", "Skeleton Enabled", &g_cfg.skeletonEnabled);
    m_gui.toggleCheckbox("chams", "Chams Enabled", &g_cfg.chamsEnabled);
    m_gui.sliderFloatValue("chamsA", "Chams Alpha", &g_cfg.chamsAlpha, 0.f, 1.f, "%.2f");

    m_gui.separator();

    // Colors
    m_gui.label("Colors", Theme::TEXT, 22.f);
    m_gui.dummy(10.f);
    m_gui.colorEdit4("Enemy Color", g_cfg.enemyColor);
    m_gui.colorEdit4("Team Color", g_cfg.teamColor);
    m_gui.colorEdit4("Skeleton Color", g_cfg.skeletonColor);
    m_gui.colorEdit4("HP Low Color", g_cfg.hpBarLowColor);
    m_gui.colorEdit4("HP Full Color", g_cfg.hpBarFullColor);

    m_gui.separator();

    // Grenades
    m_gui.label("Grenades", Theme::TEXT, 22.f);
    m_gui.dummy(10.f);
    m_gui.toggleCheckbox("grenEn", "Grenade ESP", &g_cfg.grenadeEnabled);
    m_gui.toggleCheckbox("grenTraj", "Grenade Trajectory", &g_cfg.grenadeTrajectory);
    m_gui.toggleCheckbox("bombTimer", "Bomb Timer", &g_cfg.bombTimerEnabled);
    m_gui.toggleCheckbox("specList", "Spectator List", &g_cfg.spectatorListEnabled);
    m_gui.toggleCheckbox("voteReveal", "Vote Revealer", &g_cfg.voteRevealerEnabled);
    m_gui.toggleCheckbox("radarEn", "Radar", &g_cfg.radarEnabled);
    m_gui.label("Radar Mode", Theme::TEXT_MUTED, 14.f);
    static const char* kRadarModes[] = {
        "Manual",
        "In-game overlay"
    };
    int radarMode = g_cfg.radarMode;
    if (radarMode < 0) radarMode = 0;
    if (radarMode > 1) radarMode = 1;
    if (m_gui.comboBox("##radarMode", kRadarModes, 2, &radarMode))
        g_cfg.radarMode = radarMode;

    if (g_cfg.radarMode == 0) {
        m_gui.sliderFloatValue("radarOpacity", "Radar Opacity", &g_cfg.radarBgOpacity, 0.f, 1.f, "%.2f");
    } else {
        g_cfg.radarBgOpacity = 0.f;
    }
    m_gui.sliderFloatValue("radarSize", "Radar Size", &g_cfg.radarSize, 120.f, 420.f, "%.0f");
    m_gui.sliderFloatValue("radarRange", "Radar Zoom", &g_cfg.radarRange, 400.f, 5000.f, "%.0f");
    m_gui.sliderFloatValue("radarBlip", "Radar Blip Size", &g_cfg.radarBlipSize, 2.f, 8.f, "%.1f");

    m_gui.separator();
    m_gui.label("Web Radar", Theme::TEXT, 22.f);
    m_gui.dummy(10.f);
    m_gui.toggleCheckbox("webRadarEnabled", "Web Radar Sharing", &g_cfg.webRadarEnabled);
    float webPublishMs = static_cast<float>(g_cfg.webRadarPublishMs);
    m_gui.sliderFloatValue("webRadarPublishMs", "Web Radar Publish (ms)", &webPublishMs, 1.f, 2000.f, "%.0f");
    g_cfg.webRadarPublishMs = static_cast<int>(webPublishMs + 0.5f);
    if (!g_cfg.webRadarSessionId.empty()) {
        std::string btnText = "Copy Link";
        if (m_gui.accentButton("copyWebRadarLink", btnText.c_str())) {
            if (copyUtf8ToClipboard(g_cfg.webRadarShareUrl)) {
                m_configStatus = "Web radar link copied";
            } else {
                m_configStatus = "Failed to copy web radar link";
            }
            m_configStatusTimer = 2.0f;
        }
        m_gui.label(g_cfg.webRadarShareUrl.c_str(), Theme::TEXT_MUTED, 13.f);
    } else {
        m_gui.label("Enable Web Radar Sharing to generate a link", Theme::TEXT_MUTED, 14.f);
    }

    static const char* kAAModes[] = {
        "Off", "MSAA", "FXAA Balanced", "FXAA High", "MSAA + FXAA Lite", "MSAA + FXAA High"
    };
    int aaIdx = g_cfg.aaMode;
    if (aaIdx < 0) aaIdx = 0;
    if (aaIdx > 5) aaIdx = 5;
    m_gui.label("Anti-Aliasing");
    if (m_gui.comboBox("##aaMode", kAAModes, 6, &aaIdx))
        g_cfg.aaMode = aaIdx;

    float contentHeight = m_gui.cursorY() - (clipY - m_visualsScroll);
    r.clearClipRect();

    m_visualsMaxScroll = (std::max)(0.f, contentHeight - clipH);
    if (m_visualsScrollTarget < 0.f) m_visualsScrollTarget = 0.f;
    if (m_visualsScrollTarget > m_visualsMaxScroll) m_visualsScrollTarget = m_visualsMaxScroll;
    if (m_visualsScroll < 0.f) m_visualsScroll = 0.f;
    if (m_visualsScroll > m_visualsMaxScroll) m_visualsScroll = m_visualsMaxScroll;

    // Scrollbar
    if (m_visualsMaxScroll > 0.5f) {
        float trackX = clipX + clipW - S(5.f);
        r.drawRoundedFilledRect(trackX, clipY, S(3.f), clipH, 0xFF101015, S(2.f));
        float thumbH = (std::max)(S(36.f), clipH * (clipH / (contentHeight + 0.001f)));
        float t = m_visualsScroll / m_visualsMaxScroll;
        float thumbY = clipY + (clipH - thumbH) * t;
        r.drawRoundedFilledRect(trackX, thumbY, S(3.f), thumbH, 0xFF8E95E8, S(2.f));
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Extra panel
// ═════════════════════════════════════════════════════════════════════════════

void Menu::drawExtraPanel() {
    drawPanelHeader(3);
    static const char* kLatencyModes[] = {
        "Balanced", "Low Latency", "Balanced + Prediction"
    };
    int latIdx = g_cfg.latencyMode;
    if (latIdx < 0) latIdx = 0;
    if (latIdx > 2) latIdx = 2;
    m_gui.label("Overlay Latency");
    if (m_gui.comboBox("##latencyMode", kLatencyModes, 3, &latIdx))
        g_cfg.latencyMode = latIdx;

    m_gui.separator();
    m_gui.label("Performance", Theme::TEXT, 22.f);
    m_gui.dummy(10.f);
    {
        static const char* kProcPrios[] = { "Normal", "Above Normal", "High" };
        int prio = std::clamp(g_cfg.processPriority, 0, 2);
        m_gui.label("Process Priority", Theme::TEXT_MUTED, 14.f);
        if (m_gui.comboBox("##procPrio", kProcPrios, 3, &prio))
            g_cfg.processPriority = prio;
    }
    {
        static const char* kThrPrios[] = { "Normal", "Above Normal", "Highest", "Time Critical" };
        int prio = std::clamp(g_cfg.entityThreadPriority, 0, 3);
        m_gui.label("Entity Thread Priority", Theme::TEXT_MUTED, 14.f);
        if (m_gui.comboBox("##thrPrio", kThrPrios, 4, &prio))
            g_cfg.entityThreadPriority = prio;
    }
    {
        static const char* kBgModes[] = { "Full", "Reduced", "Minimal" };
        int bg = std::clamp(g_cfg.bgMode, 0, 2);
        m_gui.label("Background Mode (unfocused)", Theme::TEXT_MUTED, 14.f);
        if (m_gui.comboBox("##bgMode", kBgModes, 3, &bg))
            g_cfg.bgMode = bg;
    }
    m_gui.label("Time Critical can improve responsiveness but uses more CPU.", Theme::TEXT_MUTED, 12.f);
    m_gui.separator();
    m_gui.toggleCheckbox("debugCon", "Debug Console", &g_cfg.debugConsole);
}

void Menu::drawIntelPanel() {
    auto& r = m_gui.renderer();
    const float s = m_uiScale;
    auto S = [s](float v) { return v * s; };

    float x = m_winX + m_sideW, y = m_winY;
    float w = m_winW - m_sideW;
    float pad = Theme::PADDING * s;

    auto intelView = MatchIntel::instance().view();

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
            m_intelScrollTarget -= wheel * S(42.f);
    }

    m_intelScroll = smoothValue(m_intelScroll, m_intelScrollTarget, 0.22f, m_frameDt);

    m_gui.setCursor(clipX, clipY - m_intelScroll);
    m_gui.setItemWidth(clipW - S(10.f));
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

    m_gui.label("Round Replay Timeline", Theme::TEXT, 22.f);
    m_gui.dummy(10.f);
    m_gui.label((std::string("Round #") + std::to_string(intelView.currentRound) + (intelView.roundLive ? " (live)" : "")).c_str(), Theme::TEXT_MUTED, 13.f);
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
    } else {
        m_gui.label("Timeline is empty for this round yet.", Theme::TEXT_MUTED, 13.f);
    }

    m_gui.separator();
    m_gui.label("Advanced Intel", Theme::TEXT, 22.f);
    m_gui.dummy(10.f);
    m_gui.label("Utility, threat, and heatmap modules are temporarily disabled until they are stable.", Theme::TEXT_MUTED, 13.f);

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
