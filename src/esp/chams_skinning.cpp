#include "esp/chams_skinning.h"

#include "cgltf.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <utility>

namespace chams_skinning {
namespace {

std::string toLower(std::string s) {
    for (char& c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

bool bonePositionValid(const Vec3& pos) {
    return std::isfinite(pos.x) && std::isfinite(pos.y) && std::isfinite(pos.z);
}

bool assignGameBoneMatrix(const Cs2Skeleton& bones, std::uint32_t gameBone, Cs2Mat3x4& out) {
    if (gameBone >= Cs2Skeleton::kMaxBones)
        return false;
    if (!bonePositionValid(bones.bones[gameBone].position))
        return false;
    out = bones.matrix((int)gameBone);
    return true;
}

void resolveUnmappedGameBones(const cgltf_skin& skin, std::vector<std::uint32_t>& mapped) {
    if (!skin.joints || mapped.empty())
        return;

    const auto jointCount = mapped.size();

    for (std::size_t ji = 0; ji < (jointCount < kCoreJointCount ? jointCount : kCoreJointCount); ++ji) {
        if (mapped[ji] < Cs2Skeleton::kMaxBones)
            continue;
        auto parent = coreFlatParent(ji);
        while (parent >= 0) {
            const auto parentIndex = (std::size_t)parent;
            if (parentIndex < mapped.size() && mapped[parentIndex] < Cs2Skeleton::kMaxBones) {
                mapped[ji] = mapped[parentIndex];
                break;
            }
            parent = coreFlatParent((std::size_t)parent);
        }
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t ji = kCoreJointCount; ji < jointCount; ++ji) {
            if (mapped[ji] < Cs2Skeleton::kMaxBones)
                continue;
            const auto parent = findParentJointIndex(skin, skin.joints[ji]);
            if (parent < 0)
                continue;
            const auto parentIndex = (std::size_t)parent;
            if (parentIndex < mapped.size() && mapped[parentIndex] < Cs2Skeleton::kMaxBones) {
                mapped[ji] = mapped[parentIndex];
                changed = true;
            }
        }
    }

    for (std::size_t ji = kCoreJointCount; ji < jointCount; ++ji) {
        if (mapped[ji] < Cs2Skeleton::kMaxBones)
            continue;
        const cgltf_node* jointNode = skin.joints[ji];
        if (jointNode && jointNode->name && jointNode->name[0]) {
            const auto named = mapBoneNameToGame(jointNode->name);
            if (named < Cs2Skeleton::kMaxBones) {
                mapped[ji] = named;
                continue;
            }
        }
        auto parent = findParentJointIndex(skin, jointNode);
        while (parent >= 0) {
            const auto parentIndex = (std::size_t)parent;
            if (parentIndex < mapped.size() && mapped[parentIndex] < Cs2Skeleton::kMaxBones) {
                mapped[ji] = mapped[parentIndex];
                break;
            }
            parent = findParentJointIndex(skin, skin.joints[parentIndex]);
        }
    }
}

std::vector<std::uint32_t> buildJointToGameBoneMap(const cgltf_skin& skin,
                                                   const cgltf_data* data,
                                                   std::vector<std::uint8_t>& directGameBone) {
    std::vector<std::uint32_t> mapped(skin.joints_count, Cs2Skeleton::kMaxBones);
    directGameBone.assign(skin.joints_count, 0);
    if (!skin.joints || !data)
        return mapped;

    for (cgltf_size ji = 0; ji < skin.joints_count; ++ji) {
        const cgltf_node* jointNode = skin.joints[ji];
        if (!jointNode)
            continue;

        if (jointNode->name && jointNode->name[0]) {
            const auto named = mapBoneNameToGame(jointNode->name);
            if (named < Cs2Skeleton::kMaxBones) {
                mapped[ji] = named;
                directGameBone[ji] = 1;
                continue;
            }
        }

        const auto core = mapCoreJointToGame((std::size_t)ji);
        if (core < Cs2Skeleton::kMaxBones && ji < (cgltf_size)kCoreJointCount) {
            mapped[ji] = core;
            directGameBone[ji] = 1;
        }
    }

    resolveUnmappedGameBones(skin, mapped);
    return mapped;
}

} // namespace

std::uint32_t mapBoneNameToGame(std::string_view name) {
    if (name.empty())
        return Cs2Skeleton::kMaxBones;

    const std::string lowered = toLower(std::string(name));

    static constexpr std::pair<const char*, std::uint32_t> kNamedBones[] = {
        { "pelvis", 0 }, { "spine_0", 1 }, { "spine_1", 2 }, { "spine_2", 3 },
        { "spine_3", 4 }, { "neck_0", 5 }, { "neck", 5 }, { "head_0", 6 }, { "head", 6 },
        { "clavicle_l", 8 }, { "arm_upper_l", 9 }, { "arm_lower_l", 10 }, { "hand_l", 11 },
        { "clavicle_r", 12 }, { "arm_upper_r", 13 }, { "arm_lower_r", 14 }, { "hand_r", 15 },
        { "leg_upper_l", 17 }, { "leg_lower_l", 18 }, { "ankle_l", 19 },
        { "leg_upper_r", 20 }, { "leg_lower_r", 21 }, { "ankle_r", 22 },
    };

    for (const auto& [boneName, boneIndex] : kNamedBones) {
        if (lowered == boneName)
            return boneIndex;
    }

    if (lowered.find("mask") != std::string::npos || lowered.find("facewear") != std::string::npos)
        return 7;

    const bool right = lowered.find("_r") != std::string::npos;

    if (lowered.find("hand") != std::string::npos)
        return right ? 15u : 11u;
    if (lowered.find("foot") != std::string::npos || lowered.find("toe") != std::string::npos)
        return right ? 22u : 19u;
    if (lowered.find("clavicle") != std::string::npos || lowered.find("shoulder") != std::string::npos)
        return right ? 12u : 8u;
    if (lowered.find("spine") != std::string::npos || lowered.find("chest") != std::string::npos
        || lowered.find("gear") != std::string::npos || lowered.find("vest") != std::string::npos)
        return 4;

    return Cs2Skeleton::kMaxBones;
}

std::uint32_t mapCoreJointToGame(std::size_t jointIndex) {
    static constexpr std::uint32_t kMap[kCoreJointCount] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 4,
        17, 18, 19, 20, 21, 22,
    };
    if (jointIndex >= kCoreJointCount)
        return Cs2Skeleton::kMaxBones;
    return kMap[jointIndex];
}

std::int32_t coreFlatParent(std::size_t jointIndex) {
    static constexpr std::int32_t kParents[kCoreJointCount] = {
        -1, 0, 1, 2, 3, 4, 5, 6, 4, 8, 9, 10, 4, 12, 13, 14, 4,
        0, 17, 18, 0, 20, 21,
    };
    if (jointIndex >= kCoreJointCount)
        return -1;
    return kParents[jointIndex];
}

std::int32_t findParentJointIndex(const cgltf_skin& skin, const cgltf_node* node) {
    if (!skin.joints || !node || !node->parent)
        return -1;
    for (cgltf_size ji = 0; ji < skin.joints_count; ++ji) {
        if (skin.joints[ji] == node->parent)
            return (std::int32_t)ji;
    }
    return findParentJointIndex(skin, node->parent);
}

void buildJointRelBind(ChamsSkinnedMesh& mesh) {
    const auto jointCount = mesh.inverseBind.size();
    if (jointCount == 0)
        return;

    mesh.relBind.assign(jointCount, Cs2Mat3x4::identity());
    std::vector<Cs2Mat3x4> bindGlobal(jointCount);
    for (std::size_t ji = 0; ji < jointCount; ++ji)
        bindGlobal[ji] = Cs2Mat3x4::inverseAffine(mesh.inverseBind[ji]);

    for (std::size_t ji = 0; ji < jointCount; ++ji) {
        const auto parent = (ji < mesh.jointParents.size()) ? mesh.jointParents[ji] : -1;
        if (parent < 0 || (std::size_t)parent >= jointCount)
            continue;
        mesh.relBind[ji] = Cs2Mat3x4::mul(
            Cs2Mat3x4::inverseAffine(bindGlobal[(std::size_t)parent]),
            bindGlobal[ji]);
    }
}

void buildJointPalette(const ChamsSkinnedMesh& mesh,
                       const Cs2Skeleton& bones,
                       std::vector<Cs2Mat3x4>& palette) {
    const auto jointCount = mesh.inverseBind.size();
    palette.assign(jointCount, Cs2Mat3x4::identity());
    std::vector<Cs2Mat3x4> animGlobal(jointCount);
    std::vector<bool> resolved(jointCount, false);

    for (std::size_t ji = 0; ji < jointCount; ++ji) {
        const auto gameBone = (ji < mesh.gameBones.size()) ? mesh.gameBones[ji] : Cs2Skeleton::kMaxBones;
        const bool direct = (ji < mesh.directGameBone.size()) && mesh.directGameBone[ji];
        if (direct && assignGameBoneMatrix(bones, gameBone, animGlobal[ji]))
            resolved[ji] = true;
    }

    for (std::size_t ji = 0; ji < jointCount && ji < kCoreJointCount; ++ji) {
        if (resolved[ji])
            continue;
        const auto gameBone = (ji < mesh.gameBones.size()) ? mesh.gameBones[ji] : Cs2Skeleton::kMaxBones;
        if (assignGameBoneMatrix(bones, gameBone, animGlobal[ji]))
            resolved[ji] = true;
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t ji = 0; ji < jointCount; ++ji) {
            if (resolved[ji])
                continue;
            const auto parent = (ji < mesh.jointParents.size()) ? mesh.jointParents[ji] : -1;
            if (parent < 0 || (std::size_t)parent >= jointCount || !resolved[(std::size_t)parent])
                continue;
            const Cs2Mat3x4& rel = (ji < mesh.relBind.size()) ? mesh.relBind[ji] : Cs2Mat3x4::identity();
            animGlobal[ji] = Cs2Mat3x4::mul(animGlobal[(std::size_t)parent], rel);
            resolved[ji] = true;
            changed = true;
        }
    }

