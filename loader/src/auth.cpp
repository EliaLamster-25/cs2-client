#include "auth.h"
#include "str_obf.h"

#include <Windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <array>
#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>
#include <vector>
#include <cstring>

#pragma comment(lib, "winhttp.lib")

namespace {

static const wchar_t* kDefaultApiHost = L"crymore.crymore-pw.workers.dev";
static const wchar_t* kApiPrefix = L"/v1";

// ── SHA-256 helper ─────────────────────────────────────────────────────────

std::string sha256Hex(const std::string& input) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::array<unsigned char, 32> digest{};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return {};

    DWORD objLen = 0;
    DWORD cb = 0;
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objLen),
                          sizeof(objLen), &cb, 0) < 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }
    std::vector<unsigned char> obj(objLen);
    if (BCryptCreateHash(alg, &hash, obj.data(), objLen, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }
    BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())),
                   static_cast<ULONG>(input.size()), 0);
    BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);

    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (unsigned char b : digest) {
        out += kHex[b >> 4];
        out += kHex[b & 0x0f];
    }
    return out;
}

std::string trimCopy(const std::string& s) {
    const auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
        return {};
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string normalizeUsername(const std::string& username) {
    std::string out = trimCopy(username);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

// ── HWID helpers ───────────────────────────────────────────────────────────

std::string getVolumeSerial() {
    wchar_t sysDir[MAX_PATH] = {};
    GetSystemDirectoryW(sysDir, MAX_PATH);
    std::wstring root;
    if (sysDir[0])
        root = std::wstring(1, sysDir[0]) + L":\\";
    else
        root = L"C:\\";

    DWORD serial = 0;
    if (!GetVolumeInformationW(root.c_str(), nullptr, 0, &serial,
                               nullptr, nullptr, nullptr, 0))
        return "";

    std::ostringstream oss;
    oss << std::hex << std::setw(8) << std::setfill('0') << serial;
    return oss.str();
}

std::string getMachineGuid() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Cryptography",
        0, KEY_READ | KEY_WOW64_64KEY, &hKey) != ERROR_SUCCESS)
        return "";

    wchar_t guid[256] = {};
    DWORD size = sizeof(guid);
    LSTATUS status = RegQueryValueExW(hKey, L"MachineGuid", nullptr, nullptr,
        reinterpret_cast<BYTE*>(guid), &size);
    RegCloseKey(hKey);

    if (status != ERROR_SUCCESS)
        return "";

    char out[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, guid, -1, out, sizeof(out), nullptr, nullptr);
    return std::string(out);
}

std::string computeHwid() {
    std::string serial = getVolumeSerial();
    std::string guid = getMachineGuid();
    if (serial.empty() && guid.empty())
        return "hwid_unavailable";
    return sha256Hex(serial + ":" + guid);
}

// ── Token generation (dev only) ────────────────────────────────────────────

std::string makeDevToken(const std::string& user) {
    const auto now = std::chrono::system_clock::now().time_since_epoch().count();
    return "dev." + sha256Hex(user + std::to_string(now)).substr(0, 32);
}

AuthResponse fail(const std::string& msg) {
    AuthResponse r{};
    r.error = msg;
    return r;
}

AuthResponse succeed(const std::string& user, const std::string& token) {
    AuthResponse r{};
    r.ok = true;
    r.session.valid = true;
    r.session.username = user;
    r.session.accessToken = token;
    r.session.refreshToken = "refresh_" + token;
    r.session.expiresAt = "placeholder-7d";
    r.session.plan = "dev";
    r.session.hwid = computeHwid();
    return r;
}

// ── JSON helpers (minimal) ─────────────────────────────────────────────────

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c; break;
        }
    }
    return out;
}

std::string jsonGetString(const std::string& json, const std::string& key) {
    const std::string quoted = "\"" + key + "\":\"";
    auto pos = json.find(quoted);
    if (pos == std::string::npos)
        return {};
    pos += quoted.size();
    std::string out;
    for (size_t i = pos; i < json.size(); ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) {
            out += json[i + 1];
            ++i;
            continue;
        }
        if (json[i] == '"')
            break;
        out += json[i];
    }
    return out;
}

