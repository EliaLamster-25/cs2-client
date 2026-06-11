#pragma once

#include "math/vector.h"
#include "overlay/renderer.h"
#include "game/entity_manager.h"
#include "game/weapon_group.h"
#include "config.h"

#include <cstdint>
#include <vector>

class Process;

struct AimStyleProfile {
    bool  valid = false;
    int   sampleCount = 0;
    float accuracy = 0.f;

    // Raw measurements from calibration session.
    float measuredReactionMs = 0.f;
    float measuredSmooth = 0.f;
    float measuredOvershootDeg = 0.f;
    float measuredFlickDegPerSec = 0.f;
    float measuredJitter = 0.f;

    // Assist tuning derived from measurements (snappier / steadier than raw human).
    float avgReactionMs = 165.f;
    float avgSmooth = 6.f;
    float avgOvershootDeg = 1.2f;
    float avgFlickDegPerSec = 680.f;
    float avgJitter = 0.35f;
};

AimStyleProfile aimStyleProfileFromGroupCalib(const AimGroupCalibProfile& group);

class AimHumanizer {
public:
    static AimHumanizer& instance();

    void resetTargetState();
    void apply(float& dxPix, float& dyPix,
               float pitchStepDeg, float yawStepDeg,
               bool hasTarget, float dtMs);

    void setProfile(const AimStyleProfile& profile);
    const AimStyleProfile& profile() const { return m_profile; }

private:
    AimHumanizer() = default;

    float reactionMs() const;
    float overshootFrac() const;
    float jitterPx() const;
    float maxStepPx(float dtMs) const;

    AimStyleProfile m_profile{};
    bool  m_hasTarget = false;
    bool  m_reactionReady = false;
    std::uint64_t m_acquireMs = 0;
    float m_overshootPhase = 0.f;
    float m_noiseT = 0.f;
    float m_lastNoiseX = 0.f;
    float m_lastNoiseY = 0.f;
    std::uint64_t m_targetLostMs = 0;

    float m_trackErrMag = 999.f;
    float m_minErrMag = 999.f;
    int   m_overshootFrames = 0;
    float m_lastDxPix = 0.f;
    float m_lastDyPix = 0.f;

    float effectiveStrength() const;
};

enum class CalibPhase {
    None,
    AwaitingWeapon,
    Flick,
    Rcs,
};

class AimCalibration {
public:
    static AimCalibration& instance();

    void update(const Process& proc, const EntityManager& em, float dtMs);
    void render(Renderer& r, const EntityManager::Snapshot& snap, const class FontAtlas* font);

    void startFullCalibration();
    void startSession(int targetCount);
    void stopSession();
    void cancelCalibration();
    void dismissSetupPrompt();

    bool isActive() const { return m_active; }
    bool isFrameworkMode() const { return m_frameworkMode; }
    bool needsSetupPrompt() const;
    bool isFrameworkComplete() const;

    CalibPhase phase() const { return m_phase; }
    int groupIndex() const { return m_groupIndex; }
    int groupCount() const { return kCalibrationGroupCount; }
    AimWeaponGroup currentGroup() const;
    const char* phaseInstruction() const;

    const AimStyleProfile& lastProfile() const { return m_profile; }
    int completedTargets() const { return m_completed; }
    int targetGoal() const { return m_goal; }
    bool sessionComplete() const;

    float lastReactionMs() const { return m_liveReactionMs; }
    float lastFlickSpeed() const { return m_liveFlickSpeed; }

private:
    struct Target {
        Vec3 world{};
        float angularRadius = 1.5f;
        std::uint64_t spawnMs = 0;
        bool hit = false;
    };

    struct Sample {
        float reactionMs = 0.f;
        float flickMs = 0.f;
        float flickDegPerSec = 0.f;
        float overshootDeg = 0.f;
        float pathJitter = 0.f;
        bool hit = false;
    };

    struct RcsSample {
        float punchPitch = 0.f;
        float punchYaw = 0.f;
        int   shotsFired = 0;
    };

    void beginGroup(int index);
    void advancePhase();
    void finishFramework();
    void applyGroupProfile(int calibIndex, AimWeaponGroup group, const AimGroupCalibProfile& profile);

    void spawnTarget(const EntityManager::Snapshot& snap);
    void finalizeSample(bool hit);
    AimGroupCalibProfile buildGroupProfile(const std::vector<Sample>& samples) const;
    void buildRcsProfile(AimGroupCalibProfile& profile, const std::vector<RcsSample>& rcsSamples) const;
    void syncGlobalFromGroups();

    bool crosshairOnTarget(const EntityManager::Snapshot& snap, const Target& t) const;
    float screenRadiusForTarget(const EntityManager::Snapshot& snap, const Vec3& eye) const;
    void ensureSpawnPlane(const EntityManager::Snapshot& snap, const PlayerData& local, const Vec3& eye);

    void updateAwaitWeapon(const Process& proc, const EntityManager& em);
    void updateFlick(const EntityManager::Snapshot& snap);
    void updateRcs(const Process& proc, const EntityManager& em);

    bool m_active = false;
    bool m_frameworkMode = false;
    CalibPhase m_phase = CalibPhase::None;
    int m_groupIndex = 0;
    int m_goal = 8;
    int m_completed = 0;
    Target m_target{};
    bool m_hasTarget = false;

    Vec3 m_spawnViewAngles{};
    Vec3 m_lastViewAngles{};
    bool m_reactionMarked = false;
    float m_reactionMs = 0.f;
    std::uint64_t m_flickStartMs = 0;
    float m_peakOvershoot = 0.f;
    float m_pathJitter = 0.f;
    float m_liveReactionMs = 0.f;
    float m_liveFlickSpeed = 0.f;

    float m_screenW = 1920.f;
    float m_screenH = 1080.f;
    float m_targetScreenR = 22.f;
    std::uint64_t m_onTargetSinceMs = 0;
    Vec3 m_lastViewFwd{};
    Vec3 m_spawnViewFwd{};
    float m_bestOnTargetAng = 999.f;

    bool m_hasSpawnPlane = false;
    Vec3 m_spawnPlaneOrigin{};
    float m_spawnPlaneZ = 0.f;
    Vec3 m_sessionViewFwdFlat{};

    std::uint64_t m_weaponMatchSinceMs = 0;
    std::uint64_t m_rcsPhaseStartMs = 0;
    int m_lastRcsShots = 0;
    int m_rcsPeakShots = 0;
    int m_rcsStartClip = -1;
    std::uint64_t m_rcsMagEmptyAtMs = 0;

    std::vector<Sample> m_samples;
    std::vector<RcsSample> m_rcsSamples;
    AimStyleProfile m_profile{};
};

/// When calibration is active, cheat features should stand down.
bool aimCalibrationBlocksFeatures();

void syncAimStyleFromConfig();
void syncAimStyleForWeaponGroup(AimWeaponGroup group);
