#include "game/cs2_bones.h"
#include "game/player.h"

#include "memory/rpm.h"
#include "offsets/netvars.h"

#include <cmath>
#include <cstring>

namespace {

bool isLikelyPtr(std::uintptr_t p) {
    return p >= 0x10000ULL && p <= 0x00007FFFFFFEFFFFULL;
}

bool isFiniteVec3(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

Vec3 forwardFromPitchYaw(float pitchDeg, float yawDeg) {
    constexpr float kDeg2Rad = 3.14159265f / 180.f;
    const float pitch = pitchDeg * kDeg2Rad;
    const float yaw = yawDeg * kDeg2Rad;
    const float cp = std::cosf(pitch);
    return {
        cp * std::cosf(yaw),
        cp * std::sinf(yaw),
        -std::sinf(pitch),
    };
}

Mat3x4 mat3x4FromCs2(const Cs2Mat3x4& a) {
    Mat3x4 out{};
    out.m[0] = a.m[0][0];
    out.m[1] = a.m[0][1];
    out.m[2] = a.m[0][2];
    out.m[3] = a.m[0][3];
    out.m[4] = a.m[1][0];
    out.m[5] = a.m[1][1];
    out.m[6] = a.m[1][2];
    out.m[7] = a.m[1][3];
    out.m[8] = a.m[2][0];
    out.m[9] = a.m[2][1];
    out.m[10] = a.m[2][2];
    out.m[11] = a.m[2][3];
    return out;
}

struct SlotMap {
    int game;
    int slot;
};

constexpr SlotMap kSlotMaps[] = {
    { 0, 0 },   // pelvis
    { 1, 2 },   // spine_0
    { 2, 2 },   // spine_1 / stomach
    { 4, 4 },   // spine_3 / chest
    { 5, 5 },   // neck
    { 6, 6 },   // head
    { 8, 8 },   // clavicle_l
    { 9, 8 },   // arm_upper_l -> shoulder slot
    { 10, 9 },  // arm_lower_l -> elbow
    { 11, 11 }, // hand_l
    { 12, 13 }, // clavicle_r
    { 13, 13 }, // arm_upper_r
    { 14, 14 }, // arm_lower_r
    { 15, 16 }, // hand_r
    { 17, 22 }, // leg_upper_l
    { 18, 23 }, // leg_lower_l
    { 19, 24 }, // ankle_l
    { 20, 25 }, // leg_upper_r
    { 21, 26 }, // leg_lower_r
    { 22, 27 }, // ankle_r
};

} // namespace

Cs2Mat3x4 Cs2Mat3x4::identity() {
    Cs2Mat3x4 out{};
    out.m[0][0] = out.m[1][1] = out.m[2][2] = 1.f;
    return out;
}

Cs2Mat3x4 Cs2Mat3x4::fromPositionRotation(const Vec3& pos, const Quat& rot) {
    const float qx = rot.x;
    const float qy = rot.y;
    const float qz = rot.z;
    const float qw = rot.w;

    const float xx = qx * qx;
    const float yy = qy * qy;
    const float zz = qz * qz;
    const float xy = qx * qy;
    const float xz = qx * qz;
    const float yz = qy * qz;
    const float wx = qw * qx;
    const float wy = qw * qy;
    const float wz = qw * qz;

    Cs2Mat3x4 out{};
    out.m[0][0] = 1.f - 2.f * (yy + zz);
    out.m[1][0] = 2.f * (xy + wz);
    out.m[2][0] = 2.f * (xz - wy);
    out.m[0][1] = 2.f * (xy - wz);
    out.m[1][1] = 1.f - 2.f * (xx + zz);
    out.m[2][1] = 2.f * (yz + wx);
    out.m[0][2] = 2.f * (xz + wy);
    out.m[1][2] = 2.f * (yz - wx);
    out.m[2][2] = 1.f - 2.f * (xx + yy);
    out.m[0][3] = pos.x;
    out.m[1][3] = pos.y;
    out.m[2][3] = pos.z;
    return out;
}

Cs2Mat3x4 Cs2Mat3x4::fromGltfColMajor(const float* colMajor16) {
    Cs2Mat3x4 out{};
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 3; ++row)
            out.m[row][col] = colMajor16[row + col * 4];
    }
    return out;
}

