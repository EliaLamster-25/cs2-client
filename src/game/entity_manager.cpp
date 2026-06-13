#include "entity_manager.h"
#include "pawn_services.h"
#include "str_obf.h"
#include "debug/overlay_log.h"
#include "game/cs2_bones.h"
#include "game/cs2_model.h"
#include "game/bone_layout.h"
#include "memory/rpm.h"
#include "offsets/offsets.h"
#include "offsets/netvars.h"
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cctype>
#include <iostream>
#include <iomanip>
#include <vector>
#include <thread>

/// @file entity_manager.cpp
/// @brief Full implementation of the per-frame entity read loop.

namespace {
constexpr float kGrenadeGravity         = 320.f;
constexpr float kGrenadeDt              = 1.f / 64.f;
constexpr float kGrenadeStopSpeedSq     = 2036.f;
constexpr float kGrenadeHullRadius      = 2.0f;
constexpr float kMolotovMaxSlopeZ       = 0.8660254f;
constexpr float kSteepBounceNormalZ     = 0.7f;
constexpr float kSteepBounceSpeedSq     = 96000.f;
constexpr float kBombPlantLength        = 3.2f;
constexpr float kPreThrowDt             = kGrenadeDt * 2.0f;
constexpr int kPreThrowRecordStep       = 2;
constexpr int kPreThrowMaxSteps         = 96;
constexpr int kPreThrowNoBspMaxSteps    = 72;
constexpr int kPreThrowMaxBounces       = 3;
constexpr int kPreThrowBspSweepMaxSteps = 48;
constexpr int kPreThrowBspSweepMaxBounces = 2;
constexpr std::uint64_t kPreThrowMinRefreshMs = 22u;
constexpr std::uint64_t kPreThrowStableReuseMs = 58u;
constexpr std::uint64_t kPreThrowBspMinRefreshMs = 96u;
constexpr std::uint64_t kPreThrowBspStableReuseMs = 220u;
constexpr float kPreThrowStrengthTolerance = 0.035f;
constexpr float kPreThrowAngleToleranceDeg = 0.7f;
constexpr float kPreThrowStartPosToleranceSq = 7.0f * 7.0f;
constexpr float kPreThrowVelocityToleranceSq = 48.0f * 48.0f;
constexpr int kMinNearbyBoneCount       = 10;
constexpr int kMinBoneLayoutScore       = 12;
constexpr int kRawBoneCount             = 30;

float horizDistSq(const Vec3& a, const Vec3& b) {
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    return dx * dx + dy * dy;
}

bool isFiniteVec3(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool hasLineOfSight(const BspWorld& world, const Vec3& eye, const Vec3& target, bool eyeInsideWall) {
    if (!world.isLoaded())
        return true;
    if (eyeInsideWall)
        return true;

    Vec3 dir = target - eye;
    const float lenSq = dir.lengthSq();
    if (lenSq < 0.01f)
        return true;

    const float len = std::sqrt(lenSq);
    dir = dir * (1.f / len);
    const Vec3 start = eye + dir * 2.0f;

    // Use any-hit sweep â€” early exits on first blocking geometry,
    // orders of magnitude faster than closest-hit sweep for visibility checks.
    return !world.sweepAny(start, target);
}

struct ChamsSegDef { int a; int b; int midSlot; };

static constexpr ChamsSegDef kChamsSegDefs[PlayerData::kChamsSegCount] = {
    { 0, 22, 0 }, { 22, 23, 1 }, { 23, 24, 2 },
    { 0, 25, 3 }, { 25, 26, 4 }, { 26, 27, 5 },
    { 5, 8, 6 }, { 8, 9, 7 }, { 9, 11, 8 },
    { 5, 13, 9 }, { 13, 14, 10 }, { 14, 16, 11 },
    { 0, 2, 12 }, { 2, 4, 13 }, { 4, 5, 14 }, { 5, 6, 15 },
};

static constexpr int kChamsBones[] = {
    0, 2, 4, 5, 6, 8, 9, 11, 13, 14, 16, 22, 23, 24, 25, 26, 27
};

static void fillChamsPartVisibility(PlayerData& p, bool visible) {
    p.chamsPartVisChecked = true;
    for (int i = 0; i < PlayerData::kBoneCount; ++i)
        p.chamsPartVisible[i] = visible;
    for (int i = 0; i < PlayerData::kChamsSegCount; ++i)
        p.chamsSegMidVisible[i] = visible;
}

static void updateChamsPartVisibility(PlayerData& p,
                                      const BspWorld& world,
                                      GameTraceVis* gameTrace,
                                      Process* proc,
                                      std::uintptr_t localPawn,
                                      const Vec3& localEye,
                                      bool eyeInsideWall,
                                      bool useGameTrace,
                                      bool useBspVis)
{
    auto mainVisFallback = [&]() -> bool {
        if (!g_cfg.visibilityCheckEnabled || !p.visibilityChecked)
            return true;
        return p.isVisible;
    };

    auto checkLos = [&](const Vec3& pt) -> bool {
        if (useGameTrace && gameTrace && proc && localPawn)
            return gameTrace->hasLineOfSight(*proc, localPawn, p.pawn, localEye, pt);
        if (useBspVis)
            return hasLineOfSight(world, localEye, pt, eyeInsideWall);
        return mainVisFallback();
    };

    if (!p.bonesValid || !p.isValid || !p.isAlive || p.isLocalPlayer) {
        p.chamsPartVisChecked = false;
        return;
    }

    if (!useGameTrace && (!useBspVis || !world.isLoaded())) {
        fillChamsPartVisibility(p, mainVisFallback());
        return;
    }

    if (g_cfg.visibilityCheckEnabled && p.visibilityChecked) {
        const float conf = p.visibilityConfidence;
        if (!p.isVisible && conf < 0.18f) {
            fillChamsPartVisibility(p, false);
            return;
        }
        if (p.isVisible && conf > 0.85f) {
            fillChamsPartVisibility(p, true);
            return;
        }
    }

    p.chamsPartVisChecked = true;
    for (int i = 0; i < PlayerData::kBoneCount; ++i)
        p.chamsPartVisible[i] = false;

    for (int boneIndex : kChamsBones) {
        if (boneIndex < 0 || boneIndex >= PlayerData::kBoneCount)
            continue;
        const Vec3& pt = p.bones[boneIndex];
        if (!isFiniteVec3(pt))
            continue;
        p.chamsPartVisible[boneIndex] = checkLos(pt);
    }

    for (const auto& seg : kChamsSegDefs) {
        if (seg.a < 0 || seg.b < 0
            || seg.a >= PlayerData::kBoneCount || seg.b >= PlayerData::kBoneCount
            || seg.midSlot < 0 || seg.midSlot >= PlayerData::kChamsSegCount)
            continue;

        const Vec3& a = p.bones[seg.a];
        const Vec3& b = p.bones[seg.b];
        if (!isFiniteVec3(a) || !isFiniteVec3(b)) {
            p.chamsSegMidVisible[seg.midSlot] = false;
            continue;
        }
        const Vec3 mid = {
            (a.x + b.x) * 0.5f,
            (a.y + b.y) * 0.5f,
            (a.z + b.z) * 0.5f,
        };
        p.chamsSegMidVisible[seg.midSlot] = checkLos(mid);
    }
}

int collectVisibilitySamples(const PlayerData& p, int mode, Vec3* out, int cap) {
    if (cap <= 0)
        return 0;

    int n = 0;
    auto push = [&](const Vec3& v) {
        if (n < cap && isFiniteVec3(v))
            out[n++] = v;
    };

    // Reduced sample counts for performance: Fast=1, Balanced=2, Strict=3
    // (was Fast=2, Balanced=3, Strict=7 â€” the brush spatial grid makes per-sweep
    //  cost negligible, but fewer sweeps still saves proportional time.)
    push(p.headPos);  // always check the head

    if (mode >= 1) {
        Vec3 chest = p.origin + Vec3{ 0.f, 0.f, 54.f };
        push(chest);
    }

    if (mode >= 2) {
        Vec3 pelvis = p.origin + Vec3{ 0.f, 0.f, 34.f };
        push(pelvis);
    }

    return n;
}

float wrappedAngleDeltaDeg(float a, float b) {
    float delta = std::fabsf(a - b);
    while (delta > 180.f)
        delta = std::fabsf(delta - 360.f);
    return delta;
}

int nearbyBoneCount(const Vec3* bones,
                    int boneCount,
                    const Vec3& origin,
                    float maxDistSq)
{
    int count = 0;
    for (int i = 0; i < boneCount; ++i) {
        const Vec3& bone = bones[i];
        if (!isFiniteVec3(bone))
            continue;
        Vec3 d = bone - origin;
        if (d.lengthSq() <= maxDistSq)
            ++count;
    }
    return count;
}

int scoreBoneLayout(const Vec3* bones,
                    int boneCount,
                    const Vec3& origin,
                    const Vec3& headPos)
{
    auto valid = [&](int index) -> bool {
        if (index < 0 || index >= boneCount)
            return false;
        const Vec3& bone = bones[index];
        if (!isFiniteVec3(bone))
            return false;
        Vec3 d = bone - origin;
        return d.lengthSq() <= (220.f * 220.f);
    };

    int score = 0;
    if (valid(bone_layout::slot::pelvis)) score += 2;
    if (valid(bone_layout::slot::spine)) score += 1;
    if (valid(bone_layout::slot::chest)) score += 2;
    if (valid(bone_layout::slot::neck)) score += 1;
    if (valid(bone_layout::slot::head)) score += 2;
    if (valid(bone_layout::slot::lShoulder)) score += 1;
    if (valid(bone_layout::slot::rShoulder)) score += 1;
    if (valid(bone_layout::slot::lHip)) score += 1;
    if (valid(bone_layout::slot::rHip)) score += 1;
    if (valid(bone_layout::slot::lKnee)) score += 1;
    if (valid(bone_layout::slot::rKnee)) score += 1;
    if (valid(bone_layout::slot::lFoot)) score += 1;
    if (valid(bone_layout::slot::rFoot)) score += 1;

    if (valid(bone_layout::slot::pelvis) && valid(bone_layout::slot::chest) && valid(bone_layout::slot::head)) {
        const Vec3& pelvis = bones[bone_layout::slot::pelvis];
        const Vec3& chest = bones[bone_layout::slot::chest];
        const Vec3& head = bones[bone_layout::slot::head];
        if (pelvis.z < chest.z && chest.z < head.z)
            score += 4;
    }

    if (valid(bone_layout::slot::head)) {
        const Vec3& head = bones[bone_layout::slot::head];
        Vec3 d = head - headPos;
        if (d.lengthSq() < (20.f * 20.f))
            score += 2;
    }

    if (valid(bone_layout::slot::lShoulder) && valid(bone_layout::slot::rShoulder)) {
        const float shoulderSep = horizDistSq(bones[bone_layout::slot::lShoulder], bones[bone_layout::slot::rShoulder]);
        if (shoulderSep > (4.f * 4.f) && shoulderSep < (48.f * 48.f))
            score += 2;
    }

    if (valid(bone_layout::slot::lHip) && valid(bone_layout::slot::rHip)) {
        const float hipSep = horizDistSq(bones[bone_layout::slot::lHip], bones[bone_layout::slot::rHip]);
        if (hipSep > (2.f * 2.f) && hipSep < (40.f * 40.f))
            score += 1;
    }

    auto legScore = [&](int hip, int knee, int foot) {
        if (valid(hip) && valid(knee) && bones[knee].z < bones[hip].z)
            ++score;
        if (valid(knee) && valid(foot) && bones[foot].z < bones[knee].z)
            ++score;
    };
    legScore(bone_layout::slot::lHip, bone_layout::slot::lKnee, bone_layout::slot::lFoot);
    legScore(bone_layout::slot::rHip, bone_layout::slot::rKnee, bone_layout::slot::rFoot);

    return score;
}

float dot3(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 cross3(const Vec3& a, const Vec3& b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

Vec3 normalize3(const Vec3& v) {
    float len = v.length();
    if (len <= 1.0e-6f)
        return {};
    return v * (1.0f / len);
}

float normalizeThrowStrength(float throwStrength) {
    return (std::fabs(throwStrength - 0.5f) < 0.1f) ? 0.5f : throwStrength;
}

float grenadeThrowBaseVelocity(GrenadeType type) {
    return (type == GrenadeType::Molotov) ? 700.f : 750.f;
}

Vec3 grenadeForwardFromView(const Vec3& viewAngles) {
    constexpr float kPi = 3.14159265f;
    constexpr float kDeg2Rad = kPi / 180.f;

    float pitchDeg = viewAngles.x;
    if (pitchDeg < -89.f) pitchDeg += 360.f;
    else if (pitchDeg > 89.f) pitchDeg -= 360.f;
    pitchDeg -= (90.f - std::fabs(pitchDeg)) * 10.f / 90.f;

    float pitch = pitchDeg * kDeg2Rad;
    float yaw   = viewAngles.y * kDeg2Rad;
    float cosPitch = std::cos(pitch);
    return {
        cosPitch * std::cos(yaw),
        cosPitch * std::sin(yaw),
        -std::sin(pitch)
    };
}

void applySteepBounceDampener(Vec3& velocity, const Vec3& normal) {
    if (normal.z <= kSteepBounceNormalZ)
        return;

    float speedSq = velocity.lengthSq();
    if (speedSq <= kSteepBounceSpeedSq)
        return;

    float speed = std::sqrt(speedSq);
    if (speed <= 1.0e-6f)
        return;

    float alignment = dot3(velocity, normal) / speed;
    if (alignment > 0.5f)
        velocity = velocity * (1.5f - alignment);
}

bool sweepGrenadeHull(const BspWorld& world,
                      const Vec3& start,
                      const Vec3& end,
                      float& tHit,
                      Vec3& hitNorm) {
    float bestT = 1.0f;
    Vec3  bestNorm{};
    bool  hit = false;

    auto considerSweep = [&](const Vec3& a, const Vec3& b) {
        float t;
        Vec3 n;
        if (world.sweep(a, b, t, n) && t < bestT) {
            bestT = t;
            bestNorm = n;
            hit = true;
        }
    };

    considerSweep(start, end);

    Vec3 segment = end - start;
    float segLen = segment.length();
    if (segLen <= 1.0e-3f) {
        if (hit) {
            tHit = bestT;
            hitNorm = bestNorm;
        }
        return hit;
    }

    Vec3 dir = segment * (1.0f / segLen);
    Vec3 helper = (std::fabs(dir.z) < 0.9f) ? Vec3{ 0.f, 0.f, 1.f } : Vec3{ 1.f, 0.f, 0.f };
    Vec3 perp1 = normalize3(cross3(dir, helper));
    if (perp1.lengthSq() <= 1.0e-6f) {
        if (hit) {
            tHit = bestT;
            hitNorm = bestNorm;
        }
        return hit;
    }
    Vec3 perp2 = normalize3(cross3(dir, perp1));

    const Vec3 offsets[] = {
        perp1 * kGrenadeHullRadius,
        perp1 * -kGrenadeHullRadius,
        perp2 * kGrenadeHullRadius,
        perp2 * -kGrenadeHullRadius,
    };
    for (const Vec3& offset : offsets)
        considerSweep(start + offset, end + offset);

    if (hit) {
        tHit = bestT;
        hitNorm = bestNorm;
    }
    return hit;
}

bool sweepGrenadePreview(const BspWorld& world,
                         const Vec3& start,
                         const Vec3& end,
                         float& tHit,
                         Vec3& hitNorm) {
    return world.sweep(start, end, tHit, hitNorm);
}

void consumeRemainingTravel(const BspWorld& world,
                            bool useBsp,
                            const Vec3& start,
                            const Vec3& velocity,
                            float remainingFrac,
                            float stepDt,
                            Vec3& pos) {
    if (remainingFrac <= 0.f)
        return;

    Vec3 postEnd = start + velocity * (remainingFrac * stepDt);
    if (useBsp) {
        float t;
        Vec3 n;
        if (sweepGrenadeHull(world, start, postEnd, t, n)) {
            pos = start + (postEnd - start) * t;
            return;
        }
    }
    pos = postEnd;
}

void consumeRemainingTravelPreview(const BspWorld& world,
                                   bool useBsp,
                                   const Vec3& start,
                                   const Vec3& velocity,
                                   float remainingFrac,
                                   float stepDt,
                                   Vec3& pos) {
    if (remainingFrac <= 0.f)
        return;

    Vec3 postEnd = start + velocity * (remainingFrac * stepDt);
    if (useBsp) {
        float t;
        Vec3 n;
        if (sweepGrenadePreview(world, start, postEnd, t, n)) {
            pos = start + (postEnd - start) * t;
            return;
        }
    }
    pos = postEnd;
}
}

bool EntityManager::init(const Process& proc) {
    // â”€â”€ Detect CS2 install path from Steam registry â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    m_cs2Path.clear();
    {
        // Steam stores its install path in two registry locations.
        static const char* kRegKeys[] = {
            "SOFTWARE\\WOW6432Node\\Valve\\Steam",  // 64-bit Windows
            "SOFTWARE\\Valve\\Steam",               // 32-bit fallback
        };
        for (auto* key : kRegKeys) {
            HKEY hk{};
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &hk) == ERROR_SUCCESS) {
                char buf[512]{};
                DWORD sz = sizeof(buf);
                if (RegQueryValueExA(hk, "InstallPath", nullptr, nullptr,
                                     reinterpret_cast<LPBYTE>(buf), &sz) == ERROR_SUCCESS) {
                    m_cs2Path = std::string(buf) +
                        "\\steamapps\\common\\Counter-Strike Global Offensive";
                }
                RegCloseKey(hk);
                if (!m_cs2Path.empty()) break;
            }
        }
        if (m_cs2Path.empty()) {
            HKEY hk{};
            if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", 0, KEY_READ, &hk) == ERROR_SUCCESS) {
                char buf[512]{};
                DWORD sz = sizeof(buf);
                if (RegQueryValueExA(hk, "SteamPath", nullptr, nullptr,
                                     reinterpret_cast<LPBYTE>(buf), &sz) == ERROR_SUCCESS) {
                    std::string steamPath = buf;
                    if (!steamPath.empty() && (steamPath.back() == '\\' || steamPath.back() == '/'))
                        steamPath.pop_back();
                    m_cs2Path = steamPath +
                        "\\steamapps\\common\\Counter-Strike Global Offensive";
                }
                RegCloseKey(hk);
            }
        }
        if (!m_cs2Path.empty())
            std::cout << "[EntityManager] CS2 path: " << m_cs2Path << '\n';
        else {
            m_cs2Path = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\"
                        "Counter-Strike Global Offensive";
            std::cerr << "[EntityManager] Steam registry not found, using default: "
                      << m_cs2Path << '\n';
        }
    }
    m_clientBase = proc.getModuleBase(OBFW("\xC8\xC7\xC2\xCE\xC5\xDF\x85\xCF\xC7\xC7", 0xAB));
    for (int attempt = 0; !m_clientBase && attempt < 15; ++attempt) {
        std::cout << "[EntityManager] Waiting for client.dll to load...\n";
        std::this_thread::sleep_for(std::chrono::seconds(2));
        m_clientBase = proc.getModuleBase(OBFW("\xC8\xC7\xC2\xCE\xC5\xDF\x85\xCF\xC7\xC7", 0xAB));
    }
    if (!m_clientBase) {
        std::cerr << "[EntityManager] client.dll not found!\n";
        return false;
    }
    std::cout << "[EntityManager] client.dll @ 0x"
              << std::hex << m_clientBase << std::dec << '\n';

