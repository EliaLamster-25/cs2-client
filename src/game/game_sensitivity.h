#pragma once

#include "memory/process.h"
#include "memory/rpm.h"
#include "offsets/netvars.h"
#include "offsets/offsets.h"
#include <cmath>
#include <cstdint>

inline bool isValidGameSensitivity(float sens) {
    return std::isfinite(sens) && sens >= 0.01f && sens <= 50.f;
}

inline bool readGameSensitivity(const Process& proc,
                                std::uintptr_t clientBase,
                                float& out,
                                std::uintptr_t localPawn = 0)
{
    if (!clientBase || offsets::client::dwSensitivity == 0)
        return false;

    const std::uintptr_t sensRva = offsets::client::dwSensitivity;
    const std::uintptr_t kFieldOffsets[] = {
        0x58u,
        0x50u,
        0x48u,
    };

    const std::uintptr_t sensHolder = mem::read<std::uintptr_t>(proc, clientBase + sensRva);
    if (sensHolder >= 0x10000ull) {
        for (const std::uintptr_t field : kFieldOffsets) {
            const float viaPtr = mem::read<float>(proc, sensHolder + field);
            if (isValidGameSensitivity(viaPtr)) {
                out = viaPtr;
                return true;
            }
        }
    }

    for (const std::uintptr_t field : kFieldOffsets) {
        const float inlineSens = mem::read<float>(proc, clientBase + sensRva + field);
        if (isValidGameSensitivity(inlineSens)) {
            out = inlineSens;
            return true;
        }
    }

    if (localPawn >= 0x10000ull) {
        const float pawnSens = mem::read<float>(proc, localPawn + netvars::pawn::m_flMouseSensitivity);
        if (isValidGameSensitivity(pawnSens)) {
            out = pawnSens;
            return true;
        }
    }

    return false;
}
