#include "steam/steam_avatars.h"

#include <Windows.h>
#include <wincodec.h>
#include <winhttp.h>
#include <wrl/client.h>
#include <objbase.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace {

using SteamAPI_Init_t = bool(__cdecl*)();
using SteamAPI_Shutdown_t = void(__cdecl*)();
using SteamAPI_RunCallbacks_t = void(__cdecl*)();
using SteamFriends_t = void*(__cdecl*)();
using SteamUtils_t = void*(__cdecl*)();

using RequestUserInformation_t = bool(__cdecl*)(void*, std::uint64_t, bool);
using GetMediumFriendAvatar_t = int(__cdecl*)(void*, std::uint64_t);
using GetImageSize_t = bool(__cdecl*)(void*, int, std::uint32_t*, std::uint32_t*);
using GetImageRGBA_t = bool(__cdecl*)(void*, int, unsigned char*, int);

static constexpr int kFriendsRequestUserInformation = 37;
static constexpr int kFriendsGetMediumFriendAvatar = 35;
static constexpr int kUtilsGetImageSize = 5;
static constexpr int kUtilsGetImageRGBA = 6;

static std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

template<typename Ret, typename... Args>
Ret callVtable(void* obj, int index, Args... args) {
    if (!obj)
        return Ret{};
    void** vtable = *reinterpret_cast<void***>(obj);
    if (!vtable || !vtable[index])
        return Ret{};
    auto fn = reinterpret_cast<Ret(__cdecl*)(void*, Args...)>(vtable[index]);
    return fn(obj, args...);
}

struct SteamApi {
    HMODULE dll = nullptr;
    SteamAPI_Init_t init = nullptr;
    SteamAPI_Shutdown_t shutdown = nullptr;
    SteamAPI_RunCallbacks_t runCallbacks = nullptr;
    SteamFriends_t friendsFn = nullptr;
    SteamUtils_t utilsFn = nullptr;
    void* friends = nullptr;
    void* utils = nullptr;
    bool ready = false;
};

static SteamApi g_steam;

static bool loadSteamApi() {
    if (g_steam.dll)
        return g_steam.ready;

    g_steam.dll = LoadLibraryW(L"steam_api64.dll");
    if (!g_steam.dll)
        return false;

    g_steam.init = reinterpret_cast<SteamAPI_Init_t>(GetProcAddress(g_steam.dll, "SteamAPI_Init"));
    g_steam.shutdown = reinterpret_cast<SteamAPI_Shutdown_t>(GetProcAddress(g_steam.dll, "SteamAPI_Shutdown"));
    g_steam.runCallbacks = reinterpret_cast<SteamAPI_RunCallbacks_t>(
        GetProcAddress(g_steam.dll, "SteamAPI_RunCallbacks"));
    g_steam.friendsFn = reinterpret_cast<SteamFriends_t>(GetProcAddress(g_steam.dll, "SteamFriends"));
    g_steam.utilsFn = reinterpret_cast<SteamUtils_t>(GetProcAddress(g_steam.dll, "SteamUtils"));

    if (!g_steam.init || !g_steam.runCallbacks || !g_steam.friendsFn || !g_steam.utilsFn)
        return false;

    if (!g_steam.init()) {
        std::fprintf(stderr, "[SteamAvatars] SteamAPI_Init failed (is Steam running?)\n");
        return false;
    }

    g_steam.friends = g_steam.friendsFn();
    g_steam.utils = g_steam.utilsFn();
    g_steam.ready = g_steam.friends && g_steam.utils;
    if (g_steam.ready)
        std::fprintf(stderr, "[SteamAvatars] Steam API ready\n");
    return g_steam.ready;
}

static void unloadSteamApi() {
    if (g_steam.ready && g_steam.shutdown)
        g_steam.shutdown();
    g_steam = {};
    if (g_steam.dll) {
        FreeLibrary(g_steam.dll);
        g_steam.dll = nullptr;
    }
}

