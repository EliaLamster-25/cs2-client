#include "offsets.h"
#include "str_obf.h"

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

    const char kObfKey = '\xAB';

    auto s1 = OBF("\x9F\x93\x8B\x93\xE9\x8B\x9B\xEF\x8B\x94\x94\x8B\x94\x94\x8B\x94\x94\x8B\x94\x94\x8B\x9F\x93\x8B\x93\x92\x8B\x9C\xE8\x8B\x99\x9F\x8B\x94\x94\x8B\x93\xE9\x8B\xED\xEA\x8B\xE8\x9A\x8B\xEE\xE9", kObfKey);
    resolvedAny |= resolveClientOffset(proc, clientBase, "dwEntityList", client::dwEntityList, s1);

    auto s2 = OBF("\x9F\x93\x8B\x93\xE9\x8B\x9B\x9E\x8B\x94\x94\x8B\x94\x94\x8B\x94\x94\x8B\x94\x94\x8B\x9F\x9A\x8B\x93\x92\x8B\xE9\xEE", kObfKey);
    resolvedAny |= resolveClientOffset(proc, clientBase, "dwLocalPlayerController", client::dwLocalPlayerController, s2);

    auto s3 = OBF("\x9F\x93\x8B\x93\xEF\x8B\x9B\xEF\x8B\x94\x94\x8B\x94\x94\x8B\x94\x94\x8B\x94\x94\x8B\x9F\x93\x8B\xE8\x9A\x8B\xEE\x9B\x8B\x9B\x9D", kObfKey);
    resolvedAny |= resolveClientOffset(proc, clientBase, "dwViewMatrix", client::dwViewMatrix, s3);

    auto s4 = OBF("\x9F\x93\x8B\x93\x92\x8B\x9A\x9E\x8B\x94\x94\x8B\x94\x94\x8B\x94\x94\x8B\x94\x94\x8B\x9F\x93\x8B\x93\x92\x8B\x9F\x99", kObfKey);
    resolvedAny |= resolveClientOffset(proc, clientBase, "dwGlobalVars", client::dwGlobalVars, s4);

    auto s5 = OBF("\x9F\x93\x8B\x93\xE9\x8B\x9A\x9E\x8B\x94\x94\x8B\x94\x94\x8B\x94\x94\x8B\x94\x94\x8B\x9F\x9A\x8B\xED\xED\x8B\xE8\x9B\x8B\x9F\x93\x8B\x93\xEF\x8B\x9F\xE8\x8B\x99\x9F\x8B\x94\x94\x8B\x9F\x9F\x8B\x93\x92\x8B\x9B\x9E\x8B\x94\x94\x8B\x94\x94\x8B\x94\x94\x8B\x94\x94", kObfKey);
    resolvedAny |= resolveClientOffset(proc, clientBase, "dwPlantedC4", client::dwPlantedC4, s5);

    auto s6 = OBF("\x9F\x93\x8B\x93\x92\x8B\x9B\x9E\x8B\x94\x94\x8B\x94\x94\x8B\x94\x94\x8B\x94\x94\x8B\xED\x9C\x8B\xE8\x9A\x8B\x94\x94\x8B\x94\x94\x8B\x94\x94\x8B\x94\x94\x8B\x9C\x9F\x8B\x94\x94\x8B\x93\x9A\x8B\xEE\x9A\x8B\x94\x94\x8B\x94\x94\x8B\x94\x94\x8B\x94\x94\x8B\x93\x92\x8B\x9B\xEF\x8B\x94\x94\x8B\x94\x94\x8B\x94\x94\x8B\x94\x94\x8B\x93\xE9\x8B\x9B\x9E\x8B\x94\x94\x8B\x94\x94\x8B\x94\x94\x8B\x94\x94\x8B\x93\x92\x8B\x9A\xEF\x8B\x94\x94\x8B\x94\x94\x8B\x94\x94\x8B\x94\x94\x8B\xEE\xE9\x8B\x94\x94\x8B\x9F\x93\x8B\x93\xE9\x8B\x9A\x9E\x8B\x94\x94\x8B\x94\x94\x8B\x94\x94\x8B\x94\x94\x8B\x9F\x93\x8B\x93\xE9\x8B\x9E\xE8\x8B\x99\x9F\x8B\x94\x94\x8B\xED\xED\x8B\xE8\x9B\x8B\x93\x92\x8B\x9B\x9E\x8B\x94\x94\x8B\x94\x94\x8B\x94\x94\x8B\x94\x94\x8B\x9F\x93\x8B\x93\xE9\x8B\xE8\x9D\x8B\x9F\x93\x8B\x93\x92\x8B\x98\x9F\x8B\xEE\xEA\x8B\x93\x9B\x8B\xE9\xEE", kObfKey);
    resolvedAny |= resolveClientOffset(proc, clientBase, "dwWeaponC4", client::dwWeaponC4, s6);

    return resolvedAny;
}