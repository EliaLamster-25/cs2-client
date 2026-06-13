#include "analytics/player_scout.h"

#include "debug/overlay_log.h"
#include "json.hpp"

#include <Windows.h>
#include <ShlObj.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unordered_set>

#pragma comment(lib, "winhttp.lib")

using json = nlohmann::json;

namespace {

std::uint64_t tickMs() {
    return GetTickCount64();
}

std::string readEnvVar(const char* name) {
    if (!name || !*name)
        return {};
    char* buf = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, name) != 0 || !buf)
        return {};
    std::string val(buf);
    free(buf);
    return val;
}

std::string trimCopy(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    std::size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t'))
        ++start;
    return s.substr(start);
}

std::string normalizeApiKey(std::string key) {
    key = trimCopy(std::move(key));
    if (key.size() >= 1 && (key.front() == '"' || key.front() == '\''))
        key.erase(0, 1);
    if (key.size() >= 1 && (key.back() == '"' || key.back() == '\''))
        key.pop_back();
    key = trimCopy(std::move(key));

    if (key.rfind("Bearer ", 0) == 0)
        key.erase(0, 7);
    key = trimCopy(std::move(key));

    if (key.size() >= 3
        && static_cast<unsigned char>(key[0]) == 0xEF
        && static_cast<unsigned char>(key[1]) == 0xBB
        && static_cast<unsigned char>(key[2]) == 0xBF) {
        key.erase(0, 3);
    }

    key.erase(std::remove_if(key.begin(), key.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }), key.end());

    // Keep printable ASCII only (Leetify keys are ASCII tokens).
    key.erase(std::remove_if(key.begin(), key.end(), [](unsigned char c) {
        return c < 0x21 || c > 0x7E;
    }), key.end());

    return key;
}

std::wstring utf8ToWide(const std::string& utf8) {
    if (utf8.empty())
        return L"";
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                                        nullptr, 0);
    if (len <= 0)
        return L"";
    std::wstring wide(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), wide.data(), len);
    return wide;
}

std::wstring apiKeyPath() {
    wchar_t localApp[MAX_PATH]{};
    if (!SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localApp)))
        return L"";
    std::wstring path = localApp;
    path += L"\\crymore\\leetify.key";
    return path;
}

bool httpGet(const std::wstring& host, const std::wstring& path, const std::string& apiKey,
             std::string& bodyOut, DWORD& statusOut) {
    bodyOut.clear();
    statusOut = 0;

    HINTERNET session = WinHttpOpen(L"CrymoreOverlay/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        return false;
    WinHttpSetTimeouts(session, 4000, 4000, 8000, 8000);

    HINTERNET connect = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return false;
    }

    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    std::wstring requestHeaders;
    if (!apiKey.empty()) {
        const std::string hdr = "_leetify_key: " + apiKey + "\r\nAuthorization: Bearer " + apiKey + "\r\n";
        requestHeaders = utf8ToWide(hdr);
    }

    bool ok = WinHttpSendRequest(
        request,
        requestHeaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : requestHeaders.c_str(),
        requestHeaders.empty() ? 0 : static_cast<DWORD>(-1L),
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
           && WinHttpReceiveResponse(request, nullptr);
    if (ok) {
        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                                WINHTTP_NO_HEADER_INDEX))
            statusOut = status;

        DWORD size = 0;
        do {
            if (!WinHttpQueryDataAvailable(request, &size))
                break;
            if (size == 0)
                break;
            const size_t offset = bodyOut.size();
            bodyOut.resize(offset + size);
            DWORD read = 0;
            if (!WinHttpReadData(request, bodyOut.data() + offset, size, &read)) {
                bodyOut.resize(offset);
                ok = false;
                break;
            }
            bodyOut.resize(offset + read);
        } while (size > 0);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return ok;
}

