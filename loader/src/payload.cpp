#include "payload.h"
#include "auth.h"
#include "debug_log.h"
#include "launch_handshake.h"
#include "payload_crypto.h"
#include "driver_identity.h"

#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>
#include <cstring>
#include <exception>
#include <cstdio>
#include <mutex>

#ifndef IDR_PAYLOAD
#define IDR_PAYLOAD 101
#endif

namespace {

HMODULE g_payloadModule = nullptr;

constexpr std::uint32_t kMagic = 0x474D5243u; // CRMG
constexpr std::uint32_t kMagicCrme = 0x454D5243u; // CRME

struct ArchiveReader {
    const unsigned char* data = nullptr;
    std::size_t size = 0;
    std::size_t pos = 0;

    bool read(void* dst, std::size_t n) {
        if (pos + n > size)
            return false;
        std::memcpy(dst, data + pos, n);
        pos += n;
        return true;
    }

    bool readU32(std::uint32_t& v) { return read(&v, sizeof(v)); }
};

std::wstring randomStem(int len = 10) {
    static const wchar_t chars[] = L"abcdefghijklmnopqrstuvwxyz0123456789";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, static_cast<int>(wcslen(chars) - 1));
    std::wstring s;
    s.reserve(len);
    for (int i = 0; i < len; ++i)
        s += chars[dist(gen)];
    return s;
}

std::filesystem::path tempPayloadRoot() {
    wchar_t tempPath[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempPath);
    return std::filesystem::path(tempPath) / L"crymore" / randomStem(12);
}

bool writeFile(const std::filesystem::path& path, const unsigned char* bytes, std::size_t len) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    if (!out)
        return false;
    out.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(len));
    if (!out.good())
        return false;
    // Strip Mark-of-the-Web so .NET DLLs in phys_tools/ can load from temp extract dir.
    const std::wstring ads = path.wstring() + L":Zone.Identifier";
    DeleteFileW(ads.c_str());
    return true;
}

bool loadEmbeddedPayload(std::vector<unsigned char>& outBytes) {
    HMODULE mod = g_payloadModule ? g_payloadModule : GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(mod, MAKEINTRESOURCEW(IDR_PAYLOAD), MAKEINTRESOURCEW(10));
    if (!res)
        return false;
    HGLOBAL mem = LoadResource(mod, res);
    if (!mem)
        return false;
    const auto* data = static_cast<const unsigned char*>(LockResource(mem));
    const DWORD size = SizeofResource(mod, res);
    if (!data || size == 0)
        return false;
    outBytes.assign(data, data + size);
    return true;
}

std::string wideToUtf8(const std::wstring& w) {
    if (w.empty())
        return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
        nullptr, 0, nullptr, nullptr);
    if (len <= 0)
        return {};
    std::string s(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
        s.data(), len, nullptr, nullptr);
    return s;
}

bool iequalsWide(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size())
        return false;
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

bool isPayloadHelperExe(const std::wstring& fname) {
    if (iequalsWide(fname, L"phys_extract.exe"))
        return true;
    if (iequalsWide(fname, L"crymore_loader.exe"))
        return true;
    if (iequalsWide(fname, L"Crymore.Loader.exe"))
        return true;
    if (iequalsWide(fname, DRV_MAPPER_EXE_NAME))
        return true;
    return false;
}

bool isOverlayHostExe(const std::wstring& fname) {
    return iequalsWide(fname, DRV_HOST_EXE_NAME);
}

