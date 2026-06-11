#pragma once

#include "memory/driver_manager.h"

#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <string>

/// Usermode client for kernel memory driver — cross-process reads when active.
class KernelMemory {
public:
    static KernelMemory& instance();

    DriverManager::SetupResult initialize();
    bool openDevice();
    void closeDevice();

    bool isReady() const { return m_device != INVALID_HANDLE_VALUE; }

    void setTargetPid(DWORD pid) { m_pid = pid; }
    DWORD targetPid() const { return m_pid; }

    bool ping() const;
    bool readRaw(std::uintptr_t address, void* buffer, std::size_t size) const;
    bool writeRaw(std::uintptr_t address, const void* buffer, std::size_t size) const;
    std::uintptr_t getPeb() const;
    std::uintptr_t moduleBase(const wchar_t* moduleName, std::size_t* outSize = nullptr) const;

private:
    KernelMemory() = default;

    HANDLE m_device = INVALID_HANDLE_VALUE;
    DWORD  m_pid = 0;
};
