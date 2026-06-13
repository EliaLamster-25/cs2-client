#pragma once

#include "memory/process.h"

#include <cstdint>
#include <string>
#include <string_view>

/// Extracts agent mesh key (e.g. tm_balkan_variantf) from a CS2 model path string.
std::string modelKeyFromPath(std::string_view path);

/// Reads the best agent model key for a pawn (for per-player chams GLB lookup).
std::string readPlayerModelKey(const Process& proc, std::uintptr_t pawn);
