#include "memory/driver_manager.h"
#include "memory/driver_shared.h"
#include "debug/overlay_log.h"

#include <Windows.h>
#include <ShlObj.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <cstdio>
#include <cwchar>

namespace {

std::string g_lastSetupDetail;

const std::string& lastSetupDetailRef() {
    return g_lastSetupDetail;
}

void setSetupDetail(std::string detail) {
    g_lastSetupDetail = std::move(detail);
}

std::wstring crymoreLogDirectory() {
    wchar_t path[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path))) {
        std::wstring dir = path;
        dir += L"\\crymore";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir;
    }
    return L".";
}

void appendCrymoreLogFile(const wchar_t* fileName, const std::string& text) {
    if (text.empty())
        return;
    if (fileName && wcscmp(fileName, L"overlay.log") == 0) {
        overlayFileLog(text);
        return;
    }
    const std::wstring path = crymoreLogDirectory() + L"\\" + fileName;
    std::ofstream out(path, std::ios::app | std::ios::binary);
    if (!out)
        return;
    out << text;
    if (!text.empty() && text.back() != '\n')
        out << '\n';
}

std::string readTailUtf8(const std::wstring& path, std::size_t maxBytes) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
        return {};
    const auto size = static_cast<std::size_t>(in.tellg());
    const auto skip = size > maxBytes ? size - maxBytes : 0;
    in.seekg(static_cast<std::streamoff>(skip));
    std::string data(size - skip, '\0');
    if (!data.empty())
        in.read(data.data(), static_cast<std::streamsize>(data.size()));
    return data;
}

HANDLE createInheritableFile(const wchar_t* path, DWORD access, DWORD creation) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    return CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, creation,
        FILE_ATTRIBUTE_NORMAL, nullptr);
}

bool driverImageMatchesIdentity(const std::wstring& sysPath) {
    std::ifstream in(sysPath, std::ios::binary);
    if (!in)
        return false;

    std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.size() < 8)
        return false;

    const wchar_t* suffix = DRV_DEVICE_SUFFIX_W;
    const size_t suffixLen = std::wcslen(suffix);
    if (suffixLen == 0)
        return false;

    for (size_t i = 0; i + suffixLen * sizeof(wchar_t) <= data.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < suffixLen; ++j) {
            const wchar_t ch = *reinterpret_cast<const wchar_t*>(&data[i + j * sizeof(wchar_t)]);
            if (ch != suffix[j]) {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}

bool parseMapperDriverEntryStatus(const std::string& log, unsigned long& codeOut) {
    const char* keys[] = { "DriverEntry returned 0x", "DriverEntry returned 0X" };
    for (const char* key : keys) {
        const size_t pos = log.find(key);
        if (pos == std::string::npos)
            continue;
        const char* start = log.c_str() + pos + std::strlen(key);
        char* end = nullptr;
        codeOut = std::strtoul(start, &end, 16);
        return end != start;
    }
    return false;
}

std::string describeDriverEntryFailure(unsigned long code) {
    char hex[16];
    std::snprintf(hex, sizeof(hex), "0x%08lX", code);
    std::string msg = std::string("DriverEntry failed (") + hex + ")";
    switch (code) {
    case 0xC0000035u:
        msg += " - driver name still registered from a previous session.";
        msg += " Try inject again (should recover automatically). If it persists, fully shut down (disable Fast Startup) and reboot.";
        break;
    case 0xC0000022u:
        msg += " - access denied. Disable Core Isolation / Memory Integrity and reboot.";
        break;
    case 0xC000009Au:
        msg += " - insufficient resources. Another kernel blocker may be active; reboot.";
        break;
    default:
        msg += ". See mapper.log in %LOCALAPPDATA%\\crymore\\";
        break;
    }
    return msg;
}

std::wstring parentDirectory(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos)
        return path;
    return path.substr(0, pos + 1);
}

std::wstring exeDirectory() {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    return parentDirectory(exePath);
}

bool fileExists(const std::wstring& path) {
    return !path.empty() && std::filesystem::exists(path);
}

std::wstring firstExisting(const std::vector<std::wstring>& candidates) {
    for (const auto& path : candidates) {
        if (fileExists(path))
            return path;
    }
    return {};
}

std::wstring firstExistingInDirs(const std::vector<std::wstring>& dirs,
                                 const std::vector<std::wstring>& fileNames) {
    for (const auto& dir : dirs) {
        for (const auto& name : fileNames) {
            const std::wstring path = dir + name;
            if (fileExists(path))
                return path;
        }
    }
    return {};
}

bool isAdmin() {
    BOOL admin = FALSE;
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID adminGroup = nullptr;
    if (AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                 DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(nullptr, adminGroup, &admin);
        FreeSid(adminGroup);
    }
    return admin != FALSE;
}

