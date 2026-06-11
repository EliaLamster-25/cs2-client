#include "loader_app.h"
#include "debug_log.h"
#include "launch_handshake.h"
#include "log_sanitize.h"
#include "protect.h"
#include "steam_launch.h"

#include <algorithm>
#include <exception>
#include <cstdio>
#include <cstring>

namespace {

std::string formatMegabytes(std::uint64_t bytes) {
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f MB", mb);
    return buf;
}

void appendUiLog(std::vector<std::string>& lines, const std::string& line) {
    const std::string clean = sanitizeUiLogLine(line);
    if (clean.empty())
        return;
    lines.push_back(clean);
    if (lines.size() > 120)
        lines.erase(lines.begin());
}

} // namespace

bool LoaderApp::init(HWND hwnd) {
    debugLogInit();
    debugLog("LoaderApp::init");
    m_hwnd = hwnd;
    m_phase = LoaderPhase::Boot;
    m_bootTimer = 0.f;
    m_ui = {};
    m_ui.logLines.reserve(32);
    runBootChecks();
    return true;
}

void LoaderApp::shutdown() {
    authClearProgressCallback();
    if (m_prefetchThread.joinable())
        m_prefetchThread.join();
    payloadClearOverlayCache();
}

void LoaderApp::pushLog(const std::string& line) {
    std::lock_guard lock(m_mu);
    appendUiLog(m_ui.logLines, line);
}

void LoaderApp::setStatus(const std::string& line) {
    std::lock_guard lock(m_mu);
    m_ui.statusLine = line;
    if (line.find("blocked") != std::string::npos ||
        line.find("Fix environment") != std::string::npos) {
        m_ui.envOk = m_env.ok;
    }
}

void LoaderApp::launchProgressThunk(const char* step, std::uint64_t bytes, std::uint64_t total, void* ctx) {
    if (!ctx)
        return;
    auto* app = static_cast<LoaderApp*>(ctx);
    if (step && std::strcmp(step, "prefetch") == 0)
        app->onPrefetchProgress(step, bytes, total);
    else
        app->onLaunchProgress(step, bytes, total);
}

void LoaderApp::onLaunchProgress(const char* step, std::uint64_t bytes, std::uint64_t total) {
    std::lock_guard lock(m_mu);
    if (step && step[0])
        m_ui.launchStep = step;
    m_ui.downloadBytes = bytes;
    m_ui.downloadTotal = total;

    if (std::string(step ? step : "") == "download") {
        if (total > 0) {
            const int pct = static_cast<int>((bytes * 100) / total);
            m_ui.statusLine = "Downloading overlay... " + std::to_string(pct) + "% (" +
                formatMegabytes(bytes) + " / " + formatMegabytes(total) + ")";
        } else if (bytes > 0) {
            m_ui.statusLine = "Downloading overlay... " + formatMegabytes(bytes);
        } else {
            m_ui.statusLine = "Downloading overlay...";
        }
    }     else if (step && step[0]) {
        if (std::strcmp(step, "decrypt") == 0)
            m_ui.statusLine = "Decrypting overlay...";
        else if (std::strcmp(step, "extract") == 0)
            m_ui.statusLine = "Extracting files...";
        else if (std::strcmp(step, "steam") == 0)
            m_ui.statusLine = "Starting Steam and CS2...";
        else
            m_ui.statusLine = std::string(step) + "...";
    }
}

void LoaderApp::onPrefetchProgress(const char* /*step*/, std::uint64_t bytes, std::uint64_t total) {
    std::lock_guard lock(m_mu);
    m_ui.downloadBytes = bytes;
    m_ui.downloadTotal = total;

    if (total > 0 && bytes >= total) {
        m_ui.prefetchBusy = false;
        m_ui.prefetchReady = true;
    } else if (bytes == 1 && total == 1) {
        m_ui.prefetchBusy = false;
        m_ui.prefetchReady = true;
    } else {
        m_ui.prefetchBusy = true;
    }

    if (m_phase == LoaderPhase::Ready && !m_ui.busy) {
        if (m_ui.prefetchBusy)
            m_ui.statusLine = "Downloading files...";
        else if (m_ui.prefetchReady)
            m_ui.statusLine = "Authenticated - ready to inject.";
    }
}

