#include "analytics/match_intel.h"

#include "config.h"
#include "json.hpp"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

using json = nlohmann::json;

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

std::filesystem::path lineupFilePath() {
    return std::filesystem::path("configs") / "utility_library.json";
}

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

void MatchIntel::loadLineupsIfNeeded() {
    if (m_lineupsLoaded)
        return;
    m_lineupsLoaded = true;

    std::ifstream in(lineupFilePath(), std::ios::binary);
    if (!in.is_open())
        return;

    try {
        json j;
        in >> j;
        if (!j.is_array())
            return;

        for (const auto& item : j) {
            UtilityLineup lu;
            lu.map = item.value("map", "");
            lu.team = item.value("team", 0);
            lu.type = item.value("type", "");
            lu.throwYaw = item.value("throwYaw", 0.f);
            lu.createdMs = item.value("createdMs", 0ull);
            lu.throwPos = Vec3(item["throwPos"].value("x", 0.f), item["throwPos"].value("y", 0.f), item["throwPos"].value("z", 0.f));
            lu.landPos = Vec3(item["landPos"].value("x", 0.f), item["landPos"].value("y", 0.f), item["landPos"].value("z", 0.f));
            if (!lu.map.empty() && !lu.type.empty())
                m_lineups.push_back(std::move(lu));
        }
    } catch (...) {
    }
}

void MatchIntel::saveLineups() {
    if (!m_lineupsDirty)
        return;

    const auto path = lineupFilePath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    json out = json::array();
    for (const auto& lu : m_lineups) {
        out.push_back({
            { "map", lu.map },
            { "team", lu.team },
            { "type", lu.type },
            { "throwYaw", lu.throwYaw },
            { "createdMs", lu.createdMs },
            { "throwPos", { { "x", lu.throwPos.x }, { "y", lu.throwPos.y }, { "z", lu.throwPos.z } } },
            { "landPos", { { "x", lu.landPos.x }, { "y", lu.landPos.y }, { "z", lu.landPos.z } } }
        });
    }

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open())
        return;
    f << out.dump(2);
    m_lineupsDirty = false;
}

void MatchIntel::registerUtilityLineup(const EntityManager::Snapshot&, const GrenadeData& grenade, std::uint64_t tNow) {
    if (!m_throwCandidate.active)
        return;
    if (tNow - m_throwCandidate.atMs > 3500ull)
        return;
    if (m_throwCandidate.type != grenade.type)
        return;
    if (!grenade.hasStableLandPos && !grenade.isDeployed)
        return;

    UtilityLineup lu;
    lu.map = m_throwCandidate.map;
    lu.team = m_throwCandidate.team;
    lu.type = grenadeTypeName(grenade.type);
    lu.throwPos = m_throwCandidate.origin;
    lu.throwYaw = m_throwCandidate.yaw;
    lu.landPos = grenade.hasStableLandPos ? grenade.stableLandPos : grenade.origin;
    lu.createdMs = tNow;

    for (const auto& e : m_lineups) {
        if (e.map != lu.map || e.team != lu.team || e.type != lu.type)
            continue;
        if (distSq2D(e.throwPos, lu.throwPos) < 90.f * 90.f
            && distSq2D(e.landPos, lu.landPos) < 140.f * 140.f) {
            m_throwCandidate.active = false;
            return;
        }
    }

    m_lineups.push_back(std::move(lu));
    if (m_lineups.size() > 1200)
        m_lineups.erase(m_lineups.begin(), m_lineups.begin() + (m_lineups.size() - 1200));
    m_throwCandidate.active = false;
    m_lineupsDirty = true;

    pushEvent("utility", "Saved utility lineup");
}

void MatchIntel::updateReplayViewLocked() {
    if (m_roundLive || m_rounds.empty())
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

void MatchIntel::update(const EntityManager::Snapshot& snap) {
    std::lock_guard<std::mutex> lg(m_mutex);
    loadLineupsIfNeeded();

    const std::uint64_t tNow = nowMs();
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

    if (!m_roundLive && (aliveT + aliveCT) >= 2) {
        m_roundLive = true;
        m_roundStartMs = tNow;
        m_openingResolved = false;
        m_localGotOpeningKill = false;
        pushEvent("round", "Round started");
    }

    const bool roundEndedByElim = m_roundLive && (aliveT == 0 || aliveCT == 0);
    const bool roundEndedByBomb = m_roundLive && m_prevBombPlanted && !snap.bomb.isPlanted;
    if (roundEndedByElim || roundEndedByBomb) {
        pushEvent("round", "Round ended");
        finalizeRound();
        ++m_currentRound;
        m_roundLive = false;
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

    if (!snap.preThrow.isActive && m_throwCandidate.active && (tNow - m_throwCandidate.atMs > 4000ull)) {
        ++m_utilityWasteHeat[heatKey(m_throwCandidate.localCellX, m_throwCandidate.localCellY)];
        m_throwCandidate.active = false;
    }

    if (snap.preThrow.isActive && local) {
        m_throwCandidate.active = true;
        m_throwCandidate.atMs = tNow;
        m_throwCandidate.map = snap.currentMapName;
        m_throwCandidate.team = local->teamNum;
        m_throwCandidate.type = snap.preThrow.type;
        m_throwCandidate.origin = local->origin;
        m_throwCandidate.yaw = local->eyeYaw;
        m_throwCandidate.localCellX = cellCoord(local->origin.x, kHeatCell);
        m_throwCandidate.localCellY = cellCoord(local->origin.y, kHeatCell);
    }

    for (const auto& g : snap.grenades) {
        if (!g.isValid)
            continue;
        if (g.timeAlive < 0.22f)
            pushEvent("utility", grenadeTypeName(g.type) + " thrown", g.origin);
        registerUtilityLineup(snap, g, tNow);
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
                if (openingWindow && !m_localGotOpeningKill)
                    ++m_failedEntryHeat[heatKey(cx, cy)];
            }
        }

        if (prev.alive && nowP.alive) {
            const float d2 = distSq2D(prev.pos, nowP.pos);
            if (d2 > 1700.f * 1700.f)
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

    if (m_cues.size() > kMaxCues)
        m_cues.resize(kMaxCues);

    m_prevBombPlanted = snap.bomb.isPlanted;
    m_prevBombDefusing = snap.bomb.isBeingDefused;
    m_prevAliveT = aliveT;
    m_prevAliveCT = aliveCT;

    updateReplayViewLocked();
    saveLineups();
}

MatchIntel::View MatchIntel::view() const {
    std::lock_guard<std::mutex> lg(m_mutex);

    View v;
    v.perfProfile = m_perfProfile;
    v.currentRound = m_currentRound;
    v.roundLive = m_roundLive;
    v.cues = m_cues;
    v.threats = m_threatCards;
    v.replayEvents = m_replayEventsView;
    v.replayEventMax = static_cast<int>(m_replayEventsView.size()) > 0 ? static_cast<int>(m_replayEventsView.size()) - 1 : 0;
    v.replayEventIndex = std::clamp(m_replayEventIndex, 0, v.replayEventMax);

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
    v.utilityWasteHeat = buildHeat(m_utilityWasteHeat);

    v.lineupCount = static_cast<int>(m_lineups.size());
    if (!m_lineups.empty()) {
        v.lineupMatches.push_back(m_lineups.back());
        if (m_lineups.size() > 1)
            v.lineupMatches.push_back(m_lineups[m_lineups.size() - 2]);
    }

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