int jsonGetInt(const std::string& json, const std::string& key, int fallback = 0) {
    const std::string needle = "\"" + key + "\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos)
        return fallback;
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
        ++pos;
    return std::atoi(json.c_str() + pos);
}

std::int64_t jsonGetInt64(const std::string& json, const std::string& key, std::int64_t fallback = 0) {
    const std::string needle = "\"" + key + "\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos)
        return fallback;
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
        ++pos;
    return _strtoi64(json.c_str() + pos, nullptr, 10);
}

bool jsonGetBool(const std::string& json, const std::string& key, bool& out) {
    const std::string t = "\"" + key + "\":true";
    const std::string f = "\"" + key + "\":false";
    if (json.find(t) != std::string::npos) {
        out = true;
        return true;
    }
    if (json.find(f) != std::string::npos) {
        out = false;
        return true;
    }
    return false;
}

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty())
        return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0)
        return {};
    std::wstring w(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), len);
    return w;
}

std::string wideToUtf8(const std::wstring& w) {
    if (w.empty())
        return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0)
        return {};
    std::string s(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), len, nullptr, nullptr);
    return s;
}

struct ApiConfig {
    std::wstring host;
    INTERNET_PORT port = 443;
    bool tls = true;
};

ApiConfig readApiConfig() {
    ApiConfig cfg{};
    cfg.host = kDefaultApiHost;
    cfg.port = 443;
    cfg.tls = true;

    char hostBuf[256] = {};
    if (GetEnvironmentVariableA("CRYMORE_API_HOST", hostBuf, sizeof(hostBuf)) > 0) {
        cfg.host = utf8ToWide(hostBuf);
        cfg.port = INTERNET_DEFAULT_HTTP_PORT;
        cfg.tls = false;
    }

    char tlsBuf[8] = {};
    if (GetEnvironmentVariableA("CRYMORE_API_TLS", tlsBuf, sizeof(tlsBuf)) > 0) {
        cfg.tls = (tlsBuf[0] == '1' || tlsBuf[0] == 'y' || tlsBuf[0] == 'Y');
        cfg.port = cfg.tls ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    }

    return cfg;
}

bool httpRequest(const wchar_t* method, const std::wstring& path,
    const std::string& body, const std::wstring& extraHeaders,
    std::string& outBody, std::string& outError) {
    const ApiConfig cfg = readApiConfig();

    HINTERNET session = WinHttpOpen(L"crymore-loader/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        outError = "WinHttpOpen failed.";
        return false;
    }

    HINTERNET connect = WinHttpConnect(session, cfg.host.c_str(), cfg.port, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        outError = "WinHttpConnect failed.";
        return false;
    }

    DWORD flags = cfg.tls ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect, method, path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        outError = "WinHttpOpenRequest failed.";
        return false;
    }

    if (cfg.tls) {
        DWORD secFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA;
        WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));
    }

    std::wstring headers = L"Content-Type: application/json\r\n";
    if (!extraHeaders.empty())
        headers += extraHeaders;

    const BOOL sent = WinHttpSendRequest(request,
        headers.c_str(), static_cast<DWORD>(headers.size()),
        body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
        static_cast<DWORD>(body.size()),
        static_cast<DWORD>(body.size()), 0);

    if (!sent || !WinHttpReceiveResponse(request, nullptr)) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        outError = "HTTP request failed.";
        return false;
    }

    outBody.clear();
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request, &avail))
            break;
        if (avail == 0)
            break;
        std::vector<char> buf(avail);
        DWORD read = 0;
        if (!WinHttpReadData(request, buf.data(), avail, &read))
            break;
        outBody.append(buf.data(), read);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return true;
}

AuthProgressFn g_progressFn = nullptr;
void* g_progressCtx = nullptr;

void reportDownloadProgress(const char* step, std::uint64_t bytes, std::uint64_t total) {
    if (g_progressFn)
        g_progressFn(step, bytes, total, g_progressCtx);
}

struct HttpEndpoint {
    std::wstring host;
    INTERNET_PORT port = 443;
    bool tls = true;
    std::wstring path;
};

