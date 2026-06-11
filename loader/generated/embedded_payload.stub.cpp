// Stub — replaced when crymore_loader builds after cs2_overlay.
#pragma once
#include <cstddef>
namespace loader_embed {
alignas(8) static const unsigned char kPayloadArchive[] = {
    0x43,0x52,0x4D,0x47,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};
static const std::size_t kPayloadArchive_size = sizeof(kPayloadArchive);
alignas(8) static const unsigned char kBrandLogoPng[] = { 0 };
static const std::size_t kBrandLogoPng_size = 0;
}
