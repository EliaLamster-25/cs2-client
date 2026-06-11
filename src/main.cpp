#include <Windows.h>
#include <iostream>
#include <thread>
#include <atomic>
#include <string>
#include <cstring>

#include "config.h"
#include "str_obf.h"
#include "launch_handshake.h"
#include "profile/user_profile.h"
#include "memory/process.h"
#include "memory/kernel_memory.h"
#include "memory/driver_manager.h"
#include "input/input_router.h"
#include "overlay/window.h"
#include "overlay/renderer.h"
#include "game/entity_manager.h"
#include "game/triggerbot.h"
#include "game/aim_assist.h"
#include "esp/esp_renderer.h"
#include "menu/menu.h"
#include "analytics/match_intel.h"
#include "game/aim_style.h"
#include "debug/aim_debug.h"
#include "web/web_radar_publisher.h"
#include "cloud/cloud_api.h"
#include "steam/steam_avatars.h"
#include "overlay/overlay_metrics.h"
#include "debug/overlay_log.h"

// XOR-obfuscated critical strings at runtime — prevent static signatures.
static const std::wstring kCs2Exe = OBFW("\xC8\xD8\x99\x85\xCE\xD3\xCE", 0xAB);
static const std::wstring kCs2Window = OBFW("\xE8\xC4\xDE\xC5\xDF\xCE\xD9\x86\xF8\xDF\xD9\xC2\xC0\xCE\x8B\x99", 0xAB);
static const std::wstring kClientDll = OBFW("\xC8\xC7\xC2\xCE\xC5\xDF\x85\xCF\xC7\xC7", 0xAB);
static const std::wstring kEngine2Dll = OBFW("\xCE\xC5\xCC\xC2\xC5\xCE\x99\x85\xCF\xC7\xC7", 0xAB);

// Build ID — debug builds only (avoid stable release signatures).
#ifdef OVERLAY_DEBUG_LOG
static const char kBuildId[] = __DATE__ " " __TIME__;
#endif

static void tapKey(WORD vk) {
    input_router::tapKey(vk);
}

#ifdef OVERLAY_DEBUG_LOG
static void initStartupConsole() {
    if (!AllocConsole())
        return;

    SetConsoleTitleW(L"overlay debug");

    FILE* out = nullptr;
    FILE* err = nullptr;
    FILE* in = nullptr;
    freopen_s(&out, "CONOUT$", "w", stdout);
    freopen_s(&err, "CONOUT$", "w", stderr);
    freopen_s(&in, "CONIN$", "r", stdin);
    (void)out;
    (void)err;
    (void)in;

    std::ios::sync_with_stdio(true);
    std::cout.clear();
    std::clog.clear();
    std::cerr.clear();
    std::cin.clear();

    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetConsoleMode(hOut, &mode))
            SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }

    LOG_INFO("=== startup ===\nBuild: %s\n\n", kBuildId);
}
#else
static void initStartupConsole() {}
#endif

[[noreturn]] static void fatalExit(int code, const char* message) {
    static char detailBuf[768];
    detailBuf[0] = '\0';
    if (message && message[0]) {
        std::snprintf(detailBuf, sizeof(detailBuf), "%s", message);
        const std::string& extra = DriverManager::lastSetupDetail();
        if (!extra.empty()) {
            std::strncat(detailBuf, "\n\n", sizeof(detailBuf) - std::strlen(detailBuf) - 1);
            std::strncat(detailBuf, extra.c_str(), sizeof(detailBuf) - std::strlen(detailBuf) - 1);
        }
        const char* logHint = "\n\nSee %LOCALAPPDATA%\\crymore\\overlay.log";
        std::strncat(detailBuf, logHint, sizeof(detailBuf) - std::strlen(detailBuf) - 1);
    }
#ifdef OVERLAY_DEBUG_LOG
    if (detailBuf[0])
        LOG_ERROR("%s\n", detailBuf);
    LOG_ERROR("\nPress Enter to exit...\n");
    std::cin.clear();
    std::cin.ignore(100000, '\n');
    std::cin.get();
#else
    if (detailBuf[0])
        MessageBoxA(nullptr, detailBuf, "Error", MB_ICONERROR | MB_OK);
#endif
    ExitProcess(static_cast<UINT>(code));
}

