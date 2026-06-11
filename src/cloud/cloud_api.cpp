#include "cloud/cloud_api.h"

#include "json.hpp"
#include <Windows.h>
#include <winhttp.h>
#include <algorithm>
#include <cstdlib>

#pragma comment(lib, "winhttp.lib")

using json = nlohmann::json;

namespace {

std::string g_token;

struct ApiEndpoint {
    std::wstring host = L"crymore.crymore-pw.workers.dev";
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    bool tls = true;
};

ApiEndpoint readEndpoint() {
    ApiEndpoint ep{};
    char hostBuf[256] = {};
    if (GetEnvironmentVariableA("CRYMORE_API_HOST", hostBuf, sizeof(hostBuf)) > 0) {
        ep.host.clear();
        ep.host.reserve(std::strlen(hostBuf));
        for (const char* p = hostBuf; *p; ++p)
            ep.host.push_back(static_cast<wchar_t>(*p));
        ep.port = INTERNET_DEFAULT_HTTP_PORT;
        ep.tls = false;
    }
    char tlsBuf[8] = {};
    if (GetEnvironmentVariableA("CRYMORE_API_TLS", tlsBuf, sizeof(tlsBuf)) > 0) {
        ep.tls = (tlsBuf[0] == '1' || tlsBuf[0] == 'y' || tlsBuf[0] == 'Y');
        ep.port = ep.tls ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    }
    return ep;
}

bool httpRequest(const wchar_t* method, const std::wstring& path,
                 const std::string& body, const std::string& bearer,
                 std::string& outBody, int& statusOut, std::string& error) {
    const ApiEndpoint ep = readEndpoint();
    HINTERNET session = WinHttpOpen(L"CrymoreOverlay/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { error = "WinHttpOpen failed."; return false; }
    WinHttpSetTimeouts(session, 4000, 4000, 8000, 8000);

    HINTERNET connect = WinHttpConnect(session, ep.host.c_str(), ep.port, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        error = "WinHttpConnect failed.";
        return false;
    }

    DWORD flags = ep.tls ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect, method, path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        error = "WinHttpOpenRequest failed.";
        return false;
    }

    if (ep.tls) {
        DWORD secFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA;
        WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));
    }

    std::wstring headers = L"Content-Type: application/json\r\n";
    if (!bearer.empty()) {
        std::string auth = "Authorization: Bearer " + bearer + "\r\n";
        headers.reserve(headers.size() + auth.size());
        for (char c : auth) headers.push_back(static_cast<wchar_t>(c));
    }

    const BOOL sent = WinHttpSendRequest(request,
        headers.c_str(), static_cast<DWORD>(headers.size()),
        body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
        static_cast<DWORD>(body.size()),
        static_cast<DWORD>(body.size()), 0);

    if (!sent || !WinHttpReceiveResponse(request, nullptr)) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        error = "HTTP request failed.";
        return false;
    }

    statusOut = 0;
    DWORD statusSize = sizeof(statusOut);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusOut, &statusSize, WINHTTP_NO_HEADER_INDEX);

    outBody.clear();
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request, &avail) || avail == 0)
            break;
        const size_t offset = outBody.size();
        outBody.resize(offset + avail);
        DWORD read = 0;
        if (!WinHttpReadData(request, outBody.data() + offset, avail, &read)) {
            error = "Read failed.";
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return false;
        }
        outBody.resize(offset + read);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return true;
}

bool parseOkBody(const std::string& body, json& out, std::string& error) {
    try {
        out = json::parse(body);
    } catch (...) {
        error = "Invalid JSON response.";
        return false;
    }
    if (!out.value("ok", false)) {
        error = out.value("error", std::string("Request failed."));
        return false;
    }
    return true;
}

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 1) return {};
    std::wstring out(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), len);
    return out;
}

} // namespace

namespace cloud_api {

void initFromEnvironment() {
    g_token.clear();
    char buf[4096] = {};
    if (GetEnvironmentVariableA("CRYMORE_ACCESS_TOKEN", buf, sizeof(buf)) > 0)
        g_token = buf;
}

bool hasToken() { return !g_token.empty(); }
const std::string& accessToken() { return g_token; }

bool listConfigs(std::vector<CloudConfigEntry>& out, std::string& error) {
    out.clear();
    if (g_token.empty()) { error = "Not signed in."; return false; }
    std::string body;
    int status = 0;
    if (!httpRequest(L"GET", L"/v1/configs", "", g_token, body, status, error))
        return false;
    json j;
    if (!parseOkBody(body, j, error)) return false;
    for (const auto& row : j.value("configs", json::array())) {
        CloudConfigEntry e{};
        e.id = row.value("id", "");
        e.name = row.value("name", "");
        e.description = row.value("description", "");
        e.isPublic = row.value("is_public", false);
        e.updatedAt = row.value("updated_at", "");
        out.push_back(std::move(e));
    }
    return true;
}

bool downloadConfig(const std::string& id, std::string& jsonOut, std::string& error) {
    if (g_token.empty()) { error = "Not signed in."; return false; }
    std::string body;
    int status = 0;
    const std::wstring path = L"/v1/configs/" + utf8ToWide(id);
    if (!httpRequest(L"GET", path, "", g_token, body, status, error))
        return false;
    json j;
    if (!parseOkBody(body, j, error)) return false;
    jsonOut = j.value("json", "");
    return !jsonOut.empty();
}

bool uploadConfig(const std::string& name, const std::string& description,
                  const std::string& jsonText, bool isPublic,
                  std::string& outId, std::string& error) {
    if (g_token.empty()) { error = "Not signed in."; return false; }
    json req;
    req["name"] = name;
    req["description"] = description;
    req["json"] = jsonText;
    req["is_public"] = isPublic;
    const std::string payload = req.dump();
    std::string body;
    int status = 0;
    if (!httpRequest(L"POST", L"/v1/configs", payload, g_token, body, status, error))
        return false;
    json j;
    if (!parseOkBody(body, j, error)) return false;
    outId = j.value("id", "");
    return !outId.empty();
}

bool deleteConfig(const std::string& id, std::string& error) {
    if (g_token.empty()) { error = "Not signed in."; return false; }
    std::string body;
    int status = 0;
    const std::wstring path = L"/v1/configs/" + utf8ToWide(id);
    if (!httpRequest(L"DELETE", path, "", g_token, body, status, error))
        return false;
    json j;
    return parseOkBody(body, j, error);
}

bool listLineupPacks(const std::string& mapFilter,
                     std::vector<CloudLineupEntry>& out, std::string& error) {
    out.clear();
    std::wstring path = L"/v1/lineups";
    if (!mapFilter.empty())
        path += L"?map=" + utf8ToWide(mapFilter);
    std::string body;
    int status = 0;
    if (!httpRequest(L"GET", path, "", g_token, body, status, error))
        return false;
    json j;
    if (!parseOkBody(body, j, error)) return false;
    for (const auto& row : j.value("packs", json::array())) {
        CloudLineupEntry e{};
        e.id = row.value("id", "");
        e.map = row.value("map", "");
        e.title = row.value("title", "");
        e.description = row.value("description", "");
        e.grenadeType = row.value("grenade_type", "");
        e.spotCount = row.value("spot_count", 0);
        e.downloadCount = row.value("download_count", 0);
        out.push_back(std::move(e));
    }
    return true;
}

bool downloadLineupPack(const std::string& id, std::string& jsonOut, std::string& error) {
    std::string body;
    int status = 0;
    const std::wstring path = L"/v1/lineups/" + utf8ToWide(id);
    if (!httpRequest(L"GET", path, "", g_token, body, status, error))
        return false;
    json j;
    if (!parseOkBody(body, j, error)) return false;
    jsonOut = j.value("json", "");
    return !jsonOut.empty();
}

bool uploadLineupPack(const std::string& title, const std::string& map,
                      const std::string& description, const std::string& grenadeType,
                      const std::string& jsonText, bool isPublic,
                      std::string& outId, std::string& error) {
    if (g_token.empty()) { error = "Not signed in."; return false; }
    json req;
    req["title"] = title;
    req["map"] = map;
    req["description"] = description;
    req["grenade_type"] = grenadeType;
    req["json"] = jsonText;
    req["is_public"] = isPublic;
    const std::string payload = req.dump();
    std::string body;
    int status = 0;
    if (!httpRequest(L"POST", L"/v1/lineups", payload, g_token, body, status, error))
        return false;
    json j;
    if (!parseOkBody(body, j, error)) return false;
    outId = j.value("id", "");
    return !outId.empty();
}

} // namespace cloud_api