PayloadLaunchResult extractArchive(const unsigned char* data, std::size_t size,
    const AuthSession& session) {
    PayloadLaunchResult out{};
    try {
        ArchiveReader ar{ data, size };

        std::uint32_t magic = 0;
        std::uint32_t version = 0;
        std::uint32_t count = 0;
        if (!ar.readU32(magic) || magic != kMagic || !ar.readU32(version) || !ar.readU32(count)) {
            out.error = "Embedded payload header is invalid (expected CRMG archive).";
            debugLog("[payload] extract: invalid archive header");
            return out;
        }
        debugLog("[payload] extract: archive v" + std::to_string(version) + " files=" + std::to_string(count));
        if (count == 0) {
            out.error = "Embedded payload is empty — rebuild loader after overlay Release build.";
            return out;
        }

        const std::filesystem::path root = tempPayloadRoot();
        std::filesystem::path overlayExe;
        std::filesystem::path fallbackExe;
        std::uint64_t bestFallbackBytes = 0;

        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t nameLen = 0;
            std::uint32_t dataLen = 0;
            if (!ar.readU32(nameLen) || nameLen == 0 || nameLen > 4096) {
                out.error = "Corrupt payload entry (name) at file " + std::to_string(i + 1) + ".";
                debugLog("[payload] extract: " + out.error);
                return out;
            }
            std::string rel(nameLen, '\0');
            if (!ar.read(rel.data(), nameLen) || !ar.readU32(dataLen)) {
                out.error = "Corrupt payload entry (header) at " + rel + ".";
                debugLog("[payload] extract: " + out.error);
                return out;
            }

            std::vector<unsigned char> bytes;
            if (dataLen > 0) {
                bytes.resize(dataLen);
                if (!ar.read(bytes.data(), dataLen)) {
                    out.error = "Corrupt payload entry (truncated) at " + rel + ".";
                    debugLog("[payload] extract: " + out.error);
                    return out;
                }
            }

            const std::filesystem::path outPath = root / std::filesystem::path(rel);
            if (dataLen > 0) {
                if (!writeFile(outPath, bytes.data(), bytes.size())) {
                    out.error = "Failed to write extracted file: " + rel;
                    return out;
                }
            } else {
                std::error_code ec;
                std::filesystem::create_directories(outPath.parent_path(), ec);
                std::ofstream empty(outPath, std::ios::binary);
                if (!empty) {
                    out.error = "Failed to write empty file: " + rel;
                    return out;
                }
            }

            const auto fname = outPath.filename().wstring();
            if (fname.size() > 4 && fname.ends_with(L".exe")) {
                if (isOverlayHostExe(fname)) {
                    overlayExe = outPath;
                } else if (!isPayloadHelperExe(fname) && dataLen > bestFallbackBytes) {
                    fallbackExe = outPath;
                    bestFallbackBytes = dataLen;
                }
            }
        }

        if (overlayExe.empty())
            overlayExe = fallbackExe;

        if (overlayExe.empty()) {
            for (const auto& e : std::filesystem::directory_iterator(root)) {
                if (e.path().extension() != L".exe")
                    continue;
                const auto fname = e.path().filename().wstring();
                if (isPayloadHelperExe(fname))
                    continue;
                if (isOverlayHostExe(fname)) {
                    overlayExe = e.path();
                    break;
                }
                if (overlayExe.empty())
                    overlayExe = e.path();
            }
        }
        if (overlayExe.empty()) {
            out.error = "No overlay executable found in embedded payload.";
            debugLog("[payload] extract: no .exe in archive");
            return out;
        }

        std::string err;
        debugLog("[payload] spawn: " + wideToUtf8(overlayExe.wstring()));
        if (!launchSpawnWithHandshake(overlayExe.wstring(), root.wstring(), session,
                out.childPid, err)) {
            out.error = err;
            debugLog("[payload] spawn failed: " + err);
            return out;
        }

        out.ok = true;
        out.launchedPath = overlayExe.wstring();
        out.launchedPathNarrow = wideToUtf8(out.launchedPath);
        return out;
    } catch (const std::exception& ex) {
        out.error = std::string("Extract/spawn exception: ") + ex.what();
        debugLog("[payload] extract exception: " + out.error);
        return out;
    } catch (...) {
        out.error = "Extract/spawn failed with unknown exception.";
        debugLog("[payload] extract exception: unknown");
        return out;
    }
}

std::filesystem::path loaderDirectory() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
}

bool jsonGetBool(const std::string& json, const std::string& key, bool& out) {
    const std::string needle = "\"" + key + "\":";
    const auto pos = json.find(needle);
    if (pos == std::string::npos)
        return false;
    const auto rest = json.substr(pos + needle.size());
    if (rest.rfind("true", 0) == 0) {
        out = true;
        return true;
    }
    if (rest.rfind("false", 0) == 0) {
        out = false;
        return true;
    }
    return false;
}

std::string jsonGetString(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\":\"";
    const auto pos = json.find(needle);
    if (pos == std::string::npos)
        return {};
    const auto start = pos + needle.size();
    const auto end = json.find('"', start);
    if (end == std::string::npos)
        return {};
    return json.substr(start, end - start);
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '\\' || c == '"')
            out += '\\';
        out += c;
    }
    return out;
}

std::string apiPathFromDownloadUrl(const std::string& url) {
    const auto pos = url.find("/v1/");
    if (pos == std::string::npos)
        return {};
    return url.substr(pos);
}

struct OverlayMeta {
    bool ok = false;
    std::string error;
    std::string version;
    std::string downloadUrl;
    std::string dekWrap;
};

struct OverlayCacheState {
    std::mutex mu;
    std::string version;
    std::vector<std::uint8_t> encrypted;
    bool prefetching = false;
    bool ready = false;
};

OverlayCacheState g_overlayCache;

OverlayMeta requestOverlayMeta(const AuthSession& session) {
    OverlayMeta meta{};
    if (!session.valid || session.accessToken.empty()) {
        meta.error = "Not signed in.";
        return meta;
    }
    if (session.payloadKey.empty()) {
        meta.error = "Session unlock key missing — sign in again.";
        return meta;
    }

    const std::string hwid = authGetHwid();
    const std::string body = std::string(R"({"hwid":")") + jsonEscape(hwid) + R"("})";

    std::string response;
    std::string httpErr;
    if (!authHttpPostJsonAuthPath("/downloads/overlay", body, session.accessToken, response, httpErr)) {
        meta.error = httpErr.empty() ? "Could not request overlay from server." : httpErr;
        return meta;
    }

    bool okFlag = false;
    if (!jsonGetBool(response, "ok", okFlag) || !okFlag) {
        meta.error = jsonGetString(response, "error");
        if (meta.error.empty())
            meta.error = "Server rejected overlay request.";
        return meta;
    }

    meta.version = jsonGetString(response, "version");
    meta.downloadUrl = jsonGetString(response, "download_url");
    meta.dekWrap = jsonGetString(response, "dek_wrap");
    if (meta.downloadUrl.empty() || meta.dekWrap.empty()) {
        meta.error = "Server returned incomplete overlay metadata.";
        return meta;
    }

    meta.ok = true;
    return meta;
}

PayloadLaunchResult decryptAndLaunch(const AuthSession& session,
    const std::vector<std::uint8_t>& encrypted,
    const std::string& dekWrap) {
    PayloadLaunchResult out{};
    if (encrypted.size() < 32) {
        out.error = "Downloaded overlay is too small.";
        return out;
    }

    authNotifyProgress("decrypt", 0, 0);
    std::vector<std::uint8_t> dek;
    std::string cryptoErr;
    if (!payloadUnwrapDek(session.payloadKey, dekWrap, dek, cryptoErr)) {
        out.error = cryptoErr;
        return out;
    }

    std::vector<std::uint8_t> plain;
    if (!payloadDecryptCrmeBytes(encrypted.data(), encrypted.size(), dek.data(), plain, cryptoErr)) {
        out.error = cryptoErr;
        return out;
    }

    authNotifyProgress("extract", 0, 0);
    return extractArchive(plain.data(), plain.size(), session);
}

} // namespace

void payloadSetModuleHandle(HMODULE module) {
    g_payloadModule = module;
}

PayloadLaunchResult launchEmbeddedPayload(const AuthSession& session) {
    std::vector<unsigned char> bytes;
    if (!loadEmbeddedPayload(bytes)) {
        debugLog("[payload] embedded resource missing");
        return { false, "Embedded payload resource missing.", {}, {}, 0 };
    }
    if (bytes.size() < 12) {
        debugLog("[payload] embedded payload too small");
        return { false, "Embedded payload is too small.", {}, {}, 0 };
    }

    std::uint32_t magic = 0;
    std::memcpy(&magic, bytes.data(), sizeof(magic));
    debugLog("[payload] embedded magic=0x" + ([](std::uint32_t m) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%08x", m);
        return std::string(buf);
    })(magic));

    if (magic == kMagicCrme) {
        if (session.payloadKey.empty()) {
            return { false, "Payload unlock key missing — sign in again.", {}, {}, 0 };
        }
        std::vector<std::uint8_t> plain;
        std::string err;
        if (!payloadDecryptCrme(bytes.data(), bytes.size(), session.payloadKey, plain, err)) {
            debugLog("[payload] embedded CRME decrypt failed: " + err);
            return { false, err, {}, {}, 0 };
        }
        bytes = std::move(plain);
    } else if (magic != kMagic) {
#ifdef CRYMORE_RELEASE
        return { false, "Embedded payload format unsupported.", {}, {}, 0 };
#else
        return { false, "Embedded payload header is invalid.", {}, {}, 0 };
#endif
    }

    return extractArchive(bytes.data(), bytes.size(), session);
}

