#include "esp/chams_mesh.h"
#include "esp/chams_skinning.h"
#include "config.h"
#include "debug/overlay_log.h"
#include "math/matrix.h"

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#include <Windows.h>
#include <ShlObj.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

enum class MeshChamsMode { Solid, Wireframe };

ChamsSkinnedMesh g_tChamsMesh;
ChamsSkinnedMesh g_ctChamsMesh;
std::unordered_map<std::string, ChamsSkinnedMesh> g_meshCache;

std::string toLower(std::string s) {
    for (char& c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

bool isGlbFile(const std::filesystem::path& p) {
    return p.has_extension() && toLower(p.extension().string()) == ".glb";
}

std::string pathUtf8(const std::filesystem::path& p) {
    const auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

bool readFileBytes(const std::filesystem::path& path, std::vector<std::uint8_t>& out) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        return false;
    const auto size = file.tellg();
    if (size <= 0)
        return false;
    out.resize((size_t)size);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(out.data()), size);
    return file.good();
}

bool meshIsThirdPersonBody(const cgltf_mesh* mesh) {
    if (!mesh || !mesh->name)
        return false;
    return toLower(mesh->name).find("thirdperson_body") != std::string::npos;
}

const cgltf_skin* findSkinForMesh(cgltf_data* data, const cgltf_mesh* mesh) {
    if (!data || !mesh)
        return nullptr;
    for (cgltf_size ni = 0; ni < data->nodes_count; ++ni) {
        const cgltf_node* node = &data->nodes[ni];
        if (node->mesh == mesh && node->skin)
            return node->skin;
    }
    return (data->skins_count > 0) ? &data->skins[0] : nullptr;
}

const cgltf_mesh* findThirdPersonBodyMesh(cgltf_data* data) {
    const cgltf_mesh* best = nullptr;
    size_t bestVerts = 0;
    for (cgltf_size mi = 0; mi < data->meshes_count; ++mi) {
        const cgltf_mesh* mesh = &data->meshes[mi];
        if (!meshIsThirdPersonBody(mesh))
            continue;
        size_t verts = 0;
        for (cgltf_size pi = 0; pi < mesh->primitives_count; ++pi) {
            const cgltf_primitive* prim = &mesh->primitives[pi];
            for (cgltf_size ai = 0; ai < prim->attributes_count; ++ai) {
                if (prim->attributes[ai].type == cgltf_attribute_type_position &&
                    prim->attributes[ai].data)
                    verts += (size_t)prim->attributes[ai].data->count;
            }
        }
        if (verts > bestVerts) {
            bestVerts = verts;
            best = mesh;
        }
    }
    return best;
}

void readJoints(const cgltf_accessor* accessor, cgltf_size vertex,
                std::array<std::uint16_t, 4>& out) {
    out = { 0, 0, 0, 0 };
    if (!accessor)
        return;
    cgltf_uint values[4]{};
    if (cgltf_accessor_read_uint(accessor, vertex, values, 4)) {
        out = {
            (std::uint16_t)values[0], (std::uint16_t)values[1],
            (std::uint16_t)values[2], (std::uint16_t)values[3],
        };
    }
}

void readWeights(const cgltf_accessor* accessor, cgltf_size vertex,
                 std::array<float, 4>& out) {
    out = { 1.f, 0.f, 0.f, 0.f };
    if (!accessor)
        return;
    float values[4]{};
    cgltf_accessor_read_float(accessor, vertex, values, 4);
    out = { values[0], values[1], values[2], values[3] };
}

bool appendPrimitive(const cgltf_primitive& prim,
                     const cgltf_skin& skin,
                     const cgltf_data* data,
                     ChamsSkinnedMesh& mesh) {
    if (prim.type != cgltf_primitive_type_triangles || prim.has_draco_mesh_compression)
        return false;

    const cgltf_accessor* posAcc = nullptr;
    const cgltf_accessor* jointsAcc = nullptr;
    const cgltf_accessor* weightsAcc = nullptr;
    for (cgltf_size ai = 0; ai < prim.attributes_count; ++ai) {
        const cgltf_attribute& attr = prim.attributes[ai];
        if (attr.type == cgltf_attribute_type_position)
            posAcc = attr.data;
        else if (attr.type == cgltf_attribute_type_joints)
            jointsAcc = attr.data;
        else if (attr.type == cgltf_attribute_type_weights)
            weightsAcc = attr.data;
    }
    if (!posAcc || !jointsAcc || !weightsAcc || posAcc->count < 3)
        return false;

    if (mesh.inverseBind.empty() && !chams_skinning::loadSkinData(skin, data, mesh))
        return false;

    const size_t baseVertex = mesh.bindPositions.size();
    mesh.bindPositions.resize(baseVertex + (size_t)posAcc->count);
    mesh.joints.resize(baseVertex + (size_t)posAcc->count);
    mesh.weights.resize(baseVertex + (size_t)posAcc->count);

    for (cgltf_size vi = 0; vi < posAcc->count; ++vi) {
        float pos[3]{};
        cgltf_accessor_read_float(posAcc, vi, pos, 3);
        mesh.bindPositions[baseVertex + (size_t)vi] = { pos[0], pos[1], pos[2] };
        readJoints(jointsAcc, vi, mesh.joints[baseVertex + (size_t)vi]);
        readWeights(weightsAcc, vi, mesh.weights[baseVertex + (size_t)vi]);
    }

    if (prim.indices) {
        const size_t baseIdx = mesh.indices.size();
        mesh.indices.resize(baseIdx + (size_t)prim.indices->count);
        for (cgltf_size ii = 0; ii < prim.indices->count; ++ii)
            mesh.indices[baseIdx + ii] =
                (std::uint32_t)baseVertex + (std::uint32_t)cgltf_accessor_read_index(prim.indices, ii);
    } else {
        const size_t baseIdx = mesh.indices.size();
        mesh.indices.resize(baseIdx + (size_t)posAcc->count);
        for (cgltf_size ii = 0; ii < posAcc->count; ++ii)
            mesh.indices[baseIdx + ii] = (std::uint32_t)baseVertex + (std::uint32_t)ii;
    }

    return true;
}

bool loadGlbFile(const std::filesystem::path& path, ChamsSkinnedMesh& out, std::string* errOut) {
    out = {};
    std::vector<std::uint8_t> bytes;
    if (!readFileBytes(path, bytes)) {
        if (errOut) *errOut = "read failed";
        return false;
    }

    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse(&options, bytes.data(), bytes.size(), &data) != cgltf_result_success || !data) {
        if (errOut) *errOut = "parse failed";
        return false;
    }

    if (cgltf_load_buffers(&options, data, pathUtf8(path).c_str()) != cgltf_result_success) {
        cgltf_free(data);
        if (errOut) *errOut = "buffers failed";
        return false;
    }

    if (data->skins_count == 0) {
        cgltf_free(data);
        if (errOut) *errOut = "no skin";
        return false;
    }

    const cgltf_mesh* bodyMesh = findThirdPersonBodyMesh(data);
    const cgltf_skin* primarySkin = nullptr;
    int parts = 0;

    for (cgltf_size ni = 0; ni < data->nodes_count; ++ni) {
        const cgltf_node& node = data->nodes[ni];
        if (!node.mesh || !node.skin)
            continue;

        std::string meshName;
        if (node.name && node.name[0])
            meshName = node.name;
        else if (node.mesh->name && node.mesh->name[0])
            meshName = node.mesh->name;

        if (bodyMesh && node.mesh != bodyMesh)
            continue;
        if (!chams_skinning::shouldLoadMeshForChams(meshName))
            continue;

        if (!primarySkin)
            primarySkin = node.skin;

        for (cgltf_size pi = 0; pi < node.mesh->primitives_count; ++pi) {
            if (appendPrimitive(node.mesh->primitives[pi], *node.skin, data, out))
                ++parts;
        }
    }

    if (primarySkin && out.inverseBind.empty())
        chams_skinning::loadSkinData(*primarySkin, data, out);

    cgltf_free(data);

    if (parts == 0 || out.bindPositions.empty() || out.indices.size() < 3) {
        out = {};
        if (errOut) *errOut = "no triangles";
        return false;
    }

    chams_skinning::refineJointMappingFromBindGeometry(out);
    out.loaded = true;
    return true;
}

