#include "web/web_radar_publisher.h"

#include "config.h"
#include "analytics/match_intel.h"
#include "game/entity_manager.h"
#include "json.hpp"
#include "memory/process.h"
#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>

#pragma comment(lib, "winhttp.lib")

using json = nlohmann::json;

namespace {
// TODO: replace with your Railway URL after deploy (e.g. https://your-app.up.railway.app)
constexpr const char* kRelayUrl = "https://ag-cs2-web-radar-production.up.railway.app/api/radar/update";
constexpr const char* kViewerBaseUrl = "https://ag-cs2-web-radar-production.up.railway.app/radar.html";

bool hasPlaceholderHost() {
    return std::strstr(kRelayUrl, "YOUR-VERCEL-PROJECT") != nullptr
        || std::strstr(kViewerBaseUrl, "YOUR-VERCEL-PROJECT") != nullptr
        || std::strstr(kRelayUrl, "YOUR-RAILWAY-URL") != nullptr
        || std::strstr(kViewerBaseUrl, "YOUR-RAILWAY-URL") != nullptr;
}

struct ParsedUrl {
    bool https = false;
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 80;
};

std::wstring widen(const std::string& s) {
    if (s.empty())
        return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0)
        return {};
    std::wstring out(static_cast<std::size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), len);
    return out;
}

bool parseUrl(const std::string& url, ParsedUrl& out) {
    const std::size_t schemePos = url.find("://");
    if (schemePos == std::string::npos)
        return false;

    const std::string scheme = url.substr(0, schemePos);
    out.https = (scheme == "https");
    out.port = out.https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;

    std::size_t hostStart = schemePos + 3;
    std::size_t pathStart = url.find('/', hostStart);
    std::string hostPort = (pathStart == std::string::npos) ? url.substr(hostStart) : url.substr(hostStart, pathStart - hostStart);
    std::string path = (pathStart == std::string::npos) ? "/" : url.substr(pathStart);

    std::size_t colonPos = hostPort.find(':');
    std::string host = hostPort;
    if (colonPos != std::string::npos) {
        host = hostPort.substr(0, colonPos);
        const std::string portStr = hostPort.substr(colonPos + 1);
        const int parsedPort = std::atoi(portStr.c_str());
        if (parsedPort > 0 && parsedPort <= 65535)
            out.port = static_cast<INTERNET_PORT>(parsedPort);
    }

    out.host = widen(host);
    out.path = widen(path);
    return !out.host.empty() && !out.path.empty();
}

std::uint64_t nowMs() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER ui{};
    ui.LowPart = ft.dwLowDateTime;
    ui.HighPart = ft.dwHighDateTime;
    constexpr std::uint64_t kWindowsToUnixEpoch100ns = 116444736000000000ULL;
    return (ui.QuadPart - kWindowsToUnixEpoch100ns) / 10000ULL;
}

const char* grenadeTypeId(GrenadeType t) {
    switch (t) {
        case GrenadeType::HE: return "hegrenade";
        case GrenadeType::Smoke: return "smokegrenade";
        case GrenadeType::Flash: return "flashbang";
        case GrenadeType::Molotov: return "molotov";
        case GrenadeType::Decoy: return "decoy";
        default: return "hegrenade";
    }
}

} // namespace

WebRadarPublisher::~WebRadarPublisher() {
    stopWorker();
}

std::string WebRadarPublisher::ensureSessionId() {
    if (!m_sessionId.empty())
        return m_sessionId;

    std::random_device rd;
    std::mt19937_64 rng(rd());
    static constexpr char kHex[] = "0123456789abcdef";

    m_sessionId.resize(24);
    for (char& ch : m_sessionId)
        ch = kHex[rng() & 0x0F];

    return m_sessionId;
}

void WebRadarPublisher::update(const Process& proc, const EntityManager& em) {
    if (!g_cfg.webRadarEnabled)
        return;

    static bool warnedPlaceholder = false;
    if (hasPlaceholderHost()) {
        if (!warnedPlaceholder) {
            std::printf("[WebRadar] Placeholder URL detected. Set kRelayUrl and kViewerBaseUrl in src/web/web_radar_publisher.cpp\n");
            warnedPlaceholder = true;
        }
        return;
    }

    const int configuredPublishMs = (std::max)(1, g_cfg.webRadarPublishMs);
    const int effectivePublishMs = (std::min)(configuredPublishMs, 33);
    const std::uint64_t publishInterval = static_cast<std::uint64_t>(effectivePublishMs);
    const std::uint64_t t = nowMs();
    if (m_lastPublishMs != 0 && (t - m_lastPublishMs) < publishInterval)
        return;

    auto players = em.players();
    const PlayerData* local = nullptr;
    const PlayerData* firstAlive = nullptr;
    for (const auto& p : players) {
        if (!firstAlive && p.isValid && p.isAlive)
            firstAlive = &p;
        if (p.isValid && p.isAlive && p.isLocalPlayer) {
            local = &p;
            break;
        }
    }
    if (!local)
        local = firstAlive;
    if (!local)
        return;

    ensureWorkerStarted();

    const std::string sessionId = ensureSessionId();
    const std::string shareUrl = std::string(kViewerBaseUrl) + "?session=" + sessionId;
    g_cfg.webRadarSessionId = sessionId;
    g_cfg.webRadarShareUrl = shareUrl;

    json payload;
    payload["sessionId"] = sessionId;
    payload["timestampMs"] = t;
    payload["publishMs"] = effectivePublishMs;
    payload["map"] = em.currentMapName();
    payload["radarRange"] = g_cfg.radarRange;

    payload["local"] = {
        { "x", local->origin.x },
        { "y", local->origin.y },
        { "yaw", local->eyeYaw },
        { "team", local->teamNum },
        { "name", local->name.empty() ? "Local" : local->name },
        { "health", local->health },
        { "armor", local->armor },
        { "weapon", local->weaponName.empty() ? "unknown" : local->weaponName }
    };

    json remote = json::array();
    int playerFallbackIdx = 1;
    for (const auto& p : players) {
        if (!p.isValid || !p.isAlive || p.isLocalPlayer)
            continue;

        std::string pname = p.name;
        if (pname.empty()) {
            pname = std::string("Player ") + std::to_string(playerFallbackIdx);
        }
        ++playerFallbackIdx;

        remote.push_back({
            { "x", p.origin.x },
            { "y", p.origin.y },
            { "yaw", p.eyeYaw },
            { "team", p.teamNum },
            { "health", p.health },
            { "armor", p.armor },
            { "dormant", p.isDormant },
            { "name", pname },
            { "weapon", p.weaponName.empty() ? "unknown" : p.weaponName }
        });
    }
    payload["players"] = std::move(remote);

    json nadeArr = json::array();
    const auto grenades = em.grenades();
    for (const auto& g : grenades) {
        if (!g.isValid)
            continue;
        json gJson = {
            { "x", g.origin.x },
            { "y", g.origin.y },
            { "type", grenadeTypeId(g.type) },
            { "deployed", g.isDeployed }
        };

        if (!g.isDeployed && g.predCount > 1) {
            json pts = json::array();
            constexpr int kMaxGrenadePredPoints = 32;
            const int sampleStep = (g.predCount > 160) ? 4 : (g.predCount > 80) ? 3 : 2;
            for (int i = 0; i < g.predCount && static_cast<int>(pts.size()) < kMaxGrenadePredPoints; i += sampleStep) {
                pts.push_back({
                    { "x", g.predPoints[i].x },
                    { "y", g.predPoints[i].y }
                });
            }
            gJson["pred"] = std::move(pts);
        }

        if (g.hasStableLandPos) {
            gJson["land"] = {
                { "x", g.stableLandPos.x },
                { "y", g.stableLandPos.y }
            };
        }

        nadeArr.push_back(std::move(gJson));
    }
    payload["grenades"] = std::move(nadeArr);

    const auto preThrow = em.preThrow();
    if (preThrow.isActive && preThrow.predCount > 1) {
        json pts = json::array();
        constexpr int kMaxPreThrowPoints = 48;
        const int sampleStep = (preThrow.predCount > 200) ? 4 : (preThrow.predCount > 100) ? 3 : 2;
        for (int i = 0; i < preThrow.predCount && static_cast<int>(pts.size()) < kMaxPreThrowPoints; i += sampleStep) {
            pts.push_back({
                { "x", preThrow.predPoints[i].x },
                { "y", preThrow.predPoints[i].y }
            });
        }

        payload["preThrow"] = {
            { "active", true },
            { "type", grenadeTypeId(preThrow.type) },
            { "points", std::move(pts) }
        };
    } else {
        payload["preThrow"] = {
            { "active", false }
        };
    }

    const auto bomb = em.bomb();
    payload["bomb"] = {
        { "isPlanted", bomb.isPlanted },
        { "isTicking", bomb.isTicking },
        { "x", bomb.origin.x },
        { "y", bomb.origin.y },
        { "site", bomb.site }
    };

    const auto intel = MatchIntel::instance().view();
    json cues = json::array();
    for (const auto& c : intel.cues) {
        cues.push_back({
            { "text", c.text },
            { "severity", c.severity }
        });
    }

    json threats = json::array();
    for (const auto& tcard : intel.threats) {
        threats.push_back({
            { "name", tcard.name },
            { "score", tcard.score },
            { "kills", tcard.kills },
            { "entry", tcard.entrySuccess },
            { "clutch", tcard.clutchRate }
        });
    }

    json replay = json::array();
    for (const auto& ev : intel.replayEvents) {
        replay.push_back({
            { "atMs", ev.atMs },
            { "type", ev.type },
            { "text", ev.text },
            { "x", ev.pos.x },
            { "y", ev.pos.y }
        });
    }

    auto heatToJson = [](const std::vector<MatchIntel::HeatPoint>& points) {
        json arr = json::array();
        for (const auto& p : points) {
            arr.push_back({
                { "cellX", p.cellX },
                { "cellY", p.cellY },
                { "count", p.count }
            });
        }
        return arr;
    };

    payload["intel"] = {
        { "currentRound", intel.currentRound },
        { "roundLive", intel.roundLive },
        { "replayIndex", intel.replayEventIndex },
        { "replayMax", intel.replayEventMax },
        { "lineupCount", intel.lineupCount },
        { "cues", std::move(cues) },
        { "threats", std::move(threats) },
        { "replay", std::move(replay) },
        { "deathHeat", heatToJson(intel.deathHeat) },
        { "failedEntryHeat", heatToJson(intel.failedEntryHeat) },
        { "utilityWasteHeat", heatToJson(intel.utilityWasteHeat) }
    };

    enqueueSnapshot(payload.dump());
    m_lastPublishMs = t;
}