Cs2Mat3x4 Cs2Mat3x4::mul(const Cs2Mat3x4& a, const Cs2Mat3x4& b) {
    Cs2Mat3x4 out{};
    for (int i = 0; i < 3; ++i) {
        out.m[i][0] = a.m[i][0] * b.m[0][0] + a.m[i][1] * b.m[1][0] + a.m[i][2] * b.m[2][0];
        out.m[i][1] = a.m[i][0] * b.m[0][1] + a.m[i][1] * b.m[1][1] + a.m[i][2] * b.m[2][1];
        out.m[i][2] = a.m[i][0] * b.m[0][2] + a.m[i][1] * b.m[1][2] + a.m[i][2] * b.m[2][2];
        out.m[i][3] = a.m[i][0] * b.m[0][3] + a.m[i][1] * b.m[1][3] + a.m[i][2] * b.m[2][3] + a.m[i][3];
    }
    return out;
}

Cs2Mat3x4 Cs2Mat3x4::inverseAffine(const Cs2Mat3x4& m) {
    Cs2Mat3x4 inv{};
    inv.m[0][0] = m.m[0][0];
    inv.m[0][1] = m.m[1][0];
    inv.m[0][2] = m.m[2][0];
    inv.m[1][0] = m.m[0][1];
    inv.m[1][1] = m.m[1][1];
    inv.m[1][2] = m.m[2][1];
    inv.m[2][0] = m.m[0][2];
    inv.m[2][1] = m.m[1][2];
    inv.m[2][2] = m.m[2][2];

    const Vec3 negT{ -m.m[0][3], -m.m[1][3], -m.m[2][3] };
    const Vec3 invT = inv.transformPoint(negT);
    inv.m[0][3] = invT.x;
    inv.m[1][3] = invT.y;
    inv.m[2][3] = invT.z;
    return inv;
}

Vec3 Cs2Mat3x4::transformPoint(const Vec3& p) const {
    return {
        p.x * m[0][0] + p.y * m[0][1] + p.z * m[0][2] + m[0][3],
        p.x * m[1][0] + p.y * m[1][1] + p.z * m[1][2] + m[1][3],
        p.x * m[2][0] + p.y * m[2][1] + p.z * m[2][2] + m[2][3],
    };
}

bool Cs2Mat3x4::isFinite() const {
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            if (!std::isfinite(m[r][c]))
                return false;
        }
    }
    return true;
}

bool Cs2Skeleton::isValid() const {
    return valid;
}

Vec3 Cs2Skeleton::position(int gameBone) const {
    if (gameBone < 0 || gameBone >= kMaxBones)
        return {};
    return bones[gameBone].position;
}

Cs2Mat3x4 Cs2Skeleton::matrix(int gameBone) const {
    if (gameBone < 0 || gameBone >= kMaxBones)
        return Cs2Mat3x4::identity();
    const auto& b = bones[gameBone];
    return Cs2Mat3x4::fromPositionRotation(b.position, b.rotation);
}

int scoreSkeletonNearOrigin(const Cs2Skeleton& skel, const Vec3& origin) {
    static constexpr int kKeyBones[] = { 0, 4, 5, 6, 8, 13, 17, 20 };
    int score = 0;
    for (int gameBone : kKeyBones) {
        if (gameBone < 0 || gameBone >= Cs2Skeleton::kMaxBones)
            continue;
        const Vec3& pos = skel.bones[gameBone].position;
        if (!isFiniteVec3(pos))
            continue;
        if (origin.lengthSq() > 1.f) {
            const Vec3 d = pos - origin;
            if (d.lengthSq() < 1.f || d.lengthSq() > 220.f * 220.f)
                continue;
        } else if (pos.lengthSq() < 1.f) {
            continue;
        }
        ++score;
    }
    return score;
}

