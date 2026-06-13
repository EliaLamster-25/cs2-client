#pragma once

#include "vector.h"

#include <cmath>
#include <cstring>

/// Row-major 3×4 bone transform as stored in the CS2 skeleton cache.
/// Translation is in the fourth element of each row (indices 3, 7, 11).
struct Mat3x4 {
    float m[12]{};

    static Mat3x4 identity() {
        Mat3x4 out{};
        out.m[0] = out.m[5] = out.m[10] = 1.f;
        return out;
    }

    static Mat3x4 fromRowMajor12(const float* src) {
        Mat3x4 out{};
        std::memcpy(out.m, src, sizeof(out.m));
        return out;
    }

    Vec3 translation() const { return { m[3], m[7], m[11] }; }

    bool isFinite() const {
        for (float f : m) {
            if (!std::isfinite(f))
                return false;
        }
        return true;
    }

    Vec3 transformPoint(const Vec3& p) const {
        return {
            p.x * m[0] + p.y * m[1] + p.z * m[2] + m[3],
            p.x * m[4] + p.y * m[5] + p.z * m[6] + m[7],
            p.x * m[8] + p.y * m[9] + p.z * m[10] + m[11],
        };
    }
};