void LoaderApp::startOverlayPrefetch() {
#if defined(CRYMORE_RELEASE)
    AuthSession sessionCopy;
    {
        std::lock_guard lock(m_mu);
        if (!m_ui.loggedIn || m_ui.busy || m_phase == LoaderPhase::Launching)
            return;
        if (m_ui.prefetchReady || payloadOverlayCacheReady()) {
            m_ui.prefetchReady = true;
            return;
        }
        if (m_ui.prefetchBusy || payloadOverlayCachePrefetching())
            return;
        sessionCopy = m_session;
        m_ui.prefetchBusy = true;
    }

    if (m_prefetchThread.joinable())
        m_prefetchThread.join();

    m_prefetchThread = std::thread([this, sessionCopy]() { runPrefetchWorker(sessionCopy); });
#endif
}

void LoaderApp::runPrefetchWorker(const AuthSession& session) {
    pushLog("[prefetch] downloading overlay in background");
    debugLog("[prefetch] worker started");
    authSetProgressCallback(&LoaderApp::launchProgressThunk, this);
    payloadPrefetchServerOverlay(session);
    authClearProgressCallback();

    const bool ready = payloadOverlayCacheReady();
    bool launchPending = false;
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_ui.prefetchBusy = false;
        m_ui.prefetchReady = ready;
        if (ready && m_ui.pendingLaunch) {
            launchPending = true;
        } else if (!m_ui.pendingLaunch) {
            m_ui.busy = false;
        }
        if (ready && m_phase == LoaderPhase::Ready && !launchPending)
            m_ui.statusLine = "Authenticated - ready to inject.";
    }
    pushLog(std::string("[prefetch] ") + (ready ? "download complete" : "failed - will retry on inject"));
    debugLog(std::string("[prefetch] worker done ready=") + (ready ? "yes" : "no"));
    if (launchPending) {
        std::thread([this]() { runLaunchPipeline(); }).detach();
    }
}

void LoaderApp::onOverlayExited() {
    std::lock_guard lock(m_mu);
    if (m_phase != LoaderPhase::Done)
        return;
    m_overlayPid = 0;
    m_ui.pendingLaunch = false;
    m_ui.busy = false;
    m_phase = LoaderPhase::Ready;
    m_ui.statusLine = m_ui.prefetchReady
        ? "Authenticated - ready to inject."
        : "Overlay closed - sign in to continue.";
    appendUiLog(m_ui.logLines, "[launch] overlay closed");
    debugLog("[launch] overlay process exited");
}

void LoaderApp::runBootChecks() {
    pushLog("[boot] crymore.pw loader");
    debugLog("[boot] log=" + debugLogPath());
    debugLog("[boot] environment checks starting");

    std::string steamDetail;
    if (steamShutdownAll(steamDetail)) {
        pushLog("[boot] closed Steam and CS2");
        debugLog("[boot] steam shutdown ok");
    } else {
        pushLog("[warn] " + steamDetail);
        debugLog("[boot] steam shutdown: " + steamDetail);
    }

    m_protect = runProtectionChecks();
    for (const auto& w : m_protect.warnings)
        pushLog("[warn] " + w);

    if (!m_protect.ok) {
        m_phase = LoaderPhase::Failed;
        setStatus("Security checks failed.");
        return;
    }

    m_env = runEnvironmentChecks();
    for (const auto& w : m_env.warnings)
        pushLog("[env] " + w);
    for (const auto& e : m_env.errors)
        pushLog("[env] " + e);

    m_ui.envOk = m_env.ok;
    if (authDevBypassEnabled())
        pushLog("[auth] dev bypass enabled (CRYMORE_DEV or LOADER_DEV_LAUNCH)");

    m_phase = LoaderPhase::Login;
    setStatus(m_env.ok ? "Sign in to continue." : "Environment blocked - resolve issues below.");
}

void LoaderApp::tick(float dt) {
    std::lock_guard lock(m_mu);
    if (m_phase == LoaderPhase::Boot) {
        m_bootTimer += dt;
        if (m_bootTimer > 0.05f)
            m_phase = LoaderPhase::Login;
    }

    if (m_phase == LoaderPhase::Ready || m_phase == LoaderPhase::Launching) {
        m_protectTimer += dt;
        if (m_protectTimer > 5.f) {
            m_protectTimer = 0.f;
            std::string reason;
            if (!protectQuickCheck(reason)) {
                appendUiLog(m_ui.logLines, "[warn] " + reason);
                m_phase = LoaderPhase::Failed;
                m_ui.statusLine = "Session blocked - security check failed.";
            }
        }
    }

    if (m_phase == LoaderPhase::Done && m_overlayPid != 0) {
        HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, m_overlayPid);
        if (!proc) {
            onOverlayExited();
        } else {
            DWORD code = STILL_ACTIVE;
            GetExitCodeProcess(proc, &code);
            CloseHandle(proc);
            if (code != STILL_ACTIVE)
                onOverlayExited();
        }
    }
}

void LoaderApp::requestLogin(const std::string& username, const std::string& password) {
    {
        std::lock_guard lock(m_mu);
        if (m_ui.busy || m_phase == LoaderPhase::Launching)
            return;
        m_ui.busy = true;
        m_ui.statusLine = "Authenticating...";
    }

    pushLog("[auth] login requested");
    const AuthResponse resp = authLogin(username, password);

    bool startPrefetch = false;
    {
        std::lock_guard lock(m_mu);
        m_ui.busy = false;

        if (!resp.ok) {
            appendUiLog(m_ui.logLines, "[auth] " + resp.error);
            m_ui.statusLine = resp.error;
            return;
        }

        m_session = resp.session;
        m_ui.loggedIn = true;
        m_phase = LoaderPhase::Ready;
        appendUiLog(m_ui.logLines, "[auth] welcome, " + m_session.username);
        m_ui.statusLine = "Authenticated - ready to launch.";
        startPrefetch = true;
    }

    if (startPrefetch)
        startOverlayPrefetch();
}

void LoaderApp::requestLogout() {
    if (m_prefetchThread.joinable())
        m_prefetchThread.join();
    payloadClearOverlayCache();

    std::lock_guard lock(m_mu);
    if (m_ui.busy || m_phase == LoaderPhase::Launching)
        return;
    m_session = {};
    m_ui.loggedIn = false;
    m_ui.busy = false;
    m_ui.prefetchBusy = false;
    m_ui.prefetchReady = false;
    m_ui.pendingLaunch = false;
    m_overlayPid = 0;
    m_phase = LoaderPhase::Login;
    m_ui.statusLine = "Sign in to continue.";
    appendUiLog(m_ui.logLines, "[auth] signed out");
}

void LoaderApp::requestLaunch() {
    bool queueUntilDownload = false;
    {
        std::lock_guard lock(m_mu);
        if (!m_ui.loggedIn)
            return;
        if (m_phase == LoaderPhase::Launching || m_phase == LoaderPhase::Done)
            return;
        if (!m_env.ok) {
            m_ui.statusLine = "Fix environment errors before launching.";
            appendUiLog(m_ui.logLines, "[launch] blocked - environment check failed");
            debugLog("[launch] blocked - environment check failed");
            return;
        }
#if defined(CRYMORE_RELEASE)
        if (m_ui.prefetchBusy && !m_ui.prefetchReady) {
            m_ui.pendingLaunch = true;
            m_ui.busy = true;
            m_ui.statusLine = "Downloading files...";
            queueUntilDownload = true;
        }
#endif
    }

    if (queueUntilDownload) {
        pushLog("[launch] queued - waiting for download");
        debugLog("[launch] queued until prefetch completes");
        return;
    }

    runLaunchPipeline();
}

void LoaderApp::runLaunchPipeline() {
    auto failLaunch = [&](const std::string& step, const std::string& err) {
        authClearProgressCallback();
        std::lock_guard lock(m_mu);
        m_ui.busy = false;
        m_ui.pendingLaunch = false;
        m_ui.launchStep.clear();
        m_ui.downloadBytes = 0;
        m_ui.downloadTotal = 0;
        m_phase = LoaderPhase::Failed;
        appendUiLog(m_ui.logLines, "[launch] " + step + ": " + err);
        debugLog("[launch] FAILED at " + step + ": " + err);
        debugLog("[launch] full log: " + debugLogPath());
        m_ui.statusLine = sanitizeUiLogLine(err);
        if (m_ui.statusLine.empty())
            m_ui.statusLine = "Launch failed.";
    };

    try {
        if (m_prefetchThread.joinable() &&
            m_prefetchThread.get_id() != std::this_thread::get_id()) {
            m_prefetchThread.join();
        }

#if defined(CRYMORE_RELEASE)
        if (!payloadOverlayCacheReady()) {
            failLaunch("payload", "Overlay download not ready. Wait for download to finish.");
            return;
        }
#endif

        pushLog("[launch] inject requested");
        debugLog("[launch] === inject requested ===");

        pushLog("[launch] validating session...");
        debugLog("[launch] validating session");
        AuthResponse valid = authValidateSession(m_session);
        if (!valid.ok) {
            pushLog("[auth] " + valid.error);
            debugLog("[auth] session validation failed: " + valid.error);
            std::lock_guard lock(m_mu);
            m_ui.statusLine = valid.error;
            m_ui.loggedIn = false;
            m_ui.busy = false;
            m_ui.pendingLaunch = false;
            m_phase = LoaderPhase::Login;
            return;
        }

        {
            std::lock_guard lock(m_mu);
            m_session = valid.session;
            m_ui.busy = true;
            m_ui.pendingLaunch = false;
            m_ui.launchStep = "prepare";
            m_ui.downloadBytes = 0;
            m_ui.downloadTotal = 0;
            m_phase = LoaderPhase::Launching;
            m_ui.statusLine = "Preparing overlay...";
        }

        const std::string keyState = valid.session.payloadKey.empty() ? "missing" : "present";
        debugLog("[launch] session ok; payload_key=" + keyState + ", plan=" + valid.session.plan);
        pushLog("[launch] starting Steam and CS2");

        {
            std::lock_guard lock(m_mu);
            m_ui.launchStep = "steam";
            m_ui.statusLine = "Starting Steam and CS2...";
        }

        std::string steamErr;
        if (!steamLaunchGamePipeline(steamErr)) {
            failLaunch("steam", steamErr);
            return;
        }
        pushLog("[launch] CS2 is running");

        pushLog("[launch] starting payload pipeline");

        authSetProgressCallback(&LoaderApp::launchProgressThunk, this);

        PayloadLaunchResult result{};
#ifdef LOADER_DEV_LAUNCH
        debugLog("[launch] mode=dev sibling+embed");
        result = launchSiblingOverlay(valid.session);
        if (!result.ok) {
            pushLog("[launch] sibling failed: " + result.error);
            result = launchEmbeddedPayload(valid.session);
        }
#elif defined(CRYMORE_RELEASE)
        pushLog("[launch] mode=server");
        debugLog("[launch] mode=server (CRYMORE_RELEASE)");
        result = launchServerPayload(valid.session);
#else
        debugLog("[launch] mode=dev embed+sibling");
        result = launchEmbeddedPayload(valid.session);
        if (!result.ok) {
            pushLog("[launch] embed failed: " + result.error);
            pushLog("[launch] trying sibling overlay (dev fallback)");
            result = launchSiblingOverlay(valid.session);
        }
#endif

        authClearProgressCallback();

        if (!result.ok) {
            failLaunch("payload", result.error);
            return;
        }

        pushLog("[launch] started pid " + std::to_string(result.childPid));
        debugLog("[launch] success pid=" + std::to_string(result.childPid));
        if (!result.launchedPathNarrow.empty())
            debugLog("[launch] exe=" + result.launchedPathNarrow);

        std::lock_guard lock(m_mu);
        m_ui.busy = false;
        m_ui.launchStep.clear();
        m_ui.downloadBytes = 0;
        m_ui.downloadTotal = 0;
        m_overlayPid = result.childPid;
        m_ui.statusLine = sanitizeUiLogLine("Overlay launched (pid " + std::to_string(result.childPid) + ").");
        m_phase = LoaderPhase::Done;
        appendUiLog(m_ui.logLines, "[launch] overlay running");
        debugLog("[launch] loader staying open");
    } catch (const std::exception& ex) {
        failLaunch("exception", ex.what());
    } catch (...) {
        failLaunch("exception", "Unknown native error during launch.");
    }
}