std::filesystem::path crymoreAppDataRoot() {
    wchar_t localApp[MAX_PATH]{};
    if (!SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localApp)))
        return {};
    return std::filesystem::path(localApp) / L"crymore";
}

bool directoryHasGlbFiles(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec))
        return false;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec)
            break;
        if (entry.is_regular_file() && isGlbFile(entry.path()))
            return true;
    }
    return false;
}

/// Ordered mesh search paths; %LOCALAPPDATA%\\crymore is preferred.
std::vector<std::filesystem::path> findMeshDirectories() {
    std::vector<std::filesystem::path> ordered;
    const auto crymoreRoot = crymoreAppDataRoot();
    if (!crymoreRoot.empty()) {
        ordered.push_back(crymoreRoot);
        ordered.push_back(crymoreRoot / L"meshes");
        ordered.push_back(crymoreRoot / L"meshes2");
    }

    wchar_t exePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        const std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
        ordered.push_back(exeDir / "meshes");
        ordered.push_back(exeDir / "meshes2");
        ordered.push_back(exeDir.parent_path() / "meshes");
        ordered.push_back(exeDir.parent_path() / "meshes2");
    }
    wchar_t cwd[MAX_PATH]{};
    if (GetCurrentDirectoryW(MAX_PATH, cwd) > 0) {
        ordered.push_back(std::filesystem::path(cwd) / "meshes");
        ordered.push_back(std::filesystem::path(cwd) / "meshes2");
    }
    ordered.emplace_back("meshes");
    ordered.emplace_back("meshes2");

    std::vector<std::filesystem::path> found;
    for (const auto& candidate : ordered) {
        if (candidate.empty())
            continue;
        std::error_code ec;
        if (!std::filesystem::is_directory(candidate, ec))
            continue;
        const auto absolute = std::filesystem::absolute(candidate, ec);
        if (ec)
            continue;
        bool duplicate = false;
        for (const auto& existing : found) {
            std::error_code sameEc;
            if (std::filesystem::equivalent(existing, absolute, sameEc) && !sameEc) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate)
            found.push_back(absolute);
    }
    return found;
}