void WebRadarPublisher::ensureWorkerStarted() {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    if (m_workersStarted)
        return;

    constexpr int kWorkerCount = 4;
    m_stopWorker = false;
    m_workers.clear();
    m_workers.reserve(kWorkerCount);
    for (int i = 0; i < kWorkerCount; ++i)
        m_workers.emplace_back(&WebRadarPublisher::workerLoop, this);
    m_workersStarted = true;
}

void WebRadarPublisher::stopWorker() {
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (!m_workersStarted)
            return;
        m_stopWorker = true;
    }
    m_queueCv.notify_all();
    for (auto& worker : m_workers) {
        if (worker.joinable())
            worker.join();
    }

    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_workersStarted = false;
    m_workers.clear();
    m_payloadQueue.clear();
}

void WebRadarPublisher::enqueueSnapshot(std::string payloadJson) {
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        // Keep queue short to avoid stale-frame backlog under network jitter.
        constexpr std::size_t kMaxQueueSize = 8;
        if (m_payloadQueue.size() >= kMaxQueueSize)
            m_payloadQueue.pop_front();
        m_payloadQueue.push_back(std::move(payloadJson));
    }
    m_queueCv.notify_one();
}

void WebRadarPublisher::workerLoop() {
    while (true) {
        std::string payload;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_queueCv.wait(lock, [&]() { return m_stopWorker || !m_payloadQueue.empty(); });

            if (m_stopWorker && m_payloadQueue.empty())
                return;

            payload = std::move(m_payloadQueue.front());
            m_payloadQueue.pop_front();
        }

        if (payload.empty())
            continue;

        postSnapshot(payload);
    }
}

bool WebRadarPublisher::postSnapshot(const std::string& payloadJson) {
    static ParsedUrl parsed{};
    static bool parsedInit = parseUrl(kRelayUrl, parsed);
    thread_local HINTERNET hSession = nullptr;
    thread_local HINTERNET hConnect = nullptr;

    auto closeConnection = [&]() {
        if (hConnect) {
            WinHttpCloseHandle(hConnect);
            hConnect = nullptr;
        }
        if (hSession) {
            WinHttpCloseHandle(hSession);
            hSession = nullptr;
        }
    };

    if (!parsedInit)
        return false;

    auto ensureConnection = [&]() -> bool {
        if (!hSession) {
            hSession = WinHttpOpen(L"Mozilla/5.0",
                                   WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                   WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS,
                                   0);
            if (!hSession)
                return false;

            WinHttpSetTimeouts(hSession, 1000, 1000, 1500, 1500);
        }

        if (!hConnect) {
            hConnect = WinHttpConnect(hSession, parsed.host.c_str(), parsed.port, 0);
            if (!hConnect)
                return false;
        }

        return true;
    };

    if (!ensureConnection()) {
        closeConnection();
        if (!ensureConnection())
            return false;
    }

    const DWORD flags = parsed.https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect,
                                            L"POST",
                                            parsed.path.c_str(),
                                            nullptr,
                                            WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            flags);
    if (!hRequest) {
        closeConnection();
        return false;
    }

    static const wchar_t* kHeaders = L"Content-Type: application/json\r\nConnection: keep-alive\r\n";

    BOOL ok = WinHttpSendRequest(hRequest,
                                 kHeaders,
                                 static_cast<DWORD>(-1L),
                                 (LPVOID)payloadJson.data(),
                                 static_cast<DWORD>(payloadJson.size()),
                                 static_cast<DWORD>(payloadJson.size()),
                                 0);

    if (ok)
        ok = WinHttpReceiveResponse(hRequest, nullptr);

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (ok) {
        WinHttpQueryHeaders(hRequest,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX,
                            &status,
                            &statusSize,
                            WINHTTP_NO_HEADER_INDEX);
    }

    WinHttpCloseHandle(hRequest);

    const bool success = ok && status >= 200 && status < 300;
    if (!success)
        closeConnection();

    return success;
}
