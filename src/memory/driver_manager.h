#pragma once

#include <string>

class DriverManager {
public:
    enum class SetupResult {
        Ready,
        NeedReboot,
        NeedAdmin,
        DriverFileMissing,
        MapperMissing,
        SetupFailed,
    };

    struct SystemStatus {
        bool is_admin = false;
        bool driver_file_exists = false;
        bool mapper_exists = false;
        bool driver_responding = false;
        bool vulnerable_driver_blocklist_enabled = false;
        bool memory_integrity_enabled = false;
    };

    static SystemStatus checkSystem();
    static void printStatus(const SystemStatus& status);
    static void printLoadBlockerScan();

    /// Map kernel driver via bundled mapper if needed. May require reboot first.
    static SetupResult setupKernelDriver();

    static bool pingDriver();
    static std::wstring resolveDriverSysPath();
    static std::wstring resolveMapperExePath();

    /** Human-readable detail from the last setupKernelDriver failure (mapper log tail, etc.). */
    static const std::string& lastSetupDetail();
};
