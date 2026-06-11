#include "memory/pattern_scan.h"

#include "memory/rpm.h"

#include <Windows.h>
#include <algorithm>
#include <string>
namespace {
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
} // namespace

namespace pattern {

std::vector<int> parse(std::string_view signature) {
    std::vector<int> bytes;
    for (std::size_t index = 0; index < signature.size();) {
        while (index < signature.size() && signature[index] == ' ')
            ++index;
        if (index >= signature.size())
            break;

        if (signature[index] == '?') {
            bytes.push_back(-1);
            ++index;
            if (index < signature.size() && signature[index] == '?')
                ++index;
            continue;
        }

        if (index + 1 >= signature.size())
            break;

        bytes.push_back(std::stoi(std::string(signature.substr(index, 2)), nullptr, 16));
        index += 2;
    }
    return bytes;
}

std::uintptr_t findInModule(const Process& proc,
                            std::uintptr_t moduleBase,
                            std::size_t scanSize,
                            std::string_view signature)
{
    const auto pattern = parse(signature);
    if (pattern.empty() || !moduleBase)
        return 0;

    const std::uintptr_t scanEnd = moduleBase + scanSize;
    std::vector<std::uint8_t> buffer(kScanChunkSize + pattern.size());

    if (proc.usesKernelMemory()) {
        std::uintptr_t readBase = moduleBase;
        while (readBase < scanEnd) {
            const std::size_t bytesToRead =
                (std::min)(static_cast<std::size_t>(scanEnd - readBase),
                           kScanChunkSize + pattern.size() - 1);
            if (!mem::readRaw(proc, readBase, buffer.data(), bytesToRead))
                break;
            if (bytesToRead < pattern.size())
                break;

            const std::size_t lastStart = bytesToRead - pattern.size();
            for (std::size_t offset = 0; offset <= lastStart; ++offset) {
                bool matched = true;
                for (std::size_t patIndex = 0; patIndex < pattern.size(); ++patIndex) {
                    const int expected = pattern[patIndex];
                    if (expected >= 0
                        && buffer[offset + patIndex] != static_cast<std::uint8_t>(expected)) {
                        matched = false;
                        break;
                    }
                }
                if (matched)
                    return readBase + offset;
            }

            if (bytesToRead <= pattern.size())
                break;
            readBase += kScanChunkSize;
        }
        return 0;
    }

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

                if (mem::readRaw(proc, readBase, buffer.data(), bytesToRead)
                    && bytesToRead >= pattern.size())
                {
                    const std::size_t lastStart = bytesToRead - pattern.size();
                    for (std::size_t offset = 0; offset <= lastStart; ++offset) {
                        bool matched = true;
                        for (std::size_t patIndex = 0; patIndex < pattern.size(); ++patIndex) {
                            int expected = pattern[patIndex];
                            if (expected >= 0
                                && buffer[offset + patIndex] != static_cast<std::uint8_t>(expected))
                            {
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

std::uintptr_t resolveRip(const Process& proc,
                          std::uintptr_t instructionAddress,
                          std::size_t displacementOffset,
                          std::size_t instructionLength)
{
    std::int32_t displacement = 0;
    if (!mem::readRaw(proc, instructionAddress + displacementOffset,
                      &displacement, sizeof(displacement)))
    {
        return 0;
    }

    return instructionAddress + instructionLength + displacement;
}

VTableSlot findVTableForFunction(const Process& proc,
                                 std::uintptr_t moduleBase,
                                 std::size_t moduleSize,
                                 std::uintptr_t fnAddress)
{
    VTableSlot out{};
    if (!moduleBase || !fnAddress)
        return out;

    std::vector<std::uint8_t> image((std::min)(moduleSize, std::size_t{0x02000000}));
    if (!mem::readRaw(proc, moduleBase, image.data(), image.size())
        || image.size() < 0x400)
    {
        return out;
    }

    // Parse PE headers to locate .rdata.
    if (image.size() < sizeof(IMAGE_DOS_HEADER))
        return out;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return out;
    if (static_cast<std::size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > image.size())
        return out;

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return out;

    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        char name[9]{};
        std::memcpy(name, section[i].Name, 8);
        if (std::string_view(name) != ".rdata")
            continue;

        const std::size_t secOff = section[i].PointerToRawData;
        const std::size_t secSize = section[i].SizeOfRawData;
        if (secOff + secSize > image.size())
            continue;

        for (std::size_t off = 0; off + 8 <= secSize; off += 8) {
            std::uint64_t value = 0;
            std::memcpy(&value, image.data() + secOff + off, 8);
            if (value != fnAddress)
                continue;

            std::size_t backScan = off;
            int functionIndex = 0;
            std::uintptr_t vtableStart = 0;
            while (backScan >= 8) {
                backScan -= 8;
                std::uint64_t candidate = 0;
                std::memcpy(&candidate, image.data() + secOff + backScan, 8);
                if (candidate > moduleBase && candidate < moduleBase + moduleSize)
                    ++functionIndex;
                else {
                    vtableStart = moduleBase + section[i].VirtualAddress + backScan + 8;
                    break;
                }
            }

            if (vtableStart == 0)
                vtableStart = moduleBase + section[i].VirtualAddress + off - functionIndex * 8;

            if (vtableStart > 0 && functionIndex >= 0) {
                out.index = functionIndex;
                out.vtableAddress = vtableStart;
                return out;
            }
        }
    }

    return out;
}

} // namespace pattern