/// @file main.cpp
/// @brief Entry point — ties together process attachment, overlay creation,
///        and the main render loop.
///
/// ## High-level flow
///
///   1. Attach to cs2.exe (read-only).
///   2. Resolve client.dll base address.
///   3. Create the transparent overlay window.
///   4. Initialise D3D11 renderer.
///   5. Enter the render loop:
///        a. Sync overlay position with the game window.
///        b. Read entity data from game memory.
///        c. Clear the overlay.
///        d. Draw AABB boxes + HP bars for each player.
///        e. Present.
///   6. Clean up on exit (ESC / WM_QUIT).

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
#ifdef _DEBUG
    // ── Anti-debug: if a debugger is present, die immediately.
    if (IsDebuggerPresent()) __fastfail(1);
#endif

    // ── 0. Fix DPI Scaling (Removes pixelation/blurriness) ───────────────
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    {
        LaunchHandshake hs{};
        std::string gateErr;
        if (!launchVerifyFromStdin(hs, gateErr)) {
#ifdef CRYMORE_REQUIRE_LAUNCH_GATE
            ExitProcess(1);
#else
            fatalExit(1, gateErr.c_str());
#endif
        }
        userProfileApplyFromHandshake(hs);
    }

    cloud_api::initFromEnvironment();

    initStartupConsole();

    if (!input_router::initialize())
        LOG_ERROR("[Main] Input inject unavailable.\n");

    // ── 1. Kernel memory (default) ─────────────────────────────────────────────
    const bool wantKernel = (g_cfg.memoryBackend == 1);
    if (wantKernel) {
        LOG_INFO("[Main] Initializing kernel backend...\n");
        DriverManager::printStatus(DriverManager::checkSystem());
        const auto setup = KernelMemory::instance().initialize();
        if (setup == DriverManager::SetupResult::Ready) {
            if (!KernelMemory::instance().openDevice()) {
                LOG_ERROR("[Main] Failed to open kernel device.\n");
                if (!g_cfg.memoryAllowWin32Fallback)
                    fatalExit(1, "Kernel device open failed.");
                g_cfg.memoryBackend = 0;
            } else {
                LOG_INFO("[Main] Kernel backend ready.\n");
            }
        } else {
            if (setup == DriverManager::SetupResult::NeedReboot)
                LOG_ERROR("[Main] Reboot required before kernel driver can load.\n");
            else if (setup == DriverManager::SetupResult::NeedAdmin)
                LOG_ERROR("[Main] Run as Administrator for kernel mode.\n");
            else if (setup == DriverManager::SetupResult::DriverFileMissing)
                LOG_ERROR("[Main] Kernel driver image not found.\n");
            else if (setup == DriverManager::SetupResult::MapperMissing)
                LOG_ERROR("[Main] Driver mapper not found.\n");
            else if (setup == DriverManager::SetupResult::SetupFailed)
                LOG_ERROR("[Main] Kernel setup failed.\n");

            if (!g_cfg.memoryAllowWin32Fallback) {
                const char* msg = "Kernel init failed.";
                switch (setup) {
                case DriverManager::SetupResult::NeedReboot:
                    msg = "Reboot required - disable HVCI / driver blocklist first.";
                    break;
                case DriverManager::SetupResult::NeedAdmin:
                    msg = "Run as Administrator (loader and overlay need elevation).";
                    break;
                case DriverManager::SetupResult::DriverFileMissing:
                    msg = "Kernel driver (.sys) missing beside overlay - republish overlay pack.";
                    break;
                case DriverManager::SetupResult::MapperMissing:
                    msg = "Driver mapper missing beside overlay - republish overlay pack.";
                    break;
                case DriverManager::SetupResult::SetupFailed:
                    msg = "Kernel driver mapping failed.";
                    break;
                default:
                    break;
                }
                fatalExit(1, msg);
            }
            LOG_ERROR("[Main] Falling back to Win32 RPM.\n");
            g_cfg.memoryBackend = 0;
        }
    }

    // ── 2. Attach to cs2.exe ───────────────────────────────────────────────────
    Process proc;
    LOG_INFO("[Main] Waiting for game process...\n");
    const bool preferKernel = (g_cfg.memoryBackend == 1);
    int attachAttempts = 0;
    while (!proc.attach(kCs2Exe, preferKernel)) {
        if (++attachAttempts % 5 == 0)
            LOG_INFO("[Main] Still waiting...\n");
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    if (proc.usesKernelMemory() && g_cfg.visibilityBackend != 0)
        g_cfg.visibilityBackend = 0;

    // ── 3. Initialise entity manager (resolves client.dll) ─────────────────────
    EntityManager entityMgr;
    if (!entityMgr.init(proc)) {
        fatalExit(1, "Init failed.");
    }

    // ── 3. Find the game window ────────────────────────────────────────────────
    HWND gameWindow = FindWindowW(nullptr, kCs2Window.c_str());
    if (!gameWindow) {
        fatalExit(1, "Game window not found.");
    }

    RECT gameRect;
    GetWindowRect(gameWindow, &gameRect);
    int screenW = gameRect.right  - gameRect.left;
    int screenH = gameRect.bottom - gameRect.top;

    // ── 4. Create overlay window ───────────────────────────────────────────────
    OverlayWindow overlay;
    if (!overlay.create(hInstance, screenW, screenH)) {
        fatalExit(1, "Overlay window failed.");
    }

    // ── 5. Initialise D3D11 renderer ───────────────────────────────────────────
    Renderer renderer;
    if (!renderer.init(overlay.hwnd(), screenW, screenH)) {
        fatalExit(1, "Renderer init failed.");
    }

    // ── 6. ESP renderer and Menu ───────────────────────────────────────────────
    EspRenderer esp;
    Menu menu;
    menu.init(renderer);
    SteamAvatars::instance().init(renderer.device());
    esp.setFont(menu.font());
    esp.setBrandLogo(menu.brandLogoSrv(), menu.brandLogoW(), menu.brandLogoH());
    Triggerbot triggerbot;
    AimAssist aimAssist;
    WebRadarPublisher webRadar;

    // ── 7. Main loop ───────────────────────────────────────────────────────────
    LOG_INFO("[Main] Render loop started.\n");

    // Background thread: entity reads at ~120 Hz, decoupled from render rate.
    std::atomic<bool> running{ true };
    struct EntityPerf {
        long long totalUs = 0;
        long long maxUs  = 0;
        int count = 0;
        int overruns = 0;
    } perf;
    DWORD cs2Pid = proc.pid();
    std::thread entityThread([&, cs2Pid]() {
        int lastProcPrio = -1;
        int lastThrPrio = -1;
        while (running.load(std::memory_order_relaxed)) {
            // Apply Windows priority when changed.
            if (g_cfg.processPriority != lastProcPrio) {
                lastProcPrio = g_cfg.processPriority;
                DWORD cls = NORMAL_PRIORITY_CLASS;
                if (lastProcPrio == 1) cls = ABOVE_NORMAL_PRIORITY_CLASS;
                else if (lastProcPrio == 2) cls = HIGH_PRIORITY_CLASS;
                SetPriorityClass(GetCurrentProcess(), cls);
            }
            if (g_cfg.entityThreadPriority != lastThrPrio) {
                lastThrPrio = g_cfg.entityThreadPriority;
                int prio = THREAD_PRIORITY_NORMAL;
                if (lastThrPrio == 1) prio = THREAD_PRIORITY_ABOVE_NORMAL;
                else if (lastThrPrio == 2) prio = THREAD_PRIORITY_HIGHEST;
                else if (lastThrPrio == 3) prio = THREAD_PRIORITY_HIGHEST;
                SetThreadPriority(GetCurrentThread(), prio);
            }

            // Background mode: reduce update rate when CS2 is not the foreground window.
            int bgMode = g_cfg.bgMode;
            if (bgMode < 0) bgMode = 0;
            if (bgMode > 2) bgMode = 2;
            bool isForeground = false;
            if (bgMode > 0) {
                HWND fg = GetForegroundWindow();
                if (fg) {
                    DWORD fgPid = 0;
                    GetWindowThreadProcessId(fg, &fgPid);
                    isForeground = (fgPid == cs2Pid);
                }
            }

            std::chrono::steady_clock::duration kUpdateInterval = std::chrono::steady_clock::duration::zero();

            if (!isForeground && bgMode > 0) {
                const std::chrono::steady_clock::duration bgMin = (bgMode == 1)
                    ? std::chrono::milliseconds(33)
                    : std::chrono::milliseconds(100);
                if (kUpdateInterval < bgMin)
                    kUpdateInterval = bgMin;
            }

            const bool needTraceAccess = !proc.usesKernelMemory()
                && g_cfg.visibilityBackend != 0
                && (g_cfg.visibilityCheckEnabled
                    || (g_cfg.aimAssistEnabled && g_cfg.aimRequireVisibility));
            if (needTraceAccess) {
                if (!proc.openExtendedHandle()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
            } else if (!proc.ensureHandle()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            auto t0 = std::chrono::steady_clock::now();
            entityMgr.update(proc);
            const auto entitySnap = entityMgr.snapshot();
            MatchIntel::instance().update(entitySnap);
            if (AimCalibration::instance().isActive())
                AimCalibration::instance().update(proc, entityMgr, 5.f);
            triggerbot.update(proc, entityMgr);
            webRadar.update(proc, entityMgr);
            auto elapsed = std::chrono::steady_clock::now() - t0;
            auto us = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
            perf.totalUs += us;
            if (us > perf.maxUs) perf.maxUs = us;
            if (us > std::chrono::duration_cast<std::chrono::microseconds>(kUpdateInterval).count())
                ++perf.overruns;
            if (++perf.count == 6000) {
                LOG_INFO("[EntityThread] avg=%lldus max=%lldus overrun=%d/6000\n",
                    perf.totalUs / 6000, perf.maxUs, perf.overruns);
                perf = {};
            }
            if (elapsed < kUpdateInterval)
                std::this_thread::sleep_for(kUpdateInterval - elapsed);
        }
    });

    while (running.load(std::memory_order_relaxed)) {
        if (!overlay.processMessages()) break;
        if (g_requestShutdown.load(std::memory_order_relaxed)) break;
        if (GetAsyncKeyState(VK_END) & 0x8000) break;
        if (!proc.isAttached()) {
            LOG_INFO("[Main] Process detached.\n");
            break;
        }

        overlay.syncWithGameWindow(gameWindow);
        renderer.resize(overlay.width(), overlay.height());

        static bool insertDown = false;
        bool insertPressed = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
        if (insertPressed && !insertDown) {
            tapKey(VK_ESCAPE);
            g_cfg.menuVisible = !g_cfg.menuVisible;
            overlay.setClickThrough(!g_cfg.menuVisible);
        }
        insertDown = insertPressed;

        renderer.beginFrame();
        SteamAvatars::instance().tick();
        aimDebugSyncConsole(g_cfg.aimDebugConsole);
        if (g_cfg.aimAssistEnabled)
            aimAssist.update(proc, entityMgr,
                static_cast<float>(renderer.screenWidth()),
                static_cast<float>(renderer.screenHeight()));
        esp.render(renderer, entityMgr, proc);
        menu.render(renderer, entityMgr, proc, overlay.hwnd());
        renderer.endFrame();
        overlay_metrics::onOverlayFrameEnd();
    }

    running.store(false, std::memory_order_relaxed);
    entityThread.join();
    SteamAvatars::instance().shutdown();
    proc.detach();
#ifdef OVERLAY_DEBUG_LOG
    LOG_INFO("[Main] Exited.\nPress Enter to close...\n");
    std::cin.clear();
    std::cin.ignore(100000, '\n');
    std::cin.get();
#endif
    return 0;
}
