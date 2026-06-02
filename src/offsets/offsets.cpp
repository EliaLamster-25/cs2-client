#include "offsets.h"

#include "memory/process.h"

#include <Windows.h>
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
constexpr std::size_t kClientScanSize = 0x04000000;
constexpr std::size_t kScanChunkSize = 0x00020000;

bool isReadableProtection(DWORD protect) {
    if ((protect & PAGE_GUARD) != 0 || (protect & PAGE_NOACCESS) != 0)
        return false;

    switch (protect & 0xFFu) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

std::vector<int> parsePattern(std::string_view pattern) {
    std::vector<int> bytes;
    for (std::size_t index = 0; index < pattern.size();) {
        while (index < pattern.size() && pattern[index] == ' ')
            ++index;
        if (index >= pattern.size())
            break;

        if (pattern[index] == '?') {
            bytes.push_back(-1);
            ++index;
            if (index < pattern.size() && pattern[index] == '?')
                ++index;
            continue;
        }

        if (index + 1 >= pattern.size())
            break;

        bytes.push_back(std::stoi(std::string(pattern.substr(index, 2)), nullptr, 16));
        index += 2;
    }
    return bytes;
}

std::uintptr_t findPattern(const Process& proc,
                           std::uintptr_t moduleBase,
                           std::size_t scanSize,
                           const std::vector<int>& pattern)
{
    if (pattern.empty())
        return 0;

    const std::uintptr_t scanEnd = moduleBase + scanSize;
    std::vector<std::uint8_t> buffer(kScanChunkSize + pattern.size());
    MEMORY_BASIC_INFORMATION mbi{};
    std::uintptr_t cursor = moduleBase;

    while (cursor < scanEnd
        && VirtualQueryEx(proc.handle(), reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi)) == sizeof(mbi))
    {
        auto regionBase = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        auto regionEnd = regionBase + mbi.RegionSize;
        if (regionEnd <= cursor) {
            cursor += 0x1000;
            continue;
        }

        if (mbi.State == MEM_COMMIT && isReadableProtection(mbi.Protect)) {
            std::uintptr_t readBase = (std::max)(cursor, regionBase);
            std::uintptr_t readEnd = (std::min)(scanEnd, regionEnd);
            while (readBase < readEnd) {
                std::size_t bytesToRead = static_cast<std::size_t>(readEnd - readBase);
                bytesToRead = (std::min)(bytesToRead, kScanChunkSize + pattern.size() - 1);

                SIZE_T bytesRead = 0;
                if (ReadProcessMemory(proc.handle(),
                                      reinterpret_cast<LPCVOID>(readBase),
                                      buffer.data(),
                                      bytesToRead,
                                      &bytesRead)
                    && bytesRead >= pattern.size())
                {
                    const std::size_t lastStart = bytesRead - pattern.size();
                    for (std::size_t offset = 0; offset <= lastStart; ++offset) {
                        bool matched = true;
                        for (std::size_t patIndex = 0; patIndex < pattern.size(); ++patIndex) {
                            int expected = pattern[patIndex];
                            if (expected >= 0 && buffer[offset + patIndex] != static_cast<std::uint8_t>(expected)) {
                                matched = false;
                                break;
                            }
                        }

                        if (matched)
                            return readBase + offset;
                    }
                }

                if (bytesToRead <= pattern.size())
                    break;
                readBase += kScanChunkSize;
            }
        }

        cursor = regionEnd;
    }

    return 0;
}

std::uintptr_t resolveRipRelative(const Process& proc,
                                  std::uintptr_t instructionAddress,
                                  std::size_t displacementOffset = 3,
                                  std::size_t instructionLength = 7)
{
    std::int32_t displacement = 0;
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(proc.handle(),
                           reinterpret_cast<LPCVOID>(instructionAddress + displacementOffset),
                           &displacement,
                           sizeof(displacement),
                           &bytesRead)
        || bytesRead != sizeof(displacement))
    {
        return 0;
    }

    return instructionAddress + instructionLength + displacement;
}

bool resolveClientOffset(const Process& proc,
                         std::uintptr_t clientBase,
                         const char* label,
                         std::uintptr_t& slot,
                         std::string_view signature)
{
    const auto pattern = parsePattern(signature);
    const auto match = findPattern(proc, clientBase, kClientScanSize, pattern);
    if (!match) {
        std::cout << "[Offsets] Using fallback " << label << " = 0x"
                  << std::hex << slot << std::dec << '\n';
        return false;
    }

    const auto absolute = resolveRipRelative(proc, match);
    if (!absolute || absolute < clientBase) {
        std::cout << "[Offsets] Bad scan result for " << label << ", keeping fallback 0x"
                  << std::hex << slot << std::dec << '\n';
        return false;
    }

    slot = absolute - clientBase;
    std::cout << "[Offsets] Resolved " << label << " = 0x"
              << std::hex << slot << std::dec << '\n';
    return true;
}
} // namespace

bool offsets::resolveRuntime(const Process& proc, std::uintptr_t clientBase) {
    if (!proc.isAttached() || clientBase == 0)
        return false;

    bool resolvedAny = false;
    resolvedAny |= resolveClientOffset(proc,
                                       clientBase,
                                       "dwEntityList",
                                       client::dwEntityList,
                                       "48 8B 0D ?? ?? ?? ?? 48 89 7C 24 ?? 8B FA C1 EB");
    resolvedAny |= resolveClientOffset(proc,
                                       clientBase,
                                       "dwLocalPlayerController",
                                       client::dwLocalPlayerController,
                                       "48 8B 05 ?? ?? ?? ?? 41 89 BE");
    resolvedAny |= resolveClientOffset(proc,
                                       clientBase,
                                       "dwViewMatrix",
                                       client::dwViewMatrix,
                                       "48 8D 0D ?? ?? ?? ?? 48 C1 E0 06");
    resolvedAny |= resolveClientOffset(proc,
                                       clientBase,
                                       "dwGlobalVars",
                                       client::dwGlobalVars,
                                       "48 89 15 ?? ?? ?? ?? 48 89 42");
    resolvedAny |= resolveClientOffset(proc,
                                       clientBase,
                                       "dwPlantedC4",
                                       client::dwPlantedC4,
                                       "48 8B 15 ?? ?? ?? ?? 41 FF C0 48 8D 4C 24 ?? 44 89 05 ?? ?? ?? ??");
    resolvedAny |= resolveClientOffset(proc,
                                       clientBase,
                                       "dwWeaponC4",
                                       client::dwWeaponC4,
                                       "48 89 05 ?? ?? ?? ?? F7 C1 ?? ?? ?? ?? 74 ?? 81 E1 ?? ?? ?? ?? 89 0D ?? ?? ?? ?? 8B 05 ?? ?? ?? ?? 89 1D ?? ?? ?? ?? EB ?? 48 8B 15 ?? ?? ?? ?? 48 8B 5C 24 ?? FF C0 89 05 ?? ?? ?? ?? 48 8B C6 48 89 34 EA 80 BE");
    return resolvedAny;
}