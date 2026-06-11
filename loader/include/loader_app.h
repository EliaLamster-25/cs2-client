#pragma once

#include "auth.h"
#include "env_check.h"
#include "protect.h"
#include "payload.h"

#include <Windows.h>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/// Runtime UI state — kept here so both app logic and UI can access it.
struct LoaderUiState {
    bool rememberMe = false;
    bool showPassword = false;
    bool busy = false;
    bool loggedIn = false;
    bool envOk = false;
    std::string statusLine;
    std::string launchStep;
    std::uint64_t downloadBytes = 0;
    std::uint64_t downloadTotal = 0;
    bool prefetchBusy = false;
    bool prefetchReady = false;
    bool pendingLaunch = false;
    std::vector<std::string> logLines;
};

enum class LoaderPhase {
    Boot,
    Login,
    Ready,
    Launching,
    Done,
    Failed
};

class LoaderApp {
public:
    bool init(HWND hwnd);
    void shutdown();
    void tick(float dt);

    LoaderPhase phase() const { return m_phase; }
    const LoaderUiState& uiState() const { return m_ui; }
    const AuthSession& session() const { return m_session; }
    const std::vector<std::string>& envErrors() const { return m_env.errors; }
    const std::vector<std::string>& envWarnings() const { return m_env.warnings; }

    void requestLogin(const std::string& username, const std::string& password);
    void requestLaunch();
    void requestLogout();
    void requestClose();
    void startOverlayPrefetch();

    std::string stateJson() const;

private:
    void pushLog(const std::string& line);
    void setStatus(const std::string& line);
    void onLaunchProgress(const char* step, std::uint64_t bytes, std::uint64_t total);
    void onPrefetchProgress(const char* step, std::uint64_t bytes, std::uint64_t total);
    void runBootChecks();
    void runPrefetchWorker(const AuthSession& session);
    void runLaunchPipeline();
    void onOverlayExited();
    static std::string jsonEscape(const std::string& s);
    static void launchProgressThunk(const char* step, std::uint64_t bytes, std::uint64_t total, void* ctx);

    mutable std::mutex m_mu;
    std::thread m_prefetchThread;
    HWND m_hwnd = nullptr;
    LoaderPhase m_phase = LoaderPhase::Boot;
    LoaderUiState m_ui{};
    AuthSession m_session{};
    EnvCheckResult m_env{};
    ProtectReport m_protect{};
    float m_bootTimer = 0.f;
    float m_protectTimer = 0.f;
    unsigned long m_overlayPid = 0;
};