int jsonInt(const json& j, const char* key, int fallback = -1) {
    if (!j.contains(key) || j[key].is_null())
        return fallback;
    if (j[key].is_number_integer())
        return j[key].get<int>();
    if (j[key].is_number_float())
        return static_cast<int>(j[key].get<double>());
    if (j[key].is_string()) {
        try {
            return std::stoi(j[key].get<std::string>());
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

float jsonFloat(const json& j, const char* key, float fallback = -1.f) {
    if (!j.contains(key) || j[key].is_null())
        return fallback;
    if (j[key].is_number())
        return j[key].get<float>();
    if (j[key].is_string()) {
        try {
            return std::stof(j[key].get<std::string>());
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

std::string jsonString(const json& j, const char* key) {
    if (!j.contains(key) || j[key].is_null())
        return {};
    if (j[key].is_string())
        return j[key].get<std::string>();
    return {};
}

std::string jsonErrorMessage(const json& j) {
    for (const char* key : {"message", "detail", "title", "error_description"}) {
        if (j.contains(key) && j[key].is_string())
            return j[key].get<std::string>();
    }
    if (!j.contains("error"))
        return {};
    if (j["error"].is_string())
        return j["error"].get<std::string>();
    if (j["error"].is_object()) {
        for (const char* key : {"message", "detail", "title", "error"}) {
            if (j["error"].contains(key) && j["error"][key].is_string())
                return j["error"][key].get<std::string>();
        }
    }
    return "API error";
}

constexpr std::uint64_t kSteam64Base = 76561197960265728ULL;

std::uint64_t normalizeSteam64(std::uint64_t raw) {
    if (raw >= kSteam64Base)
        return raw;
    if (raw > 0 && raw < 10000000000ULL)
        return kSteam64Base + raw;
    return raw;
}

bool isLikelySteam64(std::uint64_t id) {
    return id >= kSteam64Base;
}

bool isIdentifierErrorMessage(const std::string& msg) {
    return msg.find("identifier") != std::string::npos
        || msg.find("Identifier") != std::string::npos;
}

const json* jsonProfileRoot(const json& j) {
    if (j.contains("profile") && j["profile"].is_object())
        return &j["profile"];
    return &j;
}

const json* jsonField(const json& j, const json& root, const char* key) {
    if (root.contains(key))
        return &root[key];
    if (&root != &j && j.contains(key))
        return &j[key];
    return nullptr;
}

bool profileHasData(const json& j, const json& root) {
    if (jsonField(j, root, "steam64_id") || jsonField(j, root, "steam64Id")
        || jsonField(j, root, "id") || jsonField(j, root, "name")
        || jsonField(j, root, "ranks") || jsonField(j, root, "rating")
        || jsonField(j, root, "stats") || jsonField(j, root, "recent_matches")
        || jsonField(j, root, "recentMatches"))
        return true;
    return false;
}

int jsonRankValue(const json& ranks, const char* key) {
    if (!ranks.contains(key) || ranks[key].is_null())
        return -1;
    if (ranks[key].is_number_integer())
        return ranks[key].get<int>();
    if (ranks[key].is_number_float())
        return static_cast<int>(ranks[key].get<double>());
    if (ranks[key].is_object())
        return jsonInt(ranks[key], "rank", jsonInt(ranks[key], "tier", -1));
    return -1;
}

float jsonStatHeadAccuracy(const json& stats) {
    if (!stats.is_object())
        return -1.f;
    const float direct = jsonFloat(stats, "accuracy_head", -1.f);
    if (direct >= 0.f)
        return direct;
    const float camel = jsonFloat(stats, "accuracyHead", -1.f);
    if (camel >= 0.f)
        return camel;
    return jsonFloat(stats, "headshot_accuracy", -1.f);
}

void scoutLog(const char* msg) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), "[PlayerScout] %s", msg);
    overlayFileLog(buf);
}

} // namespace

PlayerScout& PlayerScout::instance() {
    static PlayerScout inst;
    return inst;
}

PlayerScout::PlayerScout() {
    m_worker = std::thread(&PlayerScout::workerMain, this);
}

PlayerScout::~PlayerScout() {
    {
        std::lock_guard lock(m_mutex);
        m_shutdown = true;
    }
    m_cv.notify_all();
    if (m_worker.joinable())
        m_worker.join();
}

void PlayerScout::ensureApiKeyLoaded() {
    std::lock_guard lock(m_mutex);
    if (m_keyLoaded)
        return;
    m_keyLoaded = true;

    const std::wstring path = apiKeyPath();
    if (!path.empty()) {
        std::ifstream in(path, std::ios::binary);
        if (in) {
            std::string key((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            key = normalizeApiKey(std::move(key));
            if (!key.empty()) {
                m_apiKey = key;
                m_keyStatus = ApiKeyStatus::Valid;
            }
        }
    }

    if (m_apiKey.empty()) {
        const std::string envKey = readEnvVar("CRYMORE_LEETIFY_API_KEY");
        if (!envKey.empty()) {
            const std::string key = normalizeApiKey(envKey);
            if (!key.empty()) {
                m_apiKey = key;
                m_keyStatus = ApiKeyStatus::Valid;
            }
        }
    }

    char msg[128];
    std::snprintf(msg, sizeof(msg), "api key loaded=%d valid=%d len=%zu",
        m_apiKey.empty() ? 0 : 1,
        (m_keyStatus == ApiKeyStatus::Valid) ? 1 : 0,
        m_apiKey.size());
    scoutLog(msg);
}

std::string PlayerScout::storedApiKey() const {
    std::lock_guard lock(m_mutex);
    return m_apiKey;
}

void PlayerScout::setApiKey(const std::string& key) {
    std::lock_guard lock(m_mutex);
    m_apiKey = normalizeApiKey(key);
    m_keyStatus = m_apiKey.empty() ? ApiKeyStatus::NotSet : ApiKeyStatus::Valid;
}

bool PlayerScout::saveApiKey(const std::string& key) {
    const std::string normalized = normalizeApiKey(key);
    const std::wstring path = apiKeyPath();
    if (path.empty())
        return false;

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);

    if (normalized.empty()) {
        std::filesystem::remove(path, ec);
        std::lock_guard lock(m_mutex);
        m_apiKey.clear();
        m_keyStatus = ApiKeyStatus::NotSet;
        return true;
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out << normalized;
    if (!out.good())
        return false;

    std::lock_guard lock(m_mutex);
    m_apiKey = normalized;
    m_keyStatus = ApiKeyStatus::Valid;
    notifyWorker();
    return true;
}

bool PlayerScout::hasApiKey() const {
    std::lock_guard lock(m_mutex);
    return !m_apiKey.empty();
}

bool PlayerScout::hasValidApiKey() const {
    std::lock_guard lock(m_mutex);
    return !m_apiKey.empty() && m_keyStatus == ApiKeyStatus::Valid;
}

bool PlayerScout::needsSetupPrompt() {
    ensureApiKeyLoaded();
    return !hasValidApiKey();
}

PlayerScout::ApiKeyStatus PlayerScout::apiKeyStatus() const {
    std::lock_guard lock(m_mutex);
    return m_keyStatus;
}

bool PlayerScout::validateApiKey() {
    ensureApiKeyLoaded();

    std::string key;
    {
        std::lock_guard lock(m_mutex);
        key = m_apiKey;
    }
    if (key.empty()) {
        std::lock_guard lock(m_mutex);
        m_keyStatus = ApiKeyStatus::NotSet;
        return false;
    }

    std::string body;
    DWORD status = 0;
    const bool ok = httpGet(L"api-public.cs-prod.leetify.com", L"/api-key/validate", key, body, status);

    bool validated = false;
    {
        std::lock_guard lock(m_mutex);
        if (ok && (status == 200 || status == 204)) {
            m_keyStatus = ApiKeyStatus::Valid;
            validated = true;
        } else if (ok && status == 401) {
            m_keyStatus = ApiKeyStatus::Invalid;
        }
    }
    if (validated)
        notifyWorker();
    if (validated)
        return true;
    if (ok && status == 401)
        return false;
    // Network / server errors: keep the saved key usable for profile fetches.
    {
        std::lock_guard lock(m_mutex);
        if (!m_apiKey.empty() && m_keyStatus != ApiKeyStatus::Invalid)
            m_keyStatus = ApiKeyStatus::Valid;
    }
    notifyWorker();
    return false;
}

bool PlayerScout::applyKeyBuffer(char* buf, std::size_t bufSize) {
    if (!buf || bufSize == 0)
        return false;

    const std::string normalized = normalizeApiKey(buf);
    if (normalized.size() >= bufSize)
        return false;

    std::memcpy(buf, normalized.c_str(), normalized.size() + 1);

    if (!saveApiKey(normalized))
        return false;

    return validateApiKey();
}

std::string PlayerScout::activeApiKey() const {
    std::lock_guard lock(m_mutex);
    return m_apiKey;
}

std::uint64_t PlayerScout::fetchIntervalMs() const {
    return 250;
}

std::string PlayerScout::suspicionLabelFor(int score) {
    score = std::clamp(score, 0, 100);
    if (score >= 75) return "High";
    if (score >= 50) return "Elevated";
    if (score >= 25) return "Moderate";
    return "Low";
}

int PlayerScout::computeSuspicion(const Row& row) {
    if (row.state != FetchState::Ready)
        return 0;

    int score = 0;
    const float aim = row.aimRating;
    const int matches = row.totalMatches;
    const float head = row.headAccPct;
    const float wr = row.winrate;

    if (aim >= 0.f) {
        if (aim >= 92.f) score += 22;
        else if (aim >= 88.f) score += 14;
        else if (aim >= 84.f) score += 8;
    }

    if (matches >= 0 && matches < 40 && aim >= 85.f)
        score += 18;
    else if (matches >= 0 && matches < 80 && aim >= 88.f)
        score += 10;

    if (head >= 0.f) {
        if (head >= 58.f) score += 16;
        else if (head >= 50.f) score += 8;
    }

    if (wr >= 0.f && matches >= 0 && matches < 60) {
        if (wr >= 0.72f) score += 12;
        else if (wr >= 0.65f) score += 6;
    }

    if (row.premier >= 15000 && matches >= 0 && matches < 50)
        score += 12;

    return std::clamp(score, 0, 100);
}

void PlayerScout::syncRoster(const EntityManager::Snapshot& snap) {
    std::lock_guard lock(m_mutex);
    const std::uint64_t now = tickMs();
    std::unordered_set<std::uint64_t> present;
    present.reserve(snap.players.size());
    int added = 0;
    for (const auto& p : snap.players) {
        if (!p.isValid || p.isBot)
            continue;

        const std::uint64_t sid = normalizeSteam64(p.steamId);
        if (sid == 0 || !isLikelySteam64(sid))
            continue;
        present.insert(sid);

        auto it = m_cache.find(sid);
        if (it == m_cache.end()) {
            CacheEntry entry;
            entry.row.steamId = sid;
            entry.row.name = p.name;
            entry.row.teamNum = p.teamNum;
            entry.row.isBot = false;
            entry.row.isLocal = p.isLocalPlayer;
            entry.row.state = FetchState::Queued;
            entry.row.status = "Queued";
            entry.lastSeenMs = now;
            m_cache.emplace(sid, std::move(entry));
            m_queue.push_back(sid);
            ++added;
        } else {
            it->second.row.name = p.name;
            it->second.row.teamNum = p.teamNum;
            it->second.row.isLocal = p.isLocalPlayer;
            it->second.lastSeenMs = now;
            if (it->second.row.state == FetchState::Queued) {
                if (it->second.row.status.find("rate limit") == std::string::npos
                    && std::find(m_queue.begin(), m_queue.end(), sid) == m_queue.end()) {
                    m_queue.push_back(sid);
                }
            } else if (it->second.row.state == FetchState::Loading) {
                // Fetch in progress — do not re-queue.
            } else if (it->second.row.state == FetchState::Error) {
                const std::uint64_t now = tickMs();
                if (now - it->second.fetchedAtMs >= 600000) {
                    it->second.row.state = FetchState::Queued;
                    it->second.row.status = "Queued";
                    if (std::find(m_queue.begin(), m_queue.end(), sid) == m_queue.end())
                        m_queue.push_back(sid);
                }
            }
        }
    }

    if (!present.empty()) {
        std::erase_if(m_cache, [&](const auto& item) {
            return present.find(item.first) == present.end();
        });
        m_queue.erase(std::remove_if(m_queue.begin(), m_queue.end(), [&](std::uint64_t sid) {
            return present.find(sid) == present.end() || m_cache.find(sid) == m_cache.end();
        }), m_queue.end());
    } else if (!m_cache.empty()) {
        m_cache.clear();
        m_queue.clear();
        m_rateLimitUntilMs = 0;
    }

    static std::uint64_t s_lastRosterLogMs = 0;
    if (added > 0 || (now - s_lastRosterLogMs >= 5000 && !m_cache.empty())) {
        s_lastRosterLogMs = now;
        char msg[160];
        std::snprintf(msg, sizeof(msg),
            "roster cache=%zu queue=%zu added=%d keyValid=%d rateWaitMs=%llu",
            static_cast<unsigned long long>(m_cache.size()),
            static_cast<unsigned long long>(m_queue.size()),
            added,
            (m_keyStatus == ApiKeyStatus::Valid && !m_apiKey.empty()) ? 1 : 0,
            static_cast<unsigned long long>(
                m_rateLimitUntilMs > now ? m_rateLimitUntilMs - now : 0));
        scoutLog(msg);
    }
}

bool PlayerScout::fetchLeetifyProfile(std::uint64_t steamId, Row& out) {
    ensureApiKeyLoaded();
    const std::string apiKey = activeApiKey();
    if (apiKey.empty()) {
        out.state = FetchState::Error;
        out.status = "No API key configured";
        return true;
    }

    const std::uint64_t resolvedId = normalizeSteam64(steamId);
    out.steamId = resolvedId;
    if (!isLikelySteam64(resolvedId)) {
        out.state = FetchState::Error;
        out.status = "Invalid Steam ID";
        return true;
    }

    auto requestProfile = [&](const wchar_t* paramName, std::string& body, DWORD& status) -> bool {
        const std::wstring path = std::wstring(L"/v3/profile?") + paramName + L"="
                                + std::to_wstring(resolvedId);
        return httpGet(L"api-public.cs-prod.leetify.com", path, apiKey, body, status);
    };

    static const wchar_t* kQueryParams[] = { L"steam64_id" };
    std::string body;
    DWORD status = 0;
    bool gotSuccess = false;

    for (const wchar_t* paramName : kQueryParams) {
        body.clear();
        status = 0;
        if (!requestProfile(paramName, body, status)) {
            out.state = FetchState::Error;
            out.status = "Network error reaching Leetify";
            char msg[128];
            std::snprintf(msg, sizeof(msg), "fetch network fail steam=%llu param=%ls",
                static_cast<unsigned long long>(resolvedId), paramName);
            scoutLog(msg);
            return true;
        }
        char msg[160];
        std::snprintf(msg, sizeof(msg), "fetch http steam=%llu param=%ls status=%lu body=%zu",
            static_cast<unsigned long long>(resolvedId), paramName,
            static_cast<unsigned long>(status),
            static_cast<unsigned long long>(body.size()));
        scoutLog(msg);
        if (status >= 200 && status < 300) {
            gotSuccess = true;
            break;
        }
        if (status == 404)
            continue;
        if (status == 400 || status == 422) {
            try {
                const json errJson = json::parse(body);
                if (isIdentifierErrorMessage(jsonErrorMessage(errJson)))
                    continue;
            } catch (...) {
            }
        }
        break;
    }

    if (!gotSuccess) {
        if (status == 401) {
            std::lock_guard lock(m_mutex);
            m_keyStatus = ApiKeyStatus::Invalid;
            out.state = FetchState::Error;
            out.status = "Invalid Leetify API key";
            return true;
        }
        if (status == 429) {
            out.state = FetchState::Queued;
            out.status = "Rate limited";
            return true;
        }
        if (status == 404) {
            out.state = FetchState::NotOnLeetify;
            out.status = "Not on Leetify";
            return true;
        }
        if (!body.empty()) {
            try {
                const json errJson = json::parse(body);
                const std::string err = jsonErrorMessage(errJson);
                if (!err.empty()) {
                    out.state = FetchState::Error;
                    out.status = err;
                    return true;
                }
            } catch (...) {
            }
        }
        if (status >= 400 && status < 600) {
            out.state = FetchState::Error;
            char msg[128];
            std::snprintf(msg, sizeof(msg), "Leetify HTTP %lu", static_cast<unsigned long>(status));
            out.status = msg;
            return true;
        }
        out.state = FetchState::Error;
        out.status = "Network error reaching Leetify";
        return true;
    }

    if (body.empty()) {
        out.state = FetchState::NotOnLeetify;
        out.status = "Not on Leetify";
        return true;
    }

    json j;
    try {
        j = json::parse(body);
    } catch (...) {
        out.state = FetchState::Error;
        out.status = "Invalid response from Leetify";
        return true;
    }

    const std::string err = jsonErrorMessage(j);
    if (!err.empty() && j.contains("error")) {
        if (err.find("registered") != std::string::npos
            || err.find("not found") != std::string::npos
            || err.find("private") != std::string::npos
            || err.find("Not Found") != std::string::npos) {
            out.state = FetchState::NotOnLeetify;
            out.status = "Not on Leetify";
            return true;
        }
        out.state = FetchState::Error;
        out.status = err;
        return true;
    }

    const json& root = *jsonProfileRoot(j);
    if (!profileHasData(j, root)) {
        if (!err.empty()) {
            out.state = FetchState::Error;
            out.status = err;
            return true;
        }
        out.state = FetchState::NotOnLeetify;
        out.status = "No Leetify profile";
        return true;
    }

    if (const json* nameField = jsonField(j, root, "name");
        nameField && nameField->is_string())
        out.name = nameField->get<std::string>();

    out.privacyMode = jsonString(root, "privacy_mode");
    if (out.privacyMode.empty())
        out.privacyMode = jsonString(j, "privacy_mode");
    out.firstMatchDate = jsonString(root, "first_match_date");
    if (out.firstMatchDate.empty())
        out.firstMatchDate = jsonString(j, "first_match_date");

    out.winrate = jsonFloat(root, "winrate", jsonFloat(j, "winrate", -1.f));
    out.totalMatches = jsonInt(root, "total_matches",
        jsonInt(root, "totalMatches",
            jsonInt(j, "total_matches", jsonInt(j, "totalMatches", -1))));

    const json* ranksField = jsonField(j, root, "ranks");
    if (ranksField && ranksField->is_object()) {
        out.leetifyRating = jsonFloat(*ranksField, "leetify", -999.f);
        out.premier = jsonRankValue(*ranksField, "premier");
        out.faceitLevel = jsonRankValue(*ranksField, "faceit");
        out.faceitElo = jsonRankValue(*ranksField, "faceit_elo");
        if (out.faceitElo < 0)
            out.faceitElo = jsonRankValue(*ranksField, "faceitElo");
        out.wingman = jsonRankValue(*ranksField, "wingman");
        out.renown = jsonRankValue(*ranksField, "renown");
    }

    const json* ratingField = jsonField(j, root, "rating");
    if (ratingField && ratingField->is_object()) {
        out.aimRating = jsonFloat(*ratingField, "aim", -1.f);
        out.positioningRating = jsonFloat(*ratingField, "positioning", -1.f);
        out.utilityRating = jsonFloat(*ratingField, "utility", -1.f);
        out.clutchRating = jsonFloat(*ratingField, "clutch", -999.f);
        out.openingRating = jsonFloat(*ratingField, "opening", -999.f);
        out.ctLeetifyRating = jsonFloat(*ratingField, "ct_leetify", -999.f);
        out.tLeetifyRating = jsonFloat(*ratingField, "t_leetify", -999.f);
    }

    const json* statsField = jsonField(j, root, "stats");
    if (statsField && statsField->is_object()) {
        out.headAccPct = jsonStatHeadAccuracy(*statsField);
        if (out.headAccPct >= 0.f && out.headAccPct <= 1.f)
            out.headAccPct *= 100.f;
        out.accuracyEnemySpotted = jsonFloat(*statsField, "accuracy_enemy_spotted", -1.f);
        if (out.accuracyEnemySpotted >= 0.f && out.accuracyEnemySpotted <= 1.f)
            out.accuracyEnemySpotted *= 100.f;
        out.sprayAccuracy = jsonFloat(*statsField, "spray_accuracy", -1.f);
        if (out.sprayAccuracy >= 0.f && out.sprayAccuracy <= 1.f)
            out.sprayAccuracy *= 100.f;
        out.preaim = jsonFloat(*statsField, "preaim", -1.f);
        out.reactionTimeMs = jsonFloat(*statsField, "reaction_time_ms", -1.f);
        out.counterStrafeGoodPct = jsonFloat(*statsField, "counter_strafing_good_shots_ratio", -1.f);
        if (out.counterStrafeGoodPct >= 0.f && out.counterStrafeGoodPct <= 1.f)
            out.counterStrafeGoodPct *= 100.f;
        out.tradeKillSuccessPct = jsonFloat(*statsField, "trade_kills_success_percentage", -1.f);
        if (out.tradeKillSuccessPct >= 0.f && out.tradeKillSuccessPct <= 1.f)
            out.tradeKillSuccessPct *= 100.f;
        out.tradedDeathSuccessPct = jsonFloat(*statsField, "traded_deaths_success_percentage", -1.f);
        if (out.tradedDeathSuccessPct >= 0.f && out.tradedDeathSuccessPct <= 1.f)
            out.tradedDeathSuccessPct *= 100.f;
        out.flashFoePerFlash = jsonFloat(*statsField, "flashbang_hit_foe_per_flashbang", -1.f);
        out.flashDuration = jsonFloat(*statsField, "flashbang_hit_foe_avg_duration", -1.f);
        out.heDamageAvg = jsonFloat(*statsField, "he_foes_damage_avg", -1.f);
        out.utilityOnDeath = jsonFloat(*statsField, "utility_on_death_avg", -1.f);
    }

    if (const json* bansField = jsonField(j, root, "bans");
        bansField && bansField->is_array()) {
        out.banCount = static_cast<int>(bansField->size());
        if (!bansField->empty() && (*bansField)[0].is_object()) {
            const std::string platform = jsonString((*bansField)[0], "platform");
            const std::string nick = jsonString((*bansField)[0], "platform_nickname");
            out.banSummary = platform.empty() ? "ban recorded" : platform;
            if (!nick.empty())
                out.banSummary += " (" + nick + ")";
        }
    }

    if (const json* recentField = jsonField(j, root, "recent_matches");
        recentField && recentField->is_array()) {
        out.recentMatches = static_cast<int>(recentField->size());
        for (const json& match : *recentField) {
            if (!match.is_object())
                continue;
            const std::string outcome = jsonString(match, "outcome");
            if (outcome == "win")
                ++out.recentWins;
        }
        if (!recentField->empty() && (*recentField)[0].is_object()) {
            out.lastMap = jsonString((*recentField)[0], "map_name");
            out.lastOutcome = jsonString((*recentField)[0], "outcome");
        }
    }

    out.state = FetchState::Ready;
    out.status = "Leetify";
    out.suspicionScore = computeSuspicion(out);
    out.suspicionLabel = suspicionLabelFor(out.suspicionScore);
    return true;
}

void PlayerScout::applyFetchResult(std::uint64_t steamId, Row fetched) {
    const std::uint64_t now = tickMs();
    auto it = m_cache.find(steamId);
    if (it == m_cache.end())
        return;

    if (fetched.state == FetchState::Queued
        && fetched.status.find("Rate") != std::string::npos) {
        m_rateLimitUntilMs = now + 12000;
        it->second.row.state = FetchState::Queued;
        it->second.row.status = "Waiting (rate limit)";
        if (std::find(m_queue.begin(), m_queue.end(), steamId) == m_queue.end())
            m_queue.push_back(steamId);
        char msg[160];
        std::snprintf(msg, sizeof(msg), "rate limited steam=%llu queue=%zu",
            static_cast<unsigned long long>(steamId),
            static_cast<unsigned long long>(m_queue.size()));
        scoutLog(msg);
        return;
    }

    const FetchState prevState = it->second.row.state;

    fetched.teamNum = it->second.row.teamNum;
    fetched.isLocal = it->second.row.isLocal;
    fetched.isBot = it->second.row.isBot;
    if (fetched.name.empty() && !it->second.row.name.empty())
        fetched.name = it->second.row.name;

    it->second.row = fetched;
    it->second.fetchedAtMs = now;

    char msg[256];
    const char* stateName = "unknown";
    switch (fetched.state) {
    case FetchState::Queued: stateName = "queued"; break;
    case FetchState::Loading: stateName = "loading"; break;
    case FetchState::Ready: stateName = "ready"; break;
    case FetchState::NotOnLeetify: stateName = "not_on_leetify"; break;
    case FetchState::Error: stateName = "error"; break;
    }
    std::snprintf(msg, sizeof(msg), "fetch done steam=%llu %s->%s status=%s inFlight=%d queue=%zu",
        static_cast<unsigned long long>(steamId),
        prevState == FetchState::Loading ? "loading" : "other",
        stateName,
        fetched.status.empty() ? "-" : fetched.status.c_str(),
        m_inFlight.load(),
        static_cast<unsigned long long>(m_queue.size()));
    scoutLog(msg);
}

void PlayerScout::launchFetch(std::uint64_t steamId) {
    ++m_inFlight;
    std::thread([this, steamId]() {
        Row fetched;
        fetched.steamId = steamId;
        fetchLeetifyProfile(steamId, fetched);

        {
            std::lock_guard lock(m_mutex);
            applyFetchResult(steamId, std::move(fetched));
        }
        m_inFlight.fetch_sub(1);
        m_cv.notify_all();
    }).detach();
}

void PlayerScout::workerMain() {
    for (;;) {
        std::vector<std::uint64_t> batch;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [&] {
                if (m_shutdown.load())
                    return true;
                if (m_apiKey.empty() || m_keyStatus == ApiKeyStatus::Invalid)
                    return false;
                if (m_inFlight.load() >= kMaxInFlight)
                    return false;
                if (m_queue.empty())
                    return false;
                const std::uint64_t now = tickMs();
                if (now < m_rateLimitUntilMs)
                    return false;
                if (now - m_lastDispatchMs < kMinDispatchIntervalMs)
                    return false;
                return true;
            });
            if (m_shutdown.load())
                break;

            const std::uint64_t now = tickMs();
            while (!m_queue.empty() && now >= m_rateLimitUntilMs
                   && static_cast<int>(batch.size()) + m_inFlight.load() < kMaxInFlight) {
                const std::uint64_t sid = m_queue.front();
                m_queue.erase(m_queue.begin());
                auto it = m_cache.find(sid);
                if (it == m_cache.end())
                    continue;

                it->second.row.state = FetchState::Loading;
                it->second.row.status = "Loading…";
                batch.push_back(sid);
            }

            if (!batch.empty()) {
                m_lastDispatchMs = now;
                char msg[128];
                std::snprintf(msg, sizeof(msg), "dispatch batch=%zu inFlight=%d queue=%zu keyValid=%d",
                    static_cast<unsigned long long>(batch.size()),
                    m_inFlight.load(),
                    static_cast<unsigned long long>(m_queue.size()),
                    (m_keyStatus == ApiKeyStatus::Valid) ? 1 : 0);
                scoutLog(msg);
            }
        }

        for (const std::uint64_t sid : batch)
            launchFetch(sid);

        if (batch.empty()) {
            std::unique_lock lock(m_mutex);
            m_cv.wait_for(lock, std::chrono::milliseconds(200));
        }
    }
}

void PlayerScout::notifyWorker() {
    m_cv.notify_one();
}

void PlayerScout::tick(const EntityManager& em) {
    ensureApiKeyLoaded();
    const auto snap = em.publishedFrame();
    if (!snap)
        return;
    syncRoster(*snap);
    notifyWorker();
}

std::vector<PlayerScout::Row> PlayerScout::rows() const {
    std::lock_guard lock(m_mutex);
    std::vector<Row> out;
    out.reserve(m_cache.size());
    for (const auto& [_, entry] : m_cache)
        out.push_back(entry.row);

    std::sort(out.begin(), out.end(), [](const Row& a, const Row& b) {
        if (a.isLocal != b.isLocal)
            return a.isLocal > b.isLocal;
        if (a.teamNum != b.teamNum)
            return a.teamNum < b.teamNum;
        return a.name < b.name;
    });
    return out;
}