    const Cs2Mat3x4 rootMatrix = bones.matrix(0);
    for (std::size_t ji = 0; ji < jointCount; ++ji) {
        if (!resolved[ji]) {
            auto parent = (ji < mesh.jointParents.size()) ? mesh.jointParents[ji] : -1;
            while (parent >= 0 && (std::size_t)parent < jointCount && !resolved[(std::size_t)parent])
                parent = (parent < (int)mesh.jointParents.size()) ? mesh.jointParents[(std::size_t)parent] : -1;

            if (parent >= 0 && (std::size_t)parent < jointCount && resolved[(std::size_t)parent]) {
                const Cs2Mat3x4& rel = (ji < mesh.relBind.size()) ? mesh.relBind[ji] : Cs2Mat3x4::identity();
                animGlobal[ji] = Cs2Mat3x4::mul(animGlobal[(std::size_t)parent], rel);
            } else {
                animGlobal[ji] = rootMatrix;
            }
        }
        palette[ji] = Cs2Mat3x4::mul(animGlobal[ji], mesh.inverseBind[ji]);
    }
}

Vec3 skinVertex(const ChamsSkinnedMesh& mesh,
                const std::vector<Cs2Mat3x4>& palette,
                std::size_t vertexIndex) {
    if (vertexIndex >= mesh.bindPositions.size()
        || vertexIndex >= mesh.joints.size()
        || vertexIndex >= mesh.weights.size())
        return {};

    const Vec3& local = mesh.bindPositions[vertexIndex];
    const auto& jp = mesh.joints[vertexIndex];
    const auto& w = mesh.weights[vertexIndex];

    Vec3 world{};
    float total = 0.f;
    for (int k = 0; k < 4; ++k) {
        if (w[(size_t)k] <= kMinWeight)
            continue;
        const auto joint = (std::size_t)jp[(size_t)k];
        if (joint >= palette.size())
            continue;
        world = world + palette[joint].transformPoint(local) * w[(size_t)k];
        total += w[(size_t)k];
    }

    if (total > kMinWeight) {
        const float inv = 1.f / total;
        world.x *= inv;
        world.y *= inv;
        world.z *= inv;
    } else if (!palette.empty()) {
        world = palette[0].transformPoint(local);
    }
    return world;
}

