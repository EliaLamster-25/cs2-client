#include "protect.h"

#include <Windows.h>
#include <winternl.h>
#include <TlHelp32.h>
#include <chrono>
#include <cstring>
#include <intrin.h>

// winternl.h doesn't expose all PROCESSINFOCLASS values — define the ones we need.
#ifndef ProcessDebugFlags
#define ProcessDebugFlags 31
#endif
#ifndef ProcessDebugObjectHandle
#define ProcessDebugObjectHandle 30
#endif

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {

using NtQueryInformationProcessFn = NTSTATUS(NTAPI*)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);
using NtSetInformationThreadFn = NTSTATUS(NTAPI*)(HANDLE, THREADINFOCLASS, PVOID, ULONG);
using NtQuerySystemInformationFn = NTSTATUS(NTAPI*)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);

// ── Debugger detection ─────────────────────────────────────────────────────

bool debuggerAttached() {
    // Basic check
    if (IsDebuggerPresent())
        return true;

    // Remote debugger
    BOOL remote = FALSE;
    CheckRemoteDebuggerPresent(GetCurrentProcess(), &remote);
    if (remote)
        return true;

    // NtQueryInformationProcess: DebugPort
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll)
        return false;

    auto ntQIP = reinterpret_cast<NtQueryInformationProcessFn>(
        GetProcAddress(ntdll, "NtQueryInformationProcess"));
    if (!ntQIP)
        return false;

    ULONG_PTR debugPort = 0;
    if (ntQIP(GetCurrentProcess(), ProcessDebugPort,
              &debugPort, sizeof(debugPort), nullptr) >= 0 && debugPort != 0)
        return true;

    // ProcessDebugFlags
    ULONG debugFlags = 0;
    if (ntQIP(GetCurrentProcess(), static_cast<PROCESSINFOCLASS>(ProcessDebugFlags),
              &debugFlags, sizeof(debugFlags), nullptr) >= 0 && debugFlags == 0)
        return true;

    // ProcessDebugObjectHandle
    HANDLE debugObj = nullptr;
    if (ntQIP(GetCurrentProcess(), static_cast<PROCESSINFOCLASS>(ProcessDebugObjectHandle),
              &debugObj, sizeof(debugObj), nullptr) >= 0 && debugObj != nullptr)
        return true;

    // PEB BeingDebugged (manual read)
    __try {
        auto* peb = reinterpret_cast<BYTE*>(__readgsqword(0x60));
        if (peb && *reinterpret_cast<BYTE*>(peb + 0x02))
            return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    // NtGlobalFlag in PEB
    __try {
        auto* peb = reinterpret_cast<BYTE*>(__readgsqword(0x60));
        if (peb) {
            DWORD nGlobal = *reinterpret_cast<DWORD*>(peb + 0xBC);
            if (nGlobal & 0x70)  // FLG_HEAP_ENABLE_TAIL_CHECK | FLG_HEAP_ENABLE_FREE_CHECK | FLG_HEAP_VALIDATE_PARAMETERS
                return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    return false;
}

// ── Anti-VM / sandbox detection ────────────────────────────────────────────

bool isVirtualMachine() {
    bool biosVm = false;
    bool productVm = false;

    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"HARDWARE\\DESCRIPTION\\System\\BIOS",
        0, KEY_READ, &hKey) == ERROR_SUCCESS) {

        wchar_t sysManufacturer[256] = {};
        DWORD size = sizeof(sysManufacturer);
        RegQueryValueExW(hKey, L"SystemManufacturer", nullptr, nullptr,
            reinterpret_cast<BYTE*>(sysManufacturer), &size);

        wchar_t sysProduct[256] = {};
        size = sizeof(sysProduct);
        RegQueryValueExW(hKey, L"SystemProductName", nullptr, nullptr,
            reinterpret_cast<BYTE*>(sysProduct), &size);

        RegCloseKey(hKey);

        // Guest VM BIOS strings — do not match bare-metal Hyper-V host / VBS.
        static const wchar_t* kGuestBios[] = {
            L"VMware", L"VirtualBox", L"VBOX", L"QEMU",
            L"innotek", L"Xen", L"Parallels", L"Bochs",
        };
        for (const auto* sig : kGuestBios) {
            if (wcsstr(sysManufacturer, sig) || wcsstr(sysProduct, sig))
                biosVm = true;
        }

        if (wcsstr(sysProduct, L"Virtual Machine") ||
            wcsstr(sysProduct, L"VMware") ||
            wcsstr(sysProduct, L"VirtualBox"))
            productVm = true;
    }

    int cpuInfo[4] = {};
    __cpuid(cpuInfo, 0x40000000);
    char hypervisor[13] = {};
    std::memcpy(hypervisor, &cpuInfo[1], 4);
    std::memcpy(hypervisor + 4, &cpuInfo[2], 4);
    std::memcpy(hypervisor + 8, &cpuInfo[3], 4);
    hypervisor[12] = '\0';

    // Known guest hypervisor vendors. "Microsoft Hv" alone is normal on physical
    // Windows 10/11 with VBS, WSL2, or Hyper-V platform — not a VM guest.
    static const char* kGuestHypervisors[] = {
        "VMwareVMware", "VBoxVBoxVBox", "KVMKVMKVM", "XenVMMXenVMM",
        "prl hyperv  ", "TCGTCGTCGTCG",
    };
    bool guestHypervisor = false;
    for (const auto* sig : kGuestHypervisors) {
        if (strncmp(hypervisor, sig, strlen(sig)) == 0) {
            guestHypervisor = true;
            break;
        }
    }

    // Hyper-V guest: Microsoft hypervisor + virtual machine product string.
    const bool hyperVGuest =
        strncmp(hypervisor, "Microsoft Hv", 12) == 0 && productVm;

    return biosVm || guestHypervisor || hyperVGuest;
}