static bool createSrvFromBgra(ID3D11Device* device,
                              const unsigned char* bgra,
                              std::uint32_t width,
                              std::uint32_t height,
                              Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& outSrv) {
    if (!device || !bgra || width == 0 || height == 0)
        return false;

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
    init.pSysMem = bgra;
    init.SysMemPitch = width * 4;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    if (FAILED(device->CreateTexture2D(&td, &init, tex.GetAddressOf())))
        return false;
    return SUCCEEDED(device->CreateShaderResourceView(tex.Get(), nullptr, outSrv.GetAddressOf()));
}

static bool createSrvFromRgba(ID3D11Device* device,
                              const unsigned char* rgba,
                              std::uint32_t width,
                              std::uint32_t height,
                              Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& outSrv) {
    if (!device || !rgba || width == 0 || height == 0)
        return false;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = rgba;
    init.SysMemPitch = width * 4;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    if (FAILED(device->CreateTexture2D(&td, &init, tex.GetAddressOf())))
        return false;
    return SUCCEEDED(device->CreateShaderResourceView(tex.Get(), nullptr, outSrv.GetAddressOf()));
}

static bool decodeWicToBgraSrv(ID3D11Device* device,
                               IWICBitmapSource* source,
                               Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& outSrv) {
    if (!device || !source)
        return false;

    Microsoft::WRL::ComPtr<IWICImagingFactory> wic;
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(wic.GetAddressOf()))))
        return false;
    if (FAILED(wic->CreateFormatConverter(converter.GetAddressOf())))
        return false;
    if (FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom)))
        return false;

    UINT width = 0;
    UINT height = 0;
    if (FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0)
        return false;

    std::vector<unsigned char> bgra(width * height * 4);
    if (FAILED(converter->CopyPixels(nullptr, width * 4,
                                     static_cast<UINT>(bgra.size()), bgra.data())))
        return false;

    return createSrvFromBgra(device, bgra.data(), width, height, outSrv);
}

static bool createSrvFromImageBytes(ID3D11Device* device,
                                    const std::vector<std::uint8_t>& bytes,
                                    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& outSrv) {
    if (!device || bytes.empty())
        return false;

    HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool needsUninit = SUCCEEDED(coHr);
    if (FAILED(coHr) && coHr != RPC_E_CHANGED_MODE)
        return false;

    Microsoft::WRL::ComPtr<IWICImagingFactory> wic;
    Microsoft::WRL::ComPtr<IWICStream> stream;
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    bool ok = false;

    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(wic.GetAddressOf())))
        && SUCCEEDED(wic->CreateStream(stream.GetAddressOf()))
        && SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()),
                                                  static_cast<DWORD>(bytes.size())))
        && SUCCEEDED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad,
                                                  decoder.GetAddressOf()))
        && SUCCEEDED(decoder->GetFrame(0, frame.GetAddressOf()))) {
        ok = decodeWicToBgraSrv(device, frame.Get(), outSrv);
    }

    if (needsUninit)
        CoUninitialize();
    return ok;
}

static bool createSrvFromImageFile(ID3D11Device* device,
                                   const std::filesystem::path& path,
                                   Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& outSrv) {
    if (!device || path.empty())
        return false;

    HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool needsUninit = SUCCEEDED(coHr);
    if (FAILED(coHr) && coHr != RPC_E_CHANGED_MODE)
        return false;

    Microsoft::WRL::ComPtr<IWICImagingFactory> wic;
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    bool ok = false;

    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(wic.GetAddressOf())))
        && SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                    WICDecodeMetadataCacheOnDemand,
                                                    decoder.GetAddressOf()))
        && SUCCEEDED(decoder->GetFrame(0, frame.GetAddressOf()))) {
        ok = decodeWicToBgraSrv(device, frame.Get(), outSrv);
    }

    if (needsUninit)
        CoUninitialize();
    return ok;
}