bool readRegDword(HKEY root, const wchar_t* subKey, const wchar_t* valueName, DWORD& out) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    DWORD size = sizeof(out);
    const LONG result = RegQueryValueExW(key, valueName, nullptr, nullptr,
                                         reinterpret_cast<LPBYTE>(&out), &size);
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

bool writeRegDword(HKEY root, const wchar_t* subKey, const wchar_t* valueName, DWORD value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, subKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    const LONG result = RegSetValueExW(key, valueName, 0, REG_DWORD,
                                       reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

bool vulnerableDriverBlocklistEnabled() {
    DWORD value = 1;
    if (!readRegDword(HKEY_LOCAL_MACHINE,
                      L"SYSTEM\\CurrentControlSet\\Control\\CI\\Config",
                      L"VulnerableDriverBlocklistEnable", value))
        return false;
    return value == 1;
}

bool memoryIntegrityEnabled() {
    DWORD value = 0;
    if (!readRegDword(HKEY_LOCAL_MACHINE,
                      L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity",
                      L"Enabled", value))
        return false;
    return value == 1;
}

bool disableVulnerableDriverBlocklist() {
    return writeRegDword(HKEY_LOCAL_MACHINE,
                         L"SYSTEM\\CurrentControlSet\\Control\\CI\\Config",
                         L"VulnerableDriverBlocklistEnable", 0);
}

bool disableMemoryIntegrity() {
    return writeRegDword(HKEY_LOCAL_MACHINE,
                         L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity",
                         L"Enabled", 0);
}

bool serviceExists(const wchar_t* name) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm)
        return false;
    SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_QUERY_STATUS);
    const bool exists = svc != nullptr;
    if (svc)
        CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return exists;
}

bool regDwordNonZero(HKEY root, const wchar_t* subKey, const wchar_t* valueName) {
    DWORD value = 0;
    if (!readRegDword(root, subKey, valueName, value))
        return false;
    return value != 0;
}

#ifdef OVERLAY_DEBUG_LOG
void printMapperFailureHints() {
    LOG_INFO("\n[!] Driver mapper failed (NtLoadDriver 0xC000009A).\n");
    LOG_INFO("  Windows blocked the vulnerable loader before your driver runs.\n");
    DriverManager::printLoadBlockerScan();
    LOG_INFO("  Fixes: reboot after disabling blocklist/HVCI; uninstall kernel AC services.\n\n");
}
#endif

bool runDriverMapper(const std::wstring& mapperExe, const std::wstring& driverSys) {
    setSetupDetail({});

    const std::wstring workDir = parentDirectory(mapperExe);
    const std::wstring cmdLine = L"\"" + mapperExe + L"\" \"" + driverSys + L"\"";
    std::vector<wchar_t> cmdBuffer(cmdLine.begin(), cmdLine.end());
    cmdBuffer.push_back(L'\0');

    wchar_t tempDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempDir);
    const std::wstring mapperLogPath = std::wstring(tempDir) + L"crymore_mapper.log";

    HANDLE hNullIn = createInheritableFile(L"NUL", GENERIC_READ, OPEN_EXISTING);
    HANDLE hLogOut = createInheritableFile(mapperLogPath.c_str(), GENERIC_WRITE, CREATE_ALWAYS);
    if (hNullIn == INVALID_HANDLE_VALUE || hLogOut == INVALID_HANDLE_VALUE) {
        if (hNullIn != INVALID_HANDLE_VALUE)
            CloseHandle(hNullIn);
        if (hLogOut != INVALID_HANDLE_VALUE)
            CloseHandle(hLogOut);
        setSetupDetail("Could not create mapper I/O handles.");
        return false;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hNullIn;
    si.hStdOutput = hLogOut;
    si.hStdError = hLogOut;

    PROCESS_INFORMATION pi{};

    LOG_INFO("[*] Mapper output:\n");
    LOG_INFO("----------------------------------------\n");

    const BOOL created = CreateProcessW(
        nullptr,
        cmdBuffer.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        workDir.empty() ? nullptr : workDir.c_str(),
        &si,
        &pi);

    CloseHandle(hNullIn);
    CloseHandle(hLogOut);

    if (!created) {
        const DWORD err = GetLastError();
        setSetupDetail("CreateProcess(mapper) failed: " + std::to_string(err));
        appendCrymoreLogFile(L"overlay.log", std::string("Mapper CreateProcess failed: ") + std::to_string(err));
        LOG_INFO("[-] CreateProcess(mapper) failed: %lu\n", err);
        return false;
    }

    const DWORD waitResult = WaitForSingleObject(pi.hProcess, 60000);
    if (waitResult == WAIT_TIMEOUT) {
        LOG_INFO("[-] Mapper timed out after 60 seconds.\n");
        TerminateProcess(pi.hProcess, 1);
    }

    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    const std::string mapperLog = readTailUtf8(mapperLogPath, 4096);
    if (!mapperLog.empty())
        appendCrymoreLogFile(L"mapper.log", mapperLog);

    LOG_INFO("----------------------------------------\n");

    unsigned long driverEntryCode = 0;
    if (parseMapperDriverEntryStatus(mapperLog, driverEntryCode) && driverEntryCode != 0) {
        LOG_INFO("[-] Mapper DriverEntry failed: 0x%lx\n", driverEntryCode);
        std::string detail = describeDriverEntryFailure(driverEntryCode);
        if (!mapperLog.empty()) {
            detail += "\n";
            detail += mapperLog;
        }
        setSetupDetail(detail);
        appendCrymoreLogFile(L"overlay.log", "Kernel driver entry failed:\n" + detail);
#ifdef OVERLAY_DEBUG_LOG
        printMapperFailureHints();
#endif
        return false;
    }

    if (exitCode != 0) {
        LOG_INFO("[-] Mapper exited with code %lu\n", exitCode);
        std::string detail = "Mapper exit code " + std::to_string(exitCode);
        if (!mapperLog.empty()) {
            detail += "\n";
            detail += mapperLog;
        }
        setSetupDetail(detail);
        appendCrymoreLogFile(L"overlay.log", "Kernel mapper failed:\n" + detail);
#ifdef OVERLAY_DEBUG_LOG
        printMapperFailureHints();
#endif
        return false;
    }

    LOG_INFO("[+] Mapper reported success.\n");
    return true;
}

} // namespace