// ── Timing-based anomaly detection ─────────────────────────────────────────

bool timingAnomaly() {
    // RDTSC overhead loop
    const auto t0 = __rdtsc();
    volatile int x = 0;
    for (volatile int i = 0; i < 100000; ++i)
        x += i;
    const auto t1 = __rdtsc();
    (void)x;

    // Expected ~100k-300k cycles for a simple loop. Debuggers/emulators can
    // inflate this by orders of magnitude.
    const auto delta = t1 - t0;
    return delta > 5000000;  // >5M cycles = suspicious
}

bool tickCountAnomaly() {
    // GetTickCount64 vs RDTSC ratio check
    const auto tsc0 = __rdtsc();
    const auto tc0 = GetTickCount64();
    Sleep(50);
    const auto tc1 = GetTickCount64();
    const auto tsc1 = __rdtsc();

    const auto elapsedTc = tc1 - tc0;
    const auto elapsedTsc = tsc1 - tsc0;

    if (elapsedTc < 35 || elapsedTc > 120)  // Sleep(50) — allow scheduler jitter
        return true;

    // Rough CPU frequency check. Very low TSC/ms can indicate single-stepping.
    if (elapsedTc > 0) {
        const auto tscPerMs = elapsedTsc / elapsedTc;
        if (tscPerMs < 200000)  // well below any modern CPU at idle
            return true;
    }

    return false;
}

// ── Tool/analysis detection ────────────────────────────────────────────────

bool blacklistedModules() {
    const wchar_t* badModules[] = {
        L"x64dbg", L"x32dbg", L"ollydbg.exe", L"ida.exe", L"ida64.exe",
        L"windbg.exe", L"dbgview.exe", L"procmon.exe", L"procexp.exe",
        L"processhacker.exe", L"cheatengine-x86_64.exe", L"cheatengine-i386.exe",
        L"cheatengine.exe", L"reclass.exe", L"reclass64.exe",
        L"scylla.exe", L"scylla_x64.exe", L"lordpe.exe", L"peid.exe",
        L"httpdebuggerui.exe", L"httpdebugger.exe", L"wireshark.exe",
        L"fiddler.exe", L"charles.exe", L"dnspy.exe", L"de4dot.exe",
    };

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                           GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE)
        return false;

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
        do {
            for (const auto* bad : badModules) {
                if (_wcsicmp(me.szModule, bad) == 0) {
                    CloseHandle(snap);
                    return true;
                }
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return false;
}

// ── Code integrity self-check ──────────────────────────────────────────────

bool selfIntegrityCheck() {
    // Verify our own PE headers haven't been patched
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(&__ImageBase);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
        reinterpret_cast<BYTE*>(&__ImageBase) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;

    // Verify section alignment hasn't been tampered
    if (nt->OptionalHeader.SectionAlignment < 0x200 ||
        nt->OptionalHeader.SectionAlignment > 0x10000)
        return false;

    return true;
}

} // namespace

ProtectReport runProtectionChecks() {
    ProtectReport out{};

    // 1. Debugger checks (hard block)
    if (debuggerAttached()) {
        out.ok = false;
        out.warnings.push_back("Debugger detected — loader refused to continue.");
        // For hard blocks, don't continue checking
        return out;
    }

    // 2. Tool detection
    if (blacklistedModules()) {
        out.ok = false;
        out.warnings.push_back("Analysis/reversing tools detected in process modules.");
        return out;
    }

    // 3. VM detection (warning only)
    if (isVirtualMachine()) {
        // Not a hard block — some users run on VMs legitimately
        out.warnings.push_back("Virtual machine environment detected (may cause issues).");
    }

    // 4. Self-integrity (warning)
    if (!selfIntegrityCheck()) {
        out.warnings.push_back("Loader integrity check: PE headers modified.");
    }

    // 5. Timing anomalies
    if (timingAnomaly()) {
        out.warnings.push_back("CPU timing anomaly detected (possible instrumentation).");
    }

    if (tickCountAnomaly()) {
        out.warnings.push_back("System timer inconsistency detected.");
    }

    return out;
}

bool protectQuickCheck(std::string& reason) {
    if (debuggerAttached()) {
        reason = "Debugger detected.";
        return false;
    }
    if (blacklistedModules()) {
        reason = "Analysis tools detected.";
        return false;
    }
    reason.clear();
    return true;
}