std::filesystem::path findMeshesDirectory() {
    const auto dirs = findMeshDirectories();
    for (const auto& dir : dirs) {
        if (directoryHasGlbFiles(dir))
            return dir;
    }
    if (!dirs.empty())
        return dirs.front();
    const auto crymoreRoot = crymoreAppDataRoot();
    if (!crymoreRoot.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(crymoreRoot, ec);
        return crymoreRoot;
    }
    return {};
}

std::filesystem::path findMeshPath(const char* fileName) {
    for (const auto& dir : findMeshDirectories()) {
        const auto full = dir / fileName;
        std::error_code ec;
        if (std::filesystem::exists(full, ec))
            return full;
    }
    return {};
}

std::filesystem::path findMeshByPrefix(const char* prefix) {
    const std::string prefixLower = toLower(prefix);
    std::filesystem::path best;
    for (const auto& dir : findMeshDirectories()) {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec)
                break;
            if (!entry.is_regular_file() || !isGlbFile(entry.path()))
                continue;
            const std::string stem = toLower(entry.path().stem().string());
            if (stem.rfind(prefixLower, 0) != 0)
                continue;
            if (best.empty() || entry.path().filename().string() < best.filename().string())
                best = entry.path();
        }
    }
    return best;
}

const ChamsSkinnedMesh* meshForTeam(int teamNum) {
    if (teamNum == 3 && g_ctChamsMesh.loaded)
        return &g_ctChamsMesh;
    if (g_tChamsMesh.loaded)
        return &g_tChamsMesh;
    if (g_ctChamsMesh.loaded)
        return &g_ctChamsMesh;
    return nullptr;
}

const ChamsSkinnedMesh* meshForPlayer(const PlayerData& player) {
    if (!player.modelKey.empty()) {
        const auto it = g_meshCache.find(toLower(player.modelKey));
        if (it != g_meshCache.end() && it->second.loaded)
            return &it->second;
    }
    return meshForTeam(player.teamNum);
}

bool projectBone(const ViewMatrix& vm, const Vec3& bone, float sw, float sh, Vec2& out) {
    if (!vm.worldToScreen(bone, out, sw, sh))
        return false;
    return std::isfinite(out.x) && std::isfinite(out.y);
}

