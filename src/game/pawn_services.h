#pragma once

#include "memory/rpm.h"
#include "memory/process.h"
#include "offsets/netvars.h"

#include <cstdint>

namespace pawn_services {

inline bool isLikelyPtr(std::uintptr_t p) {
    return p > 0x10000 && p < 0x000F'FFFFFFFFFFull;
}

inline std::uintptr_t readWeaponServices(const Process& proc, std::uintptr_t pawn) {
    static constexpr std::uintptr_t kOffsets[] = {
        netvars::pawn::m_pWeaponServices,
        0x11E0u,
        0x13D8u,
    };
    for (std::uintptr_t off : kOffsets) {
        const auto ptr = mem::read<std::uintptr_t>(proc, pawn + off);
        if (isLikelyPtr(ptr))
            return ptr;
    }
    return 0;
}

inline std::uintptr_t readObserverServices(const Process& proc, std::uintptr_t pawn) {
    static constexpr std::uintptr_t kOffsets[] = {
        netvars::pawn::m_pObserverServices,
        0x13F0u,
        0x11F8u,
    };
    for (std::uintptr_t off : kOffsets) {
        const auto ptr = mem::read<std::uintptr_t>(proc, pawn + off);
        if (isLikelyPtr(ptr))
            return ptr;
    }
    return 0;
}

inline std::uint32_t readControllerHandle(const Process& proc, std::uintptr_t pawn) {
    static constexpr std::uintptr_t kOffsets[] = {
        netvars::pawn::m_hController,
        0x13A8u,
        0x15A0u,
    };
    for (std::uintptr_t off : kOffsets) {
        const auto handle = mem::read<std::uint32_t>(proc, pawn + off);
        if (handle && handle != 0xFFFFFFFFu)
            return handle;
    }
    return 0;
}

} // namespace pawn_services