Quat quatFromMatrix3x4(const Mat3x4& m) {
    const float tr = m.m[0] + m.m[5] + m.m[10];
    Quat q{};
    if (tr > 0.f) {
        const float s = std::sqrtf(tr + 1.f) * 2.f;
        q.w = 0.25f * s;
        q.x = (m.m[6] - m.m[9]) / s;
        q.y = (m.m[8] - m.m[2]) / s;
        q.z = (m.m[1] - m.m[4]) / s;
    } else if (m.m[0] > m.m[5] && m.m[0] > m.m[10]) {
        const float s = std::sqrtf(1.f + m.m[0] - m.m[5] - m.m[10]) * 2.f;
        q.w = (m.m[6] - m.m[9]) / s;
        q.x = 0.25f * s;
        q.y = (m.m[1] + m.m[4]) / s;
        q.z = (m.m[8] + m.m[2]) / s;
    } else if (m.m[5] > m.m[10]) {
        const float s = std::sqrtf(1.f + m.m[5] - m.m[0] - m.m[10]) * 2.f;
        q.w = (m.m[8] - m.m[2]) / s;
        q.x = (m.m[1] + m.m[4]) / s;
        q.y = 0.25f * s;
        q.z = (m.m[6] + m.m[9]) / s;
    } else {
        const float s = std::sqrtf(1.f + m.m[10] - m.m[0] - m.m[5]) * 2.f;
        q.w = (m.m[1] - m.m[4]) / s;
        q.x = (m.m[8] + m.m[2]) / s;
        q.y = (m.m[6] + m.m[9]) / s;
        q.z = 0.25f * s;
    }
    return q;
}

bool readSkeletonPosQuat(const Process& proc, std::uintptr_t boneBase, Cs2Skeleton& out) {
    out = {};
    if (!isLikelyPtr(boneBase))
        return false;

    Cs2BoneCacheEntry raw[Cs2Skeleton::kMaxBones]{};
    if (!mem::readRaw(proc, boneBase, raw, sizeof(raw)))
        return false;

    for (int i = 0; i < Cs2Skeleton::kMaxBones; ++i)
        out.bones[i] = raw[i];
    return true;
}

bool readSkeletonMatrix(const Process& proc, std::uintptr_t boneBase, Cs2Skeleton& out) {
    out = {};
    if (!isLikelyPtr(boneBase))
        return false;

    constexpr int kBoneCount = 128;
    constexpr std::uintptr_t kStride = netvars::skeleton::kBoneStride;
    int validCount = 0;

    for (int i = 0; i < kBoneCount; ++i) {
        float boneMat[12]{};
        if (!mem::readArray(proc, boneBase + static_cast<std::uintptr_t>(i) * kStride, boneMat, 12))
            break;
        const Mat3x4 mat = Mat3x4::fromRowMajor12(boneMat);
        if (!mat.isFinite())
            continue;
        const Vec3 pos = mat.translation();
        if (!isFiniteVec3(pos))
            continue;
        out.bones[i].position = pos;
        out.bones[i].scale = 1.f;
        out.bones[i].rotation = quatFromMatrix3x4(mat);
        ++validCount;
    }
    return validCount >= 6;
}

bool readCs2Skeleton(const Process& proc, std::uintptr_t boneCachePtr, const Vec3& origin, Cs2Skeleton& out) {
    out = {};
    if (!isLikelyPtr(boneCachePtr))
        return false;

    Cs2Skeleton posQuat{};
    Cs2Skeleton matrix{};
    const bool hasPosQuat = readSkeletonPosQuat(proc, boneCachePtr, posQuat);
    const bool hasMatrix = readSkeletonMatrix(proc, boneCachePtr, matrix);

    const int posScore = hasPosQuat ? scoreSkeletonNearOrigin(posQuat, origin) : 0;
    const int matScore = hasMatrix ? scoreSkeletonNearOrigin(matrix, origin) : 0;

    if (matScore >= posScore && matScore >= 4) {
        out = matrix;
    } else if (posScore >= 4) {
        out = posQuat;
    } else if (matScore >= 3) {
        out = matrix;
    } else if (posScore >= 3) {
        out = posQuat;
    } else {
        return false;
    }

    out.valid = true;
    return true;
}

