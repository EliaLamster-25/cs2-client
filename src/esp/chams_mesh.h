#pragma once

#include "game/player.h"
#include "math/vector.h"
#include "overlay/renderer.h"

struct ViewMatrix;

/// Loads CS2 player GLB meshes and renders skinned chams silhouettes.
class ChamsMeshLibrary {
public:
    void initOnce();
    bool ready() const { return m_hasMesh; }
    const std::string& searchHint() const { return m_searchHint; }
    const std::string& statusMessage() const { return m_statusMessage; }

    bool drawPlayer(Renderer& r,
                    const PlayerData& player,
                    const ViewMatrix& vm,
                    unsigned int visCol,
                    unsigned int occCol,
                    bool drawVisible,
                    bool drawOccluded,
                    float sw,
                    float sh,
                    const Vec3& predDelta);

    /// Flat filled player silhouette with a thin outer outline (2D overlay style).
    bool drawPlayerSilhouette2D(Renderer& r,
                                const PlayerData& player,
                                const ViewMatrix& vm,
                                unsigned int visCol,
                                unsigned int occCol,
                                bool drawVisible,
                                bool drawOccluded,
                                float sw,
                                float sh,
                                const Vec3& predDelta);

private:
    static bool chamsBoneVisible(const PlayerData& player, int boneIndex);

    bool m_hasMesh = false;
    bool m_inited = false;
    std::string m_searchHint;
    std::string m_statusMessage;
};