DriverManager::SystemStatus DriverManager::checkSystem() {
    SystemStatus status{};
    status.is_admin = isAdmin();
    status.driver_file_exists = fileExists(resolveDriverSysPath());
    status.mapper_exists = fileExists(resolveMapperExePath());
    status.driver_responding = pingDriver();
    status.vulnerable_driver_blocklist_enabled = vulnerableDriverBlocklistEnabled();
    status.memory_integrity_enabled = memoryIntegrityEnabled();
    return status;
}

void DriverManager::printStatus(const SystemStatus& status) {
    const std::wstring driverPath = resolveDriverSysPath();
    const std::wstring mapperPath = resolveMapperExePath();

    LOG_INFO("\n=== Kernel backend status ===\n");
    LOG_INFO("  Admin:              %s\n", status.is_admin ? "yes" : "NO (required)");
    LOG_INFO("  Driver image:       %s\n", status.driver_file_exists ? "found" : "MISSING");
    if (status.driver_file_exists)
        LOG_INFOW(L"    -> %ls\n", driverPath.c_str());
    LOG_INFO("  Mapper:             %s\n", status.mapper_exists ? "found" : "MISSING");
    if (status.mapper_exists)
        LOG_INFOW(L"    -> %ls\n", mapperPath.c_str());
    LOG_INFO("  Driver responding:  %s\n", status.driver_responding ? "yes" : "no");
    LOG_INFO("  Driver blocklist:   %s\n",
             status.vulnerable_driver_blocklist_enabled ? "ON (needs reboot after disable)" : "off");
    LOG_INFO("  Memory integrity:   %s\n",
             status.memory_integrity_enabled ? "ON (needs reboot after disable)" : "off");
    LOG_INFO("============================\n\n");
    if (!status.driver_responding)
        printLoadBlockerScan();
}

void DriverManager::printLoadBlockerScan() {
    LOG_INFO("[*] Common kernel load blockers:\n");

    static const wchar_t* kKnownServices[] = {
        L"vgk",
        L"vgc",
        L"FACEIT",
        L"EasyAntiCheat_EOS",
        L"EasyAntiCheat",
        L"BEDaisy",
        L"bedaisy",
    };

    bool found = false;
    for (const wchar_t* svc : kKnownServices) {
        if (serviceExists(svc)) {
            LOG_INFOW(L"  [!] Service present: %ls  -> uninstall required (disable is not enough)\n", svc);
            found = true;
        }
    }

    if (regDwordNonZero(HKEY_LOCAL_MACHINE,
                        L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard",
                        L"EnableVirtualizationBasedSecurity")) {
        LOG_INFO("  [!] VBS (Virtualization-Based Security) appears ENABLED\n");
        found = true;
    }

    if (vulnerableDriverBlocklistEnabled()) {
        LOG_INFO("  [!] Vulnerable Driver Blocklist still ON\n");
        found = true;
    }

    if (memoryIntegrityEnabled()) {
        LOG_INFO("  [!] Memory Integrity / HVCI still ON\n");
        found = true;
    }

    if (!found)
        LOG_INFO("  [i] No known AC services found.\n");
    LOG_INFO("\n");
}