static std::filesystem::path findBotImagePath(const wchar_t* fileName) {
    std::vector<std::filesystem::path> candidates;
    candidates.emplace_back(fileName);
    candidates.emplace_back(std::filesystem::path(L"assets") / fileName);

    wchar_t exePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        const std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
        candidates.emplace_back(exeDir / fileName);
        candidates.emplace_back(exeDir / L"assets" / fileName);
        candidates.emplace_back(exeDir.parent_path() / fileName);
        candidates.emplace_back(exeDir.parent_path() / L"assets" / fileName);
    }

    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec))
            return candidate;
    }
    return {};
}

static bool httpGet(const wchar_t* host,
                    INTERNET_PORT port,
                    const wchar_t* path,
                    std::vector<std::uint8_t>& out) {
    out.clear();
    HINTERNET session = WinHttpOpen(L"AG Cs2/1.0",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        return false;

    HINTERNET connect = WinHttpConnect(session, host, port, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return false;
    }

    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path, nullptr,
                                           WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           WINHTTP_FLAG_SECURE);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    WinHttpAddRequestHeaders(request,
        L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) Valve/Steam Client\r\n",
        static_cast<DWORD>(-1L),
        WINHTTP_ADDREQ_FLAG_ADD);

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

static std::string extractXmlCdata(const std::string& xml, const char* tag) {
    const std::string open = std::string("<") + tag + "><![CDATA[";
    const std::string close = "]]></" + std::string(tag) + ">";
    const size_t start = xml.find(open);
    if (start == std::string::npos)
        return {};
    const size_t valueStart = start + open.size();
    const size_t end = xml.find(close, valueStart);
    if (end == std::string::npos)
        return {};
    return xml.substr(valueStart, end - valueStart);
}

static bool fetchAvatarUrlFromCommunity(std::uint64_t steamId, std::string& outUrl) {
    outUrl.clear();
    const std::wstring path = L"/profiles/" + std::to_wstring(steamId) + L"/?xml=1";
    std::vector<std::uint8_t> body;
    if (!httpGet(L"steamcommunity.com", INTERNET_DEFAULT_HTTPS_PORT, path.c_str(), body))
        return false;

    const std::string xml(reinterpret_cast<const char*>(body.data()), body.size());
    outUrl = extractXmlCdata(xml, "avatarMedium");
    if (outUrl.empty())
        outUrl = extractXmlCdata(xml, "avatarFull");
    if (outUrl.empty())
        outUrl = extractXmlCdata(xml, "avatarIcon");
    return !outUrl.empty();
}

static bool downloadUrl(const std::string& url, std::vector<std::uint8_t>& out) {
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
    const INTERNET_PORT port = parts.nPort ? parts.nPort
        : (secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT);
    return httpGet(host, port, path, out);
}

static bool trySteamApiAvatar(ID3D11Device* device,
                              std::uint64_t steamId,
                              Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& outSrv) {
    if (!g_steam.ready || !device)
        return false;

    callVtable<bool>(g_steam.friends, kFriendsRequestUserInformation, steamId, false);

    const int image = callVtable<int>(g_steam.friends, kFriendsGetMediumFriendAvatar, steamId);
    if (image <= 0)
        return false;

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (!callVtable<bool>(g_steam.utils, kUtilsGetImageSize, image, &width, &height))
        return false;
    if (width == 0 || height == 0)
        return false;

    std::vector<unsigned char> rgba(static_cast<size_t>(width) * height * 4);
    if (!callVtable<bool>(g_steam.utils, kUtilsGetImageRGBA, image,
                          rgba.data(), static_cast<int>(rgba.size())))
        return false;

    return createSrvFromRgba(device, rgba.data(), width, height, outSrv);
}

static bool tryHttpAvatar(ID3D11Device* device,
                          std::uint64_t steamId,
                          Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& outSrv) {
    std::string url;
    if (!fetchAvatarUrlFromCommunity(steamId, url))
        return false;

    std::vector<std::uint8_t> imageBytes;
    if (!downloadUrl(url, imageBytes))
        return false;

    return createSrvFromImageBytes(device, imageBytes, outSrv);
}

} // namespace

