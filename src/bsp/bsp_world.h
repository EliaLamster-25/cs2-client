#pragma once
#include "math/vector.h"
#include <vector>
#include <string>
#include <unordered_map>

// ─────────────────────────────────────────────────────────────────────────────
// BspWorld – loads a CS2 map's collision brushes from disk (VBSP file) and
// answers fast ray-sweep queries against solid geometry.
//
// Usage:
//   BspWorld world;
//   world.load("de_dust2", "C:\\...\\Counter-Strike Global Offensive");
//   float t;  Vec3 n;
//   if (world.sweep(posA, posB, t, n)) { /* reflected bounce */ }
// ─────────────────────────────────────────────────────────────────────────────

struct BspPlane {
    Vec3  n;   // outward normal
    float d;   // plane distance from origin  (n·x == d on the plane)
};

struct BspBrushSide {
    int planeIdx;  // index into BspWorld::m_planes
};

struct BspBrush {
    int   firstSide;
    int   numSides;
    // Conservative axis-aligned bounding box for fast rejection
    float mnX, mnY, mnZ;
    float mxX, mxY, mxZ;
};

// One triangle from a CS2 RnMesh_t physics shape (world-space, solid-surface).
struct MeshTri {
    Vec3 p[3];  // vertex positions (world space)
    Vec3 n;     // outward unit normal = normalize(cross(p[1]-p[0], p[2]-p[0]))
};

class BspWorld {
public:
    // Load solid-brush collision data from
    //   <cs2Path>/game/csgo/maps/<mapName>.bsp
    bool load(const std::string& mapName, const std::string& cs2Path);

    // Load pre-extracted solid collision triangles from a raw .tri binary file.
    // Format: no header; N triangles × 9 × float32 (v0.xyz, v1.xyz, v2.xyz).
    // Returns true when at least one triangle was loaded successfully.
    bool loadTri(const std::string& triFilePath);

    // Load Source-2 physics planes by parsing vmdl_c PHYS blocks inside the VPK.
    // Called when load() fails (CS2 retail maps don't embed a BSP brush lump).
    bool loadFromVpk(const std::string& mapName, const std::string& cs2Path);

    void clear();

    bool               isLoaded()    const { return m_loaded; }
    const std::string& currentMap()  const { return m_mapName; }

    // Sweep the line segment [a → b] against all solid brushes.
    // Returns true on first solid hit.
    //   t_hit  : parametric hit in [0, 1]  (a + (b-a)*t_hit = impact point)
    //   norm   : outward surface normal at impact
    bool sweep(const Vec3& a, const Vec3& b, float& t_hit, Vec3& norm) const;

    // Fast any-hit sweep for visibility (line-of-sight) queries.
    // Returns true as soon as ANY solid is hit — does NOT search for closest.
    // Much faster than sweep() when the ray is blocked by nearby geometry.
    bool sweepAny(const Vec3& a, const Vec3& b) const;

    // Per-frame sweep ID for brush grid deduplication (thread-safe via thread_local).
    static uint32_t& sweepCounter();

private:
    bool        m_loaded = false;
    std::string m_mapName;

    std::vector<BspPlane>     m_planes;
    std::vector<BspBrushSide> m_sides;
    std::vector<BspBrush>     m_brushes;

    // Triangle mesh data extracted from world_physics RnMesh_t shapes.
    // Stored in world space; only populated for the map's world_physics file.
    std::vector<MeshTri>                                m_tris;
    // Spatial hash: packed (cx, cy) cell key -> indices into m_tris.
    // Cell size = kTriGridCell units in XY; covers all triangles in a 2D grid.
    std::unordered_map<int64_t, std::vector<uint32_t>>  m_triGrid;

    // Spatial hash for brushes: packed (cx, cy) cell key -> brush indices.
    // Cell size = 512 units (brushes are larger than triangles).
    std::unordered_map<int64_t, std::vector<uint32_t>>  m_brushGrid;
    static constexpr float kBrushGridCell = 512.f;

    // Rebuilds m_triGrid from current contents of m_tris.
    void buildTriGrid();

    // Rebuilds m_brushGrid from current contents of m_brushes.
    void buildBrushGrid();

    // Per-brush convex sweep using enter/exit parametric tracking.
    bool sweepBrush(const Vec3& a, const Vec3& b,
                    const BspBrush& br,
                    float& t_out, Vec3& n_out) const;

    // Sweep segment [a→b] (sphere radius kSphereR) against extracted mesh triangles.
    bool sweepTris(const Vec3& a, const Vec3& b,
                   float& t_hit, Vec3& norm) const;

    // Any-hit variant: returns true immediately when first blocking triangle is found.
    // Uses per-sweep triangle dedup (visitId) to avoid re-checking triangles that
    // span multiple spatial grid cells.
    bool sweepTrisAny(const Vec3& a, const Vec3& b, uint32_t visitId) const;

    // Per-sweep triangle dedup storage (thread_local for thread safety).
    static std::vector<uint32_t>& triVisited();

    // Cell size for the triangle spatial grid (exposed for the internal sweep helper).
    static constexpr float kTriGridCell = 256.f;

    // Shared triangle-sweep implementation with per-sweep dedup via visitId.
    // When anyHit=true, returns immediately on first blocking triangle.
    bool sweepTrisInternal(const Vec3& a, const Vec3& b,
                           float& t_hit, Vec3& norm,
                           uint32_t visitId, bool anyHit) const;
};
