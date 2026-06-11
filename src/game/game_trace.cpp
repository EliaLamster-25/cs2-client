#include "game/game_trace.h"

#include "memory/pattern_scan.h"
#include "memory/rpm.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

namespace {
constexpr const char* kTraceShapePattern =
    "48 89 5C 24 ?? 48 89 4C 24 ?? 55 57";
constexpr const char* kInitTraceFilterPattern =
    "48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? 0F B6 41 ?? 33 FF 24";
constexpr const char* kGameTraceManagerPattern =
    "4C 8B 2D ? ? ? ? 24";

constexpr std::uint64_t kMaskPlayerVisible =
    (1ull << 0) | (1ull << 9) | (1ull << 10) | (1ull << 11)
    | (1ull << 12) | (1ull << 14) | (1ull << 18) | (1ull << 19)
    | (1ull << 21);

enum RayType_t : std::uint8_t { RAY_TYPE_LINE = 0 };

struct RayStorage {
    struct Line_t {
        Vec3 m_vStartOffset{};
        float m_flRadius = 0.f;
    };
    Line_t m_Line{};
    RayType_t m_eType = RAY_TYPE_LINE;
};

struct trace_storage_t {
    std::uint8_t pad1[8]{};
    void* HitEntity = nullptr;
    void* hitbox = nullptr;
    std::uint8_t pad2[0x60]{};
    Vec3 Start{};
    Vec3 End{};
    std::uint8_t pad3[0x1C]{};
    float Fraction = 1.f;
    std::uint8_t pad4[6]{};
    bool bHit = false;
    std::uint8_t pad5[0x20]{};
};

struct TraceFilterStorage {
    std::byte pad01[0x8]{};
    std::int64_t m_uTraceMask = 0;
    std::int64_t m_v1[2]{};
    std::int32_t m_arrSkipHandles[4]{};
    std::int16_t m_arrCollisions[2]{};
    std::int16_t m_v2 = 0;
    std::uint8_t m_nLayer = 0;
    std::uint8_t m_v4 = 0;
    std::uint8_t m_flags = 0;
};

using TraceShapeFn = bool(__fastcall*)(void*, RayStorage*, const Vec3&, const Vec3&, TraceFilterStorage*, trace_storage_t*);
using InitTraceFilterFn = TraceFilterStorage*(__thiscall*)(void*, std::uintptr_t, std::uint32_t, int, std::int16_t);

struct TraceShapeCtxStorage {
    TraceShapeFn TraceShape = nullptr;
    void* pGameTraceManager = nullptr;
    TraceFilterStorage* pTraceFilter = nullptr;
    Vec3 vStartPos{};
    Vec3 vEndPos{};
    trace_storage_t* pTrace = nullptr;
    RayStorage* ray = nullptr;
};

struct InitEntityTraceFilterCtxStorage {
    TraceFilterStorage* filter = nullptr;
    InitTraceFilterFn fn = nullptr;
    std::uintptr_t pSkipPawn = 0;
    std::uint32_t mask = 0;
    int layer = 0;
};

#pragma code_seg(".traceShapeSeg")
#pragma optimize("", off)
#pragma runtime_checks("", off)
#pragma check_stack(off)
__declspec(safebuffers)
DWORD WINAPI TraceShapeThread(LPVOID lpParam) {
    auto* ctx = static_cast<TraceShapeCtxStorage*>(lpParam);
    ctx->TraceShape(ctx->pGameTraceManager,
                    ctx->ray,
                    ctx->vStartPos,
                    ctx->vEndPos,
                    ctx->pTraceFilter,
                    ctx->pTrace);
    return 1;
}
DWORD WINAPI TraceShapeThreadEnd() { return 0; }

__declspec(safebuffers)
DWORD WINAPI InitEntitiesOnlyThread(LPVOID lpParam) {
    auto* ctx = static_cast<InitEntityTraceFilterCtxStorage*>(lpParam);
    ctx->fn(ctx->filter, ctx->pSkipPawn, ctx->mask, ctx->layer, 15);
    return 1;
}
DWORD WINAPI InitEntitiesOnlyThreadEnd() { return 0; }
#pragma check_stack()
#pragma runtime_checks("", restore)
#pragma optimize("", on)
#pragma code_seg()
} // namespace

