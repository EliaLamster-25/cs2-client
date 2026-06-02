#pragma once

#include <Windows.h>
#include <TlHelp32.h>
#include <cstdint>
#include <string>

/// @file process.h
/// @brief Wraps Win32 process/module enumeration for external memory access.

class Process {
public:
    Process() = default;
    ~Process();

    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    /// Attach to a running process by name (e.g. L"cs2.exe").
    /// @return true on success.
    bool attach(const std::wstring& processName);

    /// Detach / close the handle.
    void detach();

    /// Look up the base address of a loaded module (e.g. L"client.dll").
    std::uintptr_t getModuleBase(const std::wstring& moduleName) const;

    /// Open a fresh process handle (closes any existing one).
    bool openHandle();

    /// Close the current process handle.
    void closeHandle();

    /// @return Raw Win32 handle — needed by RPM wrappers.
    HANDLE handle() const { return m_handle; }

    /// @return Process ID.
    DWORD pid() const { return m_pid; }

    /// @return True if the process was found (handle may be temporarily closed).
    bool isAttached() const { return m_pid != 0; }

private:
    HANDLE m_handle = nullptr;
    DWORD  m_pid    = 0;
};
