#pragma once

#include "game/entity_manager.h"
#include "math/vector.h"

#include <array>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class MatchIntel {
public:
    struct Cue {
        std::string text;
        int severity = 0; // 0=info,1=warn,2=critical
    };

    struct ThreatCard {
        std::string name;
        int team = 0;
        float score = 0.f;
        int kills = 0;
        int openingKills = 0;
        int openingDuels = 0;
        int clutchKills = 0;
        int clutchAttempts = 0;
        float entrySuccess = 0.f;
        float clutchRate = 0.f;
    };

    struct TimelineEvent {
        std::uint64_t atMs = 0;
        std::string type;
        std::string text;
        Vec3 pos{};
    };

    struct HeatPoint {
        int cellX = 0;
        int cellY = 0;
        int count = 0;
    };

    struct View {
        int perfProfile = 1;
        int currentRound = 0;
        bool roundLive = false;
        std::string mapName;
        int storedRoundCount = 0;
        int localDeaths = 0;
        std::vector<Cue> cues;
        std::vector<ThreatCard> threats;
        std::vector<TimelineEvent> replayEvents;
        int replayEventIndex = 0;
        int replayEventMax = 0;
        int replayRoundIndex = 0;
        int replayRoundMax = 0;
        std::vector<HeatPoint> deathHeat;
        std::vector<HeatPoint> failedEntryHeat;
    };

    static MatchIntel& instance();

    void update(const EntityManager::Snapshot& snap);
    View view() const;

    void setPerformanceProfile(int profile);
    int performanceProfile() const;

    void setReplayEventIndex(int index);
    int replayEventIndex() const;

    void setReplayRoundIndex(int index);
    int replayRoundIndex() const;

    void resetSession();

private:
    MatchIntel() = default;

    struct ThreatStats {
        int team = 0;
        int kills = 0;
        int deaths = 0;
        int openingKills = 0;
        int openingDuels = 0;
        int clutchKills = 0;
        int clutchAttempts = 0;
    };

    struct PrevPlayer {
        bool alive = false;
        int team = 0;
        Vec3 pos{};
        std::string name;
    };

    struct RoundReplay {
        int id = 0;
        std::uint64_t startMs = 0;
        std::vector<TimelineEvent> events;
    };

    static std::uint64_t nowMs();
    static int cellCoord(float value, float step);
    static std::string grenadeTypeName(GrenadeType t);
    static bool isPistolLike(const std::string& weapon);

    void pushEvent(const std::string& type, const std::string& text, const Vec3& pos = {});
    void finalizeRound();
    void resetSessionLocked();
    void updateReplayViewLocked();

    mutable std::mutex m_mutex;

    int m_perfProfile = 1; // 0=LAN, 1=Balanced, 2=Stream-safe
    int m_currentRound = 1;
    bool m_roundLive = false;
    std::uint64_t m_roundStartMs = 0;
    int m_prevRoundStartCount = -1;
    int m_prevRoundEndCount = -1;
    bool m_openingResolved = false;
    bool m_localGotOpeningKill = false;

    bool m_prevBombPlanted = false;
    bool m_prevBombDefusing = false;
    int m_prevAliveT = 0;
    int m_prevAliveCT = 0;

    std::unordered_map<std::uintptr_t, PrevPlayer> m_prevPlayers;
    std::unordered_map<std::string, ThreatStats> m_threatStats;

    std::vector<Cue> m_cues;
    std::vector<ThreatCard> m_threatCards;
    std::vector<TimelineEvent> m_currentRoundEvents;
    std::deque<RoundReplay> m_rounds;

    int m_replayEventIndex = 0;
    int m_replayRoundIndex = -1;
    std::vector<TimelineEvent> m_replayEventsView;

    std::string m_lastMapName;
    int m_localDeaths = 0;

    std::unordered_map<std::string, int> m_deathHeat;
    std::unordered_map<std::string, int> m_failedEntryHeat;
};
