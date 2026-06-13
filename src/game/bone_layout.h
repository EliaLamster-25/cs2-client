#pragma once

#include "math/bone_matrix.h"
#include "game/player.h"

#include <cmath>
#include <cstring>

/// CS2 skeleton cache indices (model-state bone array) mapped to normalized PlayerData slots.
namespace bone_layout {

namespace raw {
constexpr int pelvis = 1;
constexpr int spine = 3;
constexpr int chest = 4;
constexpr int neck = 6;
constexpr int head = 7;
constexpr int lShoulder = 9;
constexpr int lElbow = 10;
constexpr int lHand = 11;
constexpr int rShoulder = 13;
constexpr int rElbow = 14;
constexpr int rHand = 15;
constexpr int lHip = 17;
constexpr int lKnee = 18;
constexpr int lFoot = 19;
constexpr int rHip = 20;
constexpr int rKnee = 21;
constexpr int rFoot = 22;
} // namespace raw

namespace slot {
constexpr int pelvis = 0;
constexpr int spine = 2;
constexpr int chest = 4;
constexpr int neck = 5;
constexpr int head = 6;
constexpr int lShoulder = 8;
constexpr int lElbow = 9;
constexpr int lHand = 11;
constexpr int rShoulder = 13;
constexpr int rElbow = 14;
constexpr int rHand = 16;
constexpr int lHip = 22;
constexpr int lKnee = 23;
constexpr int lFoot = 24;
constexpr int rHip = 25;
constexpr int rKnee = 26;
constexpr int rFoot = 27;
} // namespace slot

inline void mapBoneIfValid(const Vec3* rawBones,
                           int rawBoneCount,
                           int rawIndex,
                           Vec3* outBones,
                           int outIndex)
{
    if (rawIndex < 0 || rawIndex >= rawBoneCount)
        return;
    const Vec3& bone = rawBones[rawIndex];
    if (!std::isfinite(bone.x) || !std::isfinite(bone.y) || !std::isfinite(bone.z))
        return;
    outBones[outIndex] = bone;
}

inline void mapBoneMatrixIfValid(const Mat3x4* rawMatrices,
                                 int rawBoneCount,
                                 int rawIndex,
                                 Mat3x4* outMatrices,
                                 bool* outOk,
                                 int outIndex)
{
    if (rawIndex < 0 || rawIndex >= rawBoneCount)
        return;
    const Mat3x4& mat = rawMatrices[rawIndex];
    if (!mat.isFinite())
        return;
    outMatrices[outIndex] = mat;
    outOk[outIndex] = true;
}

inline void normalizePositions(const Vec3* rawBones, int rawBoneCount, Vec3* outBones)
{
    std::memset(outBones, 0, sizeof(Vec3) * PlayerData::kBoneCount);

    mapBoneIfValid(rawBones, rawBoneCount, raw::pelvis, outBones, slot::pelvis);
    mapBoneIfValid(rawBones, rawBoneCount, raw::spine, outBones, slot::spine);
    mapBoneIfValid(rawBones, rawBoneCount, raw::chest, outBones, slot::chest);
    mapBoneIfValid(rawBones, rawBoneCount, raw::neck, outBones, slot::neck);
    mapBoneIfValid(rawBones, rawBoneCount, raw::head, outBones, slot::head);

    mapBoneIfValid(rawBones, rawBoneCount, raw::lShoulder, outBones, slot::lShoulder);
    mapBoneIfValid(rawBones, rawBoneCount, raw::lElbow, outBones, slot::lElbow);
    mapBoneIfValid(rawBones, rawBoneCount, raw::lHand, outBones, slot::lHand);

    mapBoneIfValid(rawBones, rawBoneCount, raw::rShoulder, outBones, slot::rShoulder);
    mapBoneIfValid(rawBones, rawBoneCount, raw::rElbow, outBones, slot::rElbow);
    mapBoneIfValid(rawBones, rawBoneCount, raw::rHand, outBones, slot::rHand);

    mapBoneIfValid(rawBones, rawBoneCount, raw::lHip, outBones, slot::lHip);
    mapBoneIfValid(rawBones, rawBoneCount, raw::lKnee, outBones, slot::lKnee);
    mapBoneIfValid(rawBones, rawBoneCount, raw::lFoot, outBones, slot::lFoot);

    mapBoneIfValid(rawBones, rawBoneCount, raw::rHip, outBones, slot::rHip);
    mapBoneIfValid(rawBones, rawBoneCount, raw::rKnee, outBones, slot::rKnee);
    mapBoneIfValid(rawBones, rawBoneCount, raw::rFoot, outBones, slot::rFoot);
}

inline void normalizeMatrices(const Mat3x4* rawMatrices,
                              int rawBoneCount,
                              Mat3x4* outMatrices,
                              bool* outOk)
{
    for (int i = 0; i < PlayerData::kBoneCount; ++i)
        outOk[i] = false;

    mapBoneMatrixIfValid(rawMatrices, rawBoneCount, raw::pelvis, outMatrices, outOk, slot::pelvis);
    mapBoneMatrixIfValid(rawMatrices, rawBoneCount, raw::spine, outMatrices, outOk, slot::spine);
    mapBoneMatrixIfValid(rawMatrices, rawBoneCount, raw::chest, outMatrices, outOk, slot::chest);
    mapBoneMatrixIfValid(rawMatrices, rawBoneCount, raw::neck, outMatrices, outOk, slot::neck);
    mapBoneMatrixIfValid(rawMatrices, rawBoneCount, raw::head, outMatrices, outOk, slot::head);

    mapBoneMatrixIfValid(rawMatrices, rawBoneCount, raw::lShoulder, outMatrices, outOk, slot::lShoulder);
    mapBoneMatrixIfValid(rawMatrices, rawBoneCount, raw::lElbow, outMatrices, outOk, slot::lElbow);
    mapBoneMatrixIfValid(rawMatrices, rawBoneCount, raw::lHand, outMatrices, outOk, slot::lHand);

    mapBoneMatrixIfValid(rawMatrices, rawBoneCount, raw::rShoulder, outMatrices, outOk, slot::rShoulder);
    mapBoneMatrixIfValid(rawMatrices, rawBoneCount, raw::rElbow, outMatrices, outOk, slot::rElbow);
    mapBoneMatrixIfValid(rawMatrices, rawBoneCount, raw::rHand, outMatrices, outOk, slot::rHand);

    mapBoneMatrixIfValid(rawMatrices, rawBoneCount, raw::lHip, outMatrices, outOk, slot::lHip);
    mapBoneMatrixIfValid(rawMatrices, rawBoneCount, raw::lKnee, outMatrices, outOk, slot::lKnee);
    mapBoneMatrixIfValid(rawMatrices, rawBoneCount, raw::lFoot, outMatrices, outOk, slot::lFoot);

    mapBoneMatrixIfValid(rawMatrices, rawBoneCount, raw::rHip, outMatrices, outOk, slot::rHip);
    mapBoneMatrixIfValid(rawMatrices, rawBoneCount, raw::rKnee, outMatrices, outOk, slot::rKnee);
    mapBoneMatrixIfValid(rawMatrices, rawBoneCount, raw::rFoot, outMatrices, outOk, slot::rFoot);
}

} // namespace bone_layout