bool drawPlayerImpl(Renderer& r,
                    const PlayerData& player,
                    const ViewMatrix& vm,
                    unsigned int visCol,
                    unsigned int occCol,
                    bool drawVisible,
                    bool drawOccluded,
                    float sw,
                    float sh,
                    const Vec3& predDelta,
                    MeshChamsMode mode) {
    const ChamsSkinnedMesh* mesh = meshForPlayer(player);
    if (!mesh || !player.skeleton.isValid())
        return false;

    Cs2Skeleton bones = player.skeleton;
    if (predDelta.x != 0.f || predDelta.y != 0.f || predDelta.z != 0.f) {
        for (auto& b : bones.bones) {
            b.position.x += predDelta.x;
            b.position.y += predDelta.y;
            b.position.z += predDelta.z;
        }
    }

    thread_local std::vector<Cs2Mat3x4> palette;
    chams_skinning::buildJointPalette(*mesh, bones, palette);

    const size_t vertCount = mesh->bindPositions.size();
    thread_local std::vector<Vec3> skinned;
    skinned.resize(vertCount);
    for (size_t vi = 0; vi < vertCount; ++vi)
        skinned[vi] = chams_skinning::skinVertex(*mesh, palette, vi);

    const bool playerVis = !g_cfg.visibilityCheckEnabled || !player.visibilityChecked || player.isVisible;
    const float lineThk = std::clamp(g_cfg.chamsOutlineThickness, 0.75f, 4.f);
    bool drewAny = false;

    auto pickColor = [&]() -> unsigned int {
        if (playerVis && drawVisible)
            return visCol;
        if (!playerVis && drawOccluded)
            return occCol;
        return 0u;
    };

    auto drawTri = [&](const Vec2& p0, const Vec2& p1, const Vec2& p2, unsigned int col) {
        if (((col >> 24) & 0xFF) == 0)
            return;
        const float tri[] = { p0.x, p0.y, p1.x, p1.y, p2.x, p2.y };
        r.drawFilledConvexPolygon(tri, 3, col);
        drewAny = true;
    };

    const size_t triCount = mesh->indices.size() / 3;
    for (size_t ti = 0; ti < triCount; ++ti) {
        const Vec3& a = skinned[mesh->indices[ti * 3 + 0]];
        const Vec3& b = skinned[mesh->indices[ti * 3 + 1]];
        const Vec3& c = skinned[mesh->indices[ti * 3 + 2]];

        Vec2 sa{}, sb{}, sc{};
        if (!projectBone(vm, a, sw, sh, sa) ||
            !projectBone(vm, b, sw, sh, sb) ||
            !projectBone(vm, c, sw, sh, sc))
            continue;

        const float area = std::fabsf((sb.x - sa.x) * (sc.y - sa.y) - (sc.x - sa.x) * (sb.y - sa.y));
        if (area < 0.04f)
            continue;

        const unsigned int col = pickColor();
        if (!col)
            continue;

        if (mode == MeshChamsMode::Solid) {
            drawTri(sa, sb, sc, col);
        } else {
            r.drawLine(sa.x, sa.y, sb.x, sb.y, col, lineThk);
            r.drawLine(sb.x, sb.y, sc.x, sc.y, col, lineThk);
            r.drawLine(sc.x, sc.y, sa.x, sa.y, col, lineThk);
            drewAny = true;
        }
    }

    return drewAny;
}

} // namespace

bool ChamsMeshLibrary::chamsBoneVisible(const PlayerData& player, int boneIndex) {
    if (boneIndex < 0 || boneIndex >= PlayerData::kBoneCount)
        return !g_cfg.visibilityCheckEnabled || !player.visibilityChecked || player.isVisible;
    if (!player.chamsPartVisChecked)
        return !g_cfg.visibilityCheckEnabled || !player.visibilityChecked || player.isVisible;
    return player.chamsPartVisible[boneIndex];
}