SteamAvatars& SteamAvatars::instance() {
    static SteamAvatars s;
    return s;
}

bool SteamAvatars::loadBotAvatars() {
    if (!m_device)
        return false;

    const auto tPath = findBotImagePath(L"t_bot.jpeg");
    const auto ctPath = findBotImagePath(L"ct_bot.jpeg");
    bool ok = false;
    if (!tPath.empty() && !m_botTSrv && createSrvFromImageFile(m_device, tPath, m_botTSrv))
        ok = true;
    if (!ctPath.empty() && !m_botCtSrv && createSrvFromImageFile(m_device, ctPath, m_botCtSrv))
        ok = true;
    if (m_botTSrv)
        std::fprintf(stderr, "[SteamAvatars] Loaded T bot avatar\n");
    if (m_botCtSrv)
        std::fprintf(stderr, "[SteamAvatars] Loaded CT bot avatar\n");
    if (!ok)
        std::fprintf(stderr, "[SteamAvatars] Bot avatars not found (t_bot.jpeg / ct_bot.jpeg)\n");
    return ok;
}

bool SteamAvatars::init(ID3D11Device* device) {
    m_device = device;
    loadSteamApi();
    loadBotAvatars();
    return m_device != nullptr;
}

void SteamAvatars::shutdown() {
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_cache.clear();
    }
    m_botTSrv.Reset();
    m_botCtSrv.Reset();
    unloadSteamApi();
    m_device = nullptr;
}

void SteamAvatars::request(std::uint64_t steamId) {
    if (steamId == 0)
        return;

    std::lock_guard<std::mutex> lock(m_mu);
    Entry& entry = m_cache[steamId];
    if (entry.state == EntryState::Ready || entry.state == EntryState::Loading)
        return;

    entry.state = EntryState::Loading;
    entry.requestedMs = nowMs();
    entry.nextHttpMs = entry.requestedMs;
    entry.httpQueued = false;
}

ID3D11ShaderResourceView* SteamAvatars::get(std::uint64_t steamId) {
    if (steamId == 0)
        return nullptr;

    std::lock_guard<std::mutex> lock(m_mu);
    const auto it = m_cache.find(steamId);
    if (it == m_cache.end() || it->second.state != EntryState::Ready)
        return nullptr;
    return it->second.srv.Get();
}

ID3D11ShaderResourceView* SteamAvatars::getBot(int teamNum) {
    if (teamNum == 3 && m_botCtSrv)
        return m_botCtSrv.Get();
    if (m_botTSrv)
        return m_botTSrv.Get();
    return m_botCtSrv.Get();
}

ID3D11ShaderResourceView* SteamAvatars::resolve(bool isBot, int teamNum, std::uint64_t steamId) {
    if (isBot)
        return getBot(teamNum);
    if (steamId == 0)
        return nullptr;
    request(steamId);
    return get(steamId);
}

void SteamAvatars::tick() {
    if (!m_device)
        return;

    if (g_steam.ready && g_steam.runCallbacks)
        g_steam.runCallbacks();

    const std::uint64_t t = nowMs();
    if (!m_botTSrv || !m_botCtSrv)
        loadBotAvatars();

    std::lock_guard<std::mutex> lock(m_mu);

    for (auto& [steamId, entry] : m_cache) {
        if (entry.state != EntryState::Loading)
            continue;

        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        if (trySteamApiAvatar(m_device, steamId, srv)) {
            entry.srv = srv;
            entry.state = EntryState::Ready;
            continue;
        }

        if (t - entry.requestedMs > 15000) {
            entry.state = EntryState::Failed;
            continue;
        }

        if (!entry.httpQueued && t >= entry.nextHttpMs && t - m_lastHttpMs >= 450) {
            entry.httpQueued = true;
            m_lastHttpMs = t;
            if (tryHttpAvatar(m_device, steamId, srv)) {
                entry.srv = srv;
                entry.state = EntryState::Ready;
            } else {
                entry.httpQueued = false;
                entry.nextHttpMs = t + 1500;
            }
        }
    }
}
