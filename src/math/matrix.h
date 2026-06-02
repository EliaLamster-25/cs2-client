#pragma once

#include "vector.h"

/// @file matrix.h
/// @brief View matrix type used for world-to-screen projection.
///
/// CS2 exposes a 4×4 column-major view–projection matrix at a known offset
/// inside client.dll.  We store it as a flat float[16] so the memory read
/// is a single contiguous copy.

struct ViewMatrix {
    float m[4][4]{};

    /// World-to-screen projection.
    /// @param world  3D world-space position.
    /// @param screen Output 2D screen-space position.
    /// @param screenW  Viewport width  in pixels.
    /// @param screenH  Viewport height in pixels.
    /// @return true if the point is in front of the camera.
    bool worldToScreen(const Vec3& world, Vec2& screen,
                       float screenW, float screenH) const
    {
        // Clip-space W — if ≤ 0 the point is behind the camera.
        float w = m[3][0] * world.x + m[3][1] * world.y
                + m[3][2] * world.z + m[3][3];
        if (w < 0.001f)
            return false;

        float invW = 1.0f / w;

        // NDC → screen
        float nx = (m[0][0] * world.x + m[0][1] * world.y
                  + m[0][2] * world.z + m[0][3]) * invW;
        float ny = (m[1][0] * world.x + m[1][1] * world.y
                  + m[1][2] * world.z + m[1][3]) * invW;

        screen.x = (screenW * 0.5f) + (nx * screenW * 0.5f);
        screen.y = (screenH * 0.5f) - (ny * screenH * 0.5f);

        return true;
    }

    /// Cheap visibility test in clip-space without needing viewport dimensions.
    bool isOnScreen(const Vec3& world) const {
        float w = m[3][0] * world.x + m[3][1] * world.y
                + m[3][2] * world.z + m[3][3];
        if (w < 0.001f)
            return false;

        float invW = 1.0f / w;
        float nx = (m[0][0] * world.x + m[0][1] * world.y
                  + m[0][2] * world.z + m[0][3]) * invW;
        float ny = (m[1][0] * world.x + m[1][1] * world.y
                  + m[1][2] * world.z + m[1][3]) * invW;

        return nx >= -1.0f && nx <= 1.0f && ny >= -1.0f && ny <= 1.0f;
    }
};
