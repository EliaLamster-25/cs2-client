#pragma once

#include "math/vector.h"
#include "memory/process.h"
#include <cstdint>

/// Game-accurate visibility via CS2 TraceShape (PureLiquid-style remote call).
class GameTraceVis {
public:
    bool init(Process& proc, std::uintptr_t clientBase);
    void shutdown(Process& proc);

    bool isReady() const { return m_ready; }
    const char* statusText() const { return m_status; }

    /// Reset per-frame trace budget (keeps remote calls bounded).
    void beginFrame(int maxTraces);

    /// Line-of-sight from eye to world point. targetPawn optional (treat hit on pawn as visible).
    bool hasLineOfSight(Process& proc,
                        std::uintptr_t localPawn,
                        std::uintptr_t targetPawn,
                        const Vec3& eye,
                        const Vec3& target);

    /// PureLiquid-style single trace to enemy origin (eye → feet).
    bool isPlayerVisible(Process& proc,
                         std::uintptr_t localPawn,
                         std::uintptr_t enemyPawn,
                         const Vec3& eye,
                         const Vec3& enemyOrigin);

private:
    bool ensureResolved(Process& proc, std::uintptr_t clientBase);
    bool ensureFilter(Process& proc, std::uintptr_t localPawn);
    bool runTrace(Process& proc,
                  std::uintptr_t localPawn,
                  const Vec3& start,
                  const Vec3& end,
                  void* outTrace,
                  std::size_t traceSize);

    bool m_ready = false;
    const char* m_status = "Not initialized";
    std::uintptr_t m_clientBase = 0;
    std::uintptr_t m_traceManager = 0;
    void* m_traceShapeFn = nullptr;
    void* m_initFilterFn = nullptr;

    std::uintptr_t m_filterLocalPawn = 0;
    void* m_remoteFilter = nullptr;
    void* m_remoteTrace = nullptr;
    void* m_remoteRay = nullptr;
    void* m_remoteCtx = nullptr;
    void* m_traceShellcode = nullptr;
    void* m_filterShellcode = nullptr;
    void* m_filterCtxRemote = nullptr;

    int m_budget = 0;
};
