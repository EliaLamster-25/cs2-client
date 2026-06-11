#pragma once

#include "game/entity_manager.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

/// Fetches third-party profile stats (Leetify public API) for players in the
/// current match and computes a heuristic suspicion estimate from stat outliers.
class PlayerScout {
public:
    enum class FetchState : std::uint8_t {
        Queued,
        Loading,
        Ready,
        NotOnLeetify,
        Error,
    };

    enum class ApiKeyStatus : std::uint8_t {
        NotSet,
        Valid,
        Invalid,
    };

    struct Row {
        std::uint64_t steamId = 0;
        std::string name;
        int teamNum = 0;
        bool isBot = false;
        bool isLocal = false;

        FetchState state = FetchState::Queued;
        std::string status;

        int premier = -1;
        int faceitLevel = -1;
        int faceitElo = -1;
        float leetifyRating = -999.f;
        int wingman = -1;
        int renown = -1;
        float aimRating = -1.f;
        float positioningRating = -1.f;
        float utilityRating = -1.f;
        float clutchRating = -999.f;
        float openingRating = -999.f;
        float ctLeetifyRating = -999.f;
        float tLeetifyRating = -999.f;
        float winrate = -1.f;
        int totalMatches = -1;
        float headAccPct = -1.f;
        float accuracyEnemySpotted = -1.f;
        float sprayAccuracy = -1.f;
        float preaim = -1.f;
        float reactionTimeMs = -1.f;
        float counterStrafeGoodPct = -1.f;
        float tradeKillSuccessPct = -1.f;
        float tradedDeathSuccessPct = -1.f;
        float flashFoePerFlash = -1.f;
        float flashDuration = -1.f;
        float heDamageAvg = -1.f;
        float utilityOnDeath = -1.f;
        std::string privacyMode;
        std::string firstMatchDate;
        int banCount = 0;
        std::string banSummary;
        int recentMatches = 0;
        int recentWins = 0;
        std::string lastMap;
        std::string lastOutcome;

        int suspicionScore = 0;   ///< 0–100 heuristic, not proof of cheating
        std::string suspicionLabel;
    };

    static PlayerScout& instance();

    void tick(const EntityManager& em);
    std::vector<Row> rows() const;

    void ensureApiKeyLoaded();
    std::string storedApiKey() const;
    void setApiKey(const std::string& key);
    bool saveApiKey(const std::string& key);
    bool hasApiKey() const;
    bool hasValidApiKey() const;
    bool needsSetupPrompt();
    bool applyKeyBuffer(char* buf, std::size_t bufSize);
    ApiKeyStatus apiKeyStatus() const;
    bool validateApiKey();

private:
    PlayerScout();
    ~PlayerScout();
    PlayerScout(const PlayerScout&) = delete;
    PlayerScout& operator=(const PlayerScout&) = delete;

    struct CacheEntry {
        Row row;
        std::uint64_t fetchedAtMs = 0;
        std::uint64_t lastSeenMs = 0;
    };

    void syncRoster(const EntityManager::Snapshot& snap);
    void workerMain();
    void launchFetch(std::uint64_t steamId);
    void notifyWorker();
    bool fetchLeetifyProfile(std::uint64_t steamId, Row& out);
    void applyFetchResult(std::uint64_t steamId, Row fetched);
    static int computeSuspicion(const Row& row);
    static std::string suspicionLabelFor(int score);
    std::string activeApiKey() const;
    std::uint64_t fetchIntervalMs() const;

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_worker;
    std::atomic<bool> m_shutdown{false};
    std::atomic<int> m_inFlight{0};
    static constexpr int kMaxInFlight = 1;
    std::unordered_map<std::uint64_t, CacheEntry> m_cache;
    std::vector<std::uint64_t> m_queue;
    std::uint64_t m_lastFetchMs = 0;
    std::uint64_t m_rateLimitUntilMs = 0;
    std::uint64_t m_lastDispatchMs = 0;
    static constexpr std::uint64_t kMinDispatchIntervalMs = 900;

    std::string m_apiKey;
    ApiKeyStatus m_keyStatus = ApiKeyStatus::NotSet;
    bool m_keyLoaded = false;
};
