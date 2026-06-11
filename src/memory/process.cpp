#include "process.h"
#include "memory/kernel_memory.h"
#include "memory/rpm.h"

#include <cstdint>
#include <iostream>

namespace {

bool iequals(std::wstring a, std::wstring b) {
    auto toLower = [](wchar_t c) -> wchar_t {
        if (c >= L'A' && c <= L'Z')
            return static_cast<wchar_t>(c - (L'A' - L'a'));
        return c;
    };
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (toLower(a[i]) != toLower(b[i]))
            return false;
    }
    return true;
}

std::uintptr_t queryPebAddressKernel() {
    return KernelMemory::instance().getPeb();
}

std::uintptr_t walkPebForModule(const Process& proc,
                                std::uintptr_t peb,
                                const std::wstring& moduleName,
                                std::size_t* outSize)
{
    if (!peb)
        return 0;

    const std::uintptr_t ldr = mem::read<std::uintptr_t>(proc, peb + 0x18);
    if (!ldr)
        return 0;

    const std::uintptr_t listHead = ldr + 0x10;
    std::uintptr_t entry = mem::read<std::uintptr_t>(proc, listHead);
    for (int guard = 0; guard < 512 && entry && entry != listHead; ++guard) {
        const std::uintptr_t dllBase = mem::read<std::uintptr_t>(proc, entry + 0x30);
        const USHORT nameLen = mem::read<USHORT>(proc, entry + 0x58);
        const std::uintptr_t namePtr = mem::read<std::uintptr_t>(proc, entry + 0x60);

        if (dllBase && namePtr && nameLen >= 2 && nameLen < 512) {
            wchar_t nameBuf[260]{};
            const std::size_t readBytes = (std::min)(static_cast<std::size_t>(nameLen),
                                                     sizeof(nameBuf) - sizeof(wchar_t));
            if (mem::readRaw(proc, namePtr, nameBuf, readBytes)) {
                nameBuf[readBytes / sizeof(wchar_t)] = L'\0';
                if (iequals(nameBuf, moduleName)) {
                    if (outSize) {
                        const ULONG imageSize = mem::read<ULONG>(proc, entry + 0x40);
                        *outSize = static_cast<std::size_t>(imageSize);
                    }
                    return dllBase;
                }
            }
        }

        entry = mem::read<std::uintptr_t>(proc, entry);
    }

    return 0;
}

std::uintptr_t moduleBaseViaToolhelp(DWORD pid, const std::wstring& moduleName, std::size_t* outSize) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    std::uintptr_t base = 0;
    if (Module32FirstW(snap, &me)) {
        do {
            if (iequals(me.szModule, moduleName)) {
                base = reinterpret_cast<std::uintptr_t>(me.modBaseAddr);
                if (outSize)
                    *outSize = static_cast<std::size_t>(me.modBaseSize);
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return base;
}

} // namespace

Process::~Process() {
    detach();
}

static DWORD findProcessId(const std::wstring& processName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD pid = 0;

    if (Process32FirstW(snap, &entry)) {
        do {
            if (processName == entry.szExeFile) {
                pid = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &entry));
    }

    CloseHandle(snap);
    return pid;
}

bool Process::attach(const std::wstring& processName, bool preferKernel) {
    const DWORD pid = findProcessId(processName);
    if (!pid) {
        std::wcerr << L"[Process] Could not find " << processName << L'\n';
        return false;
    }

    closeHandle();
    m_pid = pid;
    m_useKernel = false;
    m_moduleBases.clear();
    m_moduleSizes.clear();

    if (preferKernel && KernelMemory::instance().isReady()) {
        m_useKernel = true;
        KernelMemory::instance().setTargetPid(m_pid);
        std::wcout << L"[Process] Attached (kernel) to " << processName
                   << L" (PID " << m_pid << L")\n";
        return true;
    }

    m_handle = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, m_pid);
    if (!m_handle) {
        std::wcerr << L"[Process] OpenProcess failed (error " << GetLastError() << L")\n";
        m_pid = 0;
        return false;
    }

    std::wcout << L"[Process] Attached (Win32) to " << processName
               << L" (PID " << m_pid << L")\n";
    return true;
}

void Process::detach() {
    closeHandle();
    m_pid = 0;
    m_useKernel = false;
    m_moduleBases.clear();
    m_moduleSizes.clear();
}

bool Process::ensureHandle() {
    if (m_useKernel)
        return KernelMemory::instance().isReady();
    if (m_handle)
        return true;
    if (!m_pid)
        return false;
    m_handle = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, m_pid);
    m_isExtended = false;
    return m_handle != nullptr;
}