bool readPlayerSkeleton(const Process& proc, std::uintptr_t pawn, Cs2Skeleton& out) {
    out = {};
    if (!isLikelyPtr(pawn))
        return false;

    const auto sceneNode = mem::read<std::uintptr_t>(
        proc, pawn + netvars::pawn::m_pGameSceneNode);
    if (!isLikelyPtr(sceneNode))
        return false;

    const Vec3 origin = mem::read<Vec3>(proc, sceneNode + netvars::scene_node::m_vecAbsOrigin);

    auto bonePtr = mem::read<std::uintptr_t>(
        proc, sceneNode + netvars::skeleton::m_boneArrayPtr);
    if (!isLikelyPtr(bonePtr))
        bonePtr = mem::read<std::uintptr_t>(proc, sceneNode + 0x1E0);
    if (!isLikelyPtr(bonePtr))
        bonePtr = mem::read<std::uintptr_t>(proc, sceneNode + 0x1C8);

    if (isLikelyPtr(bonePtr) && readCs2Skeleton(proc, bonePtr, origin, out))
        return true;

    // Some builds store the bone array inline on the scene node.
    return readCs2Skeleton(proc, sceneNode + netvars::skeleton::m_boneArrayPtr, origin, out);
}

void applySkeletonToPlayer(const Cs2Skeleton& skel, PlayerData& player) {
    player.skeleton = skel;
    player.bonesValid = false;
    player.boneMatricesValid = skel.isValid();
    player.boneMatrixOk.fill(false);
    std::memset(player.bones, 0, sizeof(player.bones));
    std::memset(player.boneMatrices, 0, sizeof(player.boneMatrices));

    if (!skel.isValid())
        return;

    for (const SlotMap& map : kSlotMaps) {
        if (map.game < 0 || map.game >= Cs2Skeleton::kMaxBones)
            continue;
        if (map.slot < 0 || map.slot >= PlayerData::kBoneCount)
            continue;

        const Vec3& pos = skel.bones[map.game].position;
        if (!isFiniteVec3(pos))
            continue;

        Vec3 d = pos - player.origin;
        if (player.origin.lengthSq() > 1.f && d.lengthSq() > 220.f * 220.f)
            continue;

        player.bones[map.slot] = pos;
        player.boneMatrices[map.slot] = mat3x4FromCs2(skel.matrix(map.game));
        player.boneMatrixOk[map.slot] = true;
    }

    if (isFiniteVec3(player.bones[6]))
        player.headPos = player.bones[6];

    player.bonesValid = isFiniteVec3(player.bones[6]) && isFiniteVec3(player.bones[0]);
    if (!player.bonesValid)
        return;

    const Cs2Mat3x4& headMat = skel.matrix(6);
    Vec3 boneAxis{ headMat.m[0][2], headMat.m[1][2], headMat.m[2][2] };
    const float axisLen = boneAxis.length();
    if (axisLen > 0.001f) {
        boneAxis = boneAxis * (1.f / axisLen);
        Vec3 eyeFwd = forwardFromPitchYaw(player.eyePitch, player.eyeYaw);
        const float eyeLen = eyeFwd.length();
        if (eyeLen > 0.001f) {
            eyeFwd = eyeFwd * (1.f / eyeLen);
            const float align = boneAxis.x * eyeFwd.x + boneAxis.y * eyeFwd.y + boneAxis.z * eyeFwd.z;
            if (std::fabs(align) >= 0.35f)
                player.headFacingDir = (align < 0.f) ? boneAxis * -1.f : boneAxis;
            else
                player.headFacingDir = eyeFwd;
        } else {
            player.headFacingDir = boneAxis;
        }
        player.headFacingValid = true;
    }
}
