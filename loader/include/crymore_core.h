#pragma once

#include <Windows.h>

#ifdef CRYMORE_CORE_EXPORTS
#define CRYMORE_API __declspec(dllexport)
#else
#define CRYMORE_API __declspec(dllimport)
#endif

extern "C" {

/// Initialize loader backend. Pass the host window HWND for close notifications.
CRYMORE_API int crymore_init(HWND hwnd);

CRYMORE_API void crymore_shutdown();

/// Advance internal timers (call each frame, dt in seconds).
CRYMORE_API void crymore_tick(float dt);

CRYMORE_API void crymore_login(const char* username, const char* password);
CRYMORE_API void crymore_launch();
CRYMORE_API void crymore_logout();
CRYMORE_API void crymore_close();

/// Copy UTF-8 JSON state into buf. Returns bytes written (excluding NUL), or required size if buf is null/too small.
CRYMORE_API int crymore_get_state_json(char* buf, int capacity);

} // extern "C"