bool GameTraceVis::init(Process& proc, std::uintptr_t clientBase) {
    shutdown(proc);
    m_clientBase = clientBase;
    if (!proc.openExtendedHandle()) {
        m_status = "Extended process access denied";
        return false;
    }
    if (!ensureResolved(proc, clientBase)) {
        shutdown(proc);
        return false;
    }

    m_remoteTrace = proc.remoteAlloc(sizeof(trace_storage_t));
    m_remoteRay = proc.remoteAlloc(sizeof(RayStorage));
    m_remoteCtx = proc.remoteAlloc(sizeof(TraceShapeCtxStorage));
    m_traceShellcode = proc.remoteCopyShellcode(
        reinterpret_cast<void*>(TraceShapeThread),
        reinterpret_cast<void*>(TraceShapeThreadEnd));
    m_filterShellcode = proc.remoteCopyShellcode(
        reinterpret_cast<void*>(InitEntitiesOnlyThread),
        reinterpret_cast<void*>(InitEntitiesOnlyThreadEnd));
    m_filterCtxRemote = proc.remoteAlloc(sizeof(InitEntityTraceFilterCtxStorage));

    if (!m_remoteTrace || !m_remoteRay || !m_remoteCtx || !m_traceShellcode || !m_filterShellcode) {
        m_status = "Remote allocation failed";
        shutdown(proc);
        return false;
    }

    RayStorage ray{};
    proc.remoteWrite(reinterpret_cast<std::uintptr_t>(m_remoteRay), &ray, sizeof(ray));

    m_ready = true;
    m_status = "TraceShape ready";
    std::cout << "[GameTrace] TraceShape initialized\n";
    return true;
}

void GameTraceVis::shutdown(Process& proc) {
    if (m_remoteFilter) proc.remoteFree(m_remoteFilter);
    if (m_remoteTrace) proc.remoteFree(m_remoteTrace);
    if (m_remoteRay) proc.remoteFree(m_remoteRay);
    if (m_remoteCtx) proc.remoteFree(m_remoteCtx);
    if (m_traceShellcode) proc.remoteFree(m_traceShellcode);
    if (m_filterShellcode) proc.remoteFree(m_filterShellcode);
    if (m_filterCtxRemote) proc.remoteFree(m_filterCtxRemote);

    m_remoteFilter = nullptr;
    m_remoteTrace = nullptr;
    m_remoteRay = nullptr;
    m_remoteCtx = nullptr;
    m_traceShellcode = nullptr;
    m_filterShellcode = nullptr;
    m_filterCtxRemote = nullptr;
    m_filterLocalPawn = 0;
    m_traceShapeFn = nullptr;
    m_initFilterFn = nullptr;
    m_traceManager = 0;
    m_ready = false;
    m_status = "Not initialized";
}

bool GameTraceVis::ensureResolved(Process& proc, std::uintptr_t clientBase) {
    const std::size_t clientSize = proc.getModuleSize(L"client.dll");
    if (!clientSize) {
        m_status = "client.dll size unknown";
        return false;
    }

    const std::uintptr_t traceMgrInstr = pattern::findInModule(
        proc, clientBase, clientSize, kGameTraceManagerPattern);
    if (!traceMgrInstr) {
        m_status = "GameTraceManager pattern not found";
        return false;
    }

    const std::uintptr_t traceMgrPtr = pattern::resolveRip(proc, traceMgrInstr);
    m_traceManager = mem::read<std::uintptr_t>(proc, traceMgrPtr);
    if (!m_traceManager) {
        m_status = "GameTraceManager pointer null";
        return false;
    }

    const std::uintptr_t traceShapeFn = pattern::findInModule(
        proc, clientBase, clientSize, kTraceShapePattern);
    if (!traceShapeFn) {
        m_status = "TraceShape pattern not found";
        return false;
    }

    const std::uintptr_t initFilterFn = pattern::findInModule(
        proc, clientBase, clientSize, kInitTraceFilterPattern);
    if (!initFilterFn) {
        m_status = "TraceFilter init pattern not found";
        return false;
    }

    m_traceShapeFn = reinterpret_cast<void*>(traceShapeFn);
    m_initFilterFn = reinterpret_cast<void*>(initFilterFn);
    return true;
}