bool parseHttpUrl(const std::string& url, HttpEndpoint& out) {
    const auto schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos)
        return false;

    const std::string scheme = url.substr(0, schemeEnd);
    out.tls = (scheme == "https");
    out.port = out.tls ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;

    std::size_t start = schemeEnd + 3;
    std::size_t pathStart = url.find('/', start);
    const std::string hostPort = pathStart == std::string::npos
        ? url.substr(start)
        : url.substr(start, pathStart - start);
    out.path = pathStart == std::string::npos
        ? L"/"
        : utf8ToWide(url.substr(pathStart));

    const auto colon = hostPort.find(':');
    const std::string host = colon == std::string::npos
        ? hostPort
        : hostPort.substr(0, colon);
    if (colon != std::string::npos) {
        out.port = static_cast<INTERNET_PORT>(std::atoi(hostPort.c_str() + colon + 1));
        out.tls = out.port == INTERNET_DEFAULT_HTTPS_PORT;
    }
    out.host = utf8ToWide(host);
    return !out.host.empty();
}

bool httpDownloadBinary(const HttpEndpoint& ep, const std::wstring& extraHeaders,
    std::vector<std::uint8_t>& outBody, std::string& outError, const char* stepLabel) {
    outBody.clear();

    HINTERNET session = WinHttpOpen(L"crymore-loader/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        outError = "WinHttpOpen failed.";
        return false;
    }

    HINTERNET connect = WinHttpConnect(session, ep.host.c_str(), ep.port, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        outError = "WinHttpConnect failed.";
        return false;
    }

    DWORD flags = ep.tls ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", ep.path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        outError = "WinHttpOpenRequest failed.";
        return false;
    }

    if (ep.tls) {
        DWORD secFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA;
        WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));
    }

    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    std::wstring headers = extraHeaders;
    const BOOL sent = WinHttpSendRequest(request,
        headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
        headers.empty() ? 0 : static_cast<DWORD>(headers.size()),
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0);

    if (!sent || !WinHttpReceiveResponse(request, nullptr)) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        outError = "HTTP download request failed.";
        return false;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (WinHttpQueryHeaders(request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX)
        && status >= 400) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        outError = "HTTP download failed with status " + std::to_string(status) + ".";
        return false;
    }

    std::uint64_t totalSize = 0;
    DWORD totalLen = sizeof(totalSize);
    DWORD contentLength = 0;
    if (WinHttpQueryHeaders(request,
            WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &contentLength, &totalLen, WINHTTP_NO_HEADER_INDEX)) {
        totalSize = contentLength;
    }

    reportDownloadProgress(stepLabel, 0, totalSize);

    std::uint64_t received = 0;
    std::uint64_t lastReport = 0;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request, &avail))
            break;
        if (avail == 0)
            break;

        const std::size_t offset = outBody.size();
        outBody.resize(offset + avail);
        DWORD read = 0;
        if (!WinHttpReadData(request, outBody.data() + offset, avail, &read)) {
            outBody.resize(offset);
            break;
        }
        outBody.resize(offset + read);
        received += read;

        if (received - lastReport >= 256 * 1024 || read == 0) {
            reportDownloadProgress(stepLabel, received, totalSize);
            lastReport = received;
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    if (outBody.empty()) {
        outError = "Download returned no data.";
        return false;
    }

    reportDownloadProgress(stepLabel, received, totalSize > 0 ? totalSize : received);
    return true;
}

bool httpDownloadFromApiPath(const std::wstring& path, const std::wstring& extraHeaders,
    std::vector<std::uint8_t>& outBody, std::string& outError, const char* stepLabel) {
    const ApiConfig cfg = readApiConfig();
    HttpEndpoint ep{};
    ep.host = cfg.host;
    ep.port = cfg.port;
    ep.tls = cfg.tls;
    ep.path = path;
    return httpDownloadBinary(ep, extraHeaders, outBody, outError, stepLabel);
}

bool httpPostJson(const std::wstring& subPath, const std::string& body,
    std::string& outBody, std::string& outError) {
    const std::wstring path = std::wstring(kApiPrefix) + subPath;
    return httpRequest(L"POST", path, body, L"", outBody, outError);
}

