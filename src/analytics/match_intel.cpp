#include "analytics/match_intel.h"

#include "config.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <sstream>

namespace {
constexpr float kHeatCell = 512.f;
constexpr std::size_t kMaxRounds = 24;
constexpr std::size_t kMaxEventsPerRound = 280;
constexpr std::size_t kMaxCues = 8;
constexpr std::size_t kMaxThreatCards = 8;

std::string heatKey(int x, int y) {
    return std::to_string(x) + ":" + std::to_string(y);
}

bool parseHeatKey(const std::string& key, int& x, int& y) {
    const auto p = key.find(':');
    if (p == std::string::npos)
        return false;
    try {
        x = std::stoi(key.substr(0, p));
        y = std::stoi(key.substr(p + 1));
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

namespace {
float distSq2D(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return dx * dx + dy * dy;
}
} // namespace

MatchIntel& MatchIntel::instance() {
    static MatchIntel inst;
    return inst;
}

std::uint64_t MatchIntel::nowMs() {
    return GetTickCount64();
}

int MatchIntel::cellCoord(float value, float step) {
    return static_cast<int>(std::floor(value / step));
}

std::string MatchIntel::grenadeTypeName(GrenadeType t) {
    switch (t) {
    case GrenadeType::HE: return "HE";
    case GrenadeType::Smoke: return "Smoke";
    case GrenadeType::Flash: return "Flash";
    case GrenadeType::Molotov: return "Molotov";
    case GrenadeType::Decoy: return "Decoy";
    default: return "Unknown";
    }
}

bool MatchIntel::isPistolLike(const std::string& weapon) {
    const std::string w = weapon;
    return w.find("glock") != std::string::npos
        || w.find("usp") != std::string::npos
        || w.find("p2000") != std::string::npos
        || w.find("p250") != std::string::npos
        || w.find("deagle") != std::string::npos
        || w.find("elite") != std::string::npos
        || w.find("fiveseven") != std::string::npos
        || w.find("cz75") != std::string::npos
        || w.find("revolver") != std::string::npos;
}

void MatchIntel::pushEvent(const std::string& type, const std::string& text, const Vec3& pos) {
    TimelineEvent ev;
    ev.atMs = nowMs();
    ev.type = type;
    ev.text = text;
    ev.pos = pos;

    m_currentRoundEvents.push_back(std::move(ev));
    if (m_currentRoundEvents.size() > kMaxEventsPerRound)
        m_currentRoundEvents.erase(m_currentRoundEvents.begin());
}

void MatchIntel::finalizeRound() {
    if (m_currentRoundEvents.empty())
        return;

    RoundReplay rr;
    rr.id = m_currentRound;
    rr.startMs = m_roundStartMs;
    rr.events = m_currentRoundEvents;
    m_rounds.push_back(std::move(rr));
    while (m_rounds.size() > kMaxRounds)
        m_rounds.pop_front();

    m_currentRoundEvents.clear();
    m_replayEventIndex = 0;
    m_openingResolved = false;
    m_localGotOpeningKill = false;
}

void MatchIntel::updateReplayViewLocked() {
    if (m_replayRoundIndex >= 0 && m_replayRoundIndex < static_cast<int>(m_rounds.size()))
        m_replayEventsView = m_rounds[static_cast<std::size_t>(m_replayRoundIndex)].events;
    else if (m_roundLive || m_rounds.empty())
        m_replayEventsView = m_currentRoundEvents;
    else
        m_replayEventsView = m_rounds.back().events;

    if (m_replayEventsView.empty()) {
        m_replayEventIndex = 0;
        return;
    }
    if (m_replayEventIndex < 0)
        m_replayEventIndex = 0;
    if (m_replayEventIndex >= static_cast<int>(m_replayEventsView.size()))
        m_replayEventIndex = static_cast<int>(m_replayEventsView.size()) - 1;
}

void MatchIntel::resetSessionLocked() {
    m_currentRound = 1;
    m_roundLive = false;
    m_roundStartMs = 0;
    m_prevRoundStartCount = -1;
    m_prevRoundEndCount = -1;
    m_openingResolved = false;
    m_localGotOpeningKill = false;
    m_prevBombPlanted = false;
    m_prevBombDefusing = false;
    m_prevAliveT = 0;
    m_prevAliveCT = 0;
    m_prevPlayers.clear();
    m_threatStats.clear();
    m_cues.clear();
    m_threatCards.clear();
    m_currentRoundEvents.clear();
    m_rounds.clear();
    m_replayEventIndex = 0;
    m_replayRoundIndex = -1;
    m_replayEventsView.clear();
    m_deathHeat.clear();
    m_failedEntryHeat.clear();
    m_localDeaths = 0;
}

void MatchIntel::resetSession() {
    std::lock_guard<std::mutex> lg(m_mutex);
    resetSessionLocked();
}

void MatchIntel::setReplayRoundIndex(int index) {
    std::lock_guard<std::mutex> lg(m_mutex);
    m_replayRoundIndex = index;
    updateReplayViewLocked();
}

int MatchIntel::replayRoundIndex() const {
    std::lock_guard<std::mutex> lg(m_mutex);
    return m_replayRoundIndex;
}

void MatchIntel::update(const EntityManager::Snapshot& snap) {
    std::lock_guard<std::mutex> lg(m_mutex);

    if (!snap.currentMapName.empty() && snap.currentMapName != m_lastMapName) {
        if (!m_lastMapName.empty())
            resetSessionLocked();
        m_lastMapName = snap.currentMapName;
    }

    const std::uint64_t tNow = nowMs();
    const bool streamSafe = (m_perfProfile >= 2);
    m_cues.clear();

    const PlayerData* local = nullptr;
    int aliveT = 0;
    int aliveCT = 0;
    int localTeam = snap.localTeam;
    int enemyPistolLike = 0;
    int enemyCount = 0;

    for (const auto& p : snap.players) {
        if (!p.isValid || !p.isAlive)
            continue;

        if (p.teamNum == 2) ++aliveT;
        if (p.teamNum == 3) ++aliveCT;
        if (p.isLocalPlayer)
            local = &p;
    }

    if (!local && snap.localPawn != 0) {
        for (const auto& p : snap.players) {
            if (p.isValid && p.pawn == snap.localPawn) {
                local = &p;
                break;
            }
        }
    }

    if (!local) {
        for (const auto& p : snap.players) {
            if (p.isValid && p.isAlive) {
                local = &p;
                localTeam = p.teamNum;
                break;
            }
        }
    }

    for (const auto& p : snap.players) {
        if (!p.isValid || !p.isAlive)
            continue;
        if (localTeam != 0 && p.teamNum != localTeam) {
            ++enemyCount;
            if (isPistolLike(p.weaponName))
                ++enemyPistolLike;
        }
    }

    const bool bothTeamsAlive = aliveT >= 1 && aliveCT >= 1;
    const auto& gr = snap.gameRules;

    if (gr.valid && !gr.warmupPeriod) {
        if (m_prevRoundStartCount < 0) {
            m_prevRoundStartCount = gr.roundStartCount;
            m_prevRoundEndCount = gr.roundEndCount;
            m_roundLive = gr.roundStartCount != gr.roundEndCount;
            if (gr.roundStartRoundNumber > 0)
                m_currentRound = gr.roundStartRoundNumber;
            else if (gr.totalRoundsPlayed >= 0)
                m_currentRound = gr.totalRoundsPlayed + (m_roundLive ? 1 : 0);
            if (m_roundLive)
                m_roundStartMs = tNow;
        } else {
            if (gr.roundStartCount != static_cast<std::uint8_t>(m_prevRoundStartCount)) {
                m_prevRoundStartCount = gr.roundStartCount;
                m_roundLive = true;
                m_roundStartMs = tNow;
                m_openingResolved = false;
                m_localGotOpeningKill = false;
                if (gr.roundStartRoundNumber > 0)
                    m_currentRound = gr.roundStartRoundNumber;
                else
                    ++m_currentRound;
                pushEvent("round", "Round started");
            }

            if (gr.roundEndCount != static_cast<std::uint8_t>(m_prevRoundEndCount)) {
                m_prevRoundEndCount = gr.roundEndCount;
                if (m_roundLive) {
                    pushEvent("round", "Round ended");
                    finalizeRound();
                    m_roundLive = false;
                }
            }
        }
    } else if (!gr.valid) {
        if (!m_roundLive && bothTeamsAlive) {
            m_roundLive = true;
            m_roundStartMs = tNow;
            m_openingResolved = false;
            m_localGotOpeningKill = false;
            pushEvent("round", "Round started");
        }

        const bool oneTeamEliminated = (aliveT == 0) != (aliveCT == 0);
        const bool minRoundAge = (tNow - m_roundStartMs) >= 4000ull;
        const bool roundEndedByElim = m_roundLive && oneTeamEliminated && minRoundAge;
        const bool roundEndedByBomb = m_roundLive && m_prevBombPlanted && !snap.bomb.isPlanted;
        if (roundEndedByElim || roundEndedByBomb) {
            pushEvent("round", "Round ended");
            finalizeRound();
            ++m_currentRound;
            m_roundLive = false;
        }
    }

    if (snap.bomb.isPlanted && !m_prevBombPlanted)
        pushEvent("bomb", snap.bomb.site == 1 ? "Bomb planted on B" : "Bomb planted on A", snap.bomb.origin);
    if (snap.bomb.isBeingDefused && !m_prevBombDefusing)
        pushEvent("bomb", "Defuse started", snap.bomb.origin);

    if (m_prevAliveT != 0 || m_prevAliveCT != 0) {
        const int prevDiff = m_prevAliveCT - m_prevAliveT;
        const int curDiff = aliveCT - aliveT;
        if (std::abs(curDiff - prevDiff) >= 2) {
            std::ostringstream oss;
            oss << "Man-advantage swing: CT " << aliveCT << " vs T " << aliveT;
            m_cues.push_back({ oss.str(), 1 });
        }
    }

    if (snap.bomb.isPlanted && snap.bomb.timeRemaining > 0.f && local) {
        if (distSq2D(local->origin, snap.bomb.origin) < 1800.f * 1800.f && snap.bomb.timeRemaining < 13.f) {
            std::ostringstream oss;
            oss << "Bomb window risk: " << static_cast<int>(snap.bomb.timeRemaining + 0.5f) << "s";
            m_cues.push_back({ oss.str(), 2 });
        }
    }

    if (enemyCount >= 3 && (enemyPistolLike * 100 / enemyCount) >= 66)
        m_cues.push_back({ "Eco read: enemy likely low-buy", 1 });

    if (m_roundLive) {
        const float ageS = static_cast<float>(tNow - m_roundStartMs) / 1000.f;
        int activeUtil = 0;
        for (const auto& g : snap.grenades) {
            if (!g.isValid)
                continue;
            if (g.isDeployed || g.timeAlive < 2.0f)
                ++activeUtil;
        }
        if (ageS > 45.f && activeUtil <= 1)
            m_cues.push_back({ "Low utility pressure detected", 0 });
    }

    for (const auto& g : snap.grenades) {
        if (!g.isValid)
            continue;
        if (g.timeAlive < 0.22f)
            pushEvent("utility", grenadeTypeName(g.type) + " thrown", g.origin);
    }

    std::unordered_map<std::uintptr_t, PrevPlayer> curPlayers;
    curPlayers.reserve(snap.players.size());

    const bool openingWindow = m_roundLive && (tNow - m_roundStartMs <= 20000ull);

    for (const auto& p : snap.players) {
        if (!p.isValid || p.pawn == 0)
            continue;

        PrevPlayer nowP{ p.isAlive, p.teamNum, p.origin, p.name };
        curPlayers[p.pawn] = nowP;

        const auto itPrev = m_prevPlayers.find(p.pawn);
        if (itPrev == m_prevPlayers.end())
            continue;

        const PrevPlayer& prev = itPrev->second;
        if (prev.alive && !nowP.alive) {
            const PlayerData* killer = nullptr;
            float bestD2 = 950.f * 950.f;
            for (const auto& cand : snap.players) {
                if (!cand.isValid || !cand.isAlive || cand.teamNum == p.teamNum)
                    continue;
                const float d2 = distSq2D(cand.origin, prev.pos);
                if (d2 < bestD2) {
                    bestD2 = d2;
                    killer = &cand;
                }
            }

            std::string victimName = p.name.empty() ? "Unknown" : p.name;
            std::string killerName = killer ? (killer->name.empty() ? "Unknown" : killer->name) : "Unknown";
            pushEvent("kill", killerName + " -> " + victimName, prev.pos);

            if (killer) {
                ThreatStats& ks = m_threatStats[killerName];
                ks.team = killer->teamNum;
                ks.kills += 1;

                if (openingWindow) {
                    ks.openingDuels += 1;
                    if (!m_openingResolved) {
                        ks.openingKills += 1;
                        m_openingResolved = true;
                        if (local && killer->pawn == local->pawn)
                            m_localGotOpeningKill = true;
                    }
                }

                const int killerTeamAlive = (killer->teamNum == 2) ? aliveT : aliveCT;
                const int victimTeamAlive = (killer->teamNum == 2) ? aliveCT : aliveT;
                if (killerTeamAlive <= 1 && victimTeamAlive >= 2) {
                    ks.clutchAttempts += 1;
                    ks.clutchKills += 1;
                }
            }

            if (local && p.pawn == local->pawn) {
                const int cx = cellCoord(prev.pos.x, kHeatCell);
                const int cy = cellCoord(prev.pos.y, kHeatCell);
                ++m_deathHeat[heatKey(cx, cy)];
                ++m_localDeaths;
                if (openingWindow && !m_localGotOpeningKill)
                    ++m_failedEntryHeat[heatKey(cx, cy)];
            }
        }

        if (prev.alive && nowP.alive && !streamSafe) {
            const float d2 = distSq2D(prev.pos, nowP.pos);
            if (d2 > 2400.f * 2400.f)
                pushEvent("rotate", (p.name.empty() ? std::string("Player") : p.name) + " rotate", nowP.pos);
        }
    }

    m_prevPlayers = std::move(curPlayers);

    m_threatCards.clear();
    for (const auto& kv : m_threatStats) {
        if (localTeam != 0 && kv.second.team == localTeam)
            continue;

        ThreatCard c;
        c.name = kv.first;
        c.team = kv.second.team;
        c.kills = kv.second.kills;
        c.openingKills = kv.second.openingKills;
        c.openingDuels = kv.second.openingDuels;
        c.clutchKills = kv.second.clutchKills;
        c.clutchAttempts = kv.second.clutchAttempts;
        c.entrySuccess = kv.second.openingDuels > 0 ? static_cast<float>(kv.second.openingKills) / static_cast<float>(kv.second.openingDuels) : 0.f;
        c.clutchRate = kv.second.clutchAttempts > 0 ? static_cast<float>(kv.second.clutchKills) / static_cast<float>(kv.second.clutchAttempts) : 0.f;
        c.score = c.kills * 8.f + c.openingKills * 7.f + c.clutchKills * 6.f + c.entrySuccess * 15.f + c.clutchRate * 12.f;
        m_threatCards.push_back(std::move(c));
    }

    std::sort(m_threatCards.begin(), m_threatCards.end(), [](const ThreatCard& a, const ThreatCard& b) {
        return a.score > b.score;
    });
    if (m_threatCards.size() > kMaxThreatCards)
        m_threatCards.resize(kMaxThreatCards);

    if (m_cues.size() > (streamSafe ? 4 : kMaxCues))
        m_cues.resize(streamSafe ? 4 : kMaxCues);

    m_prevBombPlanted = snap.bomb.isPlanted;
    m_prevBombDefusing = snap.bomb.isBeingDefused;
    m_prevAliveT = aliveT;
    m_prevAliveCT = aliveCT;

    updateReplayViewLocked();
}

MatchIntel::View MatchIntel::view() const {
    std::lock_guard<std::mutex> lg(m_mutex);

    View v;
    v.perfProfile = m_perfProfile;
    v.currentRound = m_currentRound;
    v.roundLive = m_roundLive;
    v.mapName = m_lastMapName;
    v.storedRoundCount = static_cast<int>(m_rounds.size());
    v.localDeaths = m_localDeaths;
    v.cues = m_cues;
    v.threats = m_threatCards;
    v.replayEvents = m_replayEventsView;
    v.replayEventMax = static_cast<int>(m_replayEventsView.size()) > 0 ? static_cast<int>(m_replayEventsView.size()) - 1 : 0;
    v.replayEventIndex = std::clamp(m_replayEventIndex, 0, v.replayEventMax);
    v.replayRoundMax = static_cast<int>(m_rounds.size()) > 0 ? static_cast<int>(m_rounds.size()) - 1 : 0;
    v.replayRoundIndex = (m_replayRoundIndex >= 0)
        ? std::clamp(m_replayRoundIndex, 0, v.replayRoundMax)
        : (v.replayRoundMax >= 0 ? v.replayRoundMax : 0);

    auto buildHeat = [](const std::unordered_map<std::string, int>& src) {
        std::vector<HeatPoint> out;
        out.reserve(src.size());
        for (const auto& kv : src) {
            HeatPoint hp;
            if (!parseHeatKey(kv.first, hp.cellX, hp.cellY))
                continue;
            hp.count = kv.second;
            out.push_back(hp);
        }
        std::sort(out.begin(), out.end(), [](const HeatPoint& a, const HeatPoint& b) { return a.count > b.count; });
        if (out.size() > 8)
            out.resize(8);
        return out;
    };

    v.deathHeat = buildHeat(m_deathHeat);
    v.failedEntryHeat = buildHeat(m_failedEntryHeat);

    return v;
}

void MatchIntel::setPerformanceProfile(int profile) {
    profile = std::clamp(profile, 0, 2);

    std::lock_guard<std::mutex> lg(m_mutex);
    m_perfProfile = profile;
}

int MatchIntel::performanceProfile() const {
    std::lock_guard<std::mutex> lg(m_mutex);
    return m_perfProfile;
}

void MatchIntel::setReplayEventIndex(int index) {
    std::lock_guard<std::mutex> lg(m_mutex);
    m_replayEventIndex = (std::max)(0, index);
}

int MatchIntel::replayEventIndex() const {
    std::lock_guard<std::mutex> lg(m_mutex);
    return m_replayEventIndex;
}
