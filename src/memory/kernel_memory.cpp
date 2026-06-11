#include "memory/kernel_memory.h"
#include "memory/driver_shared.h"
#include "debug/overlay_log.h"

#include <cstdio>
#include <cstring>
#include <vector>

KernelMemory& KernelMemory::instance() {
    static KernelMemory s_instance;
    return s_instance;
}

DriverManager::SetupResult KernelMemory::initialize() {
    return DriverManager::setupKernelDriver();
}

bool KernelMemory::openDevice() {
    closeDevice();
    m_device = CreateFileW(DRV_USER_PATH, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (m_device == INVALID_HANDLE_VALUE) {
        LOG_ERROR("[-] Failed to open kernel device (error %lu)\n", GetLastError());
        return false;
    }
    return ping();
}

void KernelMemory::closeDevice() {
    if (m_device != INVALID_HANDLE_VALUE) {
        CloseHandle(m_device);
        m_device = INVALID_HANDLE_VALUE;
    }
}

bool KernelMemory::ping() const {
    if (m_device == INVALID_HANDLE_VALUE)
        return false;

    PingResponse response{};
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(m_device, IOCTL_PING, nullptr, 0,
                                    &response, sizeof(response), &returned, nullptr);
    return ok && returned == sizeof(PingResponse) && response.magic == PING_MAGIC;
}

bool KernelMemory::readRaw(std::uintptr_t address, void* buffer, std::size_t size) const {
    if (m_device == INVALID_HANDLE_VALUE || !m_pid || !buffer || size == 0)
        return false;

    auto* dst = static_cast<std::uint8_t*>(buffer);
    std::size_t remaining = size;
    std::uintptr_t cursor = address;

    while (remaining > 0) {
        const std::size_t chunk = (std::min)(remaining, static_cast<std::size_t>(DRIVER_MAX_READ_SIZE));

        ReadMemoryRequest request{};
        request.target_pid = m_pid;
        request.source_address = static_cast<unsigned long long>(cursor);
        request.read_size = static_cast<unsigned long>(chunk);
        request.padding = 0;

        DWORD returned = 0;
        const BOOL ok = DeviceIoControl(m_device, IOCTL_READ_MEMORY,
                                        &request, sizeof(request),
                                        dst, static_cast<DWORD>(chunk),
                                        &returned, nullptr);
        if (!ok || returned != static_cast<DWORD>(chunk))
            return false;

        dst += chunk;
        cursor += chunk;
        remaining -= chunk;
    }

    return true;
}

bool KernelMemory::writeRaw(std::uintptr_t address, const void* buffer, std::size_t size) const {
    if (m_device == INVALID_HANDLE_VALUE || !m_pid || !buffer || size == 0)
        return false;

    auto* src = static_cast<const std::uint8_t*>(buffer);
    std::size_t remaining = size;
    std::uintptr_t cursor = address;

    while (remaining > 0) {
        const std::size_t chunk = (std::min)(remaining, static_cast<std::size_t>(DRIVER_MAX_WRITE_SIZE));

        WriteMemoryRequest request{};
        request.target_pid = m_pid;
        request.dest_address = static_cast<unsigned long long>(cursor);
        request.write_size = static_cast<unsigned long>(chunk);
        request.padding = 0;

        std::vector<std::uint8_t> input(sizeof(WriteMemoryRequest) + chunk);
        std::memcpy(input.data(), &request, sizeof(request));
        std::memcpy(input.data() + sizeof(request), src, chunk);

        DWORD returned = 0;
        const BOOL ok = DeviceIoControl(m_device, IOCTL_WRITE_MEMORY,
                                        input.data(), static_cast<DWORD>(input.size()),
                                        nullptr, 0,
                                        &returned, nullptr);
        if (!ok)
            return false;

        src += chunk;
        cursor += chunk;
        remaining -= chunk;
    }

    return true;
}

std::uintptr_t KernelMemory::getPeb() const {
    if (m_device == INVALID_HANDLE_VALUE || !m_pid)
        return 0;

    GetPebRequest request{};
    request.target_pid = m_pid;

    GetPebRequest response{};
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(m_device, IOCTL_GET_PEB,
                                    &request, sizeof(request),
                                    &response, sizeof(response),
                                    &returned, nullptr);
    if (!ok || returned != sizeof(GetPebRequest) || response.peb_address == 0)
        return 0;

    return static_cast<std::uintptr_t>(response.peb_address);
}

std::uintptr_t KernelMemory::moduleBase(const wchar_t* moduleName, std::size_t* outSize) const {
    if (m_device == INVALID_HANDLE_VALUE || !m_pid || !moduleName || !moduleName[0])
        return 0;

    ModuleBaseRequest request{};
    request.target_pid = m_pid;
    wcsncpy_s(request.module_name, moduleName, _TRUNCATE);

    ModuleBaseRequest response{};
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(m_device, IOCTL_GET_MODULE_BASE,
                                    &request, sizeof(request),
                                    &response, sizeof(response),
                                    &returned, nullptr);
    if (!ok || returned != sizeof(ModuleBaseRequest) || response.base_address == 0)
        return 0;

    if (outSize)
        *outSize = static_cast<std::size_t>(response.module_size);
    return static_cast<std::uintptr_t>(response.base_address);
}