bool httpPostJsonAuth(const std::wstring& subPath, const std::string& body,
    const std::string& bearer, std::string& outBody, std::string& outError) {
    const std::wstring path = std::wstring(kApiPrefix) + subPath;
    const std::wstring authHeader = L"Authorization: Bearer " + utf8ToWide(bearer) + L"\r\n";
    return httpRequest(L"POST", path, body, authHeader, outBody, outError);
}

AuthResponse parseAuthJson(const std::string& json, const std::string& hwid) {
    bool okFlag = false;
    if (!jsonGetBool(json, "ok", okFlag) || !okFlag) {
        std::string err = jsonGetString(json, "error");
        if (err.empty())
            err = "Authentication failed.";
        return fail(err);
    }

    const std::string access = jsonGetString(json, "access_token");
    if (access.empty())
        return fail("Server response missing access_token.");

    AuthResponse r{};
    r.ok = true;
    r.session.valid = true;
    r.session.accessToken = access;
    r.session.refreshToken = jsonGetString(json, "refresh_token");
    r.session.username = jsonGetString(json, "username");
    r.session.plan = jsonGetString(json, "plan");
    if (r.session.plan.empty())
        r.session.plan = "free";
    r.session.expiresAt = jsonGetString(json, "subscription_expires_at");
    r.session.avatarUrl = jsonGetString(json, "avatar_url");
    r.session.subscriptionDaysRemaining = jsonGetInt(json, "subscription_days_remaining", 0);
    r.session.subscriptionExpiresUnix = jsonGetInt64(json, "subscription_expires_unix", 0);
    r.session.payloadKey = jsonGetString(json, "payload_key");
    r.session.hwid = hwid;
    return r;
}

} // namespace

bool authDevBypassEnabled() {
#ifdef CRYMORE_RELEASE
    return false;
#else
    if (GetEnvironmentVariableA("CRYMORE_DEV", nullptr, 0) > 0)
        return true;
#ifdef LOADER_DEV_LAUNCH
    return true;
#endif
    return false;
#endif
}

std::string authGetHwid() {
    return computeHwid();
}

AuthResponse authLogin(const std::string& username, const std::string& password) {
    const std::string user = normalizeUsername(username);
    const std::string pass = trimCopy(password);
    if (user.empty())
        return fail("Username is required.");
    if (pass.empty())
        return fail("Password is required.");
    if (user.size() > 64)
        return fail("Username too long (max 64 characters).");
    if (pass.size() > 128)
        return fail("Password too long (max 128 characters).");

    const std::string hwid = computeHwid();

    if (authDevBypassEnabled()) {
        if (user == "demo" && pass == "demo") {
            auto r = succeed(user, makeDevToken(user));
            r.session.hwid = hwid;
            return r;
        }
        auto r = succeed(user, makeDevToken(user + pass));
        r.session.hwid = hwid;
        return r;
    }

    // Send plain password over TLS — server hashes the same way as the website.
    const std::string body = std::string(R"({"username":")") + jsonEscape(user) +
        R"(","password":")" + jsonEscape(pass) +
        R"(","hwid":")" + jsonEscape(hwid) + R"("})";

    std::string response;
    std::string httpErr;
    if (!httpPostJson(L"/auth/login", body, response, httpErr))
        return fail(httpErr.empty() ? "Could not reach authentication server." : httpErr);

    auto parsed = parseAuthJson(response, hwid);
    if (!parsed.ok)
        return parsed;

    return parsed;
}