    m_engine2Base = proc.getModuleBase(OBFW("\xCE\xC5\xCC\xC2\xC5\xCE\x99\x85\xCF\xC7\xC7", 0xAB));
    if (m_engine2Base)
        std::cout << "[EntityManager] engine2.dll @ 0x"
                  << std::hex << m_engine2Base << std::dec << '\n';
    else
        std::cerr << "[EntityManager] engine2.dll not found â€” BSP collision disabled\n";

    offsets::resolveRuntime(proc, m_clientBase);
    if (m_engine2Base)
        offsets::resolveEngine2Runtime(proc, m_engine2Base);

    m_frames[0] = std::make_shared<Snapshot>();
    m_frames[1] = std::make_shared<Snapshot>();
    m_readSlot.store(0, std::memory_order_release);

    return true;
}

void EntityManager::update(Process& proc) {
    auto isLikelyPtr = [](std::uintptr_t p) -> bool {
        return p > 0x10000ull && p < 0x00007FFFFFFFFFFFull;
    };

    // â”€â”€ Per-section timers â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto tNow = std::chrono::steady_clock::now();
    auto tMark = tNow;
    auto mark = [&](const char* /*name*/) {
        tMark = std::chrono::steady_clock::now();
    };

    // â”€â”€ 1. Read the view matrix into a local copy â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    ViewMatrix tmpVm{};
    mem::readArray(proc,
                   m_clientBase + offsets::client::dwViewMatrix,
                   &tmpVm.m[0][0], 16);

    // â”€â”€ 2. Resolve local player's team â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto localController = mem::read<std::uintptr_t>(
        proc, m_clientBase + offsets::client::dwLocalPlayerController);

    auto localPawnDirect = mem::read<std::uintptr_t>(
        proc, m_clientBase + offsets::client::dwLocalPlayerPawn);

    auto entityList = mem::read<std::uintptr_t>(
        proc, m_clientBase + offsets::client::dwEntityList);

    // Player slots 1-64 all live in chunk 0 â€” read once and reuse.
    auto chunk0 = mem::read<std::uintptr_t>(proc, entityList + 0x10);
    if (!isLikelyPtr(chunk0)) chunk0 = 0;

    auto resolveHandleEntity = [&](std::uint32_t handle) -> std::uintptr_t {
        if (!handle || handle == 0xFFFFFFFFu || !isLikelyPtr(entityList))
            return 0;

        auto listEntry = mem::read<std::uintptr_t>(
            proc, entityList + 0x10 + 0x8 * ((handle & 0x7FFF) >> 9));
        if (!isLikelyPtr(listEntry))
            return 0;

        auto entity = mem::read<std::uintptr_t>(
            proc, listEntry + 0x70 * (handle & 0x1FF));
        return isLikelyPtr(entity) ? entity : 0;
    };

    std::uint32_t localPawnHandle = 0;
    std::uintptr_t localPawn = isLikelyPtr(localPawnDirect) ? localPawnDirect : 0;
    int tmpLocalPing = -1;
    if (isLikelyPtr(localController)) {
        localPawnHandle = mem::read<std::uint32_t>(
            proc, localController + netvars::controller::m_hPlayerPawn);
        auto handlePawn = resolveHandleEntity(localPawnHandle);
        if (isLikelyPtr(handlePawn))
            localPawn = handlePawn;

        const auto ping = mem::read<std::uint32_t>(
            proc, localController + netvars::controller::m_iPing);
        if (ping <= 999u)
            tmpLocalPing = static_cast<int>(ping);
    }

    const bool armorInfoNeeded = g_cfg.espEnabled && g_cfg.armorEspEnabled
        && (g_cfg.armorVisibleEnabled || g_cfg.armorOccludedEnabled);
    const bool webRadarActive = g_cfg.webRadarEnabled;
    const bool weaponInfoNeeded = (g_cfg.espEnabled && g_cfg.weaponEspEnabled
        && (g_cfg.weaponVisibleEnabled || g_cfg.weaponOccludedEnabled))
        || webRadarActive;
    const bool ammoInfoNeeded = g_cfg.espEnabled && g_cfg.ammoEspEnabled
        && (g_cfg.ammoVisibleEnabled || g_cfg.ammoOccludedEnabled);
    const bool flagsInfoNeeded = g_cfg.espEnabled && g_cfg.flagsEspEnabled
        && (g_cfg.flagsVisibleEnabled || g_cfg.flagsOccludedEnabled);
    const bool needInfoEsp = g_cfg.espEnabled
        && (g_cfg.nameEspEnabled || armorInfoNeeded || weaponInfoNeeded || ammoInfoNeeded || flagsInfoNeeded);
    const bool needPlayerEsp = g_cfg.espEnabled
        && (g_cfg.boxEnabled || g_cfg.boxOccluded
            || g_cfg.hpBarEnabled || g_cfg.hpBarOccluded
            || g_cfg.skeletonEnabled || g_cfg.skeletonOccluded
            || g_cfg.chamsEnabled || g_cfg.chamsOccluded
            || needInfoEsp);
    const bool needGrenadeEsp = g_cfg.espEnabled && g_cfg.grenadeEnabled;
    const bool needGrenadeSim = needGrenadeEsp || g_cfg.grenadeTrajectory;
    const bool needBombStatus = g_cfg.bombTimerEnabled;
    const bool needSpectatorList = g_cfg.spectatorListEnabled;
    const bool needSoundEsp = g_cfg.soundEspEnabled;
    const bool needGrenadeHelper = g_cfg.grenadeHelperEnabled;
    const bool needRemotePlayers = needPlayerEsp || g_cfg.aimAssistEnabled || needSoundEsp;
    const bool needPlayerScout = true; // Player Info tab / Leetify roster (independent of ESP)
    const bool needPlayerSnapshot = needRemotePlayers || needPlayerScout || needGrenadeSim
        || needBombStatus || needSpectatorList || needSoundEsp;
    const bool needPlayerVelocity = needPlayerEsp || needSoundEsp;
    const bool needPlayerBones = g_cfg.skeletonEnabled
        || g_cfg.skeletonOccluded
        || g_cfg.chamsEnabled
        || g_cfg.chamsOccluded
        || g_cfg.aimAssistEnabled;
    const bool needChamsPartVis = (g_cfg.chamsEnabled || g_cfg.chamsOccluded)
        && g_cfg.visibilityCheckEnabled;
    // Pre-throw simulation should not depend on full ESP enablement; it is used
    // by both in-game overlay and web radar trajectory rendering.
    const bool needPreThrow = g_cfg.grenadeTrajectory;

    int tmpLocalTeam = 0;
    if (isLikelyPtr(localPawn))
        tmpLocalTeam = mem::read<int>(proc, localPawn + netvars::pawn::m_iTeamNum);

    Vec3 tmpLocalEye{};
    Vec3 tmpLocalOrigin{};
    Vec3 tmpLocalViewAngles{};
    bool hasLocalEye = false;
    bool hasLocalPlayer = false;
    if (isLikelyPtr(localPawn)) {
        Vec3 localOrigin{};
        auto localSceneNode = mem::read<std::uintptr_t>(
            proc, localPawn + netvars::pawn::m_pGameSceneNode);
        if (isLikelyPtr(localSceneNode)) {
            localOrigin = mem::read<Vec3>(
                proc, localSceneNode + netvars::scene_node::m_vecAbsOrigin);
        } else {
            localOrigin = mem::read<Vec3>(proc, localPawn + netvars::pawn::m_vOldOrigin);
        }
        Vec3 localViewOffset = mem::read<Vec3>(
            proc, localPawn + netvars::pawn::m_vecViewOffset);
        tmpLocalEye = localOrigin + localViewOffset;
        tmpLocalOrigin = localOrigin;
        hasLocalEye = isFiniteVec3(tmpLocalEye);
        hasLocalPlayer = isFiniteVec3(localOrigin);
        if (hasLocalPlayer || needGrenadeHelper) {
            const Vec2 eyeAng = mem::read<Vec2>(proc, localPawn + netvars::pawn::m_angEyeAngles);
            tmpLocalViewAngles = Vec3{ eyeAng.x, eyeAng.y, 0.f };
        }
    }

    // â”€â”€ 3. Enumerate players â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    std::array<PlayerData, cfg::kMaxPlayers> tmpPlayers{};
    std::array<SpectatorData, EntityManager::kMaxSpectators> tmpSpectators{};
    int playerIdx = 0;

    auto readPlayerName = [&](std::uintptr_t controller) {
        auto decodeAscii = [](const char* src, std::size_t len) {
            std::string out;
            out.reserve(len);
            for (std::size_t i = 0; i < len && src[i] != '\0'; ++i)
                out.push_back(src[i]);
            return out;
        };

        auto decodeUtf16Le = [](const char* src, std::size_t len) {
            std::string out;
            const std::size_t wcharCount = len / 2;
            out.reserve(wcharCount);
            for (std::size_t i = 0; i < wcharCount; ++i) {
                const unsigned char lo = static_cast<unsigned char>(src[i * 2 + 0]);
                const unsigned char hi = static_cast<unsigned char>(src[i * 2 + 1]);
                const wchar_t wc = static_cast<wchar_t>((hi << 8) | lo);
                if (wc == 0)
                    break;
                if (wc >= 32 && wc <= 126)
                    out.push_back(static_cast<char>(wc));
                else if (wc == L'_' || wc == L'-' || wc == L'.')
                    out.push_back(static_cast<char>(wc));
                else
                    out.push_back(' ');
            }
            return out;
        };

        auto sanitizeCandidate = [](std::string name) {
            for (char& ch : name) {
                unsigned char uch = static_cast<unsigned char>(ch);
                if (uch < 32 || uch == 127)
                    ch = ' ';
            }

            std::string compact;
            compact.reserve(name.size());
            bool prevSpace = true;
            int alphaNumCount = 0;
            int letterCount = 0;
            for (char ch : name) {
                unsigned char uch = static_cast<unsigned char>(ch);
                if (std::isspace(uch)) {
                    if (!prevSpace)
                        compact.push_back(' ');
                    prevSpace = true;
                    continue;
                }

                if (std::isalnum(uch)) {
                    compact.push_back(ch);
                    ++alphaNumCount;
                    if (std::isalpha(uch))
                        ++letterCount;
                    prevSpace = false;
                    continue;
                }

                if (ch == '_' || ch == '-' || ch == '.') {
                    compact.push_back(' ');
                    prevSpace = true;
                }
            }

            while (!compact.empty() && compact.back() == ' ')
                compact.pop_back();
            while (!compact.empty() && compact.front() == ' ')
                compact.erase(compact.begin());

            if (_stricmp(compact.c_str(), "unknown") == 0)
                compact.clear();

            int score = 0;
            score += static_cast<int>(compact.size());
            if (letterCount > 0) score += 16;
            if (alphaNumCount >= 3) score += 8;
            if (compact.size() <= 2) score -= 24;
            if (letterCount == 0) score -= 12;
            if (alphaNumCount <= 1) score -= 12;

            return std::pair<std::string, int>{ compact, score };
        };

        char rawSanitized[128]{};
        char rawLegacy[128]{};
        mem::readRaw(proc, controller + netvars::controller::m_sSanitizedPlayerName,
                     rawSanitized, sizeof(rawSanitized) - 1);
        mem::readRaw(proc, controller + netvars::controller::m_iszPlayerName,
                     rawLegacy, sizeof(rawLegacy) - 1);
        rawSanitized[sizeof(rawSanitized) - 1] = '\0';
        rawLegacy[sizeof(rawLegacy) - 1] = '\0';

        std::string bestName;
        int bestScore = -1000;
        auto consider = [&](const std::string& candidateRaw) {
            auto [candidate, score] = sanitizeCandidate(candidateRaw);
            if (score > bestScore) {
                bestScore = score;
                bestName = candidate;
            }
        };

        consider(decodeAscii(rawSanitized, sizeof(rawSanitized) - 1));
        consider(decodeUtf16Le(rawSanitized, sizeof(rawSanitized) - 1));
        consider(decodeAscii(rawLegacy, sizeof(rawLegacy) - 1));
        consider(decodeUtf16Le(rawLegacy, sizeof(rawLegacy) - 1));

        if (bestName.empty())
            bestName = "Player";
        return bestName;
    };

    struct WeaponSnapshot {
        std::string displayName;
        std::string weaponId;
        int clip = -1;
        int maxClip = -1;
    };

    auto readActiveWeapon = [&](std::uintptr_t pawn) {
        WeaponSnapshot out{};
        if (!isLikelyPtr(pawn) || !isLikelyPtr(entityList))
            return out;

        const auto weapSvcPtr = pawn_services::readWeaponServices(proc, pawn);
        if (!isLikelyPtr(weapSvcPtr))
            return out;

        const auto weapHandle = mem::read<std::uint32_t>(
            proc, weapSvcPtr + netvars::weapon_services::m_hActiveWeapon);
        if (!weapHandle || weapHandle == 0xFFFFFFFFu)
            return out;

        auto weapChunk = chunk0;
        if (!weapChunk || (weapHandle & 0x7FFF) >> 9 != 0) {
            weapChunk = mem::read<std::uintptr_t>(
                proc, entityList + 0x10 + 0x8 * ((weapHandle & 0x7FFF) >> 9));
        }
        if (!isLikelyPtr(weapChunk))
            return out;

        const std::uintptr_t weapIdentity = weapChunk + std::uintptr_t(0x70) * (weapHandle & 0x1FF);
        const auto weaponEntity = mem::read<std::uintptr_t>(proc, weapIdentity);
        if (isLikelyPtr(weaponEntity)) {
            out.clip = mem::read<int>(proc, weaponEntity + netvars::weapon::m_iClip1);
            if (out.clip < 0 || out.clip > 500)
                out.clip = -1;
        }

        const auto namePtr = mem::read<std::uintptr_t>(
            proc, weapIdentity + netvars::grenade::m_designerName);
        if (!isLikelyPtr(namePtr))
            return out;

        char rawName[64]{};
        mem::readArray(proc, namePtr, rawName, static_cast<std::uint32_t>(sizeof(rawName) - 1));
        rawName[sizeof(rawName) - 1] = '\0';

        std::string name(rawName);
        out.weaponId = name;

        auto inferMaxClip = [](const std::string& id) -> int {
            if (id.find("awp") != std::string::npos) return 10;
            if (id.find("ssg08") != std::string::npos) return 10;
            if (id.find("g3sg1") != std::string::npos) return 20;
            if (id.find("scar20") != std::string::npos) return 20;
            if (id.find("deagle") != std::string::npos) return 7;
            if (id.find("revolver") != std::string::npos) return 8;
            if (id.find("glock") != std::string::npos) return 20;
            if (id.find("hkp2000") != std::string::npos || id.find("usp") != std::string::npos) return 13;
            if (id.find("p250") != std::string::npos || id.find("fiveseven") != std::string::npos) return 13;
            if (id.find("elite") != std::string::npos) return 30;
            if (id.find("tec9") != std::string::npos || id.find("cz75") != std::string::npos) return 12;
            if (id.find("ak47") != std::string::npos) return 30;
            if (id.find("m4a1") != std::string::npos) return 30;
            if (id.find("aug") != std::string::npos || id.find("sg556") != std::string::npos) return 30;
            if (id.find("famas") != std::string::npos || id.find("galilar") != std::string::npos) return 25;
            if (id.find("mp9") != std::string::npos || id.find("mac10") != std::string::npos) return 30;
            if (id.find("mp7") != std::string::npos || id.find("mp5") != std::string::npos || id.find("ump45") != std::string::npos) return 30;
            if (id.find("p90") != std::string::npos || id.find("bizon") != std::string::npos) return 50;
            if (id.find("nova") != std::string::npos || id.find("xm1014") != std::string::npos || id.find("mag7") != std::string::npos || id.find("sawedoff") != std::string::npos) return 8;
            if (id.find("m249") != std::string::npos) return 100;
            if (id.find("negev") != std::string::npos) return 150;
            if (id.find("knife") != std::string::npos || id.find("grenade") != std::string::npos || id.find("c4") != std::string::npos || id.find("taser") != std::string::npos)
                return 1;
            return 30;
        };
        out.maxClip = inferMaxClip(out.weaponId);

        if (name.rfind("weapon_", 0) == 0)
            name.erase(0, 7);
        for (char& ch : name)
            if (ch == '_') ch = ' ';
        for (char& ch : name)
            if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));

        bool newWord = true;
        for (char& ch : name) {
            if (ch == ' ') {
                newWord = true;
                continue;
            }
            if (newWord && ch >= 'a' && ch <= 'z')
                ch = static_cast<char>(ch - ('a' - 'A'));
            newWord = false;
        }

        out.displayName = std::move(name);
        return out;
    };

    auto findControllerByPawnHandle = [&](std::uint32_t targetPawnHandle) -> std::uintptr_t {
        if (!targetPawnHandle || targetPawnHandle == 0xFFFFFFFFu || !isLikelyPtr(entityList))
            return 0;
        if (targetPawnHandle == localPawnHandle && isLikelyPtr(localController))
            return localController;

        auto ch = chunk0;
        for (int i = 1; i <= 64; ++i) {
            auto listEntry = ch;
            if (!listEntry) {
                listEntry = mem::read<std::uintptr_t>(
                    proc, entityList + 0x10 + 0x8 * ((i & 0x7FFF) >> 9));
                if (!isLikelyPtr(listEntry))
                    continue;
            }

            auto identityBase = listEntry + 0x70 * (i & 0x1FF);
            auto controller = mem::read<std::uintptr_t>(proc, identityBase);
            if (!isLikelyPtr(controller))
                continue;

            auto pawnHandle = mem::read<std::uint32_t>(
                proc, controller + netvars::controller::m_hPlayerPawn);
            if (pawnHandle == targetPawnHandle)
                return controller;
        }
        return 0;
    };

    auto fillLocalPlayerSnapshot = [&]() {
        if ((!needGrenadeEsp && !needBombStatus && !needPreThrow) || playerIdx >= cfg::kMaxPlayers || !isLikelyPtr(localPawn))
            return;

        PlayerData& p = tmpPlayers[playerIdx++];
        p.isValid = true;
        p.isAlive = true;
        p.isLocalPlayer = true;
        p.teamNum = tmpLocalTeam;
        p.pawn = localPawn;
        p.health = mem::read<int>(proc, localPawn + netvars::pawn::m_iHealth);
        p.armor = mem::read<int>(proc, localPawn + netvars::pawn::m_ArmorValue);

        auto sceneNode = mem::read<std::uintptr_t>(
            proc, localPawn + netvars::pawn::m_pGameSceneNode);
        if (isLikelyPtr(sceneNode)) {
            p.origin = mem::read<Vec3>(
                proc, sceneNode + netvars::scene_node::m_vecAbsOrigin);
            p.isDormant = mem::read<bool>(
                proc, sceneNode + netvars::scene_node::m_bDormant);
        } else {
            p.origin = mem::read<Vec3>(proc, localPawn + netvars::pawn::m_vOldOrigin);
        }

        Vec3 viewOffset = mem::read<Vec3>(
            proc, localPawn + netvars::pawn::m_vecViewOffset);
        p.headPos = p.origin + viewOffset;
        if (needPlayerVelocity)
            p.velocity = mem::read<Vec3>(proc, localPawn + netvars::grenade::m_vecVelocity);
        if (webRadarActive) {
            const WeaponSnapshot ws = readActiveWeapon(localPawn);
            p.weaponName = ws.displayName;
            p.weaponId = ws.weaponId;
            p.ammoClip = ws.clip;
            p.ammoMaxClip = ws.maxClip;
        }
        p.screenRelevant = tmpVm.isOnScreen(p.origin) || tmpVm.isOnScreen(p.headPos);
    };

    if ((needRemotePlayers || needPlayerScout) && entityList) {
        const bool needNameEsp = (g_cfg.espEnabled && g_cfg.nameEspEnabled) || needPlayerScout;
        const bool needWeaponEsp = weaponInfoNeeded;
        const bool needAmmoEsp = ammoInfoNeeded;
        const bool needFlagsEsp = flagsInfoNeeded;

        for (int i = 1; i <= 64 && playerIdx < cfg::kMaxPlayers; ++i) {
            auto listEntry = chunk0;
            if (!listEntry) {
                listEntry = mem::read<std::uintptr_t>(
                    proc, entityList + 0x10 + 0x8 * ((i & 0x7FFF) >> 9));
                if (!isLikelyPtr(listEntry)) continue;
            }

            auto identityBase = listEntry + 0x70 * (i & 0x1FF);
            auto controller   = mem::read<std::uintptr_t>(proc, identityBase);
            if (!isLikelyPtr(controller)) continue;

            bool pawnAlive = mem::read<bool>(
                proc, controller + netvars::controller::m_bPawnIsAlive);
            if (!pawnAlive) continue;

            auto pawnHandle = mem::read<std::uint32_t>(
                proc, controller + netvars::controller::m_hPlayerPawn);
            if (!pawnHandle || pawnHandle == 0xFFFFFFFFu) continue;

            auto pawnEntry = mem::read<std::uintptr_t>(
                proc, entityList + 0x10 + 0x8 * ((pawnHandle & 0x7FFF) >> 9));
            if (!isLikelyPtr(pawnEntry)) continue;

            auto pawn = mem::read<std::uintptr_t>(
                proc, pawnEntry + 0x70 * (pawnHandle & 0x1FF));
            if (!isLikelyPtr(pawn)) continue;

            int health = mem::read<int>(proc, pawn + netvars::pawn::m_iHealth);
            if (health <= 0 || health > 100) continue;

            PlayerData& p = tmpPlayers[playerIdx++];
            p.health   = health;
            p.armor    = mem::read<int>(proc, pawn + netvars::pawn::m_ArmorValue);
            p.teamNum  = mem::read<int>(proc, pawn + netvars::pawn::m_iTeamNum);
            p.pawn     = pawn;
            const Vec2 eyeAng = mem::read<Vec2>(proc, pawn + netvars::pawn::m_angEyeAngles);
            p.eyePitch = eyeAng.x;
            p.eyeYaw   = eyeAng.y;

            auto sceneNode = mem::read<std::uintptr_t>(
                proc, pawn + netvars::pawn::m_pGameSceneNode);
            if (isLikelyPtr(sceneNode)) {
                p.origin    = mem::read<Vec3>(
                    proc, sceneNode + netvars::scene_node::m_vecAbsOrigin);
                p.isDormant = mem::read<bool>(
                    proc, sceneNode + netvars::scene_node::m_bDormant);
            } else {
                p.origin = mem::read<Vec3>(proc, pawn + netvars::pawn::m_vOldOrigin);
            }

            Vec3 viewOffset = mem::read<Vec3>(
                proc, pawn + netvars::pawn::m_vecViewOffset);
            p.headPos = p.origin + viewOffset;
            p.screenRelevant = tmpVm.isOnScreen(p.origin) || tmpVm.isOnScreen(p.headPos);
            p.isLocalPlayer = (controller == localController);
            p.isAlive       = true;
            p.isValid       = true;
            const std::uint64_t rawSteamId = mem::read<std::uint64_t>(proc, controller + netvars::controller::m_steamID);
            constexpr std::uint64_t kSteam64Base = 76561197960265728ULL;
            std::uint64_t steamId = rawSteamId;
            if (steamId > 0 && steamId < kSteam64Base && steamId < 10000000000ULL)
                steamId += kSteam64Base;
            p.steamId = steamId;
            p.isBot = (rawSteamId == 0ull);
            p.hasDefuseKit = mem::read<std::uint8_t>(proc, controller + netvars::controller::m_bPawnHasDefuser) != 0;
            if (needPlayerVelocity)
                p.velocity = mem::read<Vec3>(proc, pawn + netvars::grenade::m_vecVelocity);
            if (needSoundEsp)
                p.shotsFired = mem::read<int>(proc, pawn + netvars::pawn::m_iShotsFired);
            if (needNameEsp)
                p.name = readPlayerName(controller);
            if (needWeaponEsp || needAmmoEsp) {
                const WeaponSnapshot ws = readActiveWeapon(pawn);
                p.weaponName = ws.displayName;
                p.weaponId = ws.weaponId;
                p.ammoClip = ws.clip;
                p.ammoMaxClip = ws.maxClip;
            }
            if (needFlagsEsp) {
                p.isScoped = mem::read<std::uint8_t>(proc, pawn + netvars::pawn::m_bIsScoped) != 0;
                p.isDefusing = mem::read<std::uint8_t>(proc, pawn + netvars::pawn::m_bIsDefusing) != 0;
                const float flashDuration = mem::read<float>(proc, pawn + netvars::pawn::m_flFlashDuration);
                const float flashAlpha = mem::read<float>(proc, pawn + netvars::pawn::m_flFlashMaxAlpha);
                p.isFlashed = (flashDuration > 0.05f) || (flashAlpha > 1.0f);
            }

            // â”€â”€ Bone world positions â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
            // sceneNode + 0x1D0  â†’  pointer to CS2 bone world-position cache.
            // Strategy:
            //   1. Try primary offset 0x1D0 (CModelState+0x80); fall back to
            //      0x1C8 (CModelState+0x78) if the primary gives a bad pointer.
            //   2. First try reading as matrix3x4_t (12 floats=48 bytes per bone,
            //      world position at indices 3,7,11 â€” the last column).
            //   3. If fewer than 4 bone positions land near the player origin,
            //      re-try reading as a plain Vec3 array (3 floats=12 bytes per
            //      bone) â€” some CS2 engine builds store only position in the cache.
            //   4. Accept a partial ReadProcessMemory result so a single
            //      page-boundary miss doesn't silently kill all bones.
            p.bonesValid = false;
            p.boneMatricesValid = false;
            p.boneMatrixOk.fill(false);
            p.skeleton = {};
            const bool needBonesForPlayer = needPlayerBones;
            if (needBonesForPlayer) {
                Cs2Skeleton skel{};
                if (readPlayerSkeleton(proc, pawn, skel))
                    applySkeletonToPlayer(skel, p);
                p.modelKey = readPlayerModelKey(proc, pawn);
            }
        }
    } else if (needPlayerSnapshot) {
        fillLocalPlayerSnapshot();
    }
    mark("player_loop");

    const bool visNeeded = g_cfg.visibilityCheckEnabled || (g_cfg.aimAssistEnabled && g_cfg.aimRequireVisibility);
    if (visNeeded && hasLocalEye) {
        const int visTick = ++m_visibilityTick;
        const bool aimOnlyVis = !g_cfg.visibilityCheckEnabled && (g_cfg.aimAssistEnabled && g_cfg.aimRequireVisibility);
        const int aimVisMode = std::clamp(g_cfg.aimVisibilityMode, 0, 1);
        const int mode = aimOnlyVis ? 0 : std::clamp(g_cfg.visibilityMode, 0, 2);
        int aimOnlyRayBudget = (aimVisMode == 1) ? 140 : 28;
        // Keep visibility checks active at long range to avoid "far players always visible" behavior.
        const float maxDistSq = aimOnlyVis
            ? ((aimVisMode == 1) ? (14000.f * 14000.f) : (8500.f * 8500.f))
            : ((g_cfg.visMaxDistance > 0.f) ? (g_cfg.visMaxDistance * g_cfg.visMaxDistance)
               : ((mode == 0) ? (12000.f * 12000.f) : (20000.f * 20000.f)));
        const float smoothAlpha = aimOnlyVis
            ? ((aimVisMode == 1) ? 0.85f : 1.0f)
            : ((mode == 0) ? 0.55f : ((mode == 1) ? 0.42f : 0.32f));
        const float confThreshold = aimOnlyVis
            ? ((aimVisMode == 1) ? 0.60f : 0.50f)
            : ((mode == 0) ? 0.20f : ((mode == 1) ? 0.34f : 0.50f));

        const int backend = std::clamp(g_cfg.visibilityBackend, 0, 2);
        if (backend != 0 && !m_gameTraceInitAttempted) {
            m_gameTraceInitAttempted = true;
            proc.openExtendedHandle();
            if (!m_gameTrace.init(proc, m_clientBase))
                m_gameTraceInitFailed = true;
        }

        bool useGameTrace = false;
        bool useBspVis = false;
        if (backend == 0) {
            useBspVis = m_bspWorld.isLoaded();
        } else if (m_gameTrace.isReady()) {
            useGameTrace = true;
        } else {
            useBspVis = m_bspWorld.isLoaded();
        }

        if (useGameTrace) {
            int traceBudget = 14;
            if (mode == 1) traceBudget = 22;
            else if (mode == 2) traceBudget = 28;
            if (aimOnlyVis && aimVisMode == 1)
                traceBudget = 26;
            m_gameTrace.beginFrame(traceBudget);
        }

        // Guard: check if local eye is inside wall geometry once per frame.
        bool eyeInsideWall = false;
        if (m_bspWorld.isLoaded() && hasLocalEye) {
            constexpr float kGuardLen = 30.f;
            Vec3 viewDir = { tmpVm.m[2][0], tmpVm.m[2][1], tmpVm.m[2][2] };
            float vdLen = viewDir.length();
            if (vdLen > 0.001f) {
                viewDir = viewDir * (1.f / vdLen);
                Vec3 behind = tmpLocalEye - viewDir * kGuardLen;
                float guardHit = 1.f;
                Vec3 guardNorm{};
                if (m_bspWorld.sweep(behind, tmpLocalEye, guardHit, guardNorm) && guardHit < 0.1f)
                    eyeInsideWall = true;
            }
        }

        for (std::size_t i = 0; i < tmpPlayers.size(); ++i) {
            auto& p = tmpPlayers[i];
            auto& visPersist = m_visibilityPersist[i];
            if (!p.isValid || !p.isAlive || p.isLocalPlayer || p.isDormant)
                continue;

            if (aimOnlyVis && (p.teamNum == 0 || p.teamNum == tmpLocalTeam))
                continue;

            p.visibilityChecked = false;
            p.isVisible = true;
            p.visibilityConfidence = 1.f;

            const Vec3 toTarget = p.origin - tmpLocalEye;
            const float distSq = toTarget.lengthSq();
            if (!isFiniteVec3(toTarget) || distSq > maxDistSq) {
                if (aimOnlyVis) {
                    p.visibilityChecked = true;
                    p.isVisible = false;
                    p.visibilityConfidence = 0.f;
                    visPersist.hasState = false;
                    visPersist.latchedVisible = false;
                    visPersist.visibleStreak = 0;
                    visPersist.occludedStreak = 0;
                }
                if (visPersist.hasState) {
                    p.visibilityChecked = true;
                    p.isVisible = visPersist.latchedVisible;
                    p.visibilityConfidence = visPersist.smoothedConfidence;
                }
                continue;
            }

            // Round-robin evaluation: each player is assigned to a subset of frames
            // via (visTick % evalStride == playerIdx % evalStride).  This distributes
            // the sweep workload evenly across frames without an arbitrary time budget
            // that abandons players mid-frame (the old budget cap caused choppiness).
            int evalStride = 1;
            if (g_cfg.visEvalBase > 0) {
                evalStride = std::clamp(g_cfg.visEvalBase, 1, 5);
            } else {
                // Stagger based on distance: close=every frame, far=every 2nd/3rd.
                if (distSq > (5000.f * 5000.f)) evalStride = 4;
                else if (distSq > (2500.f * 2500.f)) evalStride = 2;
            }

            if (!p.screenRelevant && !(g_cfg.aimAssistEnabled && g_cfg.aimRequireVisibility))
                evalStride = (std::max)(evalStride, 6);

            if (aimOnlyVis) {
                if (aimVisMode == 1) {
                    if (distSq <= (2600.f * 2600.f)) evalStride = 1;
                    else if (distSq <= (7000.f * 7000.f)) evalStride = 2;
                    else evalStride = 3;
                    if (!p.screenRelevant)
                        evalStride = (std::max)(evalStride, 3);
                } else {
                    if (distSq <= (1700.f * 1700.f)) evalStride = 1;
                    else if (distSq <= (3600.f * 3600.f)) evalStride = 2;
                    else evalStride = 3;
                    if (!p.screenRelevant)
                        evalStride = (std::max)(evalStride, 5);
                }
            }

            if (mode == 2)
                evalStride = (std::max)(1, evalStride - 1);

            if (p.screenRelevant)
                evalStride = 1;

            // Round-robin: only evaluate this player on frames where the tick aligns.
            // Always re-check players that are currently marked occluded so peeking enemies
            // flip to visible immediately instead of waiting for their slot.
            if (visPersist.hasState && visPersist.latchedVisible) {
                const int playerSlot = static_cast<int>(i);
                if ((visTick % evalStride) != (playerSlot % evalStride)) {
                    p.visibilityChecked = true;
                    p.isVisible = visPersist.latchedVisible;
                    p.visibilityConfidence = visPersist.smoothedConfidence;
                    continue;
                }
            }

            if (aimOnlyVis && aimOnlyRayBudget <= 0) {
                p.visibilityChecked = true;
                if (visPersist.hasState) {
                    p.isVisible = visPersist.latchedVisible;
                    p.visibilityConfidence = visPersist.smoothedConfidence;
                } else {
                    p.isVisible = false;
                    p.visibilityConfidence = 0.f;
                }
                continue;
            }

            int visibleHits = 0;
            const int minHits = aimOnlyVis
                ? ((aimVisMode == 1) ? 2 : 1)
                : ((mode == 2) ? 2 : 1);

            int sampleCount = 0;
            if (useGameTrace && !aimOnlyVis) {
                // PureLiquid-style: eye â†’ origin first; extra samples only when needed.
                sampleCount = 1;
                if (m_gameTrace.isPlayerVisible(proc, localPawn, p.pawn, tmpLocalEye, p.origin))
                    visibleHits = 1;
                if (mode >= 1 && visibleHits < minHits) {
                    ++sampleCount;
                    if (m_gameTrace.hasLineOfSight(proc, localPawn, p.pawn, tmpLocalEye, p.headPos))
                        visibleHits = 1;
                }
                if (mode >= 2 && visibleHits < minHits) {
                    ++sampleCount;
                    const Vec3 chest = p.origin + Vec3{ 0.f, 0.f, 48.f };
                    if (m_gameTrace.hasLineOfSight(proc, localPawn, p.pawn, tmpLocalEye, chest))
                        visibleHits = 1;
                }
            } else {
                Vec3 samples[4]{};
                if (aimOnlyVis) {
                    samples[0] = p.headPos;
                    samples[1] = p.origin + Vec3{ 0.f, 0.f, 50.f };
                    if (aimVisMode == 1) {
                        samples[2] = p.origin + Vec3{ 0.f, 0.f, 34.f };
                        sampleCount = 3;
                    } else {
                        sampleCount = 2;
                    }
                } else {
                    sampleCount = collectVisibilitySamples(p, mode, samples, 4);
                }
                if (sampleCount <= 0) {
                    if (visPersist.hasState) {
                        p.visibilityChecked = true;
                        p.isVisible = visPersist.latchedVisible;
                        p.visibilityConfidence = visPersist.smoothedConfidence;
                    }
                    continue;
                }

                for (int sampleIdx = 0; sampleIdx < sampleCount; ++sampleIdx) {
                    if (aimOnlyVis)
                        --aimOnlyRayBudget;

                    if (useGameTrace) {
                        if (m_gameTrace.hasLineOfSight(proc, localPawn, p.pawn, tmpLocalEye, samples[sampleIdx]))
                            ++visibleHits;
                    } else if (useBspVis) {
                        if (hasLineOfSight(m_bspWorld, tmpLocalEye, samples[sampleIdx], eyeInsideWall))
                            ++visibleHits;
                    } else {
                        ++visibleHits;
                    }

                    if (visibleHits >= minHits)
                        break;
                    const int remaining = sampleCount - (sampleIdx + 1);
                    if ((visibleHits + remaining) < minHits)
                        break;
                }
            }

            if (sampleCount <= 0) {
                if (visPersist.hasState) {
                    p.visibilityChecked = true;
                    p.isVisible = visPersist.latchedVisible;
                    p.visibilityConfidence = visPersist.smoothedConfidence;
                }
                continue;
            }

            p.visibilityChecked = true;
            const float rawConfidence = static_cast<float>(visibleHits) / static_cast<float>(sampleCount);
            if (!visPersist.hasState)
                visPersist.smoothedConfidence = rawConfidence;
            else if (rawConfidence >= visPersist.smoothedConfidence)
                visPersist.smoothedConfidence += (rawConfidence - visPersist.smoothedConfidence)
                    * (std::max)(smoothAlpha, p.screenRelevant ? 0.92f : 0.75f);
            else
                visPersist.smoothedConfidence += (rawConfidence - visPersist.smoothedConfidence) * smoothAlpha;

            p.visibilityConfidence = visPersist.smoothedConfidence;

            const bool rawVisible = (visibleHits >= minHits)
                                 || (visPersist.smoothedConfidence >= confThreshold);

            if (aimOnlyVis) {
                visPersist.latchedVisible = rawVisible;
                visPersist.hasState = true;
                visPersist.visibleStreak = rawVisible ? 1 : 0;
                visPersist.occludedStreak = rawVisible ? 0 : 1;
                p.isVisible = visPersist.latchedVisible;
                continue;
            }

            const int latchFrames = std::clamp(g_cfg.visibilityLatchFrames, 0, 5);
            if (rawVisible) {
                visPersist.occludedStreak = 0;
                if (visPersist.visibleStreak < (latchFrames + 4))
                    ++visPersist.visibleStreak;
                const int showGate = (latchFrames == 0) ? 1 : (latchFrames + 1);
                if (visPersist.visibleStreak >= showGate)
                    visPersist.latchedVisible = true;
            } else {
                visPersist.visibleStreak = 0;
                if (visPersist.occludedStreak < (latchFrames + 6))
                    ++visPersist.occludedStreak;
                const int occludedGate = (latchFrames == 0) ? 2 : (latchFrames + 2);
                if (visPersist.occludedStreak >= occludedGate)
                    visPersist.latchedVisible = false;
            }

            visPersist.hasState = true;
            p.isVisible = visPersist.latchedVisible;
        }
    } else {
        for (std::size_t i = 0; i < tmpPlayers.size(); ++i) {
            auto& p = tmpPlayers[i];
            auto& visPersist = m_visibilityPersist[i];
            if (!p.isValid || !p.isAlive || p.isLocalPlayer)
                continue;
            visPersist.visibleStreak = 0;
            visPersist.occludedStreak = 0;
            visPersist.nextEvalTick = 0;
            visPersist.latchedVisible = false;
            visPersist.hasState = false;
            visPersist.smoothedConfidence = 1.f;
            p.visibilityChecked = false;
            p.isVisible = true;
            p.visibilityConfidence = 1.f;
        }
    }
    mark("visibility_checks");

    if (needChamsPartVis && hasLocalEye) {
        bool eyeInsideWall = false;
        if (m_bspWorld.isLoaded()) {
            constexpr float kGuardLen = 30.f;
            Vec3 viewDir = { tmpVm.m[2][0], tmpVm.m[2][1], tmpVm.m[2][2] };
            const float vdLen = viewDir.length();
            if (vdLen > 0.001f) {
                viewDir = viewDir * (1.f / vdLen);
                const Vec3 behind = tmpLocalEye - viewDir * kGuardLen;
                float guardHit = 1.f;
                Vec3 guardNorm{};
                if (m_bspWorld.sweep(behind, tmpLocalEye, guardHit, guardNorm) && guardHit < 0.1f)
                    eyeInsideWall = true;
            }
        }

        const int chamsBackend = std::clamp(g_cfg.visibilityBackend, 0, 2);
        if (chamsBackend != 0 && !m_gameTraceInitAttempted) {
            m_gameTraceInitAttempted = true;
            proc.openExtendedHandle();
            if (!m_gameTrace.init(proc, m_clientBase))
                m_gameTraceInitFailed = true;
        }

        bool chamsUseGameTrace = false;
        bool chamsUseBspVis = false;
        if (chamsBackend == 0) {
            chamsUseBspVis = m_bspWorld.isLoaded();
        } else if (m_gameTrace.isReady()) {
            chamsUseGameTrace = true;
        } else {
            chamsUseBspVis = m_bspWorld.isLoaded();
        }

        if (chamsUseGameTrace)
            m_gameTrace.beginFrame(28);

        const bool useBspVis = chamsUseBspVis;
        const int chamsBoneCost = static_cast<int>(sizeof(kChamsBones) / sizeof(kChamsBones[0]))
            + PlayerData::kChamsSegCount;
        int chamsVisBudget = 220;

        for (auto& p : tmpPlayers) {
            if (!p.isValid || !p.isAlive || p.isLocalPlayer)
                continue;
            p.chamsPartVisChecked = false;
        }

        static int s_chamsVisRotor = 0;
        int eligibleCount = 0;
        int eligibleIdx[cfg::kMaxPlayers]{};
        for (int i = 0; i < playerIdx; ++i) {
            auto& p = tmpPlayers[i];
            if (!p.isValid || !p.isAlive || p.isLocalPlayer || p.isDormant)
                continue;
            if (!p.screenRelevant && !p.bonesValid)
                continue;
            eligibleIdx[eligibleCount++] = i;
        }

        int updated = 0;
        for (int pass = 0; pass < eligibleCount && chamsVisBudget >= chamsBoneCost; ++pass) {
            const int pi = eligibleIdx[(s_chamsVisRotor + pass) % eligibleCount];
            auto& p = tmpPlayers[pi];

            updateChamsPartVisibility(p, m_bspWorld, chamsUseGameTrace ? &m_gameTrace : nullptr,
                                      chamsUseGameTrace ? &proc : nullptr, localPawn,
                                      tmpLocalEye, eyeInsideWall, chamsUseGameTrace, useBspVis);
            chamsVisBudget -= chamsBoneCost;
            ++updated;
        }
        if (eligibleCount > 0 && updated > 0)
            s_chamsVisRotor = (s_chamsVisRotor + updated) % eligibleCount;

        for (int i = 0; i < playerIdx; ++i) {
            auto& p = tmpPlayers[i];
            if (!p.isValid || !p.isAlive || p.isLocalPlayer || p.chamsPartVisChecked)
                continue;
            const bool whole = !g_cfg.visibilityCheckEnabled
                || !p.visibilityChecked || p.isVisible;
            fillChamsPartVisibility(p, whole);
        }
    } else {
        for (auto& p : tmpPlayers) {
            if (!p.isValid || !p.isAlive || p.isLocalPlayer)
                continue;
            const bool whole = !g_cfg.visibilityCheckEnabled
                || !p.visibilityChecked || p.isVisible;
            p.chamsPartVisChecked = true;
            for (int i = 0; i < PlayerData::kBoneCount; ++i)
                p.chamsPartVisible[i] = whole;
            for (int i = 0; i < PlayerData::kChamsSegCount; ++i)
                p.chamsSegMidVisible[i] = whole;
        }
    }
    mark("chams_part_vis");

    if (needSpectatorList && entityList) {
        int spectatorIdx = 0;
        auto spChunk = chunk0;
        for (int i = 1; i <= 64 && spectatorIdx < EntityManager::kMaxSpectators; ++i) {
            auto listEntry = spChunk;
            if (!listEntry) {
                listEntry = mem::read<std::uintptr_t>(
                    proc, entityList + 0x10 + 0x8 * ((i & 0x7FFF) >> 9));
                if (!isLikelyPtr(listEntry))
                    continue;
            }

            auto identityBase = listEntry + 0x70 * (i & 0x1FF);
            auto controller = mem::read<std::uintptr_t>(proc, identityBase);
            if (!isLikelyPtr(controller) || controller == localController)
                continue;

            bool pawnAlive = mem::read<bool>(
                proc, controller + netvars::controller::m_bPawnIsAlive);

            auto pawnHandle = mem::read<std::uint32_t>(
                proc, controller + netvars::controller::m_hPlayerPawn);
            if (!pawnHandle || pawnHandle == 0xFFFFFFFFu)
                continue;

            auto pawn = resolveHandleEntity(pawnHandle);
            if (!isLikelyPtr(pawn))
                continue;

            int pawnHealth = mem::read<int>(proc, pawn + netvars::pawn::m_iHealth);
            const bool deadLike = !pawnAlive || pawnHealth <= 0;
            if (!deadLike)
                continue;

            SpectatorData& spectator = tmpSpectators[spectatorIdx++];
            spectator.isValid = true;
            spectator.watchingLocal = false;
            spectator.mode = 0;
            spectator.name = readPlayerName(controller);
            spectator.targetName = "";
            spectator.steamId = mem::read<std::uint64_t>(proc, controller + netvars::controller::m_steamID);
            spectator.isBot = (spectator.steamId == 0ull);
            spectator.teamNum = mem::read<int>(proc, pawn + netvars::pawn::m_iTeamNum);

            auto observerServices = pawn_services::readObserverServices(proc, pawn);

            if (isLikelyPtr(observerServices)) {
                const int observerMode = mem::read<std::uint8_t>(
                    proc, observerServices + netvars::observer_services::m_iObserverMode);
                spectator.mode = (observerMode >= 1 && observerMode <= 6) ? observerMode : 0;

                auto observerTargetHandle = mem::read<std::uint32_t>(
                    proc, observerServices + netvars::observer_services::m_hObserverTarget);
                const bool hasTarget = observerTargetHandle && observerTargetHandle != 0xFFFFFFFFu;
                const bool isRoaming = (observerMode == 4);

                if (isRoaming) {
                    spectator.targetName = "Free roam";
                } else if (hasTarget) {
                    spectator.watchingLocal = (observerTargetHandle == localPawnHandle);
                    auto observerTargetPawn = resolveHandleEntity(observerTargetHandle);
                    if (!spectator.watchingLocal) {
                        spectator.watchingLocal = isLikelyPtr(observerTargetPawn)
                            && isLikelyPtr(localPawn)
                            && observerTargetPawn == localPawn;
                    }

                    if (spectator.watchingLocal) {
                        spectator.targetName = "You";
                    } else {
                        auto targetController = findControllerByPawnHandle(observerTargetHandle);
                        if (!isLikelyPtr(targetController) && isLikelyPtr(observerTargetPawn)) {
                            const auto controllerHandle = pawn_services::readControllerHandle(proc, observerTargetPawn);
                            if (controllerHandle)
                                targetController = resolveHandleEntity(controllerHandle);
                        }
                        spectator.targetName = isLikelyPtr(targetController)
                            ? readPlayerName(targetController)
                            : std::string("Player");
                    }
                } else if (observerMode == 1) {
                    spectator.targetName = "Death cam";
                }
            }
        }
    }

    // â”€â”€ 4. Read grenade projectiles (entity indices 64..highest) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    // Read local player's foot Z now so we can use it as a floor estimate for
    // â”€â”€ 4a. Map / BSP tracking â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    // Detect active map via CGlobalVars + CNetworkGameClient; load collision mesh.
    if (m_clientBase) {
        if (!m_engine2Base) {
            m_engine2Base = proc.getModuleBase(OBFW("\xCE\xC5\xCC\xC2\xC5\xCE\x99\x85\xCF\xC7\xC7", 0xAB));
            if (m_engine2Base)
                offsets::resolveEngine2Runtime(proc, m_engine2Base);
        }

        std::uintptr_t ngcPtr = 0;
        if (m_engine2Base)
            ngcPtr = mem::read<std::uintptr_t>(
                proc, m_engine2Base + offsets::engine2::dwNetworkGameClient);

        // One-time diagnostics so we can tell if the offset/pointer is valid.
        static bool s_scanMsg = false;
        if (!s_scanMsg) {
            s_scanMsg = true;
            std::cout << "[BSP] Map scan: engine2Base=0x" << std::hex << m_engine2Base
                      << "  ngcPtr=0x" << ngcPtr << std::dec
                      << (isLikelyPtr(ngcPtr) ? "  (ptr OK)" : "  (BAD PTR â€” using GlobalVars)") << "\n";
        }

        // Rate-limit map scans. When BSP is loaded, rescan infrequently.
        static int s_scanTick = 0;
        static int s_heavyScanCooldown = 0;
        const bool mapKnown = !m_currentMapName.empty();
        const bool mapReady = mapKnown && m_bspWorld.isLoaded();
        const int scanEvery = (mapReady || mapKnown) ? 3000 : 300;
        if (++s_scanTick % scanEvery == 1) {
            // â”€â”€ lambda: bare map name if buf starts with known prefix â”€â”€â”€â”€â”€â”€â”€â”€â”€
            auto tryMatch = [](const char* buf, int avail) -> std::string {
                struct Pat { const char* pfx; int pfxLen; int nameStart; } pats[] = {
                    { "de_",        3, 0 }, { "cs_",        3, 0 },
                    { "ar_",        3, 0 }, { "dz_",        3, 0 },
                    { "aim_",       4, 0 }, { "awp_",       4, 0 },
                    { "fy_",        3, 0 }, { "1v1_",       4, 0 },
                    { "maps/de_",   8, 5 }, { "maps/cs_",   8, 5 },
                    { "maps/ar_",   8, 5 }, { "maps/dz_",   8, 5 },
                    { "maps/aim_",  9, 5 }, { "maps/awp_",  9, 5 },
                    { "maps\\de_",  9, 6 }, { "maps\\cs_",  9, 6 },
                    { "maps\\aim_", 10, 6 },
                    { nullptr,      0, 0 }
                };
                for (int p = 0; pats[p].pfx; ++p) {
                    if (avail < pats[p].pfxLen + 2) continue;
                    if (std::strncmp(buf, pats[p].pfx, pats[p].pfxLen) != 0) continue;
                    int start = pats[p].nameStart;
                    int slen  = pats[p].pfxLen - start;
                    while (start + slen < avail && slen < 63) {
                        unsigned char c = (unsigned char)buf[start + slen];
                        if (!std::islower(c) && !std::isdigit(c) && c != '_') break;
                        ++slen;
                    }
                    if (slen >= 5) return std::string(buf + start, slen);
                }
                return {};
            };

            auto readMem = [&](std::uintptr_t addr, char* buf, int sz) -> int {
                if (!mem::readRaw(proc, addr, buf, static_cast<std::size_t>(sz)))
                    return 0;
                return sz;
            };

            // CS2 allocates heap at addresses above 2^40 (e.g. 0x2800_0000_0000).
            // The old (p >> 40) == 0 check rejected all of those.  Use the same
            // bounds as isLikelyPtr so we can follow pointers in Pass 2/3.
            auto isHeapPtr = [](std::uintptr_t p) -> bool {
                return p > 0x10000ull && p < 0x00007FFFFFFFFFFFull;
            };

            // Persistent blacklist: map names that have no matching .bsp on disk.
            static std::vector<std::string> s_skipList;

            // tryMatchF: same as tryMatch but skips blacklisted names.
            auto tryMatchF = [&](const char* buf, int avail) -> std::string {
                auto m = tryMatch(buf, avail);
                if (!m.empty())
                    for (auto& bl : s_skipList)
                        if (bl == m) return {};
                return m;
            };

            // BSP existence check helper.
            // Retail CS2 ships maps as .vpk; mods may use raw .bsp.
            auto bspExists = [&](const std::string& name) -> bool {
                char path[520]{};
                snprintf(path, sizeof(path), "%s\\game\\csgo\\maps\\%s.vpk",
                         m_cs2Path.c_str(), name.c_str());
                if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return true;
                snprintf(path, sizeof(path), "%s\\game\\csgo\\maps\\%s.bsp",
                         m_cs2Path.c_str(), name.c_str());
                return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
            };

            // Retry loop: if the first candidate has no BSP, blacklist it and
            // rescan immediately so we find the real map on the same tick.
            std::string found;

            // Pass 0: CGlobalVars.currentMapName â€” stable even when ngc offset drifts.
            {
                const auto globalVars = mem::read<std::uintptr_t>(
                    proc, m_clientBase + offsets::client::dwGlobalVars);
                if (isLikelyPtr(globalVars)) {
                    const auto mapNamePtr = mem::read<std::uintptr_t>(proc, globalVars + 0x180);
                    if (isLikelyPtr(mapNamePtr)) {
                        const std::string raw = mem::readString(proc, mapNamePtr, 128);
                        if (!raw.empty())
                            found = tryMatchF(raw.c_str(), static_cast<int>(raw.size()));
                    }
                }
            }

            for (int attempt = 0; attempt < 10 && found.empty(); ++attempt) {

                if (!isLikelyPtr(ngcPtr))
                    break;

                // â”€â”€ Pass 1: inline scan of first 8 KB of CNetworkGameClient â”€
                {
                    char raw[0x2000]{};
                    const int bread = readMem(ngcPtr, raw, static_cast<int>(sizeof(raw)));
                    if (bread >= 0x10) {
                        for (int off = 0x08; off < (int)bread - 8 && found.empty(); ++off)
                            found = tryMatchF(raw + off, (int)bread - off);

                        // â”€â”€ Pass 2+3: heap pointer hops â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
                        if (found.empty()) {
                            int p1Range = (int)bread < 0x500 ? (int)bread : 0x500;
                            for (int off = 0x08; off + 8 <= p1Range && found.empty(); off += 8) {
                                auto p1 = *reinterpret_cast<const std::uintptr_t*>(raw + off);
                                if (!isHeapPtr(p1)) continue;
                                char buf1[512]{};
                                int  r1 = readMem(p1, buf1, sizeof(buf1));
                                if (r1 < 5) continue;
                                for (int s = 0; s < r1 && found.empty(); ++s)
                                    found = tryMatchF(buf1 + s, r1 - s);
                                if (!found.empty()) break;
                                int p2Range = r1 < 0x80 ? r1 : 0x80;
                                for (int s2 = 0; s2 + 8 <= p2Range && found.empty(); s2 += 8) {
                                    auto p2 = *reinterpret_cast<const std::uintptr_t*>(buf1 + s2);
                                    if (!isHeapPtr(p2)) continue;
                                    char buf2[256]{};
                                    int  r2 = readMem(p2, buf2, sizeof(buf2));
                                    if (r2 < 5) continue;
                                    for (int t = 0; t < r2 && found.empty(); ++t)
                                        found = tryMatchF(buf2 + t, r2 - t);
                                }
                            }
                        }
                    }
                }

                // â”€â”€ Pass 4: VirtualQueryEx scan (expensive â€” throttled) â”€â”€â”€â”€â”€â”€â”€
                // Only when quick passes fail. Without throttling, unknown maps
                // (e.g. aim_botz before prefix support) caused ~1 s stalls every
                // few seconds on the entity thread.
                if (found.empty() && !mapKnown && !mapReady && s_heavyScanCooldown <= 0
                    && !proc.usesKernelMemory()) {
                    s_heavyScanCooldown = 1500;
                    MEMORY_BASIC_INFORMATION mbi{};
                    auto scanAddr = reinterpret_cast<LPCVOID>(0x10000ull);
                    const std::uintptr_t kLimit = 0x7FFFFFFFFFFull;
                    std::vector<char> vbuf;
                    while (found.empty()) {
                        if (VirtualQueryEx(proc.handle(), scanAddr, &mbi, sizeof(mbi)) != sizeof(mbi))
                            break;
                        std::uintptr_t rBase = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
                        scanAddr = reinterpret_cast<LPCVOID>(rBase + mbi.RegionSize);
                        if (rBase >= kLimit) break;
                        if (mbi.State != MEM_COMMIT) continue;
                        if (mbi.Type != MEM_IMAGE) continue;
                        // Only scan writable pages (.data sections).
                        // .rdata (PAGE_READONLY) is full of protobuf/RTTI field-name
                        // strings that are false positives; the live map-name global
                        // lives in a PAGE_READWRITE .data page.
                        DWORD prot = mbi.Protect & ~(PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE);
                        if (prot != PAGE_READWRITE && prot != PAGE_WRITECOPY &&
                            prot != PAGE_EXECUTE_READWRITE && prot != PAGE_EXECUTE_WRITECOPY)
                            continue;
                        std::size_t sz = mbi.RegionSize;
                        if (sz > 0x100000) sz = 0x100000;
                        vbuf.resize(sz);
                        if (!mem::readRaw(proc, reinterpret_cast<std::uintptr_t>(mbi.BaseAddress),
                                          vbuf.data(), sz))
                            continue;
                        const std::size_t rb = sz;
                        if (rb < 5)
                            continue;
                        for (std::size_t i = 0; i + 3 < rb && found.empty(); ++i)
                            found = tryMatchF(vbuf.data() + i, (int)(rb - i));
                    }
                } else if (s_heavyScanCooldown > 0) {
                    --s_heavyScanCooldown;
                }

                // â”€â”€ Validate: check .bsp exists; blacklist if not â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
                if (!found.empty() && !bspExists(found)) {
                    std::cout << "[BSP] Skipping false positive: " << found << "\n";
                    bool already = false;
                    for (auto& bl : s_skipList) if (bl == found) { already = true; break; }
                    if (!already) s_skipList.push_back(found);
                    found.clear();
                    // loop retries with the updated skip list
                }

            } // end retry loop

            auto loadMapCollision = [&](const std::string& mapName) -> bool {
                if (mapName.empty())
                    return false;
                BspWorld::logCollisionToolkitStatus();
                if (m_bspWorld.isLoaded()) {
                    const std::string& cur = m_bspWorld.currentMap();
                    if (cur == mapName || cur.find(mapName) != std::string::npos)
                        return true;
                    m_bspWorld.clear();
                }

                std::cout << "[BSP] Loading collision for " << mapName << "...\n";

                bool triLoaded = false;
                {
                    auto tryTri = [&](const std::string& path) -> bool {
                        if (path.empty())
                            return false;
                        if (m_bspWorld.loadTri(path)) {
                            overlayFileLog("[BSP] Using tri cache: " + path);
                            return true;
                        }
                        return false;
                    };

                    const std::string mapCache = BspWorld::triCachePath(mapName);
                    triLoaded = tryTri(mapCache);

                    char exePath[MAX_PATH] = {};
                    if (!triLoaded && GetModuleFileNameA(nullptr, exePath, MAX_PATH)) {
                        std::string dir(exePath);
                        const auto sl = dir.find_last_of("\\/");
                        if (sl != std::string::npos) dir = dir.substr(0, sl + 1);
                        const std::string bundledTri = dir + "mapcache\\" + mapName + ".tri";
                        if (tryTri(bundledTri)) {
                            m_bspWorld.saveTriCache(mapName);
                        } else {
                            triLoaded = tryTri(dir + mapName + ".tri")
                                     || tryTri(dir + "maps\\" + mapName + ".tri");
                        }
                    }
                    if (!triLoaded)
                        triLoaded = tryTri(mapName + ".tri")
                                 || tryTri("maps\\" + mapName + ".tri");
                }

                bool ok = triLoaded;
                if (!ok) {
                    if (BspWorld::extractTriCache(mapName, m_cs2Path)) {
                        const std::string mapCache = BspWorld::triCachePath(mapName);
                        if (m_bspWorld.loadTri(mapCache)) {
                            triLoaded = true;
                            ok        = true;
                            overlayFileLog("[BSP] Using tri cache: " + mapCache);
                        } else {
                            overlayFileLog("[BSP] tri cache present but loadTri failed: " + mapCache);
                        }
                    }
                }

                if (ok && !triLoaded && m_bspWorld.triCount() >= 50000)
                    m_bspWorld.saveTriCache(mapName);

                if (ok)
                    overlayFileLog("[BSP] Collision ready for " + mapName +
                        " (" + std::to_string(m_bspWorld.triCount()) + " tris)");
                else
                    overlayFileLog("[BSP] FAILED collision for " + mapName +
                        " — need mapcache/" + mapName + ".tri or phys_extract.exe");
                return ok;
            };

            if (!found.empty() && found != m_currentMapName) {
                m_currentMapName = found;
                std::cout << "[BSP] Map detected: " << found << "\n";
                loadMapCollision(found);
            } else if (!found.empty() && !m_bspWorld.isLoaded()) {
                loadMapCollision(found);
            } else if (found.empty() && !m_currentMapName.empty() && !m_bspWorld.isLoaded()) {
                static int s_reloadTick = 0;
                if (++s_reloadTick % 300 == 1)
                    loadMapCollision(m_currentMapName);
            } else if (found.empty()) {
                static int s_failTick = 0;
                if (++s_failTick % 5 == 1)
                    std::cout << "[BSP] Scan #" << s_failTick << ": map name not found\n";
            }

        } // end scan block (rate-limited)
    } // end m_clientBase
    // grenades that haven't bounced yet (better than letting the arc go through ground).
    float tmpLocalFloorZ = 0.f;
    if (needGrenadeSim && isLikelyPtr(localPawn)) {
        auto sn = mem::read<std::uintptr_t>(
            proc, localPawn + netvars::pawn::m_pGameSceneNode);
        if (isLikelyPtr(sn))
            tmpLocalFloorZ = mem::read<Vec3>(
                proc, sn + netvars::scene_node::m_vecAbsOrigin).z;
    }

    std::array<GrenadeData, EntityManager::kMaxGrenades> tmpGrenades{};
    // FROZEN: grenade detection + physics prediction below — do not modify unless the user
    // explicitly asks. See .cursor/rules/grenade-prediction-frozen.mdc
    bool updateGrenadesThisTick = needGrenadeSim;
    if (needGrenadeSim && !m_grenadesNeedFastUpdate) {
        if (++m_grenadeSlowTick < 2) {
            updateGrenadesThisTick = false;
            tmpGrenades = m_lastPublishedGrenades;
        } else {
            m_grenadeSlowTick = 0;
        }
    } else {
        m_grenadeSlowTick = 0;
    }
    mark("spectator");
    int grenadeIdx = 0;

    if (updateGrenadesThisTick && isLikelyPtr(entityList)) {
        auto gChunk = chunk0;
        if (!isLikelyPtr(gChunk))
            gChunk = mem::read<std::uintptr_t>(proc, entityList + 0x10);
        if (isLikelyPtr(gChunk)) {
            for (int i = 65; i <= 511 && grenadeIdx < EntityManager::kMaxGrenades; ++i) {
                auto identBase = gChunk + static_cast<std::uintptr_t>(0x70) * (i & 0x1FF);
                auto entPtr    = mem::read<std::uintptr_t>(proc, identBase);
                if (!isLikelyPtr(entPtr)) continue;

                // Read designer name pointer from CEntityIdentity
                auto namePtr = mem::read<std::uintptr_t>(proc, identBase + netvars::grenade::m_designerName);
                if (!isLikelyPtr(namePtr)) continue;

                char name[32]{};
                mem::readArray(proc, namePtr, name, 31u);

                GrenadeType gtype;
                if      (!std::strncmp(name, "hegrenade_projectile",    20)) gtype = GrenadeType::HE;
                else if (!std::strncmp(name, "smokegrenade_projectile", 23)) gtype = GrenadeType::Smoke;
                else if (!std::strncmp(name, "flashbang_projectile",    20)) gtype = GrenadeType::Flash;
                else if (!std::strncmp(name, "molotov_projectile",      18)) gtype = GrenadeType::Molotov;
                else if (!std::strncmp(name, "incgrenade_projectile",   21)) gtype = GrenadeType::Molotov;
                else if (!std::strncmp(name, "decoy_projectile",        16)) gtype = GrenadeType::Decoy;
                else continue;

                auto sNode = mem::read<std::uintptr_t>(proc, entPtr + netvars::pawn::m_pGameSceneNode);
                if (!isLikelyPtr(sNode)) continue;

                GrenadeData& g   = tmpGrenades[grenadeIdx];
                int          gi  = grenadeIdx;   // index into persist array
                ++grenadeIdx;
                g.type    = gtype;
                g.origin  = mem::read<Vec3>(proc, sNode  + netvars::scene_node::m_vecAbsOrigin);
                g.velocity = mem::read<Vec3>(proc, entPtr + netvars::grenade::m_vecVelocity);

                // â”€â”€ Persistent bounce-floor tracking â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
                // Reset persistent state when the entity slot changes (grenade detonated).
                auto& ps = m_grenadePersist[gi];
                if (ps.lastEntPtr != entPtr) {
                    // If this slot previously held a Molotov, spawn a pending inferno
                    // at the ACTUAL last-seen entity origin (not the simulation result).
                    // Using the real position avoids simulation floor-height errors.
                    if (ps.lastGtype == GrenadeType::Molotov && ps.firstSeenMs != 0
                        && ps.lastOrigin.x != 0.f) {
                        for (auto& inf : m_pendingInfernos) {
                            if (!inf.active) {
                                inf.active  = true;
                                inf.pos     = ps.lastOrigin;
                                inf.startMs = GetTickCount64();
                                break;
                            }
                        }
                    }
                    ps = {};
                    ps.lastEntPtr  = entPtr;
                    ps.firstSeenMs = GetTickCount64();
                }
                ps.lastGtype  = gtype;
                ps.lastOrigin = g.origin;   // track actual position; used for PendingInferno

                // Detect floor bounce by watching vel.z sign flip: negative â†’ positive.
                // This works without relying on any potentially-stale game offsets.
                constexpr float kVelThresh = 15.f;
                bool newBounce = false;
                if (ps.prevVelZ < -kVelThresh && g.velocity.z > kVelThresh) {
                    ps.lastBounceZ = g.origin.z;
                    ps.hasFloor    = true;
                    newBounce      = true;
                }
                ps.prevVelZ = g.velocity.z;

                // floorZ: use confirmed bounce height, else local player foot Z as
                // a fallback so freshly-thrown arcs bend back to the ground level.
                float floorZ = ps.hasFloor ? ps.lastBounceZ : tmpLocalFloorZ;

                // â”€â”€ Fuse / timer data (type-specific) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
                // Use game-side m_flDetonateTime / m_flSpawnTime for accurate remaining time.
                // Falls back to wall-clock tracking if those fields are unavailable.
                {
                    // Damage radius per type
                    switch (gtype) {
                        case GrenadeType::HE:      g.hasFuse = true; g.damageRadius = 350.f; break;
                        case GrenadeType::Molotov: g.hasFuse = true; g.damageRadius = 250.f; break;
                        case GrenadeType::Smoke:   g.hasFuse = true; g.damageRadius =   0.f; break;
                        case GrenadeType::Flash:   g.hasFuse = true; g.damageRadius =   0.f; break;
                        default: break;
                    }

                    float detonateTime = mem::read<float>(proc, entPtr + netvars::grenade::m_flDetonateTime);
                    float spawnTime    = mem::read<float>(proc, entPtr + netvars::grenade::m_flSpawnTime);
                    float simTimeGame  = mem::read<float>(proc, entPtr + netvars::entity::m_flSimulationTime);

                    // Validate: all times must be positive and detonation after spawn
                    bool useGameTimer = (detonateTime > 0.1f && spawnTime > 0.1f
                                      && simTimeGame > 0.f   && detonateTime > spawnTime);
                    if (useGameTimer) {
                        g.fuseTime  = detonateTime - spawnTime;  // total fuse duration
                        g.timeAlive = simTimeGame  - spawnTime;  // elapsed since throw
                        if (g.timeAlive < 0.f) g.timeAlive = 0.f;
                        if (gtype == GrenadeType::Smoke && g.fuseTime > 6.0f)
                            g.fuseTime = 6.0f;
                    } else {
                        // Fallback to wall-clock if game fields are unavailable
                        constexpr float kHeFuse      = 1.5f;
                        constexpr float kMolotovFuse = 2.0f;
                        constexpr float kFlashFuse   = 1.5f;
                        constexpr float kSmokeFuse   = 6.0f;  // smoke flight time before deploy
                        switch (gtype) {
                            case GrenadeType::HE:      g.fuseTime = kHeFuse;      break;
                            case GrenadeType::Molotov: g.fuseTime = kMolotovFuse; break;
                            case GrenadeType::Flash:   g.fuseTime = kFlashFuse;   break;
                            case GrenadeType::Smoke:   g.fuseTime = kSmokeFuse;   break;
                            default: break;
                        }
                        g.timeAlive = ps.firstSeenMs
                            ? static_cast<float>(GetTickCount64() - ps.firstSeenMs) / 1000.f
                            : 0.f;
                    }

                    // Smoke: detect when smoke cloud has deployed (m_bDidSmokeEffect)
                    if (gtype == GrenadeType::Smoke) {
                        bool didDeploy = mem::read<uint8_t>(
                            proc, entPtr + netvars::grenade::m_bDidSmokeEffect) != 0;
                        if (didDeploy) {
                            if (!ps.deployDetected) {
                                ps.deployDetected   = true;
                                ps.deployDetectedMs = GetTickCount64();
                                ps.deployPos        = g.origin;
                            }
                            constexpr float kSmokeDeployTime = 17.5f;
                            float elapsed = static_cast<float>(
                                GetTickCount64() - ps.deployDetectedMs) / 1000.f;
                            float remaining = kSmokeDeployTime - elapsed;
                            if (remaining < 0.f) remaining = 0.f;
                            g.isDeployed    = true;
                            g.burnRemaining = remaining;
                            g.deployPos     = ps.deployPos;
                        }
                    }
                } // end fuse/timer block

                // Forward-simulate trajectory â€“ skipped for deployed grenades (inferno, smoke cloud).
                if (!g.isDeployed)
                {
                    constexpr float kMinSpeed  = 20.f;
                    constexpr int   kRecord    = 2;

                    float elasticity = mem::read<float>(
                        proc, entPtr + netvars::grenade::m_flElasticity);
                    if (elasticity <= 0.f || elasticity > 1.f) elasticity = 0.45f;

                    const bool useBsp = m_bspWorld.isLoaded();
                    
                    if (ps.predictionLocked) {
                        g.predCount = ps.lockedPredCount;
                        for (int i = 0; i < g.predCount; ++i) {
                            g.predPoints[i] = ps.lockedPredPoints[i];
                        }
                        if (g.hasFuse && gtype != GrenadeType::HE) {
                            g.fuseTime = ps.lockedSimTime;
                        }
                    } else {
                        // First time seeing this grenade: read its EXACT initial throw physics
                        Vec3 initialPos = mem::read<Vec3>(proc, entPtr + netvars::grenade::m_vInitialPosition);
                        Vec3 initialVel = mem::read<Vec3>(proc, entPtr + netvars::grenade::m_vInitialVelocity);
                        
                        // Wait until initial values are populated by the server (they might be 0 on tick 0)
                        if (initialPos.x == 0.f && initialPos.y == 0.f && initialPos.z == 0.f) {
                            initialPos = g.origin;
                            initialVel = g.velocity;
                        } else {
                            // Only lock if we successfully read the true initial values
                            ps.predictionLocked = true;
                        }

                        Vec3 pos = initialPos;
                        Vec3 vel = initialVel;
                        g.predCount = 0;
                        g.predPoints[g.predCount++] = pos;

                        // Interpolate to the exact Z=floorZ crossing point inside a tick.
                        auto applyFloorBounce = [&](const Vec3& p0, const Vec3& p1) {
                            const float dz = p1.z - p0.z; // negative (falling)
                            const float tF = (dz != 0.f) ? (floorZ - p0.z) / dz : 0.f;
                            const float tc = tF < 0.f ? 0.f : (tF > 1.f ? 1.f : tF);
                            pos   = p0 + (p1 - p0) * tc;
                            pos.z = floorZ;
                            // PhysicsClipVelocity(overbounce=2) on a flat floor + uniform
                            // elasticity â€” matches Source engine ResolveFlyCollisionCustom.
                            const float floorDamp = (gtype == GrenadeType::Smoke) ? 1.0f : 0.9f;
                            vel.x *= elasticity * floorDamp;
                            vel.y *= elasticity * floorDamp;
                            vel.z  = -vel.z * elasticity;
                            pos.z += 1.0f; // push 1 unit off surface (matches reference)
                        };

                        int bounceCount = 0;
                        int smokeWallBounces = 0;
                        int smokeLowSpeedFloorHits = 0;
                        int smokeTicksSinceWallBounce = 1000;
                        Vec3 lastHitN{0.f, 0.f, 0.f};
                        int  lastHitStep = -100;
                        float simTime = 0.f;
                        for (int s = 0; g.predCount < GrenadeData::kMaxPredPoints && bounceCount < 10; ++s) {
                            ++smokeTicksSinceWallBounce;
                            simTime += kGrenadeDt;
                            // For HE: fuseTime is total from throw; break when simTime consumes fuse.
                            // Since we simulate from t=0, we compare directly to fuseTime.
                            float fuseLimit = g.fuseTime;
                            if (gtype == GrenadeType::Smoke && smokeWallBounces > 0) {
                                float ext = smokeWallBounces * 0.35f;
                                if (ext > 1.2f) ext = 1.2f;
                                fuseLimit += ext;
                            }
                            if (g.hasFuse && simTime >= fuseLimit) {
                                g.predPoints[g.predCount++] = pos;
                                break;
                            }
                            const Vec3 prePos = pos;
                            const float newVelZ = vel.z - kGrenadeGravity * kGrenadeDt;
                            Vec3 nextPos(pos.x + vel.x * kGrenadeDt,
                                         pos.y + vel.y * kGrenadeDt,
                                         pos.z + (vel.z + newVelZ) * 0.5f * kGrenadeDt);
                            vel.z = newVelZ;

                            bool bounced      = false;
                            bool molotovFloor = false; // molotov hit a floor â†’ detonate
                            bool smokeRest    = false; // smoke low-speed floor contact â†’ deploy
                            float remainingFrac = 0.f;

                            if (useBsp) {
                                float   t;
                                Vec3    hitNorm;
                                if (sweepGrenadeHull(m_bspWorld, pos, nextPos, t, hitNorm)) {
                                    if (t < 0.f) t = 0.f;
                                    remainingFrac = 1.0f - t;
                                    const float vDotN = vel.x * hitNorm.x
                                                      + vel.y * hitNorm.y
                                                      + vel.z * hitNorm.z;
                                    // Start-solid or back-face: advance without bouncing.
                                    if (t < 0.002f || vDotN >= 0.f) {
                                        pos = nextPos;
                                    } else {
                                        // Guard against outerâ†”inner face trap.
                                        const float dotLast =
                                            hitNorm.x*lastHitN.x +
                                            hitNorm.y*lastHitN.y +
                                            hitNorm.z*lastHitN.z;
                                        if ((s - lastHitStep) < 5 &&
                                            std::fabsf(dotLast) > 0.85f) {
                                            pos = nextPos; // skip â€” trapped between parallel faces
                                        } else {
                                            pos = pos + (nextPos - pos) * t;
                                            if (gtype == GrenadeType::Molotov &&
                                                (hitNorm.z >= kMolotovMaxSlopeZ ||
                                                 vel.lengthSq() < kGrenadeStopSpeedSq)) {
                                                vel = {};
                                                molotovFloor = true;
                                            } else {
                                                // PhysicsClipVelocity(overbounce=2) + uniform elasticity.
                                                const float backoff = vDotN * 2.f;
                                                vel.x = (vel.x - hitNorm.x * backoff) * elasticity;
                                                vel.y = (vel.y - hitNorm.y * backoff) * elasticity;
                                                vel.z = (vel.z - hitNorm.z * backoff) * elasticity;
                                                if (hitNorm.z > 0.7f) {
                                                    const float floorDamp = (gtype == GrenadeType::Smoke) ? 1.0f : 0.9f;
                                                    vel.x *= floorDamp;
                                                    vel.y *= floorDamp;
                                                }
                                                applySteepBounceDampener(vel, hitNorm);
                                                if (hitNorm.z > 0.7f && vel.lengthSq() < kGrenadeStopSpeedSq)
                                                    vel = {};
                                                pos = pos + hitNorm * 4.0f; // push clear of mesh faces
                                                bounced = true;
                                                ++bounceCount;
                                                lastHitN    = hitNorm;
                                                lastHitStep = s;
                                                if (gtype == GrenadeType::Smoke && hitNorm.z <= 0.55f) {
                                                    ++smokeWallBounces;
                                                    smokeTicksSinceWallBounce = 0;
                                                    smokeLowSpeedFloorHits = 0;
                                                }
                                                if (gtype == GrenadeType::Smoke && hitNorm.z > 0.7f) {
                                                    const float speed2 = vel.lengthSq();
                                                    const float restSpeed = (smokeWallBounces > 0) ? 42.f : 55.f;
                                                    if (speed2 < restSpeed * restSpeed) {
                                                        const int requiredHits = (smokeWallBounces > 0) ? 2 : 1;
                                                        const int minTicksAfterWall = (smokeWallBounces > 0) ? 6 : 0;
                                                        if (smokeTicksSinceWallBounce >= minTicksAfterWall)
                                                            ++smokeLowSpeedFloorHits;
                                                        if (smokeLowSpeedFloorHits >= requiredHits)
                                                            smokeRest = true;
                                                    }
                                                }
                                            }
                                        }
                                    }
                                } else {
                                    pos = nextPos;
                                    // Floor safety net
                                    if (pos.z <= floorZ && vel.z < 0.f && ps.hasFloor) {
                                        applyFloorBounce(prePos, pos);
                                        const float dz = pos.z - prePos.z;
                                        const float tc = (dz != 0.f) ? (floorZ - prePos.z) / dz : 0.f;
                                        const float clampedTc = tc < 0.f ? 0.f : (tc > 1.f ? 1.f : tc);
                                        remainingFrac = 1.0f - clampedTc;
                                        if (gtype == GrenadeType::Molotov) {
                                            vel = {};
                                            molotovFloor = true;
                                        } else {
                                            bounced = true;
                                            ++bounceCount;
                                            applySteepBounceDampener(vel, { 0.f, 0.f, 1.f });
                                            if (vel.lengthSq() < kGrenadeStopSpeedSq)
                                                vel = {};
                                            if (gtype == GrenadeType::Smoke) {
                                                const float speed2 = vel.lengthSq();
                                                const float restSpeed = (smokeWallBounces > 0) ? 42.f : 55.f;
                                                if (speed2 < restSpeed * restSpeed) {
                                                    const int requiredHits = (smokeWallBounces > 0) ? 2 : 1;
                                                    const int minTicksAfterWall = (smokeWallBounces > 0) ? 6 : 0;
                                                    if (smokeTicksSinceWallBounce >= minTicksAfterWall)
                                                        ++smokeLowSpeedFloorHits;
                                                    if (smokeLowSpeedFloorHits >= requiredHits)
                                                        smokeRest = true;
                                                }
                                            }
                                        }
                                    }
                                }
                            } else {
                                // Floor-only fallback (BSP not loaded)
                                pos = nextPos;
                                if (pos.z <= floorZ && vel.z < 0.f) {
                                    const float dz = nextPos.z - prePos.z;
                                    const float tc = (dz != 0.f) ? (floorZ - prePos.z) / dz : 0.f;
                                    const float clampedTc = tc < 0.f ? 0.f : (tc > 1.f ? 1.f : tc);
                                    remainingFrac = 1.0f - clampedTc;
                                    applyFloorBounce(prePos, pos);
                                    if (gtype == GrenadeType::Molotov) {
                                        vel = {};
                                        molotovFloor = true;
                                    } else {
                                        bounced = true;
                                        ++bounceCount;
                                        applySteepBounceDampener(vel, { 0.f, 0.f, 1.f });
                                        if (vel.lengthSq() < kGrenadeStopSpeedSq)
                                            vel = {};
                                        if (gtype == GrenadeType::Smoke) {
                                            const float speed2 = vel.lengthSq();
                                            const float restSpeed = (smokeWallBounces > 0) ? 42.f : 55.f;
                                            if (speed2 < restSpeed * restSpeed) {
                                                const int requiredHits = (smokeWallBounces > 0) ? 2 : 1;
                                                const int minTicksAfterWall = (smokeWallBounces > 0) ? 6 : 0;
                                                if (smokeTicksSinceWallBounce >= minTicksAfterWall)
                                                    ++smokeLowSpeedFloorHits;
                                                if (smokeLowSpeedFloorHits >= requiredHits)
                                                    smokeRest = true;
                                            }
                                        }
                                    }
                                }
                            }

                            // Molotov/Smoke: floor contact = detonation/deploy; record final point and stop.
                            if (molotovFloor || smokeRest) {
                                g.predPoints[g.predCount++] = pos;
                                break;
                            }

                            if (bounced && remainingFrac > 0.f && vel.lengthSq() > 0.f)
                                consumeRemainingTravel(m_bspWorld, useBsp, pos, vel, remainingFrac, kGrenadeDt, pos);

                            if (gtype == GrenadeType::Decoy && (s % 12) == 0) {
                                const float speed2d = std::sqrt(vel.x*vel.x + vel.y*vel.y);
                                if (speed2d < 0.2f) {
                                    g.predPoints[g.predCount++] = pos;
                                    break;
                                }
                            }

                            if (bounced || s % kRecord == 0) {
                                g.predPoints[g.predCount++] = pos;
                                const float speed2 = vel.lengthSq();
                                if (bounced && speed2 < kMinSpeed * kMinSpeed) break;
                            }
                            if (s >= 1200) break;
                        }
                        
                        // Convert simTime to fuseTime
                        if (g.hasFuse && gtype != GrenadeType::HE)
                            g.fuseTime = simTime; // Updated because simulation is from t=0

                        ps.storedLandPos    = g.predPoints[g.predCount - 1];
                        ps.hasStoredLandPos = true;
                        
                        if (ps.predictionLocked) {
                            ps.lockedPredCount = g.predCount;
                            ps.lockedSimTime   = simTime;
                            for (int i = 0; i < g.predCount; ++i) {
                                ps.lockedPredPoints[i] = g.predPoints[i];
                            }
                        }
                    }
                } // end trajectory simulation

                // Copy stable landing into GrenadeData so the renderer can use it.
                if (ps.hasStoredLandPos) {
                    g.stableLandPos    = ps.storedLandPos;
                    g.hasStableLandPos = true;
                }
                g.isValid = true;
            }
        }
    }
    mark("grenade_sim");

    // â”€â”€ 4a. Orphaned molotov slots â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    // If a Molotov entity disappeared without another entity taking its slot
    // (e.g. the only grenade in play just exploded), the entity-reset block
    // inside the scan loop never ran. Sweep any persist slots that were NOT
    // touched this frame and spawn a PendingInferno for them.
    if (updateGrenadesThisTick) {
        for (int gi = grenadeIdx; gi < kMaxGrenades; ++gi) {
            auto& ps = m_grenadePersist[gi];
            if (ps.lastEntPtr == 0) continue;  // slot was never used
            if (ps.lastGtype == GrenadeType::Molotov && ps.lastOrigin.x != 0.f) {
                for (auto& inf : m_pendingInfernos) {
                    if (!inf.active) {
                        inf.active  = true;
                        inf.pos     = ps.lastOrigin;
                        inf.startMs = GetTickCount64();
                        break;
                    }
                }
            }
            ps = {};  // clear stale slot
        }

        // â”€â”€ 4b. Pending infernos: post-molotov burn timer â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        // When a Molotov entity disappears we start a 7-second burn timer at the
        // last predicted landing position (set in the entity-reset block above).
        {
            constexpr float kBurnTime = 7.f;
            for (auto& inf : m_pendingInfernos) {
                if (!inf.active) continue;
                float elapsed = static_cast<float>(GetTickCount64() - inf.startMs) / 1000.f;
                if (elapsed >= kBurnTime) { inf.active = false; continue; }
                if (grenadeIdx >= kMaxGrenades) break;
                GrenadeData& g = tmpGrenades[grenadeIdx++];
                g.type          = GrenadeType::Molotov;
                g.origin        = inf.pos;
                g.isDeployed    = true;
                g.burnRemaining = kBurnTime - elapsed;
                g.deployPos     = inf.pos;
                g.isValid       = true;
            }
        }

        bool needFastGrenades = (grenadeIdx == 0);
        if (!needFastGrenades) {
            for (const auto& g : tmpGrenades) {
                if (!g.isValid) continue;
                if (tmpVm.isOnScreen(g.origin)) {
                    needFastGrenades = true;
                    break;
                }
                if (g.isDeployed) {
                    if (tmpVm.isOnScreen(g.deployPos)) {
                        needFastGrenades = true;
                        break;
                    }
                    continue;
                }
                if (g.predCount > 0 && tmpVm.isOnScreen(g.predPoints[g.predCount - 1])) {
                    needFastGrenades = true;
                    break;
                }
            }
        }
        m_grenadesNeedFastUpdate = needFastGrenades;
    } else if (!needGrenadeSim) {
        for (auto& ps : m_grenadePersist)
            ps = {};
        for (auto& inf : m_pendingInfernos)
            inf = {};
        m_grenadesNeedFastUpdate = true;
        m_grenadeSlowTick = 0;
    }
    const std::uint64_t frameNowMs = GetTickCount64();

    auto resolvePlantedC4 = [&]() -> std::uintptr_t {
        if (!g_cfg.espEnabled || offsets::client::dwPlantedC4 == 0)
            return 0;

        auto first = mem::read<std::uintptr_t>(proc, m_clientBase + offsets::client::dwPlantedC4);
        std::uintptr_t candidates[2] = {
            isLikelyPtr(first) ? mem::read<std::uintptr_t>(proc, first) : 0,
            first,
        };
        for (auto candidate : candidates) {
            if (!isLikelyPtr(candidate))
                continue;
            const std::uint8_t ticking = mem::read<std::uint8_t>(proc, candidate + netvars::bomb::m_bBombTicking);
            const std::uint8_t activated = mem::read<std::uint8_t>(proc, candidate + netvars::bomb::m_bC4Activated);
            const float timerLength = mem::read<float>(proc, candidate + netvars::bomb::m_flTimerLength);
            if (ticking <= 1u && activated <= 1u && timerLength >= 0.f && timerLength < 60.f)
                return candidate;
        }
        return 0;
    };

    auto resolveWeaponC4 = [&]() -> std::uintptr_t {
        if (!g_cfg.espEnabled || offsets::client::dwWeaponC4 == 0)
            return 0;

        auto first = mem::read<std::uintptr_t>(proc, m_clientBase + offsets::client::dwWeaponC4);
        std::uintptr_t candidates[2] = {
            first,
            isLikelyPtr(first) ? mem::read<std::uintptr_t>(proc, first) : 0,
        };
        for (auto candidate : candidates) {
            if (!isLikelyPtr(candidate))
                continue;
            const std::uint8_t bombPlanted = mem::read<std::uint8_t>(proc, candidate + netvars::c4::m_bBombPlanted);
            const std::uint8_t startedArming = mem::read<std::uint8_t>(proc, candidate + netvars::c4::m_bStartedArming);
            const std::uint8_t plantingViaUse = mem::read<std::uint8_t>(proc, candidate + netvars::c4::m_bIsPlantingViaUse);
            if (bombPlanted <= 1u && startedArming <= 1u && plantingViaUse <= 1u)
                return candidate;
        }
        return 0;
    };

    BombData tmpBomb{};
    if (!g_cfg.espEnabled) {
        m_bombPersist = {};
    } else {
        const std::uintptr_t plantedC4 = resolvePlantedC4();
        if (plantedC4) {
            const bool bombTicking = mem::read<std::uint8_t>(proc, plantedC4 + netvars::bomb::m_bBombTicking) != 0;
            const bool bombActivated = mem::read<std::uint8_t>(proc, plantedC4 + netvars::bomb::m_bC4Activated) != 0;
            if (bombTicking || bombActivated) {
                tmpBomb.isPlanted = true;
                tmpBomb.isTicking = true;
                tmpBomb.isBeingDefused = mem::read<std::uint8_t>(proc, plantedC4 + netvars::bomb::m_bBeingDefused) != 0;
                tmpBomb.site = mem::read<int>(proc, plantedC4 + netvars::bomb::m_nBombSite);

                float timerLength = mem::read<float>(proc, plantedC4 + netvars::bomb::m_flTimerLength);
                if (timerLength > 1.f && timerLength < 60.f)
                    tmpBomb.timerLength = timerLength;

                float defuseLength = mem::read<float>(proc, plantedC4 + netvars::bomb::m_flDefuseLength);
                if (defuseLength > 1.f && defuseLength < 15.f)
                    tmpBomb.defuseLength = defuseLength;

                auto bombSceneNode = mem::read<std::uintptr_t>(proc, plantedC4 + netvars::pawn::m_pGameSceneNode);
                if (isLikelyPtr(bombSceneNode)) {
                    tmpBomb.origin = mem::read<Vec3>(proc, bombSceneNode + netvars::scene_node::m_vecAbsOrigin);
                } else {
                    // Fallback keeps planted C4 anchored if scene node is transiently unavailable.
                    tmpBomb.origin = mem::read<Vec3>(proc, plantedC4 + netvars::pawn::m_vOldOrigin);
                }
            }
        }

        if (tmpBomb.isPlanted) {
            m_bombPersist.plantingActive = false;
            m_bombPersist.plantingStartMs = 0;

            if (!m_bombPersist.plantedActive) {
                m_bombPersist.plantedActive = true;
                m_bombPersist.plantedStartMs = frameNowMs;
                m_bombPersist.plantedLength = tmpBomb.timerLength;
            } else if (tmpBomb.timerLength > 1.f && tmpBomb.timerLength < 60.f) {
                m_bombPersist.plantedLength = tmpBomb.timerLength;
            }

            const float plantedElapsed = static_cast<float>(frameNowMs - m_bombPersist.plantedStartMs) * 0.001f;
            tmpBomb.timerLength = m_bombPersist.plantedLength;
            tmpBomb.timeRemaining = (std::max)(0.f, m_bombPersist.plantedLength - plantedElapsed);

            if (tmpBomb.isBeingDefused) {
                if (!m_bombPersist.defusingActive) {
                    m_bombPersist.defusingActive = true;
                    m_bombPersist.defusingStartMs = frameNowMs;
                    m_bombPersist.defuseLength = (tmpBomb.defuseLength > 1.f && tmpBomb.defuseLength < 15.f)
                        ? tmpBomb.defuseLength
                        : 10.f;
                }
                const float defuseElapsed = static_cast<float>(frameNowMs - m_bombPersist.defusingStartMs) * 0.001f;
                tmpBomb.defuseLength = m_bombPersist.defuseLength;
                tmpBomb.defuseRemaining = (std::max)(0.f, m_bombPersist.defuseLength - defuseElapsed);
            } else {
                m_bombPersist.defusingActive = false;
                m_bombPersist.defusingStartMs = 0;
            }
        } else {
            m_bombPersist.plantedActive = false;
            m_bombPersist.plantedStartMs = 0;
            m_bombPersist.defusingActive = false;
            m_bombPersist.defusingStartMs = 0;

            const std::uintptr_t weaponC4 = resolveWeaponC4();
            if (weaponC4) {
                const bool bombPlanted = mem::read<std::uint8_t>(proc, weaponC4 + netvars::c4::m_bBombPlanted) != 0;
                const bool startedArming = mem::read<std::uint8_t>(proc, weaponC4 + netvars::c4::m_bStartedArming) != 0;
                const bool plantingViaUse = mem::read<std::uint8_t>(proc, weaponC4 + netvars::c4::m_bIsPlantingViaUse) != 0;
                const bool plantingNow = !bombPlanted && (startedArming || plantingViaUse);

                if (plantingNow) {
                    if (!m_bombPersist.plantingActive) {
                        m_bombPersist.plantingActive = true;
                        m_bombPersist.plantingStartMs = frameNowMs;
                        m_bombPersist.plantLength = kBombPlantLength;
                    }

                    const float plantElapsed = static_cast<float>(frameNowMs - m_bombPersist.plantingStartMs) * 0.001f;
                    const float plantRemaining = (std::max)(0.f, m_bombPersist.plantLength - plantElapsed);
                    if (plantRemaining > 0.f) {
                        tmpBomb.isPlanting = true;
                        tmpBomb.plantLength = m_bombPersist.plantLength;
                        tmpBomb.plantRemaining = plantRemaining;
                    }
                } else {
                    m_bombPersist.plantingActive = false;
                    m_bombPersist.plantingStartMs = 0;
                }
            } else {
                m_bombPersist.plantingActive = false;
                m_bombPersist.plantingStartMs = 0;
            }
        }
    }
    mark("bomb");

    // â”€â”€ 5. Pre-throw grenade trajectory â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    // While the local player is winding up to throw a grenade (m_flThrowStrength
    // > 0), simulate the predicted arc from the eye position using the current
    // view angles and throw strength so the renderer can draw a preview line.
    PreThrowData tmpPreThrow{};
    if (needPreThrow && isLikelyPtr(localPawn)) {
            // Resolve active weapon handle via weapon services
            auto weapSvcPtr = pawn_services::readWeaponServices(proc, localPawn);
            if (isLikelyPtr(weapSvcPtr)) {
                auto weapHandle = mem::read<std::uint32_t>(
                    proc, weapSvcPtr + netvars::weapon_services::m_hActiveWeapon);
                if (weapHandle && weapHandle != 0xFFFFFFFFu && isLikelyPtr(entityList)) {
                    auto weapChunk = chunk0;
                    if (!isLikelyPtr(weapChunk) || (weapHandle & 0x7FFF) >> 9 != 0)
                        weapChunk = mem::read<std::uintptr_t>(
                            proc, entityList + 0x10 + 0x8 * ((weapHandle & 0x7FFF) >> 9));
                    if (isLikelyPtr(weapChunk)) {
                        std::uintptr_t weapIdentBase =
                            weapChunk + std::uintptr_t(0x70) * (weapHandle & 0x1FF);
                        // Check that it is a throwable grenade weapon
                        GrenadeType preType = GrenadeType::Smoke;
                        bool isGrenade = false;
                        {
                            auto namePtr2 = mem::read<std::uintptr_t>(
                                proc, weapIdentBase + netvars::grenade::m_designerName);
                            if (isLikelyPtr(namePtr2)) {
                                char wname[32]{};
                                mem::readArray(proc, namePtr2, wname, 31u);
                                if      (std::strstr(wname, "hegrenade"))                { isGrenade = true; preType = GrenadeType::HE;      }
                                else if (std::strstr(wname, "smokegrenade"))             { isGrenade = true; preType = GrenadeType::Smoke;   }
                                else if (std::strstr(wname, "flashbang"))                { isGrenade = true; preType = GrenadeType::Flash;   }
                                else if (std::strstr(wname, "molotov") ||
                                         std::strstr(wname, "incgrenade"))               { isGrenade = true; preType = GrenadeType::Molotov; }
                                else if (std::strstr(wname, "decoy"))                    { isGrenade = true; preType = GrenadeType::Decoy;   }
                            }
                        }
                        if (isGrenade) {
                            auto weapPtr = mem::read<std::uintptr_t>(proc, weapIdentBase);
                            if (isLikelyPtr(weapPtr)) {
                                float throwStrength = mem::read<float>(
                                    proc, weapPtr + netvars::grenade::m_flThrowStrength);
                                if (throwStrength > 0.01f && throwStrength <= 1.0f) {
                                    // Read view angles from pawn first (live eye angles),
                                    // then fall back to client view angles if needed.
                                    Vec2 eyeAngles = mem::read<Vec2>(
                                        proc, localPawn + netvars::pawn::m_angEyeAngles);
                                    Vec3 viewAngles{ eyeAngles.x, eyeAngles.y, 0.f };
                                    if (!std::isfinite(viewAngles.x) || !std::isfinite(viewAngles.y)
                                        || (std::fabs(viewAngles.x) < 0.001f && std::fabs(viewAngles.y) < 0.001f)) {
                                        viewAngles = mem::read<Vec3>(
                                            proc, m_clientBase + offsets::client::dwViewAngles);
                                    }
                                    Vec3 playerVel = mem::read<Vec3>(
                                        proc, localPawn + netvars::grenade::m_vecVelocity);

                                    // Eye position = origin + view offset
                                    auto lpSN2 = mem::read<std::uintptr_t>(
                                        proc, localPawn + netvars::pawn::m_pGameSceneNode);
                                    Vec3 lpOrigin{};
                                    if (isLikelyPtr(lpSN2))
                                        lpOrigin = mem::read<Vec3>(
                                            proc, lpSN2 + netvars::scene_node::m_vecAbsOrigin);
                                    else
                                        lpOrigin = mem::read<Vec3>(
                                            proc, localPawn + netvars::pawn::m_vOldOrigin);
                                    Vec3 viewOff = mem::read<Vec3>(
                                        proc, localPawn + netvars::pawn::m_vecViewOffset);
                                    Vec3 startPos = lpOrigin + viewOff;

                                    const bool useBspPreview = m_bspWorld.isLoaded();
                                    (void)useBspPreview;

                                    const auto simulatePreThrow = [&]() {
                                    // Use CS2-style throw strength snapping and the per-type
                                    // base throw speed table instead of a single hardcoded launch constant.
                                    float strength = normalizeThrowStrength(throwStrength);
                                    float throwVelocity = grenadeThrowBaseVelocity(preType) * 0.9f;
                                    if (throwVelocity < 15.f) throwVelocity = 15.f;
                                    if (throwVelocity > 750.f) throwVelocity = 750.f;
                                    float throwSpeed = (strength * 0.7f + 0.3f) * throwVelocity;
                                    Vec3 dir = grenadeForwardFromView(viewAngles);
                                    Vec3 vel = dir * throwSpeed + playerVel * 1.25f;

                                    // Simulate trajectory (same algorithm as live grenades)
                                    constexpr float kElast    = 0.45f;
                                    constexpr float kMinSpeed = 20.f;
                                    const float floorZ = lpOrigin.z;
                                    const bool useBsp  = useBspPreview;

                                    // Grenade is released from the eye position.
                                    Vec3 pos = startPos;
                                    tmpPreThrow.type     = preType;
                                    tmpPreThrow.fuseTime = (preType == GrenadeType::HE)      ? 1.5f  :
                                                          (preType == GrenadeType::Flash)   ? 1.5f  :
                                                          (preType == GrenadeType::Molotov) ? 2.0f  :
                                                          (preType == GrenadeType::Smoke)   ? 6.0f  : 0.f;
                                    tmpPreThrow.predCount = 0;
                                    tmpPreThrow.predPoints[tmpPreThrow.predCount++] = pos;
                                    int bounceCount = 0;
                                    int smokeWallBounces = 0;
                                    int smokeLowSpeedFloorHits = 0;
                                    int smokeTicksSinceWallBounce = 1000;
                                    Vec3 lastHitN{0.f, 0.f, 0.f};
                                    int  lastHitStep = -100;

                                    float simTimePre = 0.f;
                                    bool  preSimDone = false;
                                    for (int s = 0;
                                         !preSimDone &&
                                         tmpPreThrow.predCount < PreThrowData::kMaxPredPoints
                                         && bounceCount < kPreThrowMaxBounces; ++s)
                                    {
                                        ++smokeTicksSinceWallBounce;
                                        simTimePre += kPreThrowDt;
                                        // Fuse termination for HE / Molotov.
                                        float preFuseLimit = tmpPreThrow.fuseTime;
                                        if (preType == GrenadeType::Smoke && smokeWallBounces > 0) {
                                            float ext = smokeWallBounces * 0.35f;
                                            if (ext > 1.2f) ext = 1.2f;
                                            preFuseLimit += ext;
                                        }
                                        if (tmpPreThrow.fuseTime > 0.f && simTimePre >= preFuseLimit) {
                                            tmpPreThrow.predPoints[tmpPreThrow.predCount++] = pos;
                                            break;
                                        }
                                        const Vec3 prePos = pos;
                                        const float newVelZ = vel.z - kGrenadeGravity * kPreThrowDt;
                                        Vec3 nextPos(pos.x + vel.x * kPreThrowDt,
                                                     pos.y + vel.y * kPreThrowDt,
                                                     pos.z + (vel.z + newVelZ) * 0.5f * kPreThrowDt);
                                        vel.z = newVelZ;
                                        bool bounced = false;
                                        bool smokeRest = false;
                                        float remainingFrac = 0.f;

                                        const bool useBspStep =
                                            useBsp
                                            && bounceCount < kPreThrowBspSweepMaxBounces
                                            && s < kPreThrowBspSweepMaxSteps;

                                        if (useBspStep) {
                                            float t; Vec3 hitNorm;
                                            if (sweepGrenadePreview(m_bspWorld, pos, nextPos, t, hitNorm)) {
                                                if (t < 0.f) t = 0.f;
                                                remainingFrac = 1.0f - t;
                                                const float vDotN = vel.x*hitNorm.x
                                                                  + vel.y*hitNorm.y
                                                                  + vel.z*hitNorm.z;
                                                // Back-face or start-solid: advance without
                                                // bouncing to avoid fold artifacts.
                                                if (t < 0.002f || vDotN >= 0.f) {
                                                    pos = nextPos;
                                                } else {
                                                    // Guard against outerâ†”inner face traps
                                                    // (decorative mesh surfaces with two
                                                    // parallel faces the grenade oscillates
                                                    // between until speed drops to zero).
                                                    const float dotLast =
                                                        hitNorm.x*lastHitN.x +
                                                        hitNorm.y*lastHitN.y +
                                                        hitNorm.z*lastHitN.z;
                                                    if ((s - lastHitStep) < 5 &&
                                                        std::fabsf(dotLast) > 0.85f) {
                                                        // Likely trapped â€” skip this bounce.
                                                        pos = nextPos;
                                                    } else {
                                                        pos = pos + (nextPos - pos) * t;
                                                        if (preType == GrenadeType::Molotov &&
                                                            (hitNorm.z >= kMolotovMaxSlopeZ ||
                                                             vel.lengthSq() < kGrenadeStopSpeedSq)) {
                                                            vel = {};
                                                            tmpPreThrow.predPoints[tmpPreThrow.predCount++] = pos;
                                                            preSimDone = true;
                                                        } else {
                                                            const float backoff = vDotN * 2.f;
                                                            vel.x = (vel.x - hitNorm.x * backoff) * kElast;
                                                            vel.y = (vel.y - hitNorm.y * backoff) * kElast;
                                                            vel.z = (vel.z - hitNorm.z * backoff) * kElast;
                                                            if (hitNorm.z > 0.7f) {
                                                                const float floorDamp = (preType == GrenadeType::Smoke) ? 1.0f : 0.9f;
                                                                vel.x *= floorDamp;
                                                                vel.y *= floorDamp;
                                                            }
                                                            applySteepBounceDampener(vel, hitNorm);
                                                            if (hitNorm.z > 0.7f && vel.lengthSq() < kGrenadeStopSpeedSq)
                                                                vel = {};
                                                            pos = pos + hitNorm * 4.0f;
                                                            bounced = true;
                                                            ++bounceCount;
                                                            lastHitN    = hitNorm;
                                                            lastHitStep = s;
                                                            if (preType == GrenadeType::Smoke && hitNorm.z <= 0.55f) {
                                                                ++smokeWallBounces;
                                                                smokeTicksSinceWallBounce = 0;
                                                                smokeLowSpeedFloorHits = 0;
                                                            }
                                                            if (preType == GrenadeType::Smoke && hitNorm.z > 0.7f) {
                                                                const float speed2 = vel.lengthSq();
                                                                const float restSpeed = (smokeWallBounces > 0) ? 42.f : 55.f;
                                                                if (speed2 < restSpeed * restSpeed) {
                                                                    const int requiredHits = (smokeWallBounces > 0) ? 2 : 1;
                                                                    const int minTicksAfterWall = (smokeWallBounces > 0) ? 6 : 0;
                                                                    if (smokeTicksSinceWallBounce >= minTicksAfterWall)
                                                                        ++smokeLowSpeedFloorHits;
                                                                    if (smokeLowSpeedFloorHits >= requiredHits)
                                                                        smokeRest = true;
                                                                }
                                                            }
                                                        }
                                                    }
                                                }
                                            } else {
                                                pos = nextPos;
                                                if (pos.z <= floorZ && vel.z < 0.f) {
                                                    const float dz = nextPos.z - prePos.z;
                                                    const float tc = (dz != 0.f) ? (floorZ - prePos.z) / dz : 0.f;
                                                    const float tc2 = tc < 0.f ? 0.f : (tc > 1.f ? 1.f : tc);
                                                    remainingFrac = 1.0f - tc2;
                                                    pos   = prePos + (nextPos - prePos) * tc2;
                                                    pos.z = floorZ + 1.f;
                                                    if (preType == GrenadeType::Molotov) {
                                                        vel = {};
                                                        tmpPreThrow.predPoints[tmpPreThrow.predCount++] = pos;
                                                        preSimDone = true;
                                                    } else {
                                                        const float floorDamp = (preType == GrenadeType::Smoke) ? 1.0f : 0.9f;
                                                        vel.x *= kElast * floorDamp;
                                                        vel.y *= kElast * floorDamp;
                                                        vel.z  = -vel.z * kElast;
                                                        applySteepBounceDampener(vel, { 0.f, 0.f, 1.f });
                                                        if (vel.lengthSq() < kGrenadeStopSpeedSq)
                                                            vel = {};
                                                        bounced = true;
                                                        ++bounceCount;
                                                        if (preType == GrenadeType::Smoke) {
                                                            const float speed2 = vel.lengthSq();
                                                            const float restSpeed = (smokeWallBounces > 0) ? 42.f : 55.f;
                                                            if (speed2 < restSpeed * restSpeed) {
                                                                const int requiredHits = (smokeWallBounces > 0) ? 2 : 1;
                                                                const int minTicksAfterWall = (smokeWallBounces > 0) ? 6 : 0;
                                                                if (smokeTicksSinceWallBounce >= minTicksAfterWall)
                                                                    ++smokeLowSpeedFloorHits;
                                                                if (smokeLowSpeedFloorHits >= requiredHits)
                                                                    smokeRest = true;
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        } else {
                                            pos = nextPos;
                                            if (pos.z <= floorZ && vel.z < 0.f) {
                                                const float dz = nextPos.z - prePos.z;
                                                const float tc = (dz != 0.f) ? (floorZ - prePos.z) / dz : 0.f;
                                                const float tc2 = tc < 0.f ? 0.f : (tc > 1.f ? 1.f : tc);
                                                pos = prePos + (nextPos - prePos) * tc2;
                                                pos.z = floorZ + 1.f;
                                                if (preType == GrenadeType::Molotov) {
                                                    vel = {};
                                                    tmpPreThrow.predPoints[tmpPreThrow.predCount++] = pos;
                                                    preSimDone = true;
                                                } else {
                                                    const float floorDamp = (preType == GrenadeType::Smoke) ? 1.0f : 0.9f;
                                                    vel.x *= kElast * floorDamp;
                                                    vel.y *= kElast * floorDamp;
                                                    vel.z  = -vel.z * kElast;
                                                    applySteepBounceDampener(vel, { 0.f, 0.f, 1.f });
                                                    if (vel.lengthSq() < kGrenadeStopSpeedSq)
                                                        vel = {};
                                                    bounced = true;
                                                    ++bounceCount;
                                                }
                                            }
                                        }

                                        if (smokeRest) {
                                            tmpPreThrow.predPoints[tmpPreThrow.predCount++] = pos;
                                            break;
                                        }
                                        if (preSimDone) break;
                                        if (bounced && remainingFrac > 0.f && vel.lengthSq() > 0.f)
                                            consumeRemainingTravelPreview(m_bspWorld, useBspStep, pos, vel, remainingFrac, kPreThrowDt, pos);
                                        if (preType == GrenadeType::Decoy && (s % 12) == 0) {
                                            const float speed2d = std::sqrt(vel.x*vel.x + vel.y*vel.y);
                                            if (speed2d < 0.2f) {
                                                tmpPreThrow.predPoints[tmpPreThrow.predCount++] = pos;
                                                break;
                                            }
                                        }
                                        if (bounced || s % kPreThrowRecordStep == 0)
                                            tmpPreThrow.predPoints[tmpPreThrow.predCount++] = pos;
                                        const float spd2 = vel.lengthSq();
                                        if (bounced && spd2 < kMinSpeed * kMinSpeed) break;
                                        // Without BSP, cap the arc at ~2 seconds so it doesn't
                                        // spiral underground forever.
                                        if (!useBsp && s >= kPreThrowNoBspMaxSteps) break;
                                        if (s >= kPreThrowMaxSteps) break;
                                    }
                                    tmpPreThrow.isActive = true;
                                    };

                                    simulatePreThrow();
                                }
                            }
                        }
                    }
                }
            }
    }
    mark("prethrow");

    GameRulesData tmpGameRules{};
    if (offsets::client::dwGameRules != 0) {
        const std::uintptr_t gameRules = mem::read<std::uintptr_t>(
            proc, m_clientBase + offsets::client::dwGameRules);
        if (isLikelyPtr(gameRules)) {
            tmpGameRules.valid = true;
            tmpGameRules.freezePeriod = mem::read<std::uint8_t>(
                proc, gameRules + netvars::game_rules::m_bFreezePeriod) != 0;
            tmpGameRules.warmupPeriod = mem::read<std::uint8_t>(
                proc, gameRules + netvars::game_rules::m_bWarmupPeriod) != 0;
            tmpGameRules.gamePhase = mem::read<int>(proc, gameRules + netvars::game_rules::m_gamePhase);
            tmpGameRules.totalRoundsPlayed = mem::read<int>(
                proc, gameRules + netvars::game_rules::m_totalRoundsPlayed);
            tmpGameRules.roundStartRoundNumber = mem::read<int>(
                proc, gameRules + netvars::game_rules::m_iRoundStartRoundNumber);
            tmpGameRules.roundWinStatus = mem::read<int>(
                proc, gameRules + netvars::game_rules::m_iRoundWinStatus);
            tmpGameRules.roundEndWinnerTeam = mem::read<int>(
                proc, gameRules + netvars::game_rules::m_iRoundEndWinnerTeam);
            tmpGameRules.roundStartCount = mem::read<std::uint8_t>(
                proc, gameRules + netvars::game_rules::m_nRoundStartCount);
            tmpGameRules.roundEndCount = mem::read<std::uint8_t>(
                proc, gameRules + netvars::game_rules::m_nRoundEndCount);
        }
    }
    mark("gamerules");

    m_preThrowPersist.valid = false;

    // â”€â”€ 6. Publish immutable frame for render / aim readers â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    {
        const int readSlot = m_readSlot.load(std::memory_order_relaxed);
        const int writeSlot = 1 - readSlot;
        if (!m_frames[writeSlot])
            m_frames[writeSlot] = std::make_shared<Snapshot>();

        Snapshot& w = *m_frames[writeSlot];
        w.players = tmpPlayers;
        w.spectators = tmpSpectators;
        w.grenades = tmpGrenades;
        w.preThrow = tmpPreThrow;
        w.bomb = tmpBomb;
        w.gameRules = tmpGameRules;
        w.viewMatrix = tmpVm;
        w.localPawn = localPawn;
        w.localTeam = tmpLocalTeam;
        w.localPing = tmpLocalPing;
        w.hasLocalPlayer = hasLocalPlayer;
        w.localOrigin = tmpLocalOrigin;
        w.localViewAngles = tmpLocalViewAngles;
        w.currentMapName = m_currentMapName;
        m_lastPublishedGrenades = tmpGrenades;
        m_readSlot.store(writeSlot, std::memory_order_release);
    }
    m_lastUpdateTickMs.store(GetTickCount64(), std::memory_order_release);
}
