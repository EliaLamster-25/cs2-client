#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <string>

enum class AimWeaponGroup : int {
    Pistols = 0,
    Heavy,
    Smg,
    Rifles,
    Snipers,
    Other,
    Count
};

struct AimGroupCalibProfile {
    bool  valid = false;
    float accuracy = 0.f;
    int   sampleCount = 0;
    float measuredReactionMs = 0.f;
    float measuredSmooth = 0.f;
    float measuredFlickSpeed = 0.f;
    float measuredJitter = 0.f;
    float measuredOvershoot = 0.f;
    float measuredPunchPitch = 0.f;
    float measuredPunchYaw = 0.f;
    float assistReactionMs = 0.f;
    float assistSmooth = 0.f;
    float assistFlickSpeed = 0.f;
    float assistJitter = 0.f;
    float assistOvershoot = 0.f;
    float rcsX = 1.f;
    float rcsY = 1.f;
    float rcsSmooth = 2.5f;
};

struct AimGroupConfig {
    int   aimBone        = 6;      // 6=head, 2=stomach(spine), 4=chest
    bool  hitboxHead     = true;
    bool  hitboxStomach  = false;
    bool  hitboxChest    = false;
    bool  hitboxPelvis   = false;
    bool  hitboxArms     = false;
    bool  hitboxLegs     = false;
    float aimFov         = 8.f;    // maximum acquisition FOV in degrees
    float aimSmooth      = 12.f;   // smoothing factor (higher = slower/smoother)
    bool  rcsEnabled     = true;   // enable recoil compensation for this weapon group
    int   rcsMode        = 1;      // 0 = Aim only, 1 = Standalone
    float rcsX           = 1.0f;   // horizontal recoil compensation scale (0..1.25)
    float rcsY           = 1.0f;   // vertical recoil compensation scale (0..1.25)
    float rcsSmooth      = 2.5f;   // recoil smoothing (higher = slower; 0 = instant)
    bool  triggerEnabled = false;  // enable triggerbot for this weapon group
    int   triggerDelayMs = 10;     // delay before first shot when crosshair enters target
    int   triggerShotCooldownMs = 80; // minimum ms between consecutive shots
    int   triggerKey     = 0;      // 0 = always active while enabled; else hold VK
};

inline constexpr std::size_t kAimWeaponGroupCount = static_cast<std::size_t>(AimWeaponGroup::Count);
inline constexpr std::size_t aimGroupIndex(AimWeaponGroup group) {
    return static_cast<std::size_t>(group);
}

/// @file config.h
/// @brief Runtime-mutable overlay settings (toggled via the menu).

struct OverlayConfig {
    // ── ESP toggles ────────────────────────────────────────────────────────────
    bool espEnabled          = true;
    bool boxEnabled          = true;
    bool boxOccluded         = true;
    bool hpBarEnabled        = true;
    bool hpBarOccluded       = true;
    bool hpTextEnabled       = false;
    bool hpTextVisibleEnabled = true;
    bool hpTextOccludedEnabled = true;
    bool skeletonEnabled     = false;
    bool skeletonOccluded    = true;
    bool chamsEnabled        = false;
    bool chamsOccluded       = true;
    int  chamsStyle          = 0;    // 0=wireframe GLB, 1=solid GLB, 2=bone silhouette, 3=capsules
    float chamsOutlineThickness = 2.f;
    bool nameEspEnabled      = true;
    bool nameEspAvatarEnabled = true;
    bool armorEspEnabled     = true;
    bool armorVisibleEnabled = true;
    bool armorOccludedEnabled = true;
    bool weaponEspEnabled    = true;
    bool weaponVisibleEnabled = true;
    bool weaponOccludedEnabled = true;
    bool weaponTextEnabled   = true;
    bool weaponIconEnabled   = false;
    int  weaponEspMode       = 0; // 0 = text, 1 = icon
    int  armorEspMode        = 0; // 0 = text, 1 = bar
    bool armorTextEnabled    = true;
    bool armorBarEnabled     = false;
    bool ammoEspEnabled      = true;
    bool ammoVisibleEnabled  = true;
    bool ammoOccludedEnabled = true;
    bool ammoTextEnabled     = true;
    bool ammoBarEnabled      = false;
    int  ammoEspMode         = 0; // 0 = text, 1 = bar
    bool flagsEspEnabled     = true;
    bool flagsVisibleEnabled = true;
    bool flagsOccludedEnabled = true;
    bool flagFlashedEnabled  = true;
    bool flagDefusingEnabled = true;
    bool flagScopedEnabled   = true;
    bool flagDefuseKitEnabled = true;
    bool enemyOnly           = false;

