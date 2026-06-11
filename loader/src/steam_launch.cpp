#include "steam_launch.h"

#include <Windows.h>
#include <Shellapi.h>
#include <TlHelp32.h>
#include <cstdio>
#include <string>
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

bool waitForProcess(const wchar_t* exeName, DWORD timeoutMs) {
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < timeoutMs) {
        if (processRunning(exeName))
            return true;
        Sleep(400);
    }
    return processRunning(exeName);
}

bool terminateProcessByName(const wchar_t* exeName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool any = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName) != 0)
                continue;
            HANDLE proc = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
            if (!proc)
                continue;
            TerminateProcess(proc, 0);
            WaitForSingleObject(proc, 5000);
            CloseHandle(proc);
            any = true;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return any;
}

std::wstring readRegString(HKEY root, const wchar_t* subKey, const wchar_t* valueName) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return {};

    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t)) {
        RegCloseKey(key);
        return {};
    }

    std::vector<wchar_t> buf(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(key, valueName, nullptr, &type,
            reinterpret_cast<LPBYTE>(buf.data()), &bytes) != ERROR_SUCCESS) {
        RegCloseKey(key);
        return {};
    }
    RegCloseKey(key);
    return buf.data();
}

std::wstring resolveSteamExe() {
    std::wstring path = readRegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamExe");
    if (!path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
        return path;

    path = readRegString(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    if (!path.empty()) {
        if (path.back() != L'\\' && path.back() != L'/')
            path += L'\\';
        path += L"steam.exe";
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
            return path;
    }

    const wchar_t* fallbacks[] = {
        L"C:\\Program Files (x86)\\Steam\\steam.exe",
        L"C:\\Program Files\\Steam\\steam.exe",
    };
    for (const wchar_t* fb : fallbacks) {
        if (GetFileAttributesW(fb) != INVALID_FILE_ATTRIBUTES)
            return fb;
    }
    return {};
}

bool shellOpen(const wchar_t* target, const wchar_t* args, std::string& detail) {
    HINSTANCE rc = ShellExecuteW(nullptr, L"open", target, args, nullptr, SW_SHOW);
    if (reinterpret_cast<INT_PTR>(rc) <= 32) {
        detail = "Could not start Steam (ShellExecute failed).";
        return false;
    }
    return true;
}

} // namespace

bool steamShutdownAll(std::string& detail) {
    static const wchar_t* kTargets[] = {
        L"cs2.exe",
        L"gameoverlayui64.exe",
        L"steamwebhelper.exe",
        L"steamservice.exe",
        L"steam.exe",
    };

    detail.clear();
    for (const wchar_t* name : kTargets)
        terminateProcessByName(name);

    Sleep(800);

    if (processRunning(L"steam.exe") || processRunning(L"cs2.exe")) {
        detail = "Could not fully close Steam/CS2. Close them manually and reopen the loader.";
        return false;
    }
    return true;
}

bool steamLaunchGamePipeline(std::string& detail) {
    detail.clear();

    const std::wstring steamExe = resolveSteamExe();
    if (steamExe.empty()) {
        detail = "Steam installation not found.";
        return false;
    }

    if (!processRunning(L"steam.exe")) {
        if (!shellOpen(steamExe.c_str(), nullptr, detail))
            return false;
        if (!waitForProcess(L"steam.exe", 120000)) {
            detail = "Steam did not start. Open Steam manually and try inject again.";
            return false;
        }
        Sleep(2500);
    }

    if (!shellOpen(L"steam://run/730", nullptr, detail))
        return false;

    constexpr DWORD kCs2WaitMs = 3 * 60 * 1000;
    if (!waitForProcess(L"cs2.exe", kCs2WaitMs)) {
        detail = "Waiting for CS2 timed out (3 min). Log into Steam, then press inject again.";
        return false;
    }

    Sleep(4000);
    return true;
}