void ChamsMeshLibrary::initOnce() {
    if (m_inited)
        return;
    m_inited = true;

    g_tChamsMesh = {};
    g_ctChamsMesh = {};
    g_meshCache.clear();

    const auto meshDirs = findMeshDirectories();
    const auto primaryDir = findMeshesDirectory();
    m_searchHint = primaryDir.empty()
        ? "%LOCALAPPDATA%\\crymore"
        : primaryDir.string();

    for (const auto& meshDir : meshDirs) {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(meshDir, ec)) {
            if (ec)
                break;
            if (!entry.is_regular_file() || !isGlbFile(entry.path()))
                continue;
            const std::string key = toLower(entry.path().stem().string());
            if (g_meshCache.count(key))
                continue;
            ChamsSkinnedMesh mesh{};
            std::string err;
            if (!loadGlbFile(entry.path(), mesh, &err))
                continue;
            g_meshCache[key] = std::move(mesh);
        }
    }

    auto tPath = findMeshPath("tm_phoenix.glb");
    if (tPath.empty())
        tPath = findMeshByPrefix("tm_");
    auto ctPath = findMeshPath("ctm_sas.glb");
    if (ctPath.empty())
        ctPath = findMeshByPrefix("ctm_");
    if (!tPath.empty() && tPath == ctPath)
        ctPath.clear();

    auto useCachedOrLoad = [](const std::filesystem::path& path, ChamsSkinnedMesh& out) {
        if (path.empty())
            return;
        const std::string key = toLower(path.stem().string());
        const auto it = g_meshCache.find(key);
        if (it != g_meshCache.end() && it->second.loaded) {
            out = it->second;
            return;
        }
        std::string err;
        loadGlbFile(path, out, &err);
    };

    useCachedOrLoad(tPath, g_tChamsMesh);
    useCachedOrLoad(ctPath, g_ctChamsMesh);

    if (g_tChamsMesh.loaded)
        m_hasMesh = true;
    if (g_ctChamsMesh.loaded)
        m_hasMesh = true;
    if (!g_meshCache.empty())
        m_hasMesh = true;

    if (m_hasMesh) {
        m_statusMessage = "Mesh models loaded (axum skinning, "
            + std::to_string(g_meshCache.size()) + " agents).";
        overlayFileLog("[ChamsMesh] cache=" + std::to_string(g_meshCache.size())
            + " T verts=" + std::to_string(g_tChamsMesh.bindPositions.size())
            + " tris=" + std::to_string(g_tChamsMesh.indices.size() / 3)
            + " CT verts=" + std::to_string(g_ctChamsMesh.bindPositions.size())
            + " tris=" + std::to_string(g_ctChamsMesh.indices.size() / 3)
            + " dir=" + m_searchHint);
    } else {
        m_statusMessage = "No GLB meshes found. Put .glb files in %LOCALAPPDATA%\\crymore";
        overlayFileLog("[ChamsMesh] " + m_statusMessage);
    }
}

bool ChamsMeshLibrary::drawPlayerWireframe(Renderer& r,
                                           const PlayerData& player,
                                           const ViewMatrix& vm,
                                           unsigned int visCol,
                                           unsigned int occCol,
                                           bool drawVisible,
                                           bool drawOccluded,
                                           float sw,
                                           float sh,
                                           const Vec3& predDelta) {
    return drawPlayerImpl(r, player, vm, visCol, occCol, drawVisible, drawOccluded,
                          sw, sh, predDelta, MeshChamsMode::Wireframe);
}

bool ChamsMeshLibrary::drawPlayerSolid(Renderer& r,
                                       const PlayerData& player,
                                       const ViewMatrix& vm,
                                       unsigned int visCol,
                                       unsigned int occCol,
                                       bool drawVisible,
                                       bool drawOccluded,
                                       float sw,
                                       float sh,
                                       const Vec3& predDelta) {
    return drawPlayerImpl(r, player, vm, visCol, occCol, drawVisible, drawOccluded,
                          sw, sh, predDelta, MeshChamsMode::Solid);
}

bool ChamsMeshLibrary::drawPlayer(Renderer& r,
                                  const PlayerData& player,
                                  const ViewMatrix& vm,
                                  unsigned int visCol,
                                  unsigned int occCol,
                                  bool drawVisible,
                                  bool drawOccluded,
                                  float sw,
                                  float sh,
                                  const Vec3& predDelta) {
    return drawPlayerSolid(r, player, vm, visCol, occCol, drawVisible, drawOccluded, sw, sh, predDelta);
}

bool ChamsMeshLibrary::drawPlayerSilhouette2D(Renderer& r,
                                              const PlayerData& player,
                                              const ViewMatrix& vm,
                                              unsigned int visCol,
                                              unsigned int occCol,
                                              bool drawVisible,
                                              bool drawOccluded,
                                              float sw,
                                              float sh,
                                              const Vec3& predDelta) {
    return drawPlayerWireframe(r, player, vm, visCol, occCol, drawVisible, drawOccluded, sw, sh, predDelta);
}
