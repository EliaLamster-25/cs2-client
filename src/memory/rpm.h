#pragma once

#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <string>
#include "process.h"
#include "memory/kernel_memory.h"

/// Type-safe memory read/write — routes through kernel driver when Process uses kernel mode.

namespace mem {

inline bool readRaw(const Process& proc, std::uintptr_t address, void* out, std::size_t size) {
    if (!out || size == 0)
        return false;
    if (proc.usesKernelMemory())
        return KernelMemory::instance().readRaw(address, out, size);
    if (!proc.handle())
        return false;
    SIZE_T bytesRead = 0;
    return ReadProcessMemory(proc.handle(), reinterpret_cast<LPCVOID>(address),
                             out, size, &bytesRead) != 0 && bytesRead == size;
}

template <typename T>
T read(const Process& proc, std::uintptr_t address) {
    T value{};
    readRaw(proc, address, &value, sizeof(T));
    return value;
}

template <typename T>
bool readArray(const Process& proc, std::uintptr_t address, T* out, std::size_t count) {
    if (!out || count == 0)
        return false;
    return readRaw(proc, address, out, sizeof(T) * count);
}

inline std::string readString(const Process& proc, std::uintptr_t address, std::size_t maxLen = 128) {
    char buf[256]{};
    if (maxLen > sizeof(buf))
        maxLen = sizeof(buf);
    readRaw(proc, address, buf, maxLen);
    buf[maxLen - 1] = '\0';
    return std::string(buf);
}

inline std::uintptr_t resolveChain(const Process& proc,
                                   std::uintptr_t base,
                                   std::initializer_list<std::uintptr_t> offsets) {
    std::uintptr_t addr = base;
    for (auto off : offsets) {
        addr = read<std::uintptr_t>(proc, addr + off);
        if (!addr)
            return 0;
    }
    return addr;
}

inline bool writeRaw(const Process& proc, std::uintptr_t address, const void* data, std::size_t size) {
    if (!data || size == 0)
        return false;
    if (proc.usesKernelMemory())
        return KernelMemory::instance().writeRaw(address, data, size);
    if (!proc.handle())
        return false;
    SIZE_T bytesWritten = 0;
    return WriteProcessMemory(proc.handle(), reinterpret_cast<LPVOID>(address),
                              data, size, &bytesWritten) != 0
           && bytesWritten == size;
}

template <typename T>
bool write(const Process& proc, std::uintptr_t address, const T& value) {
    return writeRaw(proc, address, &value, sizeof(T));
}

} // namespace mem
