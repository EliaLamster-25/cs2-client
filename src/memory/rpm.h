#pragma once

#include <Windows.h>
#include <cstdint>
#include <cstring>
#include "process.h"

/// @file rpm.h
/// @brief Type-safe ReadProcessMemory wrapper templates.
///
/// All game-memory reads go through these helpers so the rest of the
/// codebase never touches Win32 RPM directly.

namespace mem {

    /// Read a single value of type T from the target process.
    /// @return The value read, or a zero-initialized T on failure.
    template <typename T>
    T read(const Process& proc, std::uintptr_t address) {
        T value{};
        ReadProcessMemory(proc.handle(),
                          reinterpret_cast<LPCVOID>(address),
                          &value, sizeof(T), nullptr);
        return value;
    }

    /// Read a contiguous buffer of `count` elements.
    template <typename T>
    bool readArray(const Process& proc, std::uintptr_t address,
                   T* out, std::size_t count)
    {
        SIZE_T bytesRead = 0;
        BOOL ok = ReadProcessMemory(proc.handle(),
                                    reinterpret_cast<LPCVOID>(address),
                                    out, sizeof(T) * count,
                                    &bytesRead);
        return ok && bytesRead == sizeof(T) * count;
    }

    /// Convenience: read a null-terminated string (up to maxLen chars).
    inline std::string readString(const Process& proc,
                                  std::uintptr_t address,
                                  std::size_t maxLen = 128)
    {
        char buf[256]{};
        if (maxLen > sizeof(buf)) maxLen = sizeof(buf);
        ReadProcessMemory(proc.handle(),
                          reinterpret_cast<LPCVOID>(address),
                          buf, maxLen, nullptr);
        buf[maxLen - 1] = '\0';
        return std::string(buf);
    }

    /// Follow a pointer chain: base → [base + off0] → [… + off1] → …
    inline std::uintptr_t resolveChain(const Process& proc,
                                       std::uintptr_t base,
                                       std::initializer_list<std::uintptr_t> offsets)
    {
        std::uintptr_t addr = base;
        for (auto off : offsets) {
            addr = read<std::uintptr_t>(proc, addr + off);
            if (!addr) return 0;
        }
        return addr;
    }

    /// Write a single value of type T to the target process.
    template <typename T>
    bool write(const Process& proc, std::uintptr_t address, const T& value) {
        SIZE_T bytesWritten = 0;
        return WriteProcessMemory(proc.handle(),
                                  reinterpret_cast<LPVOID>(address),
                                  &value, sizeof(T), &bytesWritten) != 0
               && bytesWritten == sizeof(T);
    }

} // namespace mem
