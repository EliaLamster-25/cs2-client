#include "env_check.h"

#include <Windows.h>
#include <TlHelp32.h>
#include <vector>

namespace {

bool processRunning(const wchar_t* exeName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

bool serviceRunning(const wchar_t* serviceName) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm)
        return false;

    SC_HANDLE svc = OpenServiceW(scm, serviceName, SERVICE_QUERY_STATUS);
    if (!svc) {
        CloseServiceHandle(scm);
        return false;
    }

    SERVICE_STATUS status{};
    const bool ok = QueryServiceStatus(svc, &status) != 0;
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);

    // Also check if the service is in start-pending state (about to run)
    return ok && (status.dwCurrentState == SERVICE_RUNNING ||
                  status.dwCurrentState == SERVICE_START_PENDING);
}

// Kernel driver presence check (look for loaded kernel modules)
bool kernelDriverLoaded(const wchar_t* driverName) {
    // Enumerate loaded drivers via NtQuerySystemInformation
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll)
        return false;

    using NtQuerySystemInformationFn = NTSTATUS(NTAPI*)(
        ULONG, PVOID, ULONG, PULONG);
    auto ntQSI = reinterpret_cast<NtQuerySystemInformationFn>(
        GetProcAddress(ntdll, "NtQuerySystemInformation"));
    if (!ntQSI)
        return false;

    // SystemModuleInformation = 11
    ULONG needed = 0;
    ntQSI(11, nullptr, 0, &needed);
    if (needed == 0)
        return false;

    std::vector<BYTE> buf(needed + 0x1000);
    if (ntQSI(11, buf.data(), static_cast<ULONG>(buf.size()), &needed) < 0)
        return false;

    // RTL_PROCESS_MODULES structure
    auto* mods = reinterpret_cast<DWORD*>(buf.data());
    DWORD count = *mods;

    // Each module entry: PVOID Section, PVOID MappedBase, PVOID ImageBase,
    //   ULONG ImageSize, ULONG Flags, USHORT LoadOrderIndex, USHORT InitOrderIndex,
    //   USHORT LoadCount, USHORT OffsetToFileName, UCHAR FullPathName[256]
    struct ModuleEntry {
        PVOID Section;
        PVOID MappedBase;
        PVOID ImageBase;
        ULONG ImageSize;
        ULONG Flags;
        USHORT LoadOrderIndex;
        USHORT InitOrderIndex;
        USHORT LoadCount;
        USHORT OffsetToFileName;
        UCHAR FullPathName[256];
    };
    static_assert(sizeof(ModuleEntry) == 296, "ModuleEntry size mismatch");

    auto* entries = reinterpret_cast<ModuleEntry*>(mods + 1);
    for (DWORD i = 0; i < count; ++i) {
        const char* name = reinterpret_cast<const char*>(entries[i].FullPathName) +
                           entries[i].OffsetToFileName;
        // Convert to wchar for comparison
        wchar_t wname[256] = {};
        MultiByteToWideChar(CP_ACP, 0, name, -1, wname, 256);
        if (_wcsicmp(wname, driverName) == 0)
            return true;
    }

    return false;
}

// Check if CS2 is running
bool cs2Running() {
    return processRunning(L"cs2.exe");
}

// Check if Steam is running (not strictly AC but useful context)
bool steamRunning() {
    return processRunning(L"steam.exe") || processRunning(L"steamservice.exe");
}

} // namespace

