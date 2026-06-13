#pragma once

#include "math/vector.h"
#include "memory/process.h"

#include <array>
#include <cstdint>

struct PlayerData;

struct Quat {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
    float w = 1.f;
};

/// Row-major 3×4 as used by CS2 / axum mesh chams (mat[row][col]).
struct Cs2Mat3x4 {
    float m[3][4]{};

    static Cs2Mat3x4 identity();
    static Cs2Mat3x4 fromPositionRotation(const Vec3& pos, const Quat& rot);
    static Cs2Mat3x4 fromGltfColMajor(const float* colMajor16);
    static Cs2Mat3x4 mul(const Cs2Mat3x4& a, const Cs2Mat3x4& b);
    static Cs2Mat3x4 inverseAffine(const Cs2Mat3x4& m);

    Vec3 transformPoint(const Vec3& p) const;
    bool isFinite() const;
};

struct alignas(16) Cs2BoneCacheEntry {
    Vec3 position{};
    float scale = 1.f;
    Quat rotation{};
};

struct Cs2Skeleton {
    static constexpr int kMaxBones = 128;
    static constexpr int kInvalidBone = 128;

    std::array<Cs2BoneCacheEntry, kMaxBones> bones{};
    bool valid = false;

    bool isValid() const;
    Vec3 position(int gameBone) const;
    Cs2Mat3x4 matrix(int gameBone) const;
};

bool readCs2Skeleton(const Process& proc, std::uintptr_t boneCachePtr, const Vec3& origin, Cs2Skeleton& out);

/// Fills PlayerData legacy slot layout + Mat3x4 cache from a CS2 skeleton.
void applySkeletonToPlayer(const Cs2Skeleton& skel, PlayerData& player);

/// Read skeleton from pawn scene node (same pointer chain as axum chams).
bool readPlayerSkeleton(const Process& proc, std::uintptr_t pawn, Cs2Skeleton& out);