void LoaderApp::requestClose() {
    if (m_hwnd)
        PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
}

std::string LoaderApp::jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c; break;
        }
    }
    return out;
}

std::string LoaderApp::stateJson() const {
    std::lock_guard lock(m_mu);

    const char* phase = "login";
    switch (m_phase) {
    case LoaderPhase::Boot: phase = "boot"; break;
    case LoaderPhase::Login: phase = "login"; break;
    case LoaderPhase::Ready: phase = "ready"; break;
    case LoaderPhase::Launching: phase = "launching"; break;
    case LoaderPhase::Done: phase = "done"; break;
    case LoaderPhase::Failed: phase = "failed"; break;
    }

    std::string json = "{";
    json += "\"phase\":\"" + std::string(phase) + "\",";
    json += m_ui.loggedIn ? "\"loggedIn\":true," : "\"loggedIn\":false,";
    json += m_ui.envOk ? "\"envOk\":true," : "\"envOk\":false,";
    json += m_ui.busy ? "\"busy\":true," : "\"busy\":false,";
    json += "\"status\":\"" + jsonEscape(sanitizeUiLogLine(m_ui.statusLine)) + "\",";
    json += "\"launchStep\":\"" + jsonEscape(m_ui.launchStep) + "\",";
    json += "\"downloadBytes\":" + std::to_string(m_ui.downloadBytes) + ",";
    json += "\"downloadTotal\":" + std::to_string(m_ui.downloadTotal) + ",";
    json += m_ui.prefetchBusy ? "\"prefetchBusy\":true," : "\"prefetchBusy\":false,";
    json += m_ui.prefetchReady ? "\"prefetchReady\":true," : "\"prefetchReady\":false,";
    json += m_ui.pendingLaunch ? "\"pendingLaunch\":true," : "\"pendingLaunch\":false,";
    json += "\"username\":\"" + jsonEscape(m_session.username) + "\",";
    json += "\"plan\":\"" + jsonEscape(m_session.plan) + "\",";
    json += "\"expiresAt\":\"" + jsonEscape(m_session.expiresAt) + "\",";
    json += "\"avatarUrl\":\"" + jsonEscape(m_session.avatarUrl) + "\",";
    json += "\"subscriptionDaysRemaining\":" + std::to_string(m_session.subscriptionDaysRemaining) + ",";

    json += "\"envErrors\":[";
    for (std::size_t i = 0; i < m_env.errors.size(); ++i) {
        if (i) json += ',';
        json += "\"" + jsonEscape(sanitizeUiLogLine(m_env.errors[i])) + "\"";
    }
    json += "],";

    json += "\"logs\":[";
    for (std::size_t i = 0; i < m_ui.logLines.size(); ++i) {
        if (i) json += ',';
        json += "\"" + jsonEscape(sanitizeUiLogLine(m_ui.logLines[i])) + "\"";
    }
    json += "]}";
    return json;
}
