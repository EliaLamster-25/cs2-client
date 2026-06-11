#pragma once

#include <Windows.h>
#include <TlHelp32.h>
#include <cstdint>
#include <string>
#include <unordered_map>

/// @file process.h
/// @brief Process attachment for external memory access (kernel driver or Win32 fallback).

class Process {
public:
    Process() = default;
    ~Process();

    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    /// Attach by process name. When kernel memory is ready, avoids OpenProcess for reads.
    bool attach(const std::wstring& processName, bool preferKernel = true);

    void detach();

    std::uintptr_t getModuleBase(const std::wstring& moduleName) const;
    std::size_t getModuleSize(const std::wstring& moduleName) const;

    bool ensureHandle();
    bool openHandle();
    void closeHandle();

    HANDLE handle() const { return m_handle; }

    bool openExtendedHandle();
    void* remoteAlloc(std::size_t size, DWORD protect = PAGE_READWRITE);
    bool remoteFree(void* address);
    bool remoteWrite(std::uintptr_t address, const void* data, std::size_t size);
    void* remoteCopyShellcode(const void* start, const void* end);
    HANDLE createRemoteThread(void* startAddress, void* parameter);

    DWORD pid() const { return m_pid; }
    bool isAttached() const { return m_pid != 0; }

    /// True when reads route through kernel driver (no RPM handle required).
    bool usesKernelMemory() const { return m_useKernel; }

private:
    HANDLE m_handle = nullptr;
    DWORD  m_pid    = 0;
    bool   m_isExtended = false;
    bool   m_useKernel = false;

    mutable std::unordered_map<std::wstring, std::uintptr_t> m_moduleBases;
    mutable std::unordered_map<std::wstring, std::size_t> m_moduleSizes;
};