bool loadSkinData(const cgltf_skin& skin, const cgltf_data* data, ChamsSkinnedMesh& mesh) {
    if (!skin.joints_count || !skin.joints || !data)
        return false;

    mesh.gameBones = buildJointToGameBoneMap(skin, data, mesh.directGameBone);
    mesh.jointParents.assign(skin.joints_count, -1);
    mesh.inverseBind.assign(skin.joints_count, Cs2Mat3x4::identity());

    for (cgltf_size ji = 0; ji < skin.joints_count; ++ji) {
        const cgltf_node* jointNode = skin.joints[ji];
        if (jointNode)
            mesh.jointParents[ji] = findParentJointIndex(skin, jointNode);

        float matrix[16]{};
        if (skin.inverse_bind_matrices)
            cgltf_accessor_read_float(skin.inverse_bind_matrices, ji, matrix, 16);
        else
            matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.f;

        mesh.inverseBind[ji] = Cs2Mat3x4::fromGltfColMajor(matrix);
    }

    buildJointRelBind(mesh);
    return true;
}

void refineJointMappingFromBindGeometry(ChamsSkinnedMesh& mesh) {
    if (mesh.gameBones.size() <= kCoreJointCount || mesh.bindPositions.empty())
        return;

    const auto jointCount = mesh.gameBones.size();
    std::vector<Vec3> accum(jointCount, Vec3{});
    std::vector<float> weightSum(jointCount, 0.f);

    for (std::size_t vi = 0; vi < mesh.bindPositions.size(); ++vi) {
        if (vi >= mesh.joints.size() || vi >= mesh.weights.size())
            continue;
        const Vec3& pos = mesh.bindPositions[vi];
        for (int k = 0; k < 4; ++k) {
            if (mesh.weights[vi][(size_t)k] <= kMinWeight)
                continue;
            const auto ji = (std::size_t)mesh.joints[vi][(size_t)k];
            if (ji >= jointCount)
                continue;
            accum[ji] = accum[ji] + pos * mesh.weights[vi][(size_t)k];
            weightSum[ji] += mesh.weights[vi][(size_t)k];
        }
    }

    for (std::size_t ji = kCoreJointCount; ji < jointCount; ++ji) {
        if (weightSum[ji] < 1.f)
            continue;
        const Vec3 center = accum[ji] * (1.f / weightSum[ji]);
        const auto current = mesh.gameBones[ji];
        if (current < 17 || current > 22)
            continue;
        if (center.z <= 26.f || std::fabs(center.x) >= 9.f || std::fabs(center.y) >= 14.f)
            continue;
        mesh.gameBones[ji] = (center.z > 40.f) ? 5u : (center.z > 28.f) ? 4u : (center.z > 22.f) ? 3u : 2u;
    }
}

bool shouldLoadMeshForChams(std::string_view meshName) {
    if (meshName.empty())
        return true;
    const std::string lowered = toLower(std::string(meshName));
    return lowered.find("firstperson_") == std::string::npos;
}

} // namespace chams_skinning