    // ── Box colours (ARGB) ─────────────────────────────────────────────────────
    float enemyColor[4]   = { 1.0f, 0.27f, 0.27f, 1.0f }; // red
    float teamColor[4]    = { 0.27f, 1.0f, 0.27f, 1.0f }; // green
    float enemyVisibleColor[4]   = { 1.0f, 0.27f, 0.27f, 1.0f };
    float enemyOccludedColor[4]  = { 1.0f, 0.27f, 0.27f, 0.35f };
    float teamVisibleColor[4]    = { 0.27f, 1.0f, 0.27f, 1.0f };
    float teamOccludedColor[4]   = { 0.27f, 1.0f, 0.27f, 0.35f };
    float boxVisibleColor[4]     = { 1.0f, 0.27f, 0.27f, 1.0f };
    float boxOccludedColor[4]    = { 1.0f, 0.27f, 0.27f, 0.35f };
    float skeletonVisibleColor[4]= { 1.0f, 1.0f, 0.0f, 0.85f };
    float skeletonOccludedColor[4]= { 1.0f, 1.0f, 0.0f, 0.45f };
    float hpBarVisibleColor[4]   = { 0.0f, 1.0f, 0.0f, 1.0f };
    float hpBarOccludedColor[4]  = { 0.0f, 1.0f, 0.0f, 0.55f };
    float chamsVisibleColor[4]   = { 1.0f, 0.27f, 0.27f, 1.0f };
    float chamsOccludedColor[4]  = { 1.0f, 0.27f, 0.27f, 0.55f };
    float infoTextColor[4]       = { 1.0f, 1.0f, 1.0f, 0.98f };
    float armorBarColor[4]       = { 0.25f, 0.70f, 1.0f, 0.95f };
    float ammoBarColor[4]        = { 1.0f, 0.85f, 0.24f, 0.95f };
    float dormantColor[4] = { 0.53f, 0.53f, 0.53f, 0.55f };
    float boxThickness    = 1.5f;
    float boxWidthScale   = 1.0f;   ///< Multiplier on ESP box width (1.0 = default fit)
    float infoTextSize    = 13.f;

    // Anchor mapping for ESP info elements:
    // 0=Top, 1=Bottom, 2=Left, 3=Right,
    // 4=TopLeft, 5=TopRight, 6=BottomLeft, 7=BottomRight
    int   hpBarPosVisible   = -1;
    int   hpBarPosOccluded  = -1;
    int   hpTextPosVisible  = -1;
    int   hpTextPosOccluded = -1;
    int   namePosVisible    = -1;
    int   namePosOccluded   = -1;
    int   weaponPosVisible  = -1;
    int   weaponPosOccluded = -1;
    int   weaponTextPosVisible  = -1;
    int   weaponTextPosOccluded = -1;
    int   weaponIconPosVisible  = -1;
    int   weaponIconPosOccluded = -1;
    int   armorPosVisible   = -1;
    int   armorPosOccluded  = -1;
    int   armorTextPosVisible  = -1;
    int   armorTextPosOccluded = -1;
    int   armorBarPosVisible   = -1;
    int   armorBarPosOccluded  = -1;
    int   ammoPosVisible    = -1;
    int   ammoPosOccluded   = -1;
    int   ammoTextPosVisible  = -1;
    int   ammoTextPosOccluded = -1;
    int   ammoBarPosVisible   = -1;
    int   ammoBarPosOccluded  = -1;
    int   flagsPosVisible   = -1;
    int   flagsPosOccluded  = -1;