std::vector<std::wstring> driverImageNames() {
    std::vector<std::wstring> names;
    names.emplace_back(DRV_FILE_NAME);
#ifdef OVERLAY_DEBUG_LOG
    names.emplace_back(L"MemReaderKdmp.sys");
#endif
    return names;
}

std::wstring DriverManager::resolveDriverSysPath() {
    const std::wstring exeDir = exeDirectory();
    const std::wstring named = firstExisting({ exeDir + DRV_FILE_NAME });
    if (!named.empty())
        return named;

    return firstExistingInDirs({
        exeDir,
        exeDir + L"..\\PooDriver\\x64\\Release\\",
        exeDir + L"..\\..\\PooDriver\\x64\\Release\\",
        exeDir + L"..\\PooDriver\\",
        exeDir + L"..\\..\\PooDriver\\",
    }, driverImageNames());
}

std::wstring DriverManager::resolveMapperExePath() {
    const std::wstring exeDir = exeDirectory();
    return firstExisting({ exeDir + DRV_MAPPER_NAME });
}

bool DriverManager::pingDriver() {
    HANDLE device = CreateFileW(DRV_USER_PATH, GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, 0, nullptr);
    if (device == INVALID_HANDLE_VALUE)
        return false;

    PingResponse response{};
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(device, IOCTL_PING, nullptr, 0,
                                  &response, sizeof(response), &returned, nullptr);
    CloseHandle(device);
    return ok && returned == sizeof(PingResponse) && response.magic == PING_MAGIC;
}

DriverManager::SetupResult DriverManager::setupKernelDriver() {
    if (!isAdmin()) {
        LOG_INFO("[-] Kernel mode requires Administrator.\n");
        return SetupResult::NeedAdmin;
    }

    const std::wstring driverPath = resolveDriverSysPath();
    if (driverPath.empty()) {
        LOG_INFO("[-] Kernel driver image not found beside executable.\n");
        return SetupResult::DriverFileMissing;
    }

    const std::wstring mapperPath = resolveMapperExePath();
    if (mapperPath.empty()) {
        LOG_INFO("[-] Driver mapper not found beside executable.\n");
        return SetupResult::MapperMissing;
    }

    bool needReboot = false;

    if (vulnerableDriverBlocklistEnabled()) {
        LOG_INFO("[*] Disabling Vulnerable Driver Blocklist (reboot required)...\n");
        if (!disableVulnerableDriverBlocklist())
            return SetupResult::SetupFailed;
        needReboot = true;
    }

    if (memoryIntegrityEnabled()) {
        LOG_INFO("[*] Disabling Memory Integrity / HVCI (reboot required)...\n");
        if (!disableMemoryIntegrity())
            return SetupResult::SetupFailed;
        needReboot = true;
    }

    if (needReboot) {
        LOG_INFO("[!] Reboot required before the driver can load.\n");
        return SetupResult::NeedReboot;
    }

    if (pingDriver()) {
        LOG_INFO("[+] Kernel driver already loaded.\n");
        return SetupResult::Ready;
    }

    /* Device node may exist but not respond (stale after overlay exit / Fast Startup). */
    {
        HANDLE stale = CreateFileW(DRV_USER_PATH, GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (stale != INVALID_HANDLE_VALUE) {
            CloseHandle(stale);
            LOG_INFO("[*] Stale driver registration detected — remapping to refresh...\n");
        }
    }

    LOG_INFO("[*] Mapping kernel driver...\n");
    LOG_INFOW(L"    image:  %ls\n", driverPath.c_str());
    LOG_INFOW(L"    mapper: %ls\n", mapperPath.c_str());

    if (!driverImageMatchesIdentity(driverPath)) {
        LOG_INFO("[-] Kernel driver image does not match current build identity.\n");
        setSetupDetail("Stale kernel driver (.sys) - rebuild with WDK and republish overlay pack.");
        appendCrymoreLogFile(L"overlay.log", g_lastSetupDetail);
        return SetupResult::SetupFailed;
    }

    if (!runDriverMapper(mapperPath, driverPath))
        return SetupResult::SetupFailed;

    for (int attempt = 0; attempt < 10; ++attempt) {
        if (pingDriver()) {
            LOG_INFO("[+] Kernel driver mapped and responding.\n");
            return SetupResult::Ready;
        }
        Sleep(300);
    }

    LOG_INFO("[-] Driver mapped but device ping failed.\n");
    setSetupDetail("Driver mapped but device ping failed (stale .sys or blocked load).");
    appendCrymoreLogFile(L"overlay.log", g_lastSetupDetail);
    return SetupResult::SetupFailed;
}

const std::string& DriverManager::lastSetupDetail() {
    return lastSetupDetailRef();
}