bool Process::openHandle() {
    return ensureHandle();
}

bool Process::openExtendedHandle() {
    if (m_useKernel) {
        std::cerr << "[Process] Extended handle unavailable in kernel-only mode.\n";
        return false;
    }
    if (m_handle && m_isExtended)
        return true;
    if (!m_pid)
        return false;
    HANDLE extended = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION
                                  | PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION,
                                  FALSE, m_pid);
    if (extended) {
        closeHandle();
        m_handle = extended;
        m_isExtended = true;
        return true;
    }
    return ensureHandle();
}

void* Process::remoteAlloc(std::size_t size, DWORD protect) {
    if (!m_handle || size == 0)
        return nullptr;
    return VirtualAllocEx(m_handle, nullptr, size, MEM_COMMIT | MEM_RESERVE, protect);
}

bool Process::remoteFree(void* address) {
    if (!m_handle || !address)
        return false;
    return VirtualFreeEx(m_handle, address, 0, MEM_RELEASE) != 0;
}

bool Process::remoteWrite(std::uintptr_t address, const void* data, std::size_t size) {
    if (m_useKernel)
        return mem::writeRaw(*this, address, data, size);
    if (!m_handle || !data || size == 0)
        return false;
    SIZE_T written = 0;
    return WriteProcessMemory(m_handle, reinterpret_cast<LPVOID>(address), data, size, &written) != 0
        && written == size;
}

void* Process::remoteCopyShellcode(const void* start, const void* end) {
    if (!start || !end || end <= start)
        return nullptr;
    const auto* bStart = static_cast<const std::uint8_t*>(start);
    const auto* bEnd = static_cast<const std::uint8_t*>(end);
    const std::size_t size = static_cast<std::size_t>(bEnd - bStart);
    void* remote = remoteAlloc(size, PAGE_EXECUTE_READWRITE);
    if (!remote)
        return nullptr;
    if (!remoteWrite(reinterpret_cast<std::uintptr_t>(remote), start, size)) {
        remoteFree(remote);
        return nullptr;
    }
    return remote;
}

HANDLE Process::createRemoteThread(void* startAddress, void* parameter) {
    if (!m_handle || !startAddress)
        return nullptr;
    return ::CreateRemoteThread(m_handle, nullptr, 0,
                                reinterpret_cast<LPTHREAD_START_ROUTINE>(startAddress),
                                parameter, 0, nullptr);
}

std::size_t Process::getModuleSize(const std::wstring& moduleName) const {
    if (!m_pid)
        return 0;

    if (m_useKernel) {
        auto it = m_moduleSizes.find(moduleName);
        if (it != m_moduleSizes.end())
            return it->second;
        std::size_t size = 0;
        if (KernelMemory::instance().moduleBase(moduleName.c_str(), &size) != 0) {
            m_moduleSizes[moduleName] = size;
            return size;
        }
        return 0;
    }

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, m_pid);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    std::size_t size = 0;
    if (Module32FirstW(snap, &me)) {
        do {
            if (moduleName == me.szModule) {
                size = static_cast<std::size_t>(me.modBaseSize);
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return size;
}

void Process::closeHandle() {
    if (m_handle) {
        CloseHandle(m_handle);
        m_handle = nullptr;
    }
    m_isExtended = false;
}

std::uintptr_t Process::getModuleBase(const std::wstring& moduleName) const {
    if (!m_pid)
        return 0;

    if (m_useKernel) {
        auto cached = m_moduleBases.find(moduleName);
        if (cached != m_moduleBases.end())
            return cached->second;

        std::size_t size = 0;
        std::uintptr_t base = KernelMemory::instance().moduleBase(moduleName.c_str(), &size);

        if (!base) {
            const std::uintptr_t peb = queryPebAddressKernel();
            base = walkPebForModule(*this, peb, moduleName, &size);
            if (base)
                std::wcout << L"[Process] " << moduleName << L" via PEB walk @ 0x"
                           << std::hex << base << std::dec << L'\n';
        }

        if (!base) {
            base = moduleBaseViaToolhelp(m_pid, moduleName, &size);
            if (base)
                std::wcout << L"[Process] " << moduleName << L" via toolhelp @ 0x"
                           << std::hex << base << std::dec << L'\n';
        }

        if (base) {
            m_moduleBases[moduleName] = base;
            m_moduleSizes[moduleName] = size;
        }
        return base;
    }

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, m_pid);
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