    // Per-anchor pixel offsets applied to ESP components after anchor placement.
    // Index mapping: 0=Top, 1=Bottom, 2=Left, 3=Right,
    //                4=TopLeft, 5=TopRight, 6=BottomLeft, 7=BottomRight
    std::array<float, 8> espAnchorOffsetX{};
    std::array<float, 8> espAnchorOffsetY{};

    // Draw order for anchor-able ESP elements (lower = drawn first / closer to box)
    // Indices: 0=HpBar 1=HpText 2=Name 3=WeaponText 4=WeaponIcon
    //          5=ArmorBar 6=ArmorText 7=AmmoBar 8=AmmoText 9=Flags
    std::array<int, 10> espItemOrderVisible = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    std::array<int, 10> espItemOrderOccluded = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};

    // ── HP bar ─────────────────────────────────────────────────────────────────
    float hpBarWidth        = 4.0f;
    float hpBarLowColor[4]  = { 1.0f, 0.0f, 0.0f, 1.0f }; // red   (≤20 % HP)
    float hpBarFullColor[4] = { 0.0f, 1.0f, 0.0f, 1.0f }; // green (100 % HP)

    // ── Skeleton colour ───────────────────────────────────────────────────────
    float skeletonColor[4] = { 1.0f, 1.0f, 0.0f, 0.85f }; // yellow

    // ── Visibility Check ──────────────────────────────────────────────────────
    bool  visibilityCheckEnabled = false;
    bool  aimRequireVisibility   = false;
    int   aimVisibilityMode      = 0; // 0=Performance, 1=Accuracy
    int   visibilityMode         = 1; // 0=Fast, 1=Balanced, 2=Strict
    int   visibilityBackend      = 0; // 0=BSP (safe), 1=Game TraceShape, 2=Auto — TraceShape needs Win32 handle

    /// 0 = Win32 RPM (debug/fallback), 1 = kernel driver (default, anti-detection)
    int   memoryBackend          = 1;
    bool  memoryAllowWin32Fallback = false;
    /// 0 = SendInput, 1 = NtUser inject via win32u (default with kernel mode)
    int   inputBackend           = 1;
    int   visibilityLatchFrames  = 2;
    float visMaxDistance         = 0.f;  // 0=default per mode, else max dist in units
    int   visBudgetMs            = 12;   // cumulative budget cap for BSP sweeps (3-30)
    int   visEvalBase            = 0;    // 0=auto (distance-based), 1-5=forced eval stride

    // ── Chams alpha ───────────────────────────────────────────────────────────
    float chamsAlpha   = 0.55f;  // 0-1 opacity of the filled player silhouette

    // ── Grenades ───────────────────────────────────────────────────────────────
    bool  grenadeEnabled    = true;
    bool  grenadeTrajectory = true;
    bool  grenadeHelperEnabled = true;
    bool  grenadeHelperTestSpot = true;
    int   grenadeHelperSpotIndex = -1; // -1 = auto nearest
    std::string grenadeLineupPackPath;
    std::string grenadeLineupCloudId;
    /// Sound ESP — rings at gunshot / footstep locations.
    bool  soundEspEnabled = false;
    bool  soundEspGunshots = true;
    bool  soundEspFootsteps = true;
    bool  soundEspVisibleEnabled = true;
    bool  soundEspOccludedEnabled = true;
    float soundEspLineThickness = 1.5f;
    float soundEspRingExpand = 42.f;
    float soundEspGunshotColor[4] = { 1.00f, 0.40f, 0.27f, 0.85f };
    float soundEspFootstepColor[4] = { 0.53f, 0.67f, 1.00f, 0.75f };
    int   soundEspMode = 0; // 0 = 2D screen rings, 1 = 3D ground rings
    /// Cloud config sync (requires CRYMORE_ACCESS_TOKEN from loader).
    bool  cloudConfigEnabled = true;
    std::string cloudActiveConfigId;
    /// Pixels inset from the screen edge for off-screen grenade landing indicators (higher = closer to center).
    float grenadeOffscreenInset = 80.f;
    /// Per-type RGBA colors — index matches GrenadeType (HE, Smoke, Flash, Molotov, Decoy).
    float grenadeColors[5][4] = {
        { 1.00f, 0.24f, 0.24f, 1.00f }, // HE
        { 0.67f, 0.67f, 0.67f, 1.00f }, // Smoke
        { 1.00f, 1.00f, 0.78f, 1.00f }, // Flash
        { 1.00f, 0.55f, 0.00f, 1.00f }, // Molotov
        { 0.78f, 0.78f, 0.00f, 1.00f }, // Decoy
    };
    float grenadePreThrowColor[4] = { 0.31f, 1.00f, 0.47f, 0.86f };
    float grenadeDangerColor[4]   = { 1.00f, 0.32f, 0.32f, 1.00f }; // HE landing in damage radius
    float grenadeTrajectoryAlpha  = 0.97f;
    bool  bombTimerEnabled  = true;
    bool  spectatorListEnabled = true;
    bool  radarEnabled      = true;
    bool  webRadarEnabled   = false;
    int   webRadarPublishMs = 15;
    std::string webRadarSessionId;
    std::string webRadarShareUrl;
    int   radarMode         = 0;      // 0 = Manual, 1 = In-game overlay
    float radarPosX         = 0.82f;   // normalized top-left radar X in [0,1]
    float radarPosY         = 0.06f;   // normalized top-left radar Y in [0,1]
    float radarSize         = 210.f;
    float radarRange        = 1400.f;
    float radarBlipSize     = 3.4f;
    float radarBgOpacity    = 0.92f;   // background opacity in manual mode
    float spectatorPosX     = -1.f;    // normalized top-left spectator list X in [0,1], -1 = default
    float spectatorPosY     = -1.f;    // normalized top-left spectator list Y in [0,1], -1 = default
    // 0 = Off, 1 = MSAA only, 2 = FXAA Balanced, 3 = FXAA High,
    // 4 = MSAA + FXAA Lite, 5 = MSAA + FXAA High
    int   aaMode            = 2;
    // ── Triggerbot ─────────────────────────────────────────────────────
    bool  triggerbotEnabled = false;  // auto-fire when crosshair is on a living enemy
    int   triggerbotDelayMs = 10;     // ms delay before first shot when on target
    int   triggerbotShotCooldownMs = 80; // ms between consecutive trigger shots
    int   triggerbotKey     = 0;      // 0 = always active (when enabled); else hold this VK
    // ── Aim Assist ────────────────────────────────────────────────────
    bool  aimAssistEnabled = false;   // master switch; per-weapon settings below
    int   aimAssistKey     = 0;       // global aim key (0 = always active)
    float aimSensitivity   = 3.0f;    // global CS2 sensitivity used by aim assist
    bool  aimSensitivityManual = false; // when false, sync from game memory
    int   aimBone          = 6;       // migration fallback for old configs
    float aimFov           = 8.f;     // migration fallback for old configs
    float aimSmooth        = 5.f;     // migration fallback for old configs
    float aimBoneOffsetZ   = 0.f;
    float aimHeadForward   = 5.f;
    std::array<AimGroupConfig, kAimWeaponGroupCount> aimByWeaponGroup{};
    // ── Aim humanization ─────────────────────────────────────────────────────
    bool  aimHumanizeEnabled   = true;
    int   aimHumanizeMode      = 0;      // 0=Auto (calibrated profile), 1=Manual sliders
    int   aimAssistStyle       = 1;      // 0=Normal (manual smooth/FOV), 1=Support (brake & guide)
    float aimSupportStrength   = 0.65f; // 0..1 support mode intensity
    bool  aimDebugConsole      = false; // temporary: aim support tuning console
    bool  aimSupportAlwaysOn   = false;  // support mode: no aim key required
    bool  aimHumanizeUseProfile = true;   // kept in sync with mode (auto=true)
    float aimHumanizeStrength  = 0.75f;
    float aimCalibAssistStrength = 0.85f; // auto/calibrated assist intensity (post-calibration tweak)
    float aimHumanizeReactionMs = 0.f;   // 0 = use calibrated/manual profile
    float aimHumanizeJitter    = 0.f;    // 0 = use profile
    float aimHumanizeOvershoot = 0.f;    // 0 = use profile
    float aimHumanizeSmoothExtra = 0.f;
    float aimHumanizeSpeedScale = 1.f;
    bool  aimHumanizeMicroPause = true;
    // ── Aim calibration framework ─────────────────────────────────────────────
    bool  aimCalibrationActive = false;
    int   aimCalibrationTargets = 20;
    int   aimCalibrationTargetsPerGroup = 8;
    bool  calibrationFrameworkComplete = false;
    bool  calibrationPromptDismissed = false;
    std::array<AimGroupCalibProfile, kAimWeaponGroupCount> aimCalibByGroup{};
    bool  aimStyleProfileValid = false;
    float aimStyleReactionMs   = 165.f;
    float aimStyleSmooth       = 6.f;
    float aimStyleOvershoot    = 1.2f;
    float aimStyleFlickSpeed   = 680.f;
    float aimStyleJitter       = 0.35f;
    float aimStyleAccuracy     = 0.f;
    float aimStyleMeasuredReactionMs = 0.f;
    float aimStyleMeasuredSmooth     = 0.f;
    float aimStyleMeasuredOvershoot  = 0.f;
    float aimStyleMeasuredFlickSpeed = 0.f;
    float aimStyleMeasuredJitter     = 0.f;
    bool  showFpsWatermark = true;
    // ── Menu ───────────────────────────────────────────────────────────────────
    bool  menuVisible     = false;   // toggled by INSERT
    float menuPosX        = -1.f;    // normalized top-left menu X in [0,1], -1 = centered
    float menuPosY        = -1.f;    // normalized top-left menu Y in [0,1], -1 = centered
    float bombTimerPosX   = -1.f;    // normalized top-left bomb HUD X in [0,1], -1 = default anchor
    float bombTimerPosY   = -1.f;    // normalized top-left bomb HUD Y in [0,1], -1 = default anchor

    // ── Performance / Priority ─────────────────────────────────────────────
    int   processPriority      = 0;  // 0=Normal, 1=AboveNormal, 2=High
    int   entityThreadPriority = 0;  // 0=Normal, 1=AboveNormal, 2=Highest, 3=TimeCritical
    int   bgMode               = 0;  // 0=Full, 1=Reduced (slower when unfocused), 2=Minimal

};

/// Global config instance — defined in config.cpp.
extern OverlayConfig g_cfg;

/// Set from the menu to terminate the overlay immediately after cleanup.
extern std::atomic<bool> g_requestShutdown;

/// Convert a float[4] RGBA to ARGB unsigned int.
inline unsigned int rgbaToArgb(const float c[4]) {
    auto b = [](float v) -> unsigned int { return (unsigned int)(v * 255.f) & 0xFF; };
    return (b(c[3]) << 24) | (b(c[0]) << 16) | (b(c[1]) << 8) | b(c[2]);
}

namespace cfg {
    constexpr int kMaxPlayers = 64;
}
