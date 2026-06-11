#include "overlay/overlay_metrics.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>

namespace {
LARGE_INTEGER g_freq{};
LARGE_INTEGER g_lastFrameEnd{};
float g_overlayFps = 0.f;
float g_espMs = 0.f;
float g_entityAgeMs = 0.f;
LARGE_INTEGER g_espStart{};
bool g_freqInit = false;

void ensureFreq() {
    if (!g_freqInit) {
        QueryPerformanceFrequency(&g_freq);
        g_freqInit = true;
    }
}

float elapsedMs(const LARGE_INTEGER& a, const LARGE_INTEGER& b) {
    if (!g_freqInit || g_freq.QuadPart == 0)
        return 0.f;
    return static_cast<float>(b.QuadPart - a.QuadPart) * 1000.f
        / static_cast<float>(g_freq.QuadPart);
}

} // namespace

namespace overlay_metrics {

void onOverlayFrameEnd() {
    ensureFreq();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);

    if (g_lastFrameEnd.QuadPart > 0) {
        const float dtMs = elapsedMs(g_lastFrameEnd, now);
        if (dtMs >= 0.8f && dtMs < 500.f) {
            const float instant = 1000.f / dtMs;
            g_overlayFps = (std::isfinite(g_overlayFps) && g_overlayFps > 1.f)
                ? g_overlayFps * 0.90f + instant * 0.10f
                : instant;
        }
    }
    g_lastFrameEnd = now;
}

void onEspRenderBegin() {
    ensureFreq();
    QueryPerformanceCounter(&g_espStart);
}

void onEspRenderEnd() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const float ms = elapsedMs(g_espStart, now);
    if (ms >= 0.f && ms < 500.f) {
        g_espMs = (std::isfinite(g_espMs) && g_espMs > 0.01f)
            ? g_espMs * 0.85f + ms * 0.15f
            : ms;
    }
}

void setEntityDataAgeMs(float ms) {
    if (ms >= 0.f && ms < 5000.f)
        g_entityAgeMs = ms;
}

float overlayFps() { return g_overlayFps; }
float espFrameMs() { return g_espMs; }
float entityDataAgeMs() { return g_entityAgeMs; }

} // namespace overlay_metrics