EnvCheckResult runEnvironmentChecks() {
    EnvCheckResult out{};

    // ── Process-based checks ──────────────────────────────────────────────
    struct BlockRule {
        const wchar_t* process;
        const char* label;
        bool hardBlock;
    };

    static const BlockRule kProcessRules[] = {
        // Faceit
        { L"faceitclient.exe",        "FACEIT Client",           true  },
        { L"faceitservice.exe",       "FACEIT Service",          true  },
        { L"FACEIT.exe",              "FACEIT",                  true  },

        // Riot Vanguard
        { L"vgc.exe",                 "Riot Vanguard (vgc)",     true  },
        { L"vgtray.exe",              "Riot Vanguard Tray",      true  },

        // Easy Anti-Cheat
        { L"EasyAntiCheat.exe",       "Easy Anti-Cheat",         true  },
        { L"EasyAntiCheat_EOS.exe",   "Easy Anti-Cheat EOS",     true  },
        { L"EasyAntiCheat_Launcher.exe", "EAC Launcher",         true  },

        // BattlEye
        { L"BEService.exe",           "BattlEye Service",        true  },
        { L"BEService_x64.exe",       "BattlEye Service x64",    true  },
        { L"BEDaisy.exe",             "BattlEye Driver Loader",  true  },

        // ESEA
        { L"eseaclient.exe",          "ESEA Client",             true  },
        { L"esea.exe",                "ESEA",                    true  },

        // Other ACs
        { L"ACE-Tray.exe",            "ACE Anti-Cheat",          true  },
        { L"ACE-Guard.exe",           "ACE Guard",               true  },
        { L"GameGuard.exe",           "nProtect GameGuard",      true  },
        { L"GameMon.des",             "GameGuard Monitor",       true  },
        { L"PnkBstrA.exe",           "PunkBuster Service A",    true  },
        { L"PnkBstrB.exe",           "PunkBuster Service B",    true  },
        { L"EQU8.exe",               "EQU8 Anti-Cheat",         true  },
        { L"EQU8_Launcher.exe",      "EQU8 Launcher",            true  },
        { L"Neguard.exe",            "Neguard (Nexon)",          true  },
        { L"BlackCipher.aes",        "Nexon BlackCipher",        true  },
        { L"xigncode.exe",           "XIGNCODE3",                true  },
        { L"xm.exe",                 "XIGNCODE3 Manager",        true  },

        // Debugging / Analysis tools (warn but don't hard block)
        { L"cheatengine-x86_64.exe", "Cheat Engine",             false },
        { L"cheatengine-i386.exe",   "Cheat Engine",             false },
        { L"processhacker.exe",      "Process Hacker",           false },
    };

    for (const auto& rule : kProcessRules) {
        if (!processRunning(rule.process))
            continue;

        std::string msg = std::string(rule.label) + " is running - close it before loading.";
        if (rule.hardBlock) {
            out.ok = false;
            out.errors.push_back(msg);
        } else {
            out.warnings.push_back(msg);
        }
    }

    // ── Service-based checks ──────────────────────────────────────────────
    static const wchar_t* kBadServices[] = {
        L"vgk",       // Vanguard kernel service
        L"faceit",    // FACEIT service
        L"BEService", // BattlEye
        L"EasyAntiCheat",
        L"EasyAntiCheat_EOS",
    };

    for (const auto* svc : kBadServices) {
        if (!serviceRunning(svc))
            continue;
        out.ok = false;

        std::string name;
        // Convert wide service name to narrow for error message
        char narrow[64] = {};
        WideCharToMultiByte(CP_UTF8, 0, svc, -1, narrow, sizeof(narrow), nullptr, nullptr);
        out.errors.push_back(std::string(narrow) + " kernel service is active - "
                             "reboot and do not start the anti-cheat before loading.");
    }

    // ── Kernel driver checks ──────────────────────────────────────────────
    static const wchar_t* kBadDrivers[] = {
        L"vgk.sys",       // Vanguard
        L"BEDaisy.sys",   // BattlEye
        L"EasyAntiCheat.sys",
        L"ACE-BASE.sys",  // ACE
        L"ACE-GAME.sys",
        L"dump_wmimmc.sys", // GameGuard
        L"npggNT.des",    // nProtect
        L"xmag.dll",      // XIGNCODE3
        L"xhunter1.sys",
    };

    for (const auto* drv : kBadDrivers) {
        if (!kernelDriverLoaded(drv))
            continue;
        out.ok = false;

        char narrow[64] = {};
        WideCharToMultiByte(CP_UTF8, 0, drv, -1, narrow, sizeof(narrow), nullptr, nullptr);
        out.errors.push_back(std::string(narrow) + " kernel driver is loaded - "
                             "reboot before using the loader.");
    }

    // ── Game state checks ─────────────────────────────────────────────────
    // Steam/CS2 are managed by the loader inject pipeline (closed on boot, started on inject).

    // ── System requirements ───────────────────────────────────────────────
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    if (si.wProcessorArchitecture != PROCESSOR_ARCHITECTURE_AMD64) {
        out.ok = false;
        out.errors.push_back("64-bit Windows is required. 32-bit is not supported.");
    }

    // Windows version check (Win10+ required)
    // Check for Win10 build 1809+ or Win11
    using RtlGetVersionFn = NTSTATUS(NTAPI*)(PRTL_OSVERSIONINFOW);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        auto rtlGetVer = reinterpret_cast<RtlGetVersionFn>(
            GetProcAddress(ntdll, "RtlGetVersion"));
        if (rtlGetVer) {
            RTL_OSVERSIONINFOW vi{};
            vi.dwOSVersionInfoSize = sizeof(vi);
            if (rtlGetVer(&vi) >= 0) {
                if (vi.dwMajorVersion < 10) {
                    out.errors.push_back("Windows 10 or later is required.");
                    out.ok = false;
                } else if (vi.dwMajorVersion == 10 && vi.dwBuildNumber < 17763) {
                    out.warnings.push_back("Windows 10 build 1809+ recommended for best compatibility.");
                }
            }
        }
    }

    // Admin check (kernel driver needs admin for first load)
    BOOL isElevated = FALSE;
    HANDLE hToken = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION elev{};
        DWORD size = sizeof(elev);
        if (GetTokenInformation(hToken, TokenElevation, &elev, size, &size))
            isElevated = elev.TokenIsElevated;
        CloseHandle(hToken);
    }

    if (!isElevated) {
        out.warnings.push_back("Not running as Administrator - kernel driver loading will fail. "
                               "Right-click and 'Run as administrator' for full functionality.");
    }

    return out;
}