bool GameTraceVis::ensureFilter(Process& proc, std::uintptr_t localPawn) {
    if (m_remoteFilter && m_filterLocalPawn == localPawn)
        return true;

    if (m_remoteFilter) {
        proc.remoteFree(m_remoteFilter);
        m_remoteFilter = nullptr;
    }

    m_remoteFilter = proc.remoteAlloc(sizeof(TraceFilterStorage));
    if (!m_remoteFilter)
        return false;

    InitEntityTraceFilterCtxStorage ctx{};
    ctx.filter = static_cast<TraceFilterStorage*>(m_remoteFilter);
    ctx.fn = reinterpret_cast<InitTraceFilterFn>(m_initFilterFn);
    ctx.pSkipPawn = localPawn;
    ctx.mask = static_cast<std::uint32_t>(kMaskPlayerVisible);
    ctx.layer = 4;

    if (!proc.remoteWrite(reinterpret_cast<std::uintptr_t>(m_filterCtxRemote), &ctx, sizeof(ctx)))
        return false;

    HANDLE hThread = proc.createRemoteThread(m_filterShellcode, m_filterCtxRemote);
    if (!hThread)
        return false;

    WaitForSingleObject(hThread, 5000);
    CloseHandle(hThread);

    m_filterLocalPawn = localPawn;
    return true;
}

bool GameTraceVis::runTrace(Process& proc,
                            std::uintptr_t localPawn,
                            const Vec3& start,
                            const Vec3& end,
                            void* outTrace,
                            std::size_t traceSize)
{
    if (!m_ready || m_budget <= 0 || !m_traceShapeFn || !m_traceManager || !outTrace)
        return false;

    if (!ensureFilter(proc, localPawn))
        return false;

    TraceShapeCtxStorage ctx{};
    ctx.TraceShape = reinterpret_cast<TraceShapeFn>(m_traceShapeFn);
    ctx.pGameTraceManager = reinterpret_cast<void*>(m_traceManager);
    ctx.pTraceFilter = static_cast<TraceFilterStorage*>(m_remoteFilter);
    ctx.pTrace = static_cast<trace_storage_t*>(m_remoteTrace);
    ctx.ray = static_cast<RayStorage*>(m_remoteRay);
    ctx.vStartPos = start;
    ctx.vEndPos = end;

    if (!proc.remoteWrite(reinterpret_cast<std::uintptr_t>(m_remoteCtx), &ctx, sizeof(ctx)))
        return false;

    HANDLE hThread = proc.createRemoteThread(m_traceShellcode, m_remoteCtx);
    if (!hThread)
        return false;

    const DWORD waitResult = WaitForSingleObject(hThread, 800);
    CloseHandle(hThread);
    if (waitResult != WAIT_OBJECT_0)
        return false;

    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(proc.handle(), m_remoteTrace, outTrace, traceSize, &bytesRead)
        || bytesRead != traceSize)
    {
        return false;
    }

    --m_budget;
    return true;
}

void GameTraceVis::beginFrame(int maxTraces) {
    m_budget = (std::max)(0, maxTraces);
}

bool GameTraceVis::hasLineOfSight(Process& proc,
                                  std::uintptr_t localPawn,
                                  std::uintptr_t targetPawn,
                                  const Vec3& eye,
                                  const Vec3& target)
{
    if (!m_ready || m_budget <= 0)
        return false;

    Vec3 dir = target - eye;
    const float lenSq = dir.lengthSq();
    if (lenSq < 0.01f)
        return true;

    const float len = std::sqrtf(lenSq);
    dir = dir * (1.f / len);
    const Vec3 start = eye + dir * 2.f;

    trace_storage_t tr{};
    if (!runTrace(proc, localPawn, start, target, &tr, sizeof(tr)))
        return false;

    if (tr.Fraction >= 0.97f)
        return true;

    if (targetPawn) {
        const auto hitAddr = reinterpret_cast<std::uintptr_t>(tr.HitEntity);
        if (hitAddr && hitAddr == targetPawn)
            return true;
    }

    return false;
}

bool GameTraceVis::isPlayerVisible(Process& proc,
                                   std::uintptr_t localPawn,
                                   std::uintptr_t enemyPawn,
                                   const Vec3& eye,
                                   const Vec3& enemyOrigin)
{
    return hasLineOfSight(proc, localPawn, enemyPawn, eye, enemyOrigin);
}
