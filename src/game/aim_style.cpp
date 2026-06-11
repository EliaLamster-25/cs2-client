#include "aim_style.h"
#include "config.h"
#include "gui/font.h"
#include "gui/gui.h"
#include "math/matrix.h"
#include "memory/rpm.h"
#include "offsets/netvars.h"
#include "game/weapon_group.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

namespace {
struct FeatureBackup {
    bool espEnabled = true;
    bool aimAssistEnabled = false;
    bool triggerbotEnabled = false;
    bool chamsEnabled = false;
    bool chamsOccluded = false;
};

FeatureBackup s_featureBackup{};
bool s_featureBackupActive = false;

unsigned int withAlpha(unsigned int color, unsigned int alpha) {
    return (color & 0x00FFFFFFu) | ((alpha & 0xFFu) << 24);
}

unsigned int lerpColor(unsigned int c0, unsigned int c1, float t) {
    t = std::clamp(t, 0.f, 1.f);
    auto ch = [&](int shift) -> unsigned int {
        const float a = static_cast<float>((c0 >> shift) & 0xFF);
        const float b = static_cast<float>((c1 >> shift) & 0xFF);
        return static_cast<unsigned int>(a + (b - a) * t + 0.5f);
    };
    return (ch(24) << 24) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

void enterCalibrationSafeMode() {
    if (s_featureBackupActive)
        return;
    s_featureBackup.espEnabled = g_cfg.espEnabled;
    s_featureBackup.aimAssistEnabled = g_cfg.aimAssistEnabled;
    s_featureBackup.triggerbotEnabled = g_cfg.triggerbotEnabled;
    s_featureBackup.chamsEnabled = g_cfg.chamsEnabled;
    s_featureBackup.chamsOccluded = g_cfg.chamsOccluded;
    g_cfg.espEnabled = false;
    g_cfg.aimAssistEnabled = false;
    g_cfg.triggerbotEnabled = false;
    g_cfg.chamsEnabled = false;
    g_cfg.chamsOccluded = false;
    s_featureBackupActive = true;
}

void exitCalibrationSafeMode() {
    if (!s_featureBackupActive)
        return;
    g_cfg.espEnabled = s_featureBackup.espEnabled;
    g_cfg.aimAssistEnabled = s_featureBackup.aimAssistEnabled;
    g_cfg.triggerbotEnabled = s_featureBackup.triggerbotEnabled;
    g_cfg.chamsEnabled = s_featureBackup.chamsEnabled;
    g_cfg.chamsOccluded = s_featureBackup.chamsOccluded;
    s_featureBackupActive = false;
}

constexpr float kPi = 3.14159265f;
constexpr float kRad2Deg = 180.f / kPi;
constexpr float kDeg2Rad = kPi / 180.f;

std::uint64_t nowMs() {
    return GetTickCount64();
}

float normalizeAngle(float ang) {
    while (ang > 180.f) ang -= 360.f;
    while (ang < -180.f) ang += 360.f;
    return ang;
}

float rand01() {
    static thread_local std::mt19937 rng{ std::random_device{}() };
    static thread_local std::uniform_real_distribution<float> dist(0.f, 1.f);
    return dist(rng);
}

float randRange(float a, float b) {
    return a + (b - a) * rand01();
}

Vec3 forwardFromAngles(float pitchDeg, float yawDeg) {
    const float pitch = pitchDeg * kDeg2Rad;
    const float yaw = yawDeg * kDeg2Rad;
    const float cp = std::cosf(pitch);
    return { cp * std::cosf(yaw), cp * std::sinf(yaw), -std::sinf(pitch) };
}

Vec3 normalize3(const Vec3& v) {
    const float len = v.length();
    if (len < 0.001f)
        return {};
    return v * (1.f / len);
}

void cameraBasisFromViewMatrix(const ViewMatrix& vm, Vec3& fwd, Vec3& right, Vec3& up) {
    fwd = normalize3({ vm.m[2][0], vm.m[2][1], vm.m[2][2] });
    right = normalize3({ vm.m[0][0], vm.m[0][1], vm.m[0][2] });
    up = normalize3({ vm.m[1][0], vm.m[1][1], vm.m[1][2] });
}

float angleBetweenDeg(const Vec3& a, const Vec3& b) {
    const Vec3 na = normalize3(a);
    const Vec3 nb = normalize3(b);
    if (na.lengthSq() < 0.001f || nb.lengthSq() < 0.001f)
        return 0.f;
    const float dot = std::clamp(na.x * nb.x + na.y * nb.y + na.z * nb.z, -1.f, 1.f);
    return std::acosf(dot) * kRad2Deg;
}

Vec3 readAimPunch(const Process& proc, std::uintptr_t localPawn) {
    const auto aimPunchSvc = mem::read<std::uintptr_t>(
        proc, localPawn + netvars::pawn::m_pAimPunchServices);
    if (!aimPunchSvc)
        return {};
    const Vec3 predictable = mem::read<Vec3>(
        proc, aimPunchSvc + netvars::aim_punch_services::m_predictableBaseAngle);
    const Vec3 unpredictable = mem::read<Vec3>(
        proc, aimPunchSvc + netvars::aim_punch_services::m_unpredictableBaseAngle);
    return {
        predictable.x + unpredictable.x,
        predictable.y + unpredictable.y,
        predictable.z + unpredictable.z
    };
}

} // namespace

bool aimCalibrationBlocksFeatures() {
    return g_cfg.aimCalibrationActive || AimCalibration::instance().isActive();
}

AimStyleProfile aimStyleProfileFromGroupCalib(const AimGroupCalibProfile& group) {
    AimStyleProfile p;
    if (!group.valid)
        return p;
    p.valid = true;
    p.sampleCount = group.sampleCount;
    p.accuracy = group.accuracy;
    p.measuredReactionMs = group.measuredReactionMs;
    p.measuredSmooth = group.measuredSmooth;
    p.measuredOvershootDeg = group.measuredOvershoot;
    p.measuredFlickDegPerSec = group.measuredFlickSpeed;
    p.measuredJitter = group.measuredJitter;
    p.avgReactionMs = group.assistReactionMs;
    p.avgSmooth = group.assistSmooth;
    p.avgOvershootDeg = group.assistOvershoot;
    p.avgFlickDegPerSec = group.assistFlickSpeed;
    p.avgJitter = group.assistJitter;
    return p;
}

void syncAimStyleForWeaponGroup(AimWeaponGroup group) {
    const auto& gp = g_cfg.aimCalibByGroup[aimGroupIndex(group)];
    if (gp.valid)
        AimHumanizer::instance().setProfile(aimStyleProfileFromGroupCalib(gp));
    else
        syncAimStyleFromConfig();
}

void syncAimStyleFromConfig() {
    for (int i = kCalibrationGroupCount - 1; i >= 0; --i) {
        const auto group = kCalibrationGroups[i];
        const auto& gp = g_cfg.aimCalibByGroup[aimGroupIndex(group)];
        if (gp.valid) {
            AimHumanizer::instance().setProfile(aimStyleProfileFromGroupCalib(gp));
            return;
        }
    }
    if (!g_cfg.aimStyleProfileValid)
        return;
    AimStyleProfile p;
    p.valid = true;
    p.accuracy = g_cfg.aimStyleAccuracy;
    p.measuredReactionMs = g_cfg.aimStyleMeasuredReactionMs;
    p.measuredSmooth = g_cfg.aimStyleMeasuredSmooth;
    p.measuredOvershootDeg = g_cfg.aimStyleMeasuredOvershoot;
    p.measuredFlickDegPerSec = g_cfg.aimStyleMeasuredFlickSpeed;
    p.measuredJitter = g_cfg.aimStyleMeasuredJitter;
    p.avgReactionMs = g_cfg.aimStyleReactionMs;
    p.avgSmooth = g_cfg.aimStyleSmooth;
    p.avgOvershootDeg = g_cfg.aimStyleOvershoot;
    p.avgFlickDegPerSec = g_cfg.aimStyleFlickSpeed;
    p.avgJitter = g_cfg.aimStyleJitter;
    AimHumanizer::instance().setProfile(p);
}

AimHumanizer& AimHumanizer::instance() {
    static AimHumanizer inst;
    return inst;
}

void AimHumanizer::setProfile(const AimStyleProfile& profile) {
    m_profile = profile;
}

void AimHumanizer::resetTargetState() {
    m_hasTarget = false;
    m_reactionReady = false;
    m_overshootPhase = 0.f;
    m_targetLostMs = 0;
    m_trackErrMag = 999.f;
    m_minErrMag = 999.f;
    m_overshootFrames = 0;
    m_lastDxPix = 0.f;
    m_lastDyPix = 0.f;
}

float AimHumanizer::effectiveStrength() const {
    if (g_cfg.aimHumanizeMode == 0)
        return std::clamp(g_cfg.aimCalibAssistStrength, 0.f, 1.f);
    return std::clamp(g_cfg.aimHumanizeStrength, 0.f, 1.f);
}

float AimHumanizer::reactionMs() const {
    if (g_cfg.aimHumanizeMode == 1 && g_cfg.aimHumanizeReactionMs > 1.f)
        return g_cfg.aimHumanizeReactionMs;
    if (g_cfg.aimHumanizeUseProfile && m_profile.valid)
        return (std::max)(15.f, m_profile.avgReactionMs);
    if (g_cfg.aimStyleProfileValid)
        return (std::max)(15.f, g_cfg.aimStyleReactionMs);
    return 15.f;
}

float AimHumanizer::overshootFrac() const {
    if (g_cfg.aimHumanizeMode == 1 && g_cfg.aimHumanizeOvershoot > 0.001f)
        return g_cfg.aimHumanizeOvershoot;
    if (g_cfg.aimHumanizeUseProfile && m_profile.valid)
        return std::clamp(m_profile.avgOvershootDeg / 12.f, 0.02f, 0.22f);
    return 0.06f;
}

float AimHumanizer::jitterPx() const {
    if (g_cfg.aimHumanizeMode == 1 && g_cfg.aimHumanizeJitter > 0.001f)
        return g_cfg.aimHumanizeJitter;
    if (g_cfg.aimHumanizeUseProfile && m_profile.valid)
        return std::clamp(m_profile.avgJitter, 0.05f, 2.5f);
    return 0.25f;
}

float AimHumanizer::maxStepPx(float dtMs) const {
    const float sens = (std::max)(0.1f, g_cfg.aimSensitivity);
    const float degPerPx = sens * 0.022f;
    float degPerSec = 720.f;
    if (g_cfg.aimHumanizeUseProfile && m_profile.valid)
        degPerSec = (std::max)(180.f, m_profile.avgFlickDegPerSec);
    degPerSec *= std::clamp(g_cfg.aimHumanizeSpeedScale, 0.35f, 1.5f);
    const float maxDeg = degPerSec * (dtMs / 1000.f);
    return maxDeg / degPerPx;
}

void AimHumanizer::apply(float& dxPix, float& dyPix,
                         float pitchStepDeg, float yawStepDeg,
                         bool hasTarget, float dtMs) {
    const float strength = effectiveStrength();
    if (!g_cfg.aimHumanizeEnabled || strength < 0.01f) {
        resetTargetState();
        return;
    }

    if (!hasTarget) {
        if (m_hasTarget) {
            if (m_targetLostMs == 0)
                m_targetLostMs = nowMs();
            if (nowMs() - m_targetLostMs < 120)
                return;
            resetTargetState();
        }
        return;
    }
    m_targetLostMs = 0;

    if (!m_hasTarget) {
        m_hasTarget = true;
        m_acquireMs = nowMs();
        m_reactionReady = false;
        m_trackErrMag = 999.f;
        m_minErrMag = 999.f;
        m_overshootFrames = 0;
        m_lastDxPix = 0.f;
        m_lastDyPix = 0.f;
    }

    const float reactScale = 0.55f + 0.45f * (1.f - strength);
    const float reactMs = reactionMs() * reactScale * (0.88f + rand01() * 0.22f);
    if (!m_reactionReady) {
        if (static_cast<float>(nowMs() - m_acquireMs) < reactMs) {
            dxPix = 0.f;
            dyPix = 0.f;
            return;
        }
        m_reactionReady = true;
    }

    const float errMag = std::sqrtf(pitchStepDeg * pitchStepDeg + yawStepDeg * yawStepDeg);

    if (errMag > m_trackErrMag * 1.8f || errMag > 12.f) {
        m_minErrMag = errMag;
        m_overshootFrames = 0;
    } else if (errMag < m_minErrMag - 0.04f) {
        m_minErrMag = errMag;
        m_overshootFrames = 0;
    } else if (errMag > m_minErrMag + 0.07f && m_minErrMag < 4.f) {
        ++m_overshootFrames;
    }
    m_trackErrMag = errMag;

    const float brakeStart = m_profile.valid
        ? (std::max)(0.65f, m_profile.avgOvershootDeg * 2.4f + 0.35f)
        : 1.6f;
    if (errMag < brakeStart && errMag > 0.001f) {
        const float t = errMag / brakeStart;
        const float brake = 0.38f + 0.62f * t * t;
        const float brakeMix = (0.28f + 0.72f * strength) * (1.f - t * 0.35f);
        dxPix = dxPix * (1.f - brakeMix) + dxPix * brake * brakeMix;
        dyPix = dyPix * (1.f - brakeMix) + dyPix * brake * brakeMix;
    }

    if (m_overshootFrames >= 1 && errMag < 4.5f) {
        const float damp = std::clamp((0.18f + overshootFrac() * 1.4f) * strength, 0.12f, 0.62f);
        dxPix *= (1.f - damp);
        dyPix *= (1.f - damp);
        if (m_overshootFrames >= 2)
            m_overshootFrames = 1;
    }

    const float pathBlend = 0.10f + 0.14f * strength;
    if (std::fabs(m_lastDxPix) > 0.001f || std::fabs(m_lastDyPix) > 0.001f) {
        dxPix = dxPix * (1.f - pathBlend) + m_lastDxPix * pathBlend;
        dyPix = dyPix * (1.f - pathBlend) + m_lastDyPix * pathBlend;
    }

    const float maxStep = maxStepPx((std::max)(1.f, dtMs));
    const float speedHeadroom = 1.f + (0.14f + 0.10f * strength) * (1.f - strength * 0.35f);
    const float mag = std::sqrtf(dxPix * dxPix + dyPix * dyPix);
    if (mag > maxStep && mag > 0.001f) {
        const float s = (maxStep * speedHeadroom) / mag;
        if (s < 1.f) {
            dxPix *= s;
            dyPix *= s;
        }
    }

    if (mag < maxStep * 0.32f && strength > 0.01f) {
        const float steady = 0.90f + 0.08f * (1.f - strength);
        dxPix *= steady;
        dyPix *= steady;
    }

    const float assistBlend = 0.72f + 0.28f * strength;
    dxPix *= assistBlend;
    dyPix *= assistBlend;

    m_lastDxPix = dxPix;
    m_lastDyPix = dyPix;
}

AimCalibration& AimCalibration::instance() {
    static AimCalibration inst;
    return inst;
}

bool AimCalibration::needsSetupPrompt() const {
    return !g_cfg.calibrationFrameworkComplete && !g_cfg.calibrationPromptDismissed && !m_active;
}

bool AimCalibration::isFrameworkComplete() const {
    return g_cfg.calibrationFrameworkComplete;
}

void AimCalibration::dismissSetupPrompt() {
    g_cfg.calibrationPromptDismissed = true;
}

bool AimCalibration::sessionComplete() const {
    if (!m_frameworkMode)
        return m_completed >= m_goal && m_goal > 0;
    return g_cfg.calibrationFrameworkComplete;
}

AimWeaponGroup AimCalibration::currentGroup() const {
    if (m_groupIndex < 0 || m_groupIndex >= kCalibrationGroupCount)
        return AimWeaponGroup::Other;
    return kCalibrationGroups[m_groupIndex];
}

const char* AimCalibration::phaseInstruction() const {
    if (!m_active)
        return "";
    switch (m_phase) {
    case CalibPhase::AwaitingWeapon:
        return "Equip the requested weapon class in-game.";
    case CalibPhase::Flick:
        return "Flick to each circle and hold crosshair on it briefly.";
    case CalibPhase::Rcs:
        return "Spray at a wall while holding fire to measure recoil.";
    default:
        return "";
    }
}

void AimCalibration::startFullCalibration() {
    enterCalibrationSafeMode();
    m_active = true;
    m_frameworkMode = true;
    m_phase = CalibPhase::AwaitingWeapon;
    m_groupIndex = 0;
    m_goal = std::clamp(g_cfg.aimCalibrationTargetsPerGroup, 5, 30);
    m_completed = 0;
    m_samples.clear();
    m_rcsSamples.clear();
    m_hasTarget = false;
    m_hasSpawnPlane = false;
    m_weaponMatchSinceMs = 0;
    m_profile = {};
    g_cfg.aimCalibrationActive = true;
    beginGroup(0);
}

void AimCalibration::startSession(int targetCount) {
    g_cfg.aimCalibrationTargetsPerGroup = std::clamp(targetCount, 5, 30);
    startFullCalibration();
}

void AimCalibration::beginGroup(int index) {
    m_groupIndex = index;
    m_completed = 0;
    m_samples.clear();
    m_rcsSamples.clear();
    m_hasTarget = false;
    m_hasSpawnPlane = false;
    m_weaponMatchSinceMs = 0;
    m_lastRcsShots = 0;
    m_rcsPeakShots = 0;
    m_rcsStartClip = -1;
    m_rcsMagEmptyAtMs = 0;
    m_phase = CalibPhase::AwaitingWeapon;
}

void AimCalibration::stopSession() {
    if (m_frameworkMode && m_groupIndex < kCalibrationGroupCount && !m_samples.empty()) {
        const AimGroupCalibProfile profile = buildGroupProfile(m_samples);
        applyGroupProfile(m_groupIndex, currentGroup(), profile);
    }
    cancelCalibration();
}

void AimCalibration::cancelCalibration() {
    m_active = false;
    m_frameworkMode = false;
    m_phase = CalibPhase::None;
    m_hasTarget = false;
    m_hasSpawnPlane = false;
    g_cfg.aimCalibrationActive = false;
    exitCalibrationSafeMode();
}

void AimCalibration::advancePhase() {
    if (m_phase == CalibPhase::AwaitingWeapon) {
        m_phase = CalibPhase::Flick;
        m_completed = 0;
        m_hasTarget = false;
        m_hasSpawnPlane = false;
        return;
    }
    if (m_phase == CalibPhase::Flick) {
        AimGroupCalibProfile profile = buildGroupProfile(m_samples);
        applyGroupProfile(m_groupIndex, currentGroup(), profile);
        if (weaponGroupSupportsRcs(currentGroup())) {
            m_phase = CalibPhase::Rcs;
            m_rcsPhaseStartMs = nowMs();
            m_lastRcsShots = 0;
            m_rcsPeakShots = 0;
            m_rcsStartClip = -1;
            m_rcsMagEmptyAtMs = 0;
            m_rcsSamples.clear();
            m_hasTarget = false;
        } else if (m_groupIndex + 1 >= kCalibrationGroupCount) {
            finishFramework();
        } else {
            beginGroup(m_groupIndex + 1);
        }
        return;
    }
    if (m_phase == CalibPhase::Rcs) {
        auto& stored = g_cfg.aimCalibByGroup[aimGroupIndex(currentGroup())];
        buildRcsProfile(stored, m_rcsSamples);
        applyGroupProfile(m_groupIndex, currentGroup(), stored);

        if (m_groupIndex + 1 >= kCalibrationGroupCount) {
            finishFramework();
            return;
        }
        beginGroup(m_groupIndex + 1);
    }
}

void AimCalibration::applyGroupProfile(int calibIndex, AimWeaponGroup group,
                                       const AimGroupCalibProfile& profile) {
    const std::size_t cfgIdx = aimGroupIndex(group);
    g_cfg.aimCalibByGroup[cfgIdx] = profile;

    if (!profile.valid)
        return;

    auto& aim = g_cfg.aimByWeaponGroup[cfgIdx];
    if (weaponGroupSupportsRcs(group)) {
        aim.rcsEnabled = true;
        aim.rcsMode = 1;
        aim.rcsX = profile.rcsX;
        aim.rcsY = profile.rcsY;
        aim.rcsSmooth = profile.rcsSmooth;
    } else {
        aim.rcsEnabled = false;
    }

    (void)calibIndex;
}

void AimCalibration::buildRcsProfile(AimGroupCalibProfile& profile,
                                     const std::vector<RcsSample>& rcsSamples) const {
    if (rcsSamples.size() < 3) {
        profile.rcsX = 0.85f;
        profile.rcsY = 0.85f;
        profile.rcsSmooth = 2.5f;
        return;
    }

    float pitchSum = 0.f;
    float yawSum = 0.f;
    float pitchVar = 0.f;
    for (const auto& s : rcsSamples) {
        pitchSum += std::fabsf(s.punchPitch);
        yawSum += std::fabsf(s.punchYaw);
    }
    const float n = static_cast<float>(rcsSamples.size());
    const float avgPitch = pitchSum / n;
    const float avgYaw = yawSum / n;
    for (const auto& s : rcsSamples) {
        const float dp = std::fabsf(s.punchPitch) - avgPitch;
        const float dy = std::fabsf(s.punchYaw) - avgYaw;
        pitchVar += dp * dp + dy * dy;
    }
    pitchVar = std::sqrtf(pitchVar / n);

    profile.measuredPunchPitch = avgPitch;
    profile.measuredPunchYaw = avgYaw;
    profile.rcsY = std::clamp(0.52f + avgPitch * 1.65f, 0.52f, 1.0f);
    profile.rcsX = std::clamp(0.45f + avgYaw * 1.35f, 0.45f, 0.95f);
    profile.rcsSmooth = std::clamp(1.8f + pitchVar * 2.2f, 1.8f, 5.5f);
}

AimGroupCalibProfile AimCalibration::buildGroupProfile(const std::vector<Sample>& samples) const {
    AimGroupCalibProfile profile{};
    if (samples.empty())
        return profile;

    float reactSum = 0.f;
    float flickMsSum = 0.f;
    float overSum = 0.f;
    float overMax = 0.f;
    float speedSum = 0.f;
    float jitterSum = 0.f;
    int flickSamples = 0;
    int hits = 0;
    int overSamples = 0;
    for (const auto& s : samples) {
        reactSum += s.reactionMs;
        if (s.flickMs > 1.f) {
            flickMsSum += s.flickMs;
            ++flickSamples;
        }
        if (s.hit && s.overshootDeg > 0.05f) {
            overSum += s.overshootDeg;
            overMax = (std::max)(overMax, s.overshootDeg);
            ++overSamples;
        }
        speedSum += s.flickDegPerSec;
        jitterSum += s.pathJitter;
        if (s.hit) ++hits;
    }

    const float n = static_cast<float>(samples.size());
    const float accuracy = static_cast<float>(hits) / n;
    const float rawReaction = reactSum / n;
    const float rawFlickMs = flickSamples > 0 ? flickMsSum / static_cast<float>(flickSamples) : 180.f;
    const float rawSmoothEst = (std::max)(1.5f, rawFlickMs * 0.11f);
    const float rawOvershoot = overSamples > 0
        ? overSum / static_cast<float>(overSamples)
        : overMax * 0.5f;
    const float rawSpeed = speedSum / n;
    const float rawJitter = jitterSum / n;
    const float help = std::clamp(0.40f + (1.f - accuracy) * 0.42f, 0.40f, 0.85f);
    const float snappiness = std::clamp(0.48f + accuracy * 0.22f, 0.48f, 0.72f);

    profile.valid = true;
    profile.sampleCount = static_cast<int>(samples.size());
    profile.accuracy = accuracy;
    profile.measuredReactionMs = rawReaction;
    profile.measuredSmooth = rawSmoothEst;
    profile.measuredOvershoot = rawOvershoot;
    profile.measuredFlickSpeed = rawSpeed;
    profile.measuredJitter = rawJitter;
    profile.assistReactionMs = std::clamp(
        rawReaction * (0.26f + 0.14f * accuracy), 14.f, rawReaction * 0.62f);
    profile.assistSmooth = std::clamp(
        rawSmoothEst * snappiness, 1.02f, rawSmoothEst * 0.74f);
    profile.assistFlickSpeed = std::clamp(
        rawSpeed * (1.04f + help * 0.12f), 340.f, 1580.f);
    profile.assistOvershoot = std::clamp(
        (std::max)(rawOvershoot * 0.28f, 0.35f), 0.35f, 1.4f);
    profile.assistJitter = std::clamp(rawJitter * 0.10f, 0.02f, 0.18f);
    profile.rcsX = 0.85f;
    profile.rcsY = 0.85f;
    profile.rcsSmooth = 2.5f;
    return profile;
}

void AimCalibration::syncGlobalFromGroups() {
    float react = 0.f, smooth = 0.f, speed = 0.f, acc = 0.f;
    int count = 0;
    for (int i = 0; i < kCalibrationGroupCount; ++i) {
        const auto& gp = g_cfg.aimCalibByGroup[aimGroupIndex(kCalibrationGroups[i])];
        if (!gp.valid)
            continue;
        react += gp.assistReactionMs;
        smooth += gp.assistSmooth;
        speed += gp.assistFlickSpeed;
        acc += gp.accuracy;
        ++count;
    }
    if (count == 0)
        return;

    g_cfg.aimStyleProfileValid = true;
    g_cfg.aimStyleReactionMs = react / count;
    g_cfg.aimStyleSmooth = smooth / count;
    g_cfg.aimStyleFlickSpeed = speed / count;
    g_cfg.aimStyleAccuracy = acc / count;
    g_cfg.aimStyleOvershoot = 0.4f;
    g_cfg.aimStyleJitter = 0.12f;
}

void AimCalibration::finishFramework() {
    syncGlobalFromGroups();
    g_cfg.calibrationFrameworkComplete = true;
    g_cfg.aimHumanizeEnabled = true;
    g_cfg.aimHumanizeMode = 0;
    g_cfg.aimHumanizeUseProfile = true;

    float accSum = 0.f;
    int valid = 0;
    for (int i = 0; i < kCalibrationGroupCount; ++i) {
        const auto& gp = g_cfg.aimCalibByGroup[aimGroupIndex(kCalibrationGroups[i])];
        if (gp.valid) {
            accSum += gp.accuracy;
            ++valid;
        }
    }
    if (valid > 0)
        g_cfg.aimCalibAssistStrength = std::clamp(0.50f + (accSum / valid) * 0.32f, 0.50f, 0.82f);

    syncAimStyleFromConfig();
    cancelCalibration();
}

void AimCalibration::ensureSpawnPlane(const EntityManager::Snapshot& snap,
                                      const PlayerData& local,
                                      const Vec3& eye) {
    if (m_hasSpawnPlane)
        return;

    m_spawnPlaneOrigin = local.origin;
    m_spawnPlaneZ = eye.z;

    Vec3 fwd{}, right{}, up{};
    cameraBasisFromViewMatrix(snap.viewMatrix, fwd, right, up);
    m_sessionViewFwdFlat = { fwd.x, fwd.y, 0.f };
    if (m_sessionViewFwdFlat.lengthSq() < 0.001f)
        m_sessionViewFwdFlat = { 1.f, 0.f, 0.f };
    else
        m_sessionViewFwdFlat = normalize3(m_sessionViewFwdFlat);

    m_hasSpawnPlane = true;
}

float AimCalibration::screenRadiusForTarget(const EntityManager::Snapshot& snap, const Vec3& eye) const {
    const float dist = (m_target.world - eye).length();
    float screenR = std::clamp(720.f / (dist + 180.f), 8.f, 26.f);
    Vec2 sc{};
    if (snap.viewMatrix.worldToScreen(m_target.world, sc, m_screenW, m_screenH)) {
        const Vec3 offset = m_target.world + normalize3({ snap.viewMatrix.m[0][0], snap.viewMatrix.m[0][1], snap.viewMatrix.m[0][2] }) * 36.f;
        Vec2 scOff{};
        if (snap.viewMatrix.worldToScreen(offset, scOff, m_screenW, m_screenH))
            screenR = (std::max)(screenR, std::hypotf(scOff.x - sc.x, scOff.y - sc.y) * 1.15f);
    }
    return screenR;
}

bool AimCalibration::crosshairOnTarget(const EntityManager::Snapshot& snap, const Target& t) const {
    Vec2 sc{};
    if (!snap.viewMatrix.worldToScreen(t.world, sc, m_screenW, m_screenH))
        return false;

    const float cx = m_screenW * 0.5f;
    const float cy = m_screenH * 0.5f;
    const float dx = sc.x - cx;
    const float dy = sc.y - cy;
    return std::hypotf(dx, dy) <= m_targetScreenR * 1.2f;
}

void AimCalibration::spawnTarget(const EntityManager::Snapshot& snap) {
    const PlayerData* local = nullptr;
    for (const auto& p : snap.players) {
        if (p.isValid && p.isLocalPlayer && p.isAlive) {
            local = &p;
            break;
        }
    }
    if (!local)
        return;

    Vec3 eye = local->headPos;
    if (eye.lengthSq() < 1.f)
        eye = local->origin + Vec3{ 0.f, 0.f, 64.f };

    ensureSpawnPlane(snap, *local, eye);

    const float dist = randRange(2400.f, 4200.f);
    const float yawOff = randRange(-18.f, 18.f) * kDeg2Rad;
    const Vec3 rightFlat{
        -m_sessionViewFwdFlat.y,
        m_sessionViewFwdFlat.x,
        0.f
    };
    Vec3 dirFlat = m_sessionViewFwdFlat * std::cosf(yawOff) + rightFlat * std::sinf(yawOff);
    dirFlat = normalize3(dirFlat);
    if (dirFlat.lengthSq() < 0.001f)
        dirFlat = m_sessionViewFwdFlat;

    m_target.world = {
        m_spawnPlaneOrigin.x + dirFlat.x * dist,
        m_spawnPlaneOrigin.y + dirFlat.y * dist,
        m_spawnPlaneZ
    };
    m_target.angularRadius = std::clamp(2.4f - dist / 900.f, 0.8f, 1.8f);
    m_target.spawnMs = nowMs();
    m_target.hit = false;
    m_hasTarget = true;
    m_onTargetSinceMs = 0;

    Vec3 fwd{}, right{}, up{};
    cameraBasisFromViewMatrix(snap.viewMatrix, fwd, right, up);
    m_spawnViewFwd = fwd;
    m_lastViewFwd = fwd;
    m_spawnViewAngles = { local->eyePitch, local->eyeYaw, 0.f };
    m_lastViewAngles = m_spawnViewAngles;
    m_reactionMarked = false;
    m_reactionMs = 0.f;
    m_flickStartMs = 0;
    m_peakOvershoot = 0.f;
    m_pathJitter = 0.f;
    m_bestOnTargetAng = 999.f;
    m_targetScreenR = screenRadiusForTarget(snap, eye);
}

void AimCalibration::finalizeSample(bool hit) {
    Sample s;
    s.hit = hit;
    s.reactionMs = m_reactionMs;
    if (m_flickStartMs > 0 && m_target.spawnMs > 0) {
        s.flickMs = static_cast<float>(nowMs() - m_flickStartMs);
        if (s.flickMs > 1.f) {
            const float travel = angleBetweenDeg(m_spawnViewFwd, m_lastViewFwd);
            s.flickDegPerSec = travel / (s.flickMs / 1000.f);
        }
    }
    s.overshootDeg = m_peakOvershoot;
    s.pathJitter = m_pathJitter;
    m_samples.push_back(s);

    if (hit)
        ++m_completed;

    m_hasTarget = false;
    m_onTargetSinceMs = 0;
    m_liveReactionMs = s.reactionMs;
    m_liveFlickSpeed = s.flickDegPerSec;

    if (m_completed >= m_goal)
        advancePhase();
}

void AimCalibration::updateAwaitWeapon(const Process& proc, const EntityManager& em) {
    const std::uintptr_t clientBase = em.clientBase();
    const std::uintptr_t localPawn = em.localPawn();
    if (!clientBase || !localPawn)
        return;

    static bool s_spaceWasDown = false;
    const bool spaceDown = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
    const bool spacePressed = spaceDown && !s_spaceWasDown;
    s_spaceWasDown = spaceDown;
    if (spacePressed) {
        advancePhase();
        return;
    }

    const AimWeaponGroup expected = currentGroup();
    const AimWeaponGroup active = resolveActiveWeaponGroup(proc, clientBase, localPawn);

    if (active == expected) {
        if (m_weaponMatchSinceMs == 0)
            m_weaponMatchSinceMs = nowMs();
        if (nowMs() - m_weaponMatchSinceMs >= 450ull)
            advancePhase();
    } else {
        m_weaponMatchSinceMs = 0;
    }
}

void AimCalibration::updateFlick(const EntityManager::Snapshot& snap) {
    if (!m_hasTarget)
        spawnTarget(snap);
    if (!m_hasTarget)
        return;

    const PlayerData* local = nullptr;
    for (const auto& p : snap.players) {
        if (p.isValid && p.isLocalPlayer && p.isAlive) {
            local = &p;
            break;
        }
    }
    if (!local)
        return;

    Vec3 viewFwd{}, right{}, up{};
    cameraBasisFromViewMatrix(snap.viewMatrix, viewFwd, right, up);

    Vec3 eye = local->headPos;
    if (eye.lengthSq() < 1.f)
        eye = local->origin + Vec3{ 0.f, 0.f, 64.f };

    const Vec3 toTargetDir = normalize3(m_target.world - eye);
    const float onTargetAng = angleBetweenDeg(viewFwd, toTargetDir);

    const float moved = angleBetweenDeg(m_lastViewFwd, viewFwd);
    if (moved > 0.08f)
        m_pathJitter += moved * 0.04f;

    if (!m_reactionMarked && moved > 0.2f) {
        m_reactionMarked = true;
        m_reactionMs = static_cast<float>(nowMs() - m_target.spawnMs);
        m_flickStartMs = nowMs();
    }

    if (m_flickStartMs > 0) {
        if (onTargetAng < m_bestOnTargetAng)
            m_bestOnTargetAng = onTargetAng;
        if (m_bestOnTargetAng < 2.5f && onTargetAng > m_bestOnTargetAng + 0.25f)
            m_peakOvershoot = (std::max)(m_peakOvershoot, onTargetAng - m_bestOnTargetAng);
    }

    m_lastViewFwd = viewFwd;
    m_lastViewAngles = { local->eyePitch, local->eyeYaw, 0.f };

    if (crosshairOnTarget(snap, m_target)) {
        if (m_onTargetSinceMs == 0)
            m_onTargetSinceMs = nowMs();
        if (nowMs() - m_onTargetSinceMs >= 45ull)
            finalizeSample(true);
    } else {
        m_onTargetSinceMs = 0;
    }

    if (nowMs() - m_target.spawnMs > 5000ull)
        finalizeSample(false);
}

void AimCalibration::updateRcs(const Process& proc, const EntityManager& em) {
    const std::uintptr_t localPawn = em.localPawn();
    if (!localPawn)
        return;

    const std::uintptr_t clientBase = em.clientBase();
    const int clip = readActiveWeaponClip(proc, clientBase, localPawn);
    if (m_rcsStartClip < 0 && clip > 0)
        m_rcsStartClip = clip;

    const int shotsFired = mem::read<int>(proc, localPawn + netvars::pawn::m_iShotsFired);
    if (shotsFired > m_rcsPeakShots)
        m_rcsPeakShots = shotsFired;

    if (shotsFired > 1 && shotsFired > m_lastRcsShots) {
        const Vec3 punch = readAimPunch(proc, localPawn);
        if (std::isfinite(punch.x) && std::isfinite(punch.y)) {
            RcsSample s;
            s.punchPitch = punch.x;
            s.punchYaw = punch.y;
            s.shotsFired = shotsFired;
            m_rcsSamples.push_back(s);
        }
        m_lastRcsShots = shotsFired;
    }

    const bool magEmpty = clip == 0 && m_rcsStartClip > 0
        && m_rcsPeakShots >= (std::max)(3, m_rcsStartClip - 1);
    if (magEmpty) {
        if (m_rcsMagEmptyAtMs == 0)
            m_rcsMagEmptyAtMs = nowMs();
    } else {
        m_rcsMagEmptyAtMs = 0;
    }

    const bool enoughSamples = static_cast<int>(m_rcsSamples.size()) >= 5;
    const bool timedOut = nowMs() - m_rcsPhaseStartMs >= 90000ull;
    const bool magDone = magEmpty && m_rcsMagEmptyAtMs != 0
        && (nowMs() - m_rcsMagEmptyAtMs >= 400ull || shotsFired <= 1);

    if ((magDone && enoughSamples) || (magDone && m_rcsPeakShots >= 8) || timedOut)
        advancePhase();
}

void AimCalibration::update(const Process& proc, const EntityManager& em, float dtMs) {
    (void)dtMs;
    if (!m_active)
        return;

    const auto snap = em.snapshot();

    switch (m_phase) {
    case CalibPhase::AwaitingWeapon:
        updateAwaitWeapon(proc, em);
        break;
    case CalibPhase::Flick:
        updateFlick(snap);
        break;
    case CalibPhase::Rcs:
        updateRcs(proc, em);
        break;
    default:
        break;
    }
}

void AimCalibration::render(Renderer& r, const EntityManager::Snapshot& snap, const FontAtlas* font) {
    if (!m_active)
        return;

    m_screenW = static_cast<float>(r.screenWidth());
    m_screenH = static_cast<float>(r.screenHeight());

    if (m_phase == CalibPhase::Flick && m_hasTarget) {
        const auto& vm = snap.viewMatrix;
        Vec2 sc{};
        if (vm.worldToScreen(m_target.world, sc, m_screenW, m_screenH)) {
            const float screenR = m_targetScreenR;
            const unsigned int accent = Theme::ACCENT;
            const unsigned int accentSoft = withAlpha(accent, 0x28);
            const unsigned int accentMid = withAlpha(accent, 0x90);
            const unsigned int accentRing = withAlpha(accent, 0xE8);

            r.drawFilledCircle(sc.x, sc.y, screenR * 1.55f, accentSoft);
            r.drawCircle(sc.x, sc.y, screenR * 1.12f, accentMid, 1.25f);
            r.drawCircle(sc.x, sc.y, screenR, accentRing, 2.f);
            r.drawFilledCircle(sc.x, sc.y, screenR * 0.22f, accentRing);
            r.drawFilledCircle(sc.x, sc.y, screenR * 0.08f, 0xCCFFFFFF);

            if (font) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "%d / %d", m_completed, m_goal);
                const float tw = static_cast<float>(std::strlen(buf)) * 7.2f;
                r.drawText(*font, sc.x - tw * 0.5f, sc.y - screenR - 22.f, buf, Theme::TEXT, 13.f);
            }
        }
    }

    if (!font)
        return;

    const char* phaseName = "";
    switch (m_phase) {
    case CalibPhase::AwaitingWeapon: phaseName = "Equip weapon"; break;
    case CalibPhase::Flick:          phaseName = "Flick targets"; break;
    case CalibPhase::Rcs:            phaseName = "RCS spray"; break;
    default: break;
    }

    char instruction[256];
    char hint[160];
    float progress = 0.f;
    const bool groupHasRcs = weaponGroupSupportsRcs(currentGroup());
    if (m_phase == CalibPhase::AwaitingWeapon) {
        std::snprintf(instruction, sizeof(instruction), "Equip %s", aimWeaponGroupEquipHint(currentGroup()));
        std::snprintf(hint, sizeof(hint), "Auto-starts when equipped  -  press SPACE to skip wait");
        progress = 0.f;
    } else if (m_phase == CalibPhase::Flick) {
        std::snprintf(instruction, sizeof(instruction), "Flick to %d targets", m_goal);
        std::snprintf(hint, sizeof(hint), "%d / %d complete  -  RT %.0f ms  -  %.0f deg/s",
            m_completed, m_goal, m_liveReactionMs, m_liveFlickSpeed);
        const float flickT = m_goal > 0 ? static_cast<float>(m_completed) / static_cast<float>(m_goal) : 0.f;
        progress = groupHasRcs ? flickT * 0.5f : flickT;
    } else {
        std::snprintf(instruction, sizeof(instruction), "Empty the full magazine at a wall");
        std::snprintf(hint, sizeof(hint), "%zu samples  -  %d / %d shots",
            m_rcsSamples.size(), m_rcsPeakShots, m_rcsStartClip > 0 ? m_rcsStartClip : 0);
        float rcsT = 0.f;
        if (m_rcsStartClip > 0)
            rcsT = std::clamp(static_cast<float>(m_rcsPeakShots) / static_cast<float>(m_rcsStartClip), 0.f, 1.f);
        progress = groupHasRcs ? (0.5f + rcsT * 0.5f) : 1.f;
    }

    const float uiScale = std::clamp(m_screenW / 1920.f, 0.85f, 1.15f);
    const float kBannerW = std::floor(560.f * uiScale);
    const float kBannerH = std::floor(252.f * uiScale);
    const float kCorner = std::floor(12.f * uiScale);
    const float bannerX = std::floor((m_screenW - kBannerW) * 0.5f);
    const float bannerY = std::floor(m_screenH * 0.038f);

    for (int i = 0; i < 2; ++i) {
        const float spread = std::floor((3.f + i * 3.f) * uiScale);
        const unsigned int alpha = static_cast<unsigned int>(14.f - i * 5.f);
        r.drawRoundedFilledRect(
            bannerX - spread, bannerY - spread * 0.9f,
            kBannerW + spread * 2.f, kBannerH + spread * 2.05f,
            withAlpha(0xFF000000, alpha), kCorner + spread * 0.64f);
    }

    r.drawRoundedFilledRect(bannerX, bannerY, kBannerW, kBannerH, 0xFF0A0D15, kCorner);
    r.drawRoundedRect(bannerX, bannerY, kBannerW, kBannerH, 0xFF2B3041, kCorner, 1.f);

    const float pad = std::floor(18.f * uiScale);
    const float headerY = bannerY + pad;
    r.drawText(*font, bannerX + pad, headerY, "Calibration", Theme::TEXT_MUTED, std::floor(17.f * uiScale));
    r.drawText(*font, bannerX + pad, headerY + std::floor(22.f * uiScale),
        aimWeaponGroupLabel(currentGroup()), Theme::TEXT, std::floor(26.f * uiScale));

    char stepBuf[32];
    std::snprintf(stepBuf, sizeof(stepBuf), "%d / %d", m_groupIndex + 1, kCalibrationGroupCount);
    const float stepW = std::strlen(stepBuf) * 10.f + 26.f;
    const float stepX = bannerX + kBannerW - pad - stepW;
    const float stepY = headerY + 12.f;
    r.drawRoundedFilledRect(stepX, stepY, stepW, 30.f, withAlpha(Theme::ACCENT, 36), 7.f);
    r.drawRoundedRect(stepX, stepY, stepW, 30.f, withAlpha(Theme::ACCENT, 120), 7.f, 1.f);
    r.drawText(*font, stepX + 12.f, stepY + 6.f, stepBuf, Theme::ACCENT, 17.f * uiScale);

    const float dotsY = bannerY + 66.f * uiScale;
    const float dotGap = 8.f;
    const float dotW = (kBannerW - pad * 2.f - dotGap * (kCalibrationGroupCount - 1)) / kCalibrationGroupCount;
    for (int i = 0; i < kCalibrationGroupCount; ++i) {
        const float dx = bannerX + pad + i * (dotW + dotGap);
        const bool done = i < m_groupIndex;
        const bool active = i == m_groupIndex;
        unsigned int dotBg = done ? withAlpha(Theme::ACCENT, 90)
            : active ? withAlpha(Theme::ACCENT, 45) : 0xFF171927;
        unsigned int dotBorder = done ? Theme::ACCENT
            : active ? withAlpha(Theme::ACCENT, 180) : 0xFF2A3040;
        r.drawRoundedFilledRect(dx, dotsY, dotW, 6.f, dotBg, 3.f);
        if (active)
            r.drawRoundedFilledRect(dx, dotsY, dotW * progress, 6.f, Theme::ACCENT, 3.f);
        r.drawRoundedRect(dx, dotsY, dotW, 6.f, dotBorder, 3.f, 1.f);
    }

    const float sepY = dotsY + 16.f * uiScale;
    r.drawFilledRect(bannerX + pad, sepY, kBannerW - pad * 2.f, 1.f, 0xFF2B3041);

    const float phaseW = std::strlen(phaseName) * 9.2f + 24.f;
    r.drawRoundedFilledRect(bannerX + pad, sepY + 12.f, phaseW, 28.f, withAlpha(Theme::ACCENT, 28), 6.f);
    r.drawText(*font, bannerX + pad + 10.f, sepY + 17.f * uiScale, phaseName, Theme::ACCENT, 16.f * uiScale);

    r.drawText(*font, bannerX + pad, sepY + std::floor(48.f * uiScale), instruction, Theme::TEXT, std::floor(20.f * uiScale));
    r.drawText(*font, bannerX + pad, sepY + std::floor(78.f * uiScale), hint, Theme::TEXT_MUTED, std::floor(16.f * uiScale));

    const float footY = bannerY + kBannerH - std::floor(34.f * uiScale);
    r.drawRoundedFilledRect(bannerX + pad, footY, kBannerW - pad * 2.f, std::floor(26.f * uiScale), 0xFF11131C, 6.f * uiScale);
    r.drawText(*font, bannerX + pad + 12.f, footY + std::floor(5.f * uiScale),
        "Combat features paused during calibration.", Theme::TEXT_MUTED, std::floor(15.f * uiScale));
}
