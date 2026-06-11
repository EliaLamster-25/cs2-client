#pragma once

#include "memory/process.h"
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

/// Pattern scan helpers shared by offset resolution and game-trace init.
namespace pattern {

std::vector<int> parse(std::string_view signature);

std::uintptr_t findInModule(const Process& proc,
                            std::uintptr_t moduleBase,
                            std::size_t scanSize,
                            std::string_view signature);

std::uintptr_t resolveRip(const Process& proc,
                          std::uintptr_t instructionAddress,
                          std::size_t displacementOffset = 3,
                          std::size_t instructionLength = 7);

struct VTableSlot {
    int index = -1;
    std::uintptr_t vtableAddress = 0;
};

/// Scan .rdata of a remote module for a vtable containing fnAddress.
VTableSlot findVTableForFunction(const Process& proc,
                                 std::uintptr_t moduleBase,
                                 std::size_t moduleSize,
                                 std::uintptr_t fnAddress);

} // namespace pattern
