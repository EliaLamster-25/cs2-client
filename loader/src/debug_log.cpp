#include "debug_log.h"

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace {

std::mutex g_mu;
std::filesystem::path g_path;
bool g_ready = false;

void ensureReady() {
    if (g_ready)
        return;

    wchar_t localApp[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localApp, MAX_PATH) == 0)
        g_path = std::filesystem::temp_directory_path() / L"crymore" / L"loader.log";
    else
        g_path = std::filesystem::path(localApp) / L"crymore" / L"loader.log";

    std::error_code ec;
    std::filesystem::create_directories(g_path.parent_path(), ec);
    g_ready = true;
}

std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

} // namespace

void debugLogInit() {
    std::lock_guard lock(g_mu);
    ensureReady();
}

void debugLog(const std::string& line) {
    std::lock_guard lock(g_mu);
    ensureReady();
    std::ofstream out(g_path, std::ios::app);
    if (!out)
        return;
    out << "[" << timestamp() << "] " << line << '\n';
    out.flush();
}

std::string debugLogPath() {
    std::lock_guard lock(g_mu);
    ensureReady();
    return g_path.string();
}
