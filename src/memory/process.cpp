#include "process.h"
#include <iostream>

/// @file process.cpp
/// @brief Process attach / module enumeration via Win32 Toolhelp snapshots.

Process::~Process() {
    detach();
}

bool Process::attach(const std::wstring& processName) {
    // Snapshot all processes.
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    bool found = false;
    if (Process32FirstW(snap, &entry)) {
        do {
            if (processName == entry.szExeFile) {
                m_pid = entry.th32ProcessID;
                found = true;
                break;
            }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);

    if (!found) {
        std::wcerr << L"[Process] Could not find " << processName << L'\n';
        return false;
    }

    m_handle = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
                           FALSE, m_pid);
    if (!m_handle) {
        std::wcerr << L"[Process] OpenProcess failed (error "
                   << GetLastError() << L")\n";
        return false;
    }

    std::wcout << L"[Process] Attached to " << processName
               << L" (PID " << m_pid << L")\n";
    return true;
}

void Process::detach() {
    if (m_handle) {
        CloseHandle(m_handle);
        m_handle = nullptr;
        m_pid    = 0;
    }
}

std::uintptr_t Process::getModuleBase(const std::wstring& moduleName) const {
    if (!m_pid) return 0;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                          m_pid);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);

    std::uintptr_t base = 0;
    if (Module32FirstW(snap, &me)) {
        do {
            if (moduleName == me.szModule) {
                base = reinterpret_cast<std::uintptr_t>(me.modBaseAddr);
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return base;
}