AuthResponse authValidateSession(const AuthSession& session) {
    if (!session.valid || session.accessToken.empty())
        return fail("No active session.");

    if (session.accessToken.rfind("dev.", 0) == 0) {
        AuthResponse r{};
        r.ok = true;
        r.session = session;
        return r;
    }

    const std::string hwid = computeHwid();
    const std::string body = std::string(R"({"hwid":")") + jsonEscape(hwid) + R"("})";
    std::string response;
    std::string httpErr;
    if (!httpPostJsonAuth(L"/auth/validate", body, session.accessToken, response, httpErr))
        return fail(httpErr.empty() ? "Could not validate session online." : httpErr);

    bool okFlag = false;
    if (!jsonGetBool(response, "ok", okFlag) || !okFlag) {
        std::string err = jsonGetString(response, "error");
        return fail(err.empty() ? "Session invalid." : err);
    }

    AuthResponse r{};
    r.ok = true;
    r.session = session;
    r.session.hwid = hwid;
    r.session.username = jsonGetString(response, "username");
    if (r.session.username.empty())
        r.session.username = session.username;
    r.session.plan = jsonGetString(response, "plan");
    if (r.session.plan.empty())
        r.session.plan = session.plan;
    r.session.expiresAt = jsonGetString(response, "subscription_expires_at");
    if (r.session.expiresAt.empty())
        r.session.expiresAt = session.expiresAt;
    r.session.avatarUrl = jsonGetString(response, "avatar_url");
    if (r.session.avatarUrl.empty())
        r.session.avatarUrl = session.avatarUrl;
    r.session.subscriptionDaysRemaining =
        jsonGetInt(response, "subscription_days_remaining", session.subscriptionDaysRemaining);
    r.session.subscriptionExpiresUnix =
        jsonGetInt64(response, "subscription_expires_unix", session.subscriptionExpiresUnix);
    const std::string payloadKey = jsonGetString(response, "payload_key");
    if (!payloadKey.empty())
        r.session.payloadKey = payloadKey;
    else if (!session.payloadKey.empty())
        r.session.payloadKey = session.payloadKey;
    return r;
}

AuthResponse authRefreshSession(const AuthSession& session) {
    if (!session.valid || session.refreshToken.empty())
        return fail("No refresh token available.");

    if (session.refreshToken.rfind("refresh_dev.", 0) == 0) {
        AuthResponse r{};
        r.ok = true;
        r.session = session;
        return r;
    }

    const std::string hwid = computeHwid();
    const std::string body = std::string(R"({"refresh_token":")") + jsonEscape(session.refreshToken) +
        R"(","hwid":")" + jsonEscape(hwid) + R"("})";

    std::string response;
    std::string httpErr;
    if (!httpPostJson(L"/auth/refresh", body, response, httpErr))
        return fail(httpErr.empty() ? "Could not refresh session." : httpErr);

    return parseAuthJson(response, hwid);
}

bool authHttpPostJsonAuthPath(const char* subPathUtf8, const std::string& body,
    const std::string& bearer, std::string& outBody, std::string& outError) {
    const std::wstring path = std::wstring(kApiPrefix) + utf8ToWide(subPathUtf8);
    const std::wstring authHeader = L"Authorization: Bearer " + utf8ToWide(bearer) + L"\r\n";
    return httpRequest(L"POST", path, body, authHeader, outBody, outError);
}

bool authHttpGetBinaryPath(const char* subPathUtf8, const std::wstring& extraHeaders,
    std::vector<std::uint8_t>& outBody, std::string& outError) {
    const std::wstring path = utf8ToWide(subPathUtf8);
    return httpDownloadFromApiPath(path, extraHeaders, outBody, outError, "download");
}

void authSetProgressCallback(AuthProgressFn fn, void* user) {
    g_progressFn = fn;
    g_progressCtx = user;
}

void authClearProgressCallback() {
    g_progressFn = nullptr;
    g_progressCtx = nullptr;
}

void authNotifyProgress(const char* step, std::uint64_t bytes, std::uint64_t total) {
    reportDownloadProgress(step, bytes, total);
}

bool authHttpDownloadUrl(const std::string& urlOrPath, std::vector<std::uint8_t>& outBody,
    std::string& outError, const char* progressStep) {
    if (!progressStep || !progressStep[0])
        progressStep = "download";
    const char* step = progressStep;
    if (urlOrPath.rfind("http://", 0) == 0 || urlOrPath.rfind("https://", 0) == 0) {
        HttpEndpoint ep{};
        if (!parseHttpUrl(urlOrPath, ep)) {
            outError = "Invalid download URL.";
            return false;
        }
        return httpDownloadBinary(ep, L"", outBody, outError, step);
    }

    if (urlOrPath.empty()) {
        outError = "Empty download path.";
        return false;
    }

    const std::wstring path = urlOrPath[0] == '/'
        ? utf8ToWide(urlOrPath)
        : std::wstring(kApiPrefix) + utf8ToWide(urlOrPath);
    return httpDownloadFromApiPath(path, L"", outBody, outError, step);
}
