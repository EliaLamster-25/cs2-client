#pragma once

#include "game/cs2_bones.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct cgltf_skin;
struct cgltf_node;
struct cgltf_data;

/// Axum-style GLB mesh payload for CPU skinned chams.
struct ChamsSkinnedMesh {
    std::vector<Vec3> bindPositions;
    std::vector<std::array<std::uint16_t, 4>> joints;
    std::vector<std::array<float, 4>> weights;
    std::vector<std::uint32_t> indices;

    std::vector<std::uint32_t> gameBones;
    std::vector<std::int32_t> jointParents;
    std::vector<Cs2Mat3x4> inverseBind;
    std::vector<Cs2Mat3x4> relBind;
    std::vector<std::uint8_t> directGameBone;

    bool loaded = false;
};

namespace chams_skinning {

constexpr float kMinWeight = 1e-6f;
constexpr std::size_t kCoreJointCount = 23;

std::uint32_t mapBoneNameToGame(std::string_view name);
std::uint32_t mapCoreJointToGame(std::size_t jointIndex);
std::int32_t coreFlatParent(std::size_t jointIndex);
std::int32_t findParentJointIndex(const cgltf_skin& skin, const cgltf_node* node);

void buildJointRelBind(ChamsSkinnedMesh& mesh);
void buildJointPalette(const ChamsSkinnedMesh& mesh,
                       const Cs2Skeleton& bones,
                       std::vector<Cs2Mat3x4>& palette);

Vec3 skinVertex(const ChamsSkinnedMesh& mesh,
                const std::vector<Cs2Mat3x4>& palette,
                std::size_t vertexIndex);

bool loadSkinData(const cgltf_skin& skin, const cgltf_data* data, ChamsSkinnedMesh& mesh);
void refineJointMappingFromBindGeometry(ChamsSkinnedMesh& mesh);

bool shouldLoadMeshForChams(std::string_view meshName);

} // namespace chams_skinning
