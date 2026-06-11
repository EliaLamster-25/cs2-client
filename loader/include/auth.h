#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct AuthSession {
    bool valid = false;
    std::string accessToken;
    std::string refreshToken;
    std::string username;
    std::string expiresAt;
    std::string plan;
    std::string hwid;
    std::string avatarUrl;
    int subscriptionDaysRemaining = 0;
    std::int64_t subscriptionExpiresUnix = 0;
    std::string payloadKey;
};

struct AuthResponse {
    bool ok = false;
    std::string error;
    AuthSession session;
};

/// Placeholder auth — future HTTPS API at api.crymore.pw (see auth.cpp).
AuthResponse authLogin(const std::string& username, const std::string& password);
AuthResponse authValidateSession(const AuthSession& session);
AuthResponse authRefreshSession(const AuthSession& session);
std::string authGetHwid();
bool authDevBypassEnabled();

bool authHttpPostJsonAuthPath(const char* subPathUtf8, const std::string& body,
    const std::string& bearer, std::string& outBody, std::string& outError);

using AuthProgressFn = void(*)(const char* step, std::uint64_t bytes, std::uint64_t total, void* user);
void authSetProgressCallback(AuthProgressFn fn, void* user);
void authClearProgressCallback();
void authNotifyProgress(const char* step, std::uint64_t bytes, std::uint64_t total);

bool authHttpGetBinaryPath(const char* subPathUtf8, const std::wstring& extraHeaders,
    std::vector<std::uint8_t>& outBody, std::string& outError);
/** Download binary from a full URL (https://…) or API path (/v1/…). Follows redirects. */
bool authHttpDownloadUrl(const std::string& urlOrPath, std::vector<std::uint8_t>& outBody,
    std::string& outError, const char* progressStep = "download");
