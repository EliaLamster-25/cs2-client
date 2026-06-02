#pragma once

#include <cmath>

/// @file vector.h
/// @brief Lightweight 3D vector matching Source 2 engine's Vector layout.

struct Vec3 {
    float x = 0.f, y = 0.f, z = 0.f;

    Vec3() = default;
    Vec3(float x, float y, float z) : x(x), y(y), z(z) {}

    Vec3 operator+(const Vec3& o) const { return { x + o.x, y + o.y, z + o.z }; }
    Vec3 operator-(const Vec3& o) const { return { x - o.x, y - o.y, z - o.z }; }
    Vec3 operator*(float s)       const { return { x * s,   y * s,   z * s   }; }

    float lengthSq() const { return x * x + y * y + z * z; }
    float length()  const { return std::sqrt(lengthSq()); }
};

struct Vec2 {
    float x = 0.f, y = 0.f;

    Vec2() = default;
    Vec2(float x, float y) : x(x), y(y) {}
};