PayloadLaunchResult launchSiblingOverlay(const AuthSession& session) {
    PayloadLaunchResult out{};
    const auto dir = loaderDirectory();
    debugLog("[payload] sibling search in " + wideToUtf8(dir.wstring()));
    std::filesystem::path chosen;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        if (!e.is_regular_file() || e.path().extension() != L".exe")
            continue;
        const auto name = e.path().filename().wstring();
        if (isPayloadHelperExe(name))
            continue;
        if (isOverlayHostExe(name)) {
            chosen = e.path();
            break;
        }
        if (chosen.empty())
            chosen = e.path();
    }
    if (chosen.empty()) {
        out.error = "No overlay executable found next to loader.";
        debugLog("[payload] sibling overlay not found");
        return out;
    }

    std::string err;
    if (!launchSpawnWithHandshake(chosen.wstring(), dir.wstring(), session,
            out.childPid, err)) {
        out.error = err;
        debugLog("[payload] sibling spawn failed: " + err);
        return out;
    }

    out.ok = true;
    out.launchedPath = chosen.wstring();
    out.launchedPathNarrow = wideToUtf8(out.launchedPath);
    return out;
}

PayloadLaunchResult launchServerPayload(const AuthSession& session) {
    PayloadLaunchResult out{};
    debugLog("[payload] POST /downloads/overlay (launch)");
    const OverlayMeta meta = requestOverlayMeta(session);
    if (!meta.ok) {
        out.error = meta.error;
        debugLog("[payload] overlay metadata failed: " + out.error);
        return out;
    }

    std::vector<std::uint8_t> encrypted;
    {
        std::lock_guard lock(g_overlayCache.mu);
        if (g_overlayCache.ready && !g_overlayCache.encrypted.empty() &&
            (meta.version.empty() || g_overlayCache.version == meta.version)) {
            encrypted = g_overlayCache.encrypted;
            debugLog("[payload] using cached overlay (" + std::to_string(encrypted.size()) + " bytes)");
        }
    }

    if (encrypted.empty()) {
        debugLog("[payload] download " + meta.downloadUrl.substr(0, std::min<std::size_t>(120, meta.downloadUrl.size())));
        authNotifyProgress("download", 0, 0);
        std::string httpErr;
        if (!authHttpDownloadUrl(meta.downloadUrl, encrypted, httpErr)) {
            out.error = httpErr.empty() ? "Overlay download failed." : httpErr;
            debugLog("[payload] overlay download failed: " + out.error);
            return out;
        }
        debugLog("[payload] downloaded encrypted bytes=" + std::to_string(encrypted.size()));

        std::lock_guard lock(g_overlayCache.mu);
        g_overlayCache.version = meta.version;
        g_overlayCache.encrypted = encrypted;
        g_overlayCache.ready = true;
        g_overlayCache.prefetching = false;
    }

    return decryptAndLaunch(session, encrypted, meta.dekWrap);
}

void payloadClearOverlayCache() {
    std::lock_guard<std::mutex> lock(g_overlayCache.mu);
    g_overlayCache.version.clear();
    g_overlayCache.encrypted.clear();
    g_overlayCache.prefetching = false;
    g_overlayCache.ready = false;
}

bool payloadOverlayCachePrefetching() {
    std::lock_guard lock(g_overlayCache.mu);
    return g_overlayCache.prefetching;
}

bool payloadOverlayCacheReady() {
    std::lock_guard lock(g_overlayCache.mu);
    return g_overlayCache.ready && !g_overlayCache.encrypted.empty();
}

void payloadPrefetchServerOverlay(const AuthSession& session) {
    {
        std::lock_guard lock(g_overlayCache.mu);
        if (g_overlayCache.prefetching)
            return;
        if (g_overlayCache.ready && !g_overlayCache.encrypted.empty())
            return;
        g_overlayCache.prefetching = true;
    }

    debugLog("[payload] prefetch started");
    const OverlayMeta meta = requestOverlayMeta(session);
    if (!meta.ok) {
        debugLog("[payload] prefetch metadata failed: " + meta.error);
        std::lock_guard lock(g_overlayCache.mu);
        g_overlayCache.prefetching = false;
        return;
    }

    authNotifyProgress("prefetch", 0, 0);
    std::vector<std::uint8_t> encrypted;
    std::string httpErr;
    if (!authHttpDownloadUrl(meta.downloadUrl, encrypted, httpErr, "prefetch")) {
        debugLog("[payload] prefetch download failed: " + httpErr);
        std::lock_guard lock(g_overlayCache.mu);
        g_overlayCache.prefetching = false;
        return;
    }

    const auto byteCount = encrypted.size();
    {
        std::lock_guard lock(g_overlayCache.mu);
        g_overlayCache.version = meta.version;
        g_overlayCache.encrypted = std::move(encrypted);
        g_overlayCache.ready = true;
        g_overlayCache.prefetching = false;
    }
    authNotifyProgress("prefetch", 1, 1);
    debugLog("[payload] prefetch complete (" + std::to_string(byteCount) + " bytes)");
}
