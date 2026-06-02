#include <Windows.h>
#include <iostream>
#include <thread>
#include <atomic>
#include <string>

#include "config.h"
#include "str_obf.h"
#include "memory/process.h"
#include "overlay/window.h"
#include "overlay/renderer.h"
#include "game/entity_manager.h"
#include "game/triggerbot.h"
#include "game/aim_assist.h"
#include "esp/esp_renderer.h"
#include "menu/menu.h"
#include "analytics/match_intel.h"
#include "web/web_radar_publisher.h"

// XOR-obfuscated critical strings at runtime — prevent static signatures.
static const std::wstring kCs2Exe = OBFW("c|2/|1|", 0xAB);
static const std::wstring kCs2Window = OBFW("\xae\xaf\xb6\xbe\xb4\xb3\xbe\xd0\xb0\xb6\xb0\xb7\xde\xbc\xb7\xb6\xb0\xde\xb9\xb5\xbe\xb0\xad\xb4\xb6\xb7\xb8\xb0\xd0\xb4\xb0\xb9\xb8\xb4\xb6\xb0\xd1\xb2\xb6\xb7\xb0\xb9\xb3\xb4\xb2\xb0\xd4\xd7", 0xAB);
static const std::wstring kClientDll = OBFW("\xad\xa2\xb0\x9e\xbe\xb2\xb6\xd1\xb7\xa2\xa2", 0xAB);
static const std::wstring kEngine2Dll = OBFW("\x9f\xb6\xb3\xb0\xb0\xb6\xd7\xb7\xa2\xa2", 0xAB);

static void tapKey(WORD vk) {
    INPUT inputs[2]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = vk;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = vk;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, inputs, sizeof(INPUT));
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
    // ── 0. Fix DPI Scaling (Removes pixelation/blurriness) ───────────────
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // ── Allocate a console for debug output ──────────────────────────────
    AllocConsole();
    FILE* fDummy;
    freopen_s(&fDummy, "CONOUT$", "w", stdout);
    freopen_s(&fDummy, "CONOUT$", "w", stderr);
    std::cout.clear();
    std::clog.clear();
    std::cerr.clear();

    // ── 1. Attach to cs2.exe ───────────────────────────────────────────────────
    Process proc;
    std::cout << "[Main] Waiting for cs2.exe...\n";
    while (!proc.attach(kCs2Exe)) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    // ── 2. Initialise entity manager (resolves client.dll) ─────────────────────
    EntityManager entityMgr;
    if (!entityMgr.init(proc)) {
        std::cerr << "[Main] EntityManager init failed.\n";
        return 1;
    }

    // Close the initial attach handle; entity thread will open per-cycle.
    proc.closeHandle();

    // ── 3. Find the game window ────────────────────────────────────────────────
    HWND gameWindow = FindWindowW(nullptr, kCs2Window.c_str());
    if (!gameWindow) {
        std::cerr << "[Main] Could not find game window '"
                  << "Counter-Strike 2" << "'\n";
        return 1;
    }

    RECT gameRect;
    GetWindowRect(gameWindow, &gameRect);
    int screenW = gameRect.right  - gameRect.left;
    int screenH = gameRect.bottom - gameRect.top;

    // ── 4. Create overlay window ───────────────────────────────────────────────
    OverlayWindow overlay;
    if (!overlay.create(hInstance, screenW, screenH)) {
        return 1;
    }

    // ── 5. Initialise D3D11 renderer ───────────────────────────────────────────
    Renderer renderer;
    if (!renderer.init(overlay.hwnd(), screenW, screenH)) {
        return 1;
    }

    // ── 6. ESP renderer and Menu ───────────────────────────────────────────────
    EspRenderer esp;
    Menu menu;
    menu.init(renderer);
    esp.setFont(menu.font());
    Triggerbot triggerbot;
    AimAssist aimAssist;
    WebRadarPublisher webRadar;

    // ── 7. Main loop ───────────────────────────────────────────────────────────
    std::cout << "[Main] Entering render loop. Press INSERT to toggle menu, END to exit.\n";

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
                else if (lastThrPrio == 3) prio = THREAD_PRIORITY_TIME_CRITICAL;
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

            int latencyMode = g_cfg.latencyMode;
            if (latencyMode < 0) latencyMode = 0;
            if (latencyMode > 2) latencyMode = 2;
            auto kUpdateInterval = (latencyMode == 1)
                ? std::chrono::milliseconds(3)
                : std::chrono::milliseconds(6);

            if (!isForeground && bgMode > 0) {
                if (bgMode == 1)
                    kUpdateInterval = (std::max)(kUpdateInterval, std::chrono::milliseconds(33));
                else
                    kUpdateInterval = (std::max)(kUpdateInterval, std::chrono::milliseconds(100));
            }

            // Open process handle for this update cycle, close when done.
            if (!proc.openHandle()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            auto t0 = std::chrono::steady_clock::now();
            entityMgr.update(proc);
            MatchIntel::instance().update(entityMgr.snapshot());
            triggerbot.update(proc, entityMgr);
            if (g_cfg.aimAssistEnabled)
                aimAssist.update(proc, entityMgr);
            webRadar.update(proc, entityMgr);
            proc.closeHandle();
            auto elapsed = std::chrono::steady_clock::now() - t0;
            auto us = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
            perf.totalUs += us;
            if (us > perf.maxUs) perf.maxUs = us;
            if (us > std::chrono::duration_cast<std::chrono::microseconds>(kUpdateInterval).count())
                ++perf.overruns;
            if (++perf.count == 300) {
                std::printf("[EntityThread] avg=%lldus max=%lldus overrun=%d/300 budget=%lldus\n",
                    perf.totalUs / 300, perf.maxUs, perf.overruns,
                    (long long)std::chrono::duration_cast<std::chrono::microseconds>(kUpdateInterval).count());
                perf = {};
            }
            if (elapsed < kUpdateInterval)
                std::this_thread::sleep_for(kUpdateInterval - elapsed);
        }
    });

    while (running.load(std::memory_order_relaxed)) {
        if (!overlay.processMessages()) break;
        if (GetAsyncKeyState(VK_END) & 0x8000) break;
        if (!proc.isAttached()) {
            std::cout << "[Main] Process detached.  Exiting.\n";
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
        esp.render(renderer, entityMgr);
        menu.render(renderer, entityMgr, overlay.hwnd());
        renderer.endFrame();
    }

    running.store(false, std::memory_order_relaxed);
    entityThread.join();
    proc.detach();
    std::cout << "[Main] Exited cleanly.\n";
    return 0;
}
