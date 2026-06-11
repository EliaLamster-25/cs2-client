#include "crymore_core.h"
#include "loader_app.h"
#include "payload.h"
#include "debug_log.h"

#include <cstring>
#include <exception>
#include <sstream>

namespace {

LoaderApp g_app;
HMODULE g_module = nullptr;

LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS* info) {
    if (info && info->ExceptionRecord) {
        std::ostringstream oss;
        oss << "Unhandled native exception 0x"
            << std::hex << info->ExceptionRecord->ExceptionCode
            << " at " << info->ExceptionRecord->ExceptionAddress;
        debugLog(oss.str());
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        payloadSetModuleHandle(module);
        SetUnhandledExceptionFilter(unhandledExceptionFilter);
        debugLogInit();
    }
    return TRUE;
}

extern "C" {

CRYMORE_API int crymore_init(HWND hwnd) {
    if (g_module)
        payloadSetModuleHandle(g_module);
    debugLog("crymore_init");
    return g_app.init(hwnd) ? 1 : 0;
}

CRYMORE_API void crymore_shutdown() {
    debugLog("crymore_shutdown");
    g_app.shutdown();
}

CRYMORE_API void crymore_tick(float dt) {
    g_app.tick(dt);
}

CRYMORE_API void crymore_login(const char* username, const char* password) {
    if (!username || !password)
        return;
    try {
        g_app.requestLogin(username, password);
    } catch (const std::exception& ex) {
        debugLog(std::string("crymore_login exception: ") + ex.what());
    } catch (...) {
        debugLog("crymore_login exception: unknown");
    }
}

CRYMORE_API void crymore_launch() {
    try {
        debugLog("crymore_launch called");
        g_app.requestLaunch();
    } catch (const std::exception& ex) {
        debugLog(std::string("crymore_launch exception: ") + ex.what());
    } catch (...) {
        debugLog("crymore_launch exception: unknown");
    }
}

CRYMORE_API void crymore_logout() {
    g_app.requestLogout();
}

CRYMORE_API void crymore_close() {
    g_app.requestClose();
}

CRYMORE_API int crymore_get_state_json(char* buf, int capacity) {
    std::string json;
    try {
        json = g_app.stateJson();
    } catch (const std::exception& ex) {
        debugLog(std::string("stateJson exception: ") + ex.what());
        json = R"({"phase":"failed","loggedIn":false,"envOk":false,"busy":false,"status":"Native state error — see loader.log","logs":["[error] native state serialization failed"]})";
    } catch (...) {
        debugLog("stateJson exception: unknown");
        json = R"({"phase":"failed","loggedIn":false,"envOk":false,"busy":false,"status":"Native state error — see loader.log","logs":["[error] native state serialization failed"]})";
    }
    const int need = static_cast<int>(json.size());
    if (!buf || capacity <= 0)
        return need + 1;
    if (capacity < need + 1)
        return need + 1;
    std::memcpy(buf, json.c_str(), static_cast<std::size_t>(need + 1));
    return need;
}

} // extern "C"
