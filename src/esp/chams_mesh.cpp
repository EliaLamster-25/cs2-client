#include "esp/chams_mesh.h"

#include "config.h"
#include "math/matrix.h"

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#include <Windows.h>
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

struct Mat4 {
    float m[16]{};

    static Mat4 identity() {
        Mat4 out{};
        out.m[0] = out.m[5] = out.m[10] = out.m[15] = 1.f;
        return out;
    }

    Vec3 transformPoint(const Vec3& p) const {
        return {
            p.x * m[0] + p.y * m[4] + p.z * m[8] + m[12],
            p.x * m[1] + p.y * m[5] + p.z * m[9] + m[13],
            p.x * m[2] + p.y * m[6] + p.z * m[10] + m[14],
        };
    }
};

struct SkinnedMesh {
    struct Vertex {
        Vec3 bindPos{}; // Source hammer units (GLB POSITION accessor space)
        std::array<std::uint16_t, 4> joints{};
        std::array<float, 4> weights{};
    };

    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<Vec3> bindJointPos;
    std::vector<Vec3> bindJointScene;
    std::vector<Mat4> inverseBind;
    std::vector<Mat4> bindSkinMatrix;
    std::vector<Vec3> posedHammer;
    std::vector<Vec3> localHammer;
    std::vector<int> vertBoneSlot;
    std::vector<int> jointToBoneSlot;
    std::vector<float> jointInfluence;
    std::vector<uint8_t> jointValid;
    std::array<int, PlayerData::kBoneCount> slotRepJoint{};
    std::array<Vec3, PlayerData::kBoneCount> bindSlotPos{};
    Vec3 bindPelvis{};
    Vec3 bindFoot{};
    Vec3 bindHead{};
    float bindFootZ = 0.f;
    float bindFullHeight = 1.f;
    float bindHeight = 1.f;
    float bindYaw = 0.f;
    bool loaded = false;
    bool rigidMesh = false;
};

namespace {

Mat4 mat4FromGltf(const float* src) {
    Mat4 out{};
    std::memcpy(out.m, src, sizeof(out.m));
    return out;
}

Mat4 mat4Mul(const Mat4& a, const Mat4& b) {
    Mat4 out{};
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            out.m[c * 4 + r] =
                a.m[0 * 4 + r] * b.m[c * 4 + 0] +
                a.m[1 * 4 + r] * b.m[c * 4 + 1] +
                a.m[2 * 4 + r] * b.m[c * 4 + 2] +
                a.m[3 * 4 + r] * b.m[c * 4 + 3];
        }
    }
    return out;
}

Mat4 mat4RotationOnly(const Mat4& m) {
    Mat4 out = m;
    out.m[12] = out.m[13] = out.m[14] = 0.f;
    return out;
}

Mat4 mat4Translation(const Vec3& t) {
    Mat4 out = Mat4::identity();
    out.m[12] = t.x;
    out.m[13] = t.y;
    out.m[14] = t.z;
    return out;
}

// CS2 GLB: skeleton nodes sit under a ~0.0254 root scale (meters) but mesh vertex
// POSITION accessors are still in Source hammer units — scale before skinning.
constexpr float kHammerToGltfScene = 0.0254f;

Vec3 hammerVertexToGltfScene(const Vec3& hammerPos) {
    return {
        hammerPos.x * kHammerToGltfScene,
        hammerPos.y * kHammerToGltfScene,
        hammerPos.z * kHammerToGltfScene,
    };
}

Vec3 hammerToGltfScene(const Vec3& h) {
    return { h.x * kHammerToGltfScene, h.z * kHammerToGltfScene, h.y * kHammerToGltfScene };
}

Vec3 gltfSceneToHammer(const Vec3& s) {
    const float inv = 1.f / kHammerToGltfScene;
    return { s.x * inv, s.z * inv, s.y * inv };
}

Vec3 sceneToHammerMode(const Vec3& s, int mode) {
    const float inv = 1.f / kHammerToGltfScene;
    const float x = s.x * inv;
    const float y = s.y * inv;
    const float z = s.z * inv;
    switch (mode) {
    case 1: return { x, y, z };
    case 2: return { y, x, z };
    case 3: return { z, x, y };
    case 4: return { x, -z, y };
    case 5: return { -y, x, z };
    default: return { x, z, y };
    }
}

float hammerHeightZ(const std::vector<Vec3>& pts, float& outMinZ, float& outMaxZ) {
    outMinZ = 1e30f;
    outMaxZ = -1e30f;
    for (const Vec3& p : pts) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
            continue;
        outMinZ = (std::min)(outMinZ, p.z);
        outMaxZ = (std::max)(outMaxZ, p.z);
    }
    if (outMinZ > outMaxZ)
        return 0.f;
    return outMaxZ - outMinZ;
}

int pickSceneToHammerMode(const std::vector<Vec3>& sceneVerts) {
    int bestMode = 0;
    float bestScore = 1e30f;
    for (int mode = 0; mode < 6; ++mode) {
        float minZ = 0.f;
        float maxZ = 0.f;
        std::vector<Vec3> trial;
        trial.reserve(sceneVerts.size());
        for (const Vec3& s : sceneVerts) {
            const Vec3 h = sceneToHammerMode(s, mode);
            if (std::isfinite(h.x) && std::isfinite(h.y) && std::isfinite(h.z))
                trial.push_back(h);
        }
        const float h = hammerHeightZ(trial, minZ, maxZ);
        float score = std::abs(h - 72.f);
        if (h < 24.f || h > 160.f)
            score += 500.f;
        if (score < bestScore) {
            bestScore = score;
            bestMode = mode;
        }
    }
    return bestMode;
}

Vec3 mat4TranslationVec(const float* m) {
    return { m[12], m[13], m[14] };
}

std::string toLower(std::string s) {
    for (char& c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

bool isGlbFile(const std::filesystem::path& path) {
    return toLower(path.extension().string()) == ".glb";
}

std::string pathUtf8(const std::filesystem::path& path) {
#if defined(__cpp_lib_char8_t)
    const std::u8string u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
#else
    return path.u8string();
#endif
}

bool readFileBytes(const std::filesystem::path& path, std::vector<std::uint8_t>& out) {
#ifdef _WIN32
    const HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (file == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 12) {
        CloseHandle(file);
        return false;
    }

    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = ReadFile(
        file,
        out.data(),
        static_cast<DWORD>(out.size()),
        &read,
        nullptr
    );
    CloseHandle(file);
    return ok && read == out.size();
#else
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        return false;
    const std::streamsize sz = file.tellg();
    if (sz <= 12)
        return false;
    out.resize(static_cast<size_t>(sz));
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(out.data()), sz))
        return false;
    return true;
#endif
}

int mapJointNameToBoneSlot(const std::string& rawName) {
    const std::string name = toLower(rawName);

    auto has = [&](const char* part) { return name.find(part) != std::string::npos; };

    if (name == "pelvis" || has("pelvis"))
        return 0;
    if (has("spine_0") || name == "spine0")
        return 2;
    if (has("spine_1") || has("spine_2") || name == "spine1" || name == "spine2")
        return 4;
    if (has("neck"))
        return 5;
    if (has("head"))
        return 6;

    const bool left = has("_l") || has("left");
    const bool right = has("_r") || has("right");

    if (has("clavicle") || has("shoulder")) {
        if (left) return 8;
        if (right) return 13;
    }
    if (has("arm_upper") || has("upperarm")) {
        if (left) return 8;
        if (right) return 13;
    }
    if (has("arm_lower") || has("forearm") || has("lowerarm")) {
        if (left) return 9;
        if (right) return 14;
    }
    if (has("hand") || has("wrist")) {
        if (left) return 11;
        if (right) return 16;
    }
    if (has("leg_upper") || has("thigh") || has("hip")) {
        if (left) return 22;
        if (right) return 25;
    }
    if (has("leg_lower") || has("knee") || has("calf")) {
        if (left) return 23;
        if (right) return 26;
    }
    if (has("ankle") || has("foot") || has("toe")) {
        if (left) return 24;
        if (right) return 27;
    }

    return -1;
}

bool meshIsThirdPersonBody(const cgltf_mesh* mesh) {
    if (!mesh || !mesh->name)
        return false;
    const std::string name = toLower(mesh->name);
    return name.find("thirdperson_body") != std::string::npos;
}

const cgltf_mesh* findThirdPersonBodyMesh(cgltf_data* data) {
    if (!data)
        return nullptr;

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
                    prim->attributes[ai].data) {
                    verts += (size_t)prim->attributes[ai].data->count;
                }
            }
        }
        if (verts > bestVerts) {
            bestVerts = verts;
            best = mesh;
        }
    }
    return best;
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

void setupSkinBindData(const cgltf_skin* skin, SkinnedMesh& out) {
    if (!skin)
        return;

    out.jointToBoneSlot.assign(skin->joints_count, -1);
    out.bindJointPos.assign(skin->joints_count, Vec3{});
    out.bindJointScene.assign(skin->joints_count, Vec3{});
    out.inverseBind.assign(skin->joints_count, Mat4::identity());
    out.bindSkinMatrix.assign(skin->joints_count, Mat4::identity());

    for (cgltf_size ji = 0; ji < skin->joints_count; ++ji) {
        const cgltf_node* jointNode = skin->joints[ji];
        Mat4 bindGlobal = Mat4::identity();
        if (jointNode) {
            float worldMat[16]{};
            cgltf_node_transform_world(jointNode, worldMat);
            bindGlobal = mat4FromGltf(worldMat);
            out.bindJointScene[ji] = mat4TranslationVec(worldMat);
            if (jointNode->name)
                out.jointToBoneSlot[ji] = mapJointNameToBoneSlot(jointNode->name);
        }

        if (skin->inverse_bind_matrices) {
            float ibm[16]{};
            if (cgltf_accessor_read_float(skin->inverse_bind_matrices, ji, ibm, 16) == 16)
                out.inverseBind[ji] = mat4FromGltf(ibm);
        }

        out.bindSkinMatrix[ji] = mat4Mul(bindGlobal, out.inverseBind[ji]);
    }
}

bool buildRawHammerBindPose(SkinnedMesh& mesh) {
    if (mesh.vertices.empty())
        return false;

    mesh.posedHammer.resize(mesh.vertices.size());
    for (size_t vi = 0; vi < mesh.vertices.size(); ++vi)
        mesh.posedHammer[vi] = mesh.vertices[vi].bindPos;

    float minZ = 1e30f;
    float maxZ = -1e30f;
    for (const Vec3& p : mesh.posedHammer) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
            return false;
        minZ = (std::min)(minZ, p.z);
        maxZ = (std::max)(maxZ, p.z);
    }
    if (minZ > maxZ)
        return false;

    const float fullHeight = maxZ - minZ;
    if (fullHeight < 1.f)
        return false;

    // CS2 GLB verts span feet (minZ) to head (maxZ). Pelvis sits ~40% up, not near the feet.
    const float pelvisZ = minZ + fullHeight * 0.40f;
    const float chestZ = minZ + fullHeight * 0.55f;
    const float headZ = minZ + fullHeight * 0.92f;
    const float bandPelvis = fullHeight * 0.07f;
    const float bandChest = fullHeight * 0.10f;
    const float bandHead = fullHeight * 0.08f;

    Vec3 pelvisSum{};
    Vec3 chestSum{};
    Vec3 headSum{};
    int pelvisCount = 0;
    int chestCount = 0;
    int headCount = 0;

    for (const Vec3& p : mesh.posedHammer) {
        if (std::fabs(p.z - pelvisZ) < bandPelvis) {
            pelvisSum = pelvisSum + p;
            ++pelvisCount;
        }
        if (std::fabs(p.z - chestZ) < bandChest) {
            chestSum = chestSum + p;
            ++chestCount;
        }
        if (std::fabs(p.z - headZ) < bandHead) {
            headSum = headSum + p;
            ++headCount;
        }
    }

    mesh.bindPelvis = pelvisCount > 0
        ? pelvisSum * (1.f / (float)pelvisCount)
        : Vec3{ 0.f, 0.f, pelvisZ };
    mesh.bindHead = headCount > 0
        ? headSum * (1.f / (float)headCount)
        : Vec3{ 0.f, 0.f, headZ };

    mesh.bindYaw = 0.f;
    if (chestCount > 0) {
        const Vec3 chest = chestSum * (1.f / (float)chestCount);
        const Vec3 forward = chest - mesh.bindPelvis;
        if (forward.x * forward.x + forward.y * forward.y > 1.f)
            mesh.bindYaw = std::atan2f(forward.y, forward.x);
    }

    mesh.bindHeight = mesh.bindHead.z - mesh.bindPelvis.z;
    if (mesh.bindHeight < 8.f)
        mesh.bindHeight = fullHeight * 0.52f;

    mesh.bindFootZ = minZ;
    mesh.bindFullHeight = fullHeight;
    mesh.bindFoot = Vec3{ mesh.bindPelvis.x, mesh.bindPelvis.y, minZ };
    {
        Vec3 footSum{};
        int footCount = 0;
        const float footBand = fullHeight * 0.05f;
        for (const Vec3& p : mesh.posedHammer) {
            if (p.z - minZ < footBand) {
                footSum = footSum + p;
                ++footCount;
            }
        }
        if (footCount > 0)
            mesh.bindFoot = footSum * (1.f / (float)footCount);
    }

    mesh.localHammer.resize(mesh.posedHammer.size());
    for (size_t vi = 0; vi < mesh.posedHammer.size(); ++vi)
        mesh.localHammer[vi] = mesh.posedHammer[vi] - mesh.bindPelvis;

    return true;
}

size_t maxJointIndexFromVertices(const SkinnedMesh& mesh) {
    size_t maxJi = 0;
    for (const auto& v : mesh.vertices) {
        for (int k = 0; k < 4; ++k)
            maxJi = (std::max)(maxJi, (size_t)v.joints[(size_t)k]);
    }
    return maxJi + 1;
}

void computeBindJointsFromWeights(SkinnedMesh& mesh) {
    if (mesh.vertices.empty() || mesh.posedHammer.size() != mesh.vertices.size())
        return;

    const size_t jointCount = (std::max)(mesh.jointToBoneSlot.size(), maxJointIndexFromVertices(mesh));
    if (mesh.jointToBoneSlot.size() < jointCount)
        mesh.jointToBoneSlot.resize(jointCount, -1);
    mesh.bindJointPos.assign(jointCount, Vec3{});
    mesh.jointInfluence.assign(jointCount, 0.f);
    mesh.jointValid.assign(jointCount, 0);

    std::vector<Vec3> sums(jointCount, Vec3{});
    std::vector<float> wsums(jointCount, 0.f);

    for (size_t vi = 0; vi < mesh.vertices.size(); ++vi) {
        const Vec3& bindVert = mesh.posedHammer[vi];
        const auto& v = mesh.vertices[vi];
        for (int k = 0; k < 4; ++k) {
            const float w = v.weights[(size_t)k];
            if (w <= 0.0001f)
                continue;
            const size_t ji = v.joints[(size_t)k];
            if (ji >= jointCount)
                continue;
            sums[ji] = sums[ji] + bindVert * w;
            wsums[ji] += w;
            mesh.jointInfluence[ji] += w;
        }
    }

    for (size_t ji = 0; ji < jointCount; ++ji) {
        if (wsums[ji] > 0.0001f) {
            mesh.bindJointPos[ji] = sums[ji] * (1.f / wsums[ji]);
            mesh.jointValid[ji] = 1;
        }
    }
}

bool bakeBindPoseHammer(SkinnedMesh& mesh) {
    if (mesh.vertices.empty() || mesh.bindSkinMatrix.empty())
        return false;

    std::vector<Vec3> sceneVerts(mesh.vertices.size());
    for (size_t vi = 0; vi < mesh.vertices.size(); ++vi) {
        const auto& v = mesh.vertices[vi];
        Vec3 scenePos{};
        for (int k = 0; k < 4; ++k) {
            const float w = v.weights[(size_t)k];
            if (w <= 0.0001f)
                continue;
            const size_t ji = v.joints[(size_t)k];
            if (ji >= mesh.bindSkinMatrix.size())
                continue;
            scenePos = scenePos + mesh.bindSkinMatrix[ji].transformPoint(v.bindPos) * w;
        }
        if (!std::isfinite(scenePos.x) || !std::isfinite(scenePos.y) || !std::isfinite(scenePos.z))
            return false;
        sceneVerts[vi] = scenePos;
    }

    // Baked scene positions are in glTF meters; fixed axis remap to Source hammer (Z-up).
    mesh.posedHammer.resize(mesh.vertices.size());
    for (size_t vi = 0; vi < sceneVerts.size(); ++vi)
        mesh.posedHammer[vi] = gltfSceneToHammer(sceneVerts[vi]);

    for (size_t ji = 0; ji < mesh.bindJointScene.size(); ++ji)
        mesh.bindJointPos[ji] = gltfSceneToHammer(mesh.bindJointScene[ji]);

    mesh.bindPelvis = mesh.posedHammer[0];
    mesh.bindHead = mesh.posedHammer[0];
    for (const Vec3& p : mesh.posedHammer) {
        if (p.z < mesh.bindPelvis.z)
            mesh.bindPelvis = p;
        if (p.z > mesh.bindHead.z)
            mesh.bindHead = p;
    }

    for (size_t ji = 0; ji < mesh.jointToBoneSlot.size(); ++ji) {
        if (mesh.jointToBoneSlot[ji] == 6)
            mesh.bindHead = mesh.bindJointPos[ji];
        if (mesh.jointToBoneSlot[ji] == 0)
            mesh.bindPelvis = mesh.bindJointPos[ji];
    }

    mesh.bindHeight = (mesh.bindHead - mesh.bindPelvis).length();
    if (mesh.bindHeight < 1.f)
        mesh.bindHeight = 72.f;

    Vec3 pelvisAnchor = mesh.bindPelvis;
    for (size_t ji = 0; ji < mesh.jointToBoneSlot.size(); ++ji) {
        if (mesh.jointToBoneSlot[ji] == 0) {
            pelvisAnchor = mesh.bindJointPos[ji];
            break;
        }
    }
    const Vec3 pelvisShift = pelvisAnchor - mesh.bindPelvis;
    if (pelvisShift.lengthSq() > 0.01f) {
        for (Vec3& p : mesh.posedHammer)
            p = p + pelvisShift;
        mesh.bindPelvis = pelvisAnchor;
        mesh.bindHead = mesh.bindHead + pelvisShift;
    }

    mesh.localHammer.resize(mesh.posedHammer.size());
    for (size_t vi = 0; vi < mesh.posedHammer.size(); ++vi)
        mesh.localHammer[vi] = mesh.posedHammer[vi] - mesh.bindPelvis;

    return true;
}

void finalizeMeshFacing(SkinnedMesh& mesh) {
    int jiL = -1;
    int jiR = -1;
    for (size_t ji = 0; ji < mesh.jointToBoneSlot.size(); ++ji) {
        if (mesh.jointToBoneSlot[ji] == 8)
            jiL = (int)ji;
        if (mesh.jointToBoneSlot[ji] == 13)
            jiR = (int)ji;
    }
    if (jiL >= 0 && jiR >= 0) {
        const Vec3 sh = mesh.bindJointPos[(size_t)jiR] - mesh.bindJointPos[(size_t)jiL];
        if (sh.x * sh.x + sh.y * sh.y > 1.f)
            mesh.bindYaw = std::atan2f(sh.y, sh.x);
    }
}

void mapJointsSpatial(SkinnedMesh& mesh) {
    if (mesh.bindHeight < 1.f)
        return;

    struct Ref {
        int slot;
        float relZ;
        float relSide;
    };

    static constexpr Ref kRefs[] = {
        { 0, 0.04f, 0.00f },
        { 2, 0.28f, 0.00f },
        { 4, 0.52f, 0.00f },
        { 5, 0.68f, 0.00f },
        { 6, 0.90f, 0.00f },
        { 8, 0.62f, -0.14f },
        { 9, 0.48f, -0.20f },
        { 11, 0.34f, -0.22f },
        { 13, 0.62f, 0.14f },
        { 14, 0.48f, 0.20f },
        { 16, 0.34f, 0.22f },
        { 22, 0.02f, -0.10f },
        { 23, -0.22f, -0.10f },
        { 24, -0.46f, -0.10f },
        { 25, 0.02f, 0.10f },
        { 26, -0.22f, 0.10f },
        { 27, -0.46f, 0.10f },
    };

    constexpr float kMaxDistSq = 0.08f * 0.08f;
    constexpr float kMinInfluence = 2.f;

    // Drop joints with no skinning influence; keep any name-based mappings.
    for (size_t ji = 0; ji < mesh.jointToBoneSlot.size(); ++ji) {
        if (!mesh.jointValid[ji])
            mesh.jointToBoneSlot[ji] = -1;
    }

    // Exactly one GLB joint per game bone — pick highest mesh influence in range.
    for (const Ref& ref : kRefs) {
        int bestJoint = -1;
        float bestScore = -1.f;
        for (size_t ji = 0; ji < mesh.bindJointPos.size(); ++ji) {
            if (!mesh.jointValid[ji] || mesh.jointInfluence[ji] < kMinInfluence)
                continue;
            if (mesh.jointToBoneSlot[ji] >= 0)
                continue;

            const Vec3& p = mesh.bindJointPos[ji];
            const float relZ = (p.z - mesh.bindPelvis.z) / mesh.bindHeight;
            const float relSide = p.y / mesh.bindHeight;
            const float dz = relZ - ref.relZ;
            const float dy = relSide - ref.relSide;
            const float dist = dz * dz + dy * dy;
            if (dist > kMaxDistSq)
                continue;

            const float score = mesh.jointInfluence[ji] / (1.f + dist * 40.f);
            if (score > bestScore) {
                bestScore = score;
                bestJoint = (int)ji;
            }
        }
        if (bestJoint >= 0)
            mesh.jointToBoneSlot[(size_t)bestJoint] = ref.slot;
    }
}

void fillBindSlotPositions(SkinnedMesh& mesh) {
    mesh.bindSlotPos.fill(mesh.bindPelvis);

    for (size_t ji = 0; ji < mesh.jointToBoneSlot.size(); ++ji) {
        const int slot = mesh.jointToBoneSlot[ji];
        if (slot < 0 || slot >= PlayerData::kBoneCount || !mesh.jointValid[ji])
            continue;
        const int cur = mesh.slotRepJoint[(size_t)slot];
        if (cur < 0 || (int)ji != cur)
            continue;
        mesh.bindSlotPos[(size_t)slot] = mesh.bindJointPos[ji];
    }

    struct Ref {
        int slot;
        float relZ;
        float relSide;
    };
    static constexpr Ref kRefs[] = {
        { 0, 0.00f, 0.00f },
        { 2, 0.24f, 0.00f },
        { 4, 0.48f, 0.00f },
        { 5, 0.64f, 0.00f },
        { 6, 0.86f, 0.00f },
        { 8, 0.58f, -0.14f },
        { 9, 0.44f, -0.20f },
        { 11, 0.30f, -0.22f },
        { 13, 0.58f, 0.14f },
        { 14, 0.44f, 0.20f },
        { 16, 0.30f, 0.22f },
        { 22, 0.00f, -0.10f },
        { 23, -0.24f, -0.10f },
        { 24, -0.48f, -0.10f },
        { 25, 0.00f, 0.10f },
        { 26, -0.24f, 0.10f },
        { 27, -0.48f, 0.10f },
    };

    for (const Ref& ref : kRefs) {
        if (mesh.slotRepJoint[(size_t)ref.slot] >= 0)
            continue;
        mesh.bindSlotPos[(size_t)ref.slot] = {
            mesh.bindPelvis.x,
            mesh.bindPelvis.y + ref.relSide * mesh.bindHeight,
            mesh.bindPelvis.z + ref.relZ * mesh.bindHeight,
        };
    }

    mesh.bindSlotPos[0] = mesh.bindPelvis;
    mesh.bindSlotPos[6] = mesh.bindHead;
}

int spatialBoneSlot(const SkinnedMesh& mesh, const Vec3& bindVert) {
    const float relZ = (bindVert.z - mesh.bindFootZ) / mesh.bindFullHeight;
    const float relSide = (bindVert.y - mesh.bindPelvis.y) / mesh.bindHeight;
    const float absSide = std::fabsf(relSide);
    const bool left = relSide < 0.f;

    if (relZ < 0.42f && absSide > 0.04f) {
        if (relZ < 0.14f)
            return left ? 24 : 27;
        if (relZ < 0.28f)
            return left ? 23 : 26;
        return left ? 22 : 25;
    }
    if (relZ >= 0.28f && relZ <= 0.78f && absSide > 0.10f) {
        if (relZ < 0.48f)
            return left ? 11 : 16;
        if (relZ < 0.62f)
            return left ? 9 : 14;
        return left ? 8 : 13;
    }
    if (relZ > 0.84f)
        return 6;
    if (relZ > 0.68f)
        return 5;
    if (relZ > 0.52f)
        return 4;
    if (relZ > 0.36f)
        return 2;
    return 0;
}

void assignVertexBoneSlots(SkinnedMesh& mesh) {
    mesh.vertBoneSlot.assign(mesh.vertices.size(), 0);

    for (size_t vi = 0; vi < mesh.vertices.size(); ++vi) {
        const Vec3& bindVert = mesh.posedHammer[vi];
        const auto& v = mesh.vertices[vi];

        int bestSlot = -1;
        float bestWeight = 0.f;
        for (int k = 0; k < 4; ++k) {
            const float w = v.weights[(size_t)k];
            if (w <= 0.05f)
                continue;
            const size_t ji = v.joints[(size_t)k];
            if (ji >= mesh.jointToBoneSlot.size())
                continue;
            if (!mesh.jointValid.empty() && !mesh.jointValid[ji])
                continue;
            const int slot = mesh.jointToBoneSlot[ji];
            if (slot < 0 || slot >= PlayerData::kBoneCount)
                continue;
            if (w > bestWeight) {
                bestWeight = w;
                bestSlot = slot;
            }
        }

        if (bestSlot < 0 || bestWeight < 0.35f)
            bestSlot = spatialBoneSlot(mesh, bindVert);

        mesh.vertBoneSlot[vi] = bestSlot;
    }
}

Vec3 clampDisplacement(const Vec3& pos, const Vec3& anchor, float maxDist) {
    const Vec3 delta = pos - anchor;
    const float lenSq = delta.lengthSq();
    const float maxSq = maxDist * maxDist;
    if (lenSq <= maxSq || lenSq < 1e-6f)
        return pos;
    return anchor + delta * (maxDist / std::sqrtf(lenSq));
}

void finalizeSkeletonMapping(SkinnedMesh& mesh) {
    mapJointsSpatial(mesh);

    mesh.slotRepJoint.fill(-1);
    for (size_t ji = 0; ji < mesh.jointToBoneSlot.size(); ++ji) {
        const int slot = mesh.jointToBoneSlot[ji];
        if (slot < 0 || slot >= PlayerData::kBoneCount)
            continue;
        const int cur = mesh.slotRepJoint[(size_t)slot];
        if (cur < 0 ||
            mesh.jointInfluence[ji] > mesh.jointInfluence[(size_t)cur]) {
            mesh.slotRepJoint[(size_t)slot] = (int)ji;
        }
    }

    fillBindSlotPositions(mesh);
    assignVertexBoneSlots(mesh);

    mesh.localHammer.resize(mesh.posedHammer.size());
    for (size_t vi = 0; vi < mesh.posedHammer.size(); ++vi)
        mesh.localHammer[vi] = mesh.posedHammer[vi] - mesh.bindPelvis;
}

Vec3 vecCross(const Vec3& a, const Vec3& b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

Vec3 vecNormalize(const Vec3& v) {
    const float len = v.length();
    if (len < 1e-4f)
        return {};
    return v * (1.f / len);
}

Vec3 rotateVectorToDir(const Vec3& v, const Vec3& bindDir, const Vec3& gameDir) {
    const Vec3 b = vecNormalize(bindDir);
    const Vec3 g = vecNormalize(gameDir);
    if (b.lengthSq() < 1e-6f || g.lengthSq() < 1e-6f)
        return v;

    const float dot = b.x * g.x + b.y * g.y + b.z * g.z;
    const Vec3 axis = vecCross(b, g);
    const float axisLen = axis.length();
    if (axisLen < 1e-4f)
        return dot < 0.f ? v * -1.f : v;

    const Vec3 k = axis * (1.f / axisLen);
    const float angle = std::acosf((std::clamp)(dot, -1.f, 1.f));
    const float cosA = std::cosf(angle);
    const float sinA = std::sinf(angle);
    const Vec3 kv = vecCross(k, v);
    const float kd = k.x * v.x + k.y * v.y + k.z * v.z;
    return v * cosA + kv * sinA + k * (kd * (1.f - cosA));
}

int parentSlotForRotation(int slot) {
    switch (slot) {
    case 2: return 0;
    case 4: return 2;
    case 5: return 4;
    case 6: return 5;
    case 9: return 8;
    case 11: return 9;
    case 14: return 13;
    case 16: return 14;
    case 23: return 22;
    case 24: return 23;
    case 26: return 25;
    case 27: return 26;
    default: return -1;
    }
}

Vec3 lerp3(const Vec3& a, const Vec3& b, float t) {
    return a + (b - a) * t;
}

void finalizeBindBounds(SkinnedMesh& mesh) {
    mesh.bindPelvis = mesh.vertices.empty() ? Vec3{} : mesh.vertices[0].bindPos;
    mesh.bindHead = mesh.bindPelvis;

    if (!mesh.bindJointPos.empty()) {
        mesh.bindPelvis = mesh.bindJointPos[0];
        mesh.bindHead = mesh.bindJointPos[0];
        for (const Vec3& jp : mesh.bindJointPos) {
            if (jp.z < mesh.bindPelvis.z)
                mesh.bindPelvis = jp;
            if (jp.z > mesh.bindHead.z)
                mesh.bindHead = jp;
        }
    }

    for (size_t ji = 0; ji < mesh.jointToBoneSlot.size(); ++ji) {
        if (mesh.jointToBoneSlot[ji] == 6)
            mesh.bindHead = mesh.bindJointPos[ji];
        if (mesh.jointToBoneSlot[ji] == 0)
            mesh.bindPelvis = mesh.bindJointPos[ji];
    }

    mesh.bindHeight = (mesh.bindHead - mesh.bindPelvis).length();
    if (mesh.bindHeight < 1.f)
        mesh.bindHeight = 72.f;
}

bool appendSkinnedPrimitive(const cgltf_primitive* prim,
                            const cgltf_skin* skin,
                            SkinnedMesh& out,
                            size_t& vertexBase,
                            size_t maxIndices) {
    if (!prim || !skin || !prim->indices || prim->has_draco_mesh_compression)
        return false;
    if (out.indices.size() >= maxIndices)
        return false;

    const cgltf_accessor* posAcc = nullptr;
    const cgltf_accessor* jointsAcc = nullptr;
    const cgltf_accessor* weightsAcc = nullptr;
    for (cgltf_size ai = 0; ai < prim->attributes_count; ++ai) {
        const cgltf_attribute& attr = prim->attributes[ai];
        if (attr.type == cgltf_attribute_type_position)
            posAcc = attr.data;
        else if (attr.type == cgltf_attribute_type_joints)
            jointsAcc = attr.data;
        else if (attr.type == cgltf_attribute_type_weights)
            weightsAcc = attr.data;
    }
    if (!posAcc || !jointsAcc || !weightsAcc)
        return false;

    std::vector<float> positions(posAcc->count * 3);
    if (cgltf_accessor_unpack_floats(posAcc, positions.data(), positions.size()) != positions.size())
        return false;

    std::vector<float> weights(weightsAcc->count * cgltf_num_components(weightsAcc->type));
    if (cgltf_accessor_unpack_floats(weightsAcc, weights.data(), weights.size()) != weights.size())
        return false;

    const int jointComps = (int)cgltf_num_components(jointsAcc->type);
    std::vector<std::uint16_t> jointFlat(jointsAcc->count * (size_t)jointComps);
    for (cgltf_size i = 0; i < jointsAcc->count; ++i) {
        std::uint32_t tmp[4]{};
        if (!cgltf_accessor_read_uint(jointsAcc, i, tmp, (cgltf_size)jointComps))
            return false;
        for (int c = 0; c < jointComps; ++c)
            jointFlat[i * (size_t)jointComps + (size_t)c] = (std::uint16_t)tmp[c];
    }

    const int weightComps = (int)cgltf_num_components(weightsAcc->type);
    vertexBase = out.vertices.size();
    out.vertices.resize(vertexBase + (size_t)posAcc->count);

    for (cgltf_size vi = 0; vi < posAcc->count; ++vi) {
        auto& v = out.vertices[vertexBase + (size_t)vi];
        v.bindPos = {
            positions[vi * 3 + 0],
            positions[vi * 3 + 1],
            positions[vi * 3 + 2],
        };
        float wsum = 0.f;
        for (int c = 0; c < 4; ++c) {
            const int jointIdx = (c < jointComps) ? (int)jointFlat[vi * (size_t)jointComps + (size_t)c] : 0;
            const float w = (c < weightComps) ? weights[vi * (size_t)weightComps + (size_t)c] : 0.f;
            v.joints[(size_t)c] = (std::uint16_t)std::clamp(jointIdx, 0, (int)skin->joints_count - 1);
            v.weights[(size_t)c] = w;
            wsum += w;
        }
        if (wsum > 0.0001f) {
            for (float& w : v.weights)
                w /= wsum;
        } else {
            v.weights = { 1.f, 0.f, 0.f, 0.f };
        }
    }

    const size_t room = maxIndices - out.indices.size();
    const size_t toAdd = (std::min)(room, (size_t)prim->indices->count);
    const size_t baseIdx = out.indices.size();
    out.indices.resize(baseIdx + toAdd);
    for (size_t i = 0; i < toAdd; ++i) {
        std::uint32_t idx = 0;
        if (cgltf_accessor_read_uint(prim->indices, i, &idx, 1) != 1)
            return false;
        out.indices[baseIdx + i] = (std::uint32_t)vertexBase + idx;
    }

    vertexBase = out.vertices.size();
    return toAdd >= 3;
}

bool appendRigidPrimitive(const cgltf_primitive* prim,
                          SkinnedMesh& out,
                          size_t& vertexBase,
                          size_t maxIndices) {
    if (!prim || !prim->indices || prim->has_draco_mesh_compression)
        return false;
    if (out.indices.size() >= maxIndices)
        return false;

    const cgltf_accessor* posAcc = nullptr;
    for (cgltf_size ai = 0; ai < prim->attributes_count; ++ai) {
        if (prim->attributes[ai].type == cgltf_attribute_type_position)
            posAcc = prim->attributes[ai].data;
    }
    if (!posAcc)
        return false;

    std::vector<float> positions(posAcc->count * 3);
    if (cgltf_accessor_unpack_floats(posAcc, positions.data(), positions.size()) != positions.size())
        return false;

    vertexBase = out.vertices.size();
    out.vertices.resize(vertexBase + (size_t)posAcc->count);
    for (cgltf_size vi = 0; vi < posAcc->count; ++vi) {
        auto& v = out.vertices[vertexBase + (size_t)vi];
        v.bindPos = {
            positions[vi * 3 + 0],
            positions[vi * 3 + 1],
            positions[vi * 3 + 2],
        };
        v.weights = { 1.f, 0.f, 0.f, 0.f };
    }

    const size_t room = maxIndices - out.indices.size();
    const size_t toAdd = (std::min)(room, (size_t)prim->indices->count);
    const size_t baseIdx = out.indices.size();
    out.indices.resize(baseIdx + toAdd);
    for (size_t i = 0; i < toAdd; ++i) {
        std::uint32_t idx = 0;
        if (cgltf_accessor_read_uint(prim->indices, i, &idx, 1) != 1)
            return false;
        out.indices[baseIdx + i] = (std::uint32_t)vertexBase + idx;
    }

    vertexBase = out.vertices.size();
    return toAdd >= 3;
}

std::filesystem::path findMeshesDirectory() {
    std::vector<std::filesystem::path> candidates;

    wchar_t cwd[MAX_PATH]{};
    if (GetCurrentDirectoryW(MAX_PATH, cwd) > 0) {
        std::filesystem::path cwdPath = cwd;
        candidates.push_back(cwdPath / "meshes");
        candidates.push_back(cwdPath.parent_path() / "meshes");
    }

    wchar_t exePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        std::filesystem::path dir = std::filesystem::path(exePath).parent_path();
        for (int depth = 0; depth < 8 && !dir.empty(); ++depth) {
            candidates.push_back(dir / "meshes");
            dir = dir.parent_path();
        }
    }

    candidates.emplace_back("meshes");
    candidates.emplace_back("../meshes");
    candidates.emplace_back("../../meshes");
    candidates.emplace_back("../../../meshes");

    for (const auto& p : candidates) {
        if (p.empty())
            continue;
        std::error_code ec;
        if (std::filesystem::is_directory(p, ec))
            return std::filesystem::absolute(p, ec);
    }
    return {};
}

std::filesystem::path findMeshPath(const char* fileName) {
    if (const auto direct = findMeshesDirectory(); !direct.empty()) {
        const auto full = direct / fileName;
        std::error_code ec;
        if (std::filesystem::exists(full, ec))
            return full;
    }

    std::vector<std::filesystem::path> candidates;
    candidates.emplace_back(std::filesystem::path("meshes") / fileName);
    candidates.emplace_back(std::filesystem::path("../meshes") / fileName);
    candidates.emplace_back(std::filesystem::path("../../meshes") / fileName);
    candidates.emplace_back(fileName);

    wchar_t exePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        const std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
        candidates.emplace_back(exeDir / "meshes" / fileName);
        candidates.emplace_back(exeDir.parent_path() / "meshes" / fileName);
    }

    for (const auto& p : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(p, ec))
            return p;
    }
    return {};
}

std::filesystem::path findMeshByPrefix(const char* prefix) {
    const auto dir = findMeshesDirectory();
    if (dir.empty())
        return {};

    std::filesystem::path best;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file())
            continue;
        if (!isGlbFile(entry.path()))
            continue;
        const std::string stem = toLower(entry.path().stem().string());
        const std::string want = toLower(prefix);
        if (stem.rfind(want, 0) == 0 && (best.empty() || entry.path().filename().string() < best.filename().string()))
            best = entry.path();
    }
    return best;
}

bool readAccessorFloats(const cgltf_accessor* acc, std::vector<float>& out) {
    if (!acc || acc->component_type != cgltf_component_type_r_32f)
        return false;
    out.resize(acc->count * cgltf_num_components(acc->type));
    return cgltf_accessor_unpack_floats(acc, out.data(), out.size()) == out.size();
}

bool readAccessorU16(const cgltf_accessor* acc, std::vector<std::uint16_t>& out) {
    if (!acc)
        return false;
    out.resize(acc->count);
    for (cgltf_size i = 0; i < acc->count; ++i) {
        std::uint32_t v = 0;
        if (cgltf_accessor_read_uint(acc, i, &v, 1) != 1)
            return false;
        out[i] = (std::uint16_t)v;
    }
    return true;
}

} // namespace

SkinnedMesh g_tChamsMesh;
SkinnedMesh g_ctChamsMesh;

namespace {

bool hasDracoCompression(const cgltf_data* data) {
    if (!data)
        return false;
    for (cgltf_size i = 0; i < data->extensions_required_count; ++i) {
        if (data->extensions_required[i] &&
            std::strcmp(data->extensions_required[i], "KHR_draco_mesh_compression") == 0)
            return true;
    }
    for (cgltf_size mi = 0; mi < data->meshes_count; ++mi) {
        const cgltf_mesh* mesh = &data->meshes[mi];
        for (cgltf_size pi = 0; pi < mesh->primitives_count; ++pi) {
            const cgltf_primitive* prim = &mesh->primitives[pi];
            if (prim->has_draco_mesh_compression)
                return true;
        }
    }
    return false;
}

bool loadGltfBuffers(cgltf_options& options, cgltf_data* data, const std::filesystem::path& path) {
    if (cgltf_load_buffers(&options, data, nullptr) == cgltf_result_success)
        return true;
    const std::string utf8Path = pathUtf8(path);
    return cgltf_load_buffers(&options, data, utf8Path.c_str()) == cgltf_result_success;
}

bool pickSkinnedPrimitive(cgltf_data* data, const cgltf_skin** outSkin, const cgltf_primitive** outPrim) {
    if (!data)
        return false;

    for (cgltf_size mi = 0; mi < data->meshes_count; ++mi) {
        const cgltf_mesh* mesh = &data->meshes[mi];
        for (cgltf_size pi = 0; pi < mesh->primitives_count; ++pi) {
            const cgltf_primitive* prim = &mesh->primitives[pi];
            if (!prim->indices)
                continue;

            const cgltf_accessor* posAcc = nullptr;
            const cgltf_accessor* jointsAcc = nullptr;
            const cgltf_accessor* weightsAcc = nullptr;
            for (cgltf_size ai = 0; ai < prim->attributes_count; ++ai) {
                const cgltf_attribute& attr = prim->attributes[ai];
                if (attr.type == cgltf_attribute_type_position)
                    posAcc = attr.data;
                else if (attr.type == cgltf_attribute_type_joints)
                    jointsAcc = attr.data;
                else if (attr.type == cgltf_attribute_type_weights)
                    weightsAcc = attr.data;
            }
            if (!posAcc || !jointsAcc || !weightsAcc)
                continue;
            if (prim->has_draco_mesh_compression)
                continue;

            const cgltf_skin* skin = (data->skins_count > 0) ? &data->skins[0] : nullptr;
            for (cgltf_size ni = 0; ni < data->nodes_count; ++ni) {
                const cgltf_node* node = &data->nodes[ni];
                if (node->mesh == mesh && node->skin) {
                    skin = node->skin;
                    break;
                }
            }
            if (!skin || skin->joints_count == 0)
                continue;

            *outSkin = skin;
            *outPrim = prim;
            return true;
        }
    }
    return false;
}

bool pickRigidPrimitive(cgltf_data* data, const cgltf_primitive** outPrim) {
    if (!data)
        return false;

    size_t bestVerts = 0;
    const cgltf_primitive* best = nullptr;
    for (cgltf_size mi = 0; mi < data->meshes_count; ++mi) {
        const cgltf_mesh* mesh = &data->meshes[mi];
        for (cgltf_size pi = 0; pi < mesh->primitives_count; ++pi) {
            const cgltf_primitive* prim = &mesh->primitives[pi];
            if (!prim->indices || prim->has_draco_mesh_compression)
                continue;
            const cgltf_accessor* posAcc = nullptr;
            for (cgltf_size ai = 0; ai < prim->attributes_count; ++ai) {
                if (prim->attributes[ai].type == cgltf_attribute_type_position)
                    posAcc = prim->attributes[ai].data;
            }
            if (!posAcc)
                continue;
            if (posAcc->count > bestVerts) {
                bestVerts = posAcc->count;
                best = prim;
            }
        }
    }
    if (!best)
        return false;
    *outPrim = best;
    return true;
}

bool loadSkinnedMesh(const std::filesystem::path& path, SkinnedMesh& out, std::string* errOut) {
    out = {};
    cgltf_data* data = nullptr;
    auto fail = [&](const char* msg) {
        if (data) {
            cgltf_free(data);
            data = nullptr;
        }
        if (errOut)
            *errOut = path.filename().string() + ": " + msg;
        return false;
    };

    std::vector<std::uint8_t> fileBytes;
    if (!readFileBytes(path, fileBytes))
        return fail("could not read file");

    cgltf_options options{};
    if (cgltf_parse(&options, fileBytes.data(), fileBytes.size(), &data) != cgltf_result_success || !data)
        return fail("glb parse failed");

    if (hasDracoCompression(data))
        return fail("uses Draco compression (unsupported)");

    if (!loadGltfBuffers(options, data, path))
        return fail("glb buffers failed");

    constexpr size_t kMaxIndices = 48000;

    auto primitiveIndexCount = [](const cgltf_primitive* prim) -> size_t {
        return (prim && prim->indices) ? (size_t)prim->indices->count : 0;
    };

    const cgltf_mesh* bodyMesh = findThirdPersonBodyMesh(data);
    const cgltf_skin* skin = nullptr;
    size_t vertexBase = 0;
    int partsAdded = 0;

    if (bodyMesh && (skin = findSkinForMesh(data, bodyMesh)) != nullptr) {
        setupSkinBindData(skin, out);
        std::vector<cgltf_size> primOrder(bodyMesh->primitives_count);
        for (cgltf_size pi = 0; pi < bodyMesh->primitives_count; ++pi)
            primOrder[pi] = pi;
        std::sort(primOrder.begin(), primOrder.end(), [&](cgltf_size a, cgltf_size b) {
            return primitiveIndexCount(&bodyMesh->primitives[a]) >
                   primitiveIndexCount(&bodyMesh->primitives[b]);
        });
        for (cgltf_size pi : primOrder) {
            if (appendSkinnedPrimitive(&bodyMesh->primitives[pi], skin, out, vertexBase, kMaxIndices))
                ++partsAdded;
        }
        if (partsAdded == 0)
            return fail("thirdperson_body had no valid submeshes");
    } else {
        const cgltf_primitive* prim = nullptr;
        if (pickSkinnedPrimitive(data, &skin, &prim)) {
            setupSkinBindData(skin, out);
            if (appendSkinnedPrimitive(prim, skin, out, vertexBase, kMaxIndices))
                partsAdded = 1;
        } else if (pickRigidPrimitive(data, &prim)) {
            out.rigidMesh = true;
            if (appendRigidPrimitive(prim, out, vertexBase, kMaxIndices))
                partsAdded = 1;
        }
        if (partsAdded == 0)
            return fail("no mesh primitives found");
    }

    if (!buildRawHammerBindPose(out))
        return fail("bind pose build failed");

    computeBindJointsFromWeights(out);
    finalizeSkeletonMapping(out);

    if (out.indices.size() % 3 != 0)
        out.indices.resize(out.indices.size() - (out.indices.size() % 3));

    out.loaded = !out.vertices.empty() && out.indices.size() >= 3;
    cgltf_free(data);
    data = nullptr;
    if (!out.loaded)
        return fail("mesh had no triangles");
    return true;
}

bool loadSkinnedMesh(const std::filesystem::path& path, SkinnedMesh& out) {
    return loadSkinnedMesh(path, out, nullptr);
}

bool projectBone(const ViewMatrix& vm, const Vec3& bone, float sw, float sh, Vec2& out) {
    if (!vm.worldToScreen(bone, out, sw, sh))
        return false;
    return std::isfinite(out.x) && std::isfinite(out.y);
}

const SkinnedMesh* meshForTeam(int teamNum) {
    if (teamNum == 3 && g_ctChamsMesh.loaded)
        return &g_ctChamsMesh;
    if (g_tChamsMesh.loaded)
        return &g_tChamsMesh;
    if (g_ctChamsMesh.loaded)
        return &g_ctChamsMesh;
    return nullptr;
}

} // namespace

void ChamsMeshLibrary::initOnce() {
    if (m_inited)
        return;
    m_inited = true;

    g_tChamsMesh = {};
    g_ctChamsMesh = {};

    const auto meshDir = findMeshesDirectory();
    if (!meshDir.empty())
        m_searchHint = meshDir.string();
    else
        m_searchHint = "meshes/ next to the executable";

    auto tPath = findMeshPath("tm_phoenix.glb");
    if (tPath.empty())
        tPath = findMeshByPrefix("tm_");
    if (tPath.empty() && !meshDir.empty()) {
        for (const auto& entry : std::filesystem::directory_iterator(meshDir)) {
            if (!entry.is_regular_file() || !isGlbFile(entry.path()))
                continue;
            tPath = entry.path();
            break;
        }
    }

    auto ctPath = findMeshPath("ctm_sas.glb");
    if (ctPath.empty())
        ctPath = findMeshByPrefix("ctm_");
    if (!tPath.empty() && tPath == ctPath)
        ctPath.clear();
    if (ctPath.empty() && !meshDir.empty()) {
        for (const auto& entry : std::filesystem::directory_iterator(meshDir)) {
            if (!entry.is_regular_file() || !isGlbFile(entry.path()))
                continue;
            if (entry.path() != tPath) {
                ctPath = entry.path();
                break;
            }
        }
    }

    int glbCount = 0;
    if (!meshDir.empty()) {
        for (const auto& entry : std::filesystem::directory_iterator(meshDir)) {
            if (entry.is_regular_file() && isGlbFile(entry.path()))
                ++glbCount;
        }
    }

    std::string tErr;
    std::string ctErr;

    auto logMeshStats = [](const char* label, const SkinnedMesh& mesh) {
        float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f, minZ = 1e30f, maxZ = -1e30f;
        for (const Vec3& p : mesh.posedHammer) {
            minX = (std::min)(minX, p.x);
            maxX = (std::max)(maxX, p.x);
            minY = (std::min)(minY, p.y);
            maxY = (std::max)(maxY, p.y);
            minZ = (std::min)(minZ, p.z);
            maxZ = (std::max)(maxZ, p.z);
        }
        int mapped = 0;
        for (int slot : mesh.jointToBoneSlot) {
            if (slot >= 0)
                ++mapped;
        }
        std::cout << "[ChamsMesh] " << label << " verts=" << mesh.vertices.size()
                  << " tris=" << mesh.indices.size() / 3
                  << " height=" << mesh.bindHeight
                  << " mappedJoints=" << mapped << "/" << mesh.jointToBoneSlot.size()
                  << " bounds Z=[" << minZ << "," << maxZ << "]\n";
    };

    if (!tPath.empty() && loadSkinnedMesh(tPath, g_tChamsMesh, &tErr)) {
        logMeshStats("T model", g_tChamsMesh);
        m_hasMesh = true;
    }
    if (!ctPath.empty() && loadSkinnedMesh(ctPath, g_ctChamsMesh, &ctErr)) {
        logMeshStats("CT model", g_ctChamsMesh);
        m_hasMesh = true;
    }

    if (m_hasMesh) {
        m_statusMessage = "Mesh models loaded.";
    } else {
        m_statusMessage = std::to_string(glbCount) + " .glb in " + m_searchHint;
        if (!tErr.empty())
            m_statusMessage += " | " + tErr;
        if (!ctErr.empty())
            m_statusMessage += " | " + ctErr;
        if (glbCount == 0)
            m_statusMessage = "No .glb files in " + m_searchHint;
        std::cout << "[ChamsMesh] " << m_statusMessage << '\n';
    }
}

bool ChamsMeshLibrary::chamsBoneVisible(const PlayerData& player, int boneIndex) {
    if (boneIndex < 0 || boneIndex >= PlayerData::kBoneCount)
        return !g_cfg.visibilityCheckEnabled || !player.visibilityChecked || player.isVisible;
    if (!player.chamsPartVisChecked)
        return !g_cfg.visibilityCheckEnabled || !player.visibilityChecked || player.isVisible;
    return player.chamsPartVisible[boneIndex];
}

static unsigned int scaleMeshArgbAlpha(unsigned int col, float mul) {
    const unsigned int a = static_cast<unsigned int>(
        (std::min)(255.f, static_cast<float>((col >> 24) & 0xFF) * mul));
    return (col & 0x00FFFFFFu) | (a << 24);
}

static uint64_t silhouetteEdgeKey(int x0, int y0, int x1, int y1) {
    if (x0 > x1 || (x0 == x1 && y0 > y1)) {
        std::swap(x0, x1);
        std::swap(y0, y1);
    }
    return (static_cast<uint64_t>(static_cast<uint32_t>(x0)) << 48)
         | (static_cast<uint64_t>(static_cast<uint32_t>(y0)) << 32)
         | (static_cast<uint64_t>(static_cast<uint32_t>(x1)) << 16)
         | static_cast<uint64_t>(static_cast<uint32_t>(y1));
}

static void decodeSilhouetteEdgeKey(uint64_t key, int& x0, int& y0, int& x1, int& y1) {
    x0 = static_cast<int>(static_cast<uint16_t>((key >> 48) & 0xFFFF));
    y0 = static_cast<int>(static_cast<uint16_t>((key >> 32) & 0xFFFF));
    x1 = static_cast<int>(static_cast<uint16_t>((key >> 16) & 0xFFFF));
    y1 = static_cast<int>(static_cast<uint16_t>(key & 0xFFFF));
}

static bool drawPlayerImpl(Renderer& r,
                           const PlayerData& player,
                           const ViewMatrix& vm,
                           unsigned int visCol,
                           unsigned int occCol,
                           bool drawVisible,
                           bool drawOccluded,
                           float sw,
                           float sh,
                           const Vec3& predDelta,
                           bool silhouette2d);

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
    return drawPlayerImpl(r, player, vm, visCol, occCol, drawVisible, drawOccluded,
                          sw, sh, predDelta, false);
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
    return drawPlayerImpl(r, player, vm, visCol, occCol, drawVisible, drawOccluded,
                          sw, sh, predDelta, true);
}

static bool drawPlayerImpl(Renderer& r,
                                  const PlayerData& player,
                                  const ViewMatrix& vm,
                                  unsigned int visCol,
                                  unsigned int occCol,
                                  bool drawVisible,
                                  bool drawOccluded,
                                  float sw,
                                  float sh,
                                  const Vec3& predDelta,
                                  bool silhouette2d) {
    const SkinnedMesh* mesh = meshForTeam(player.teamNum);
    if (!mesh || !player.bonesValid)
        return false;

    const Vec3 gamePelvis = player.bones[0] + predDelta;
    const Vec3 gameHead = player.bones[6] + predDelta;
    const float gameHeight = (gameHead - gamePelvis).length();
    if (gameHeight < 8.f)
        return false;

    Vec3 gameFeet = player.origin + predDelta;
    if (std::isfinite(player.bones[24].x) && std::isfinite(player.bones[27].x)) {
        gameFeet = (player.bones[24] + player.bones[27]) * 0.5f + predDelta;
    }

    const float gameFullHeight = (std::max)(gameHead.z - gameFeet.z, gameHeight);
    const float bodyScale = gameFullHeight / mesh->bindFullHeight;
    const float torsoScale = gameHeight / mesh->bindHeight;

    thread_local std::vector<Vec3> skinned;
    skinned.resize(mesh->vertices.size());

    const bool haveBindPose = mesh->posedHammer.size() == mesh->vertices.size() &&
                              !mesh->bindJointPos.empty();
    const float maxPlanarSq = (gameHeight * 0.65f) * (gameHeight * 0.65f);
    const float footDrop = 12.f;
    const float headRise = gameHeight * 0.22f;

    auto bindSlot = [&](int slot) -> Vec3 {
        if (slot >= 0 && slot < PlayerData::kBoneCount)
            return mesh->bindSlotPos[(size_t)slot];
        return mesh->bindPelvis;
    };

    auto gameSlot = [&](int slot) -> Vec3 {
        return player.bones[slot] + predDelta;
    };

    auto slotOk = [&](int slot) -> bool {
        return slot >= 0 && slot < PlayerData::kBoneCount &&
               std::isfinite(player.bones[slot].x);
    };

    const float maxBoneOffset = (std::max)(gameHeight * 0.22f, 12.f);

    // Align the bind mesh's forward axis to the player's live facing direction.
    // Without this the silhouette is placed at a fixed world yaw and appears
    // edge-on / collapsed when the player faces a different way. Derive the
    // lateral (left→right) axis from the shoulder bones, fall back to the hips.
    float cz = 1.f;
    float sz = 0.f;
    {
        Vec3 bindLat{};
        Vec3 gameLat{};
        if (slotOk(8) && slotOk(13)) {
            bindLat = bindSlot(13) - bindSlot(8);
            gameLat = gameSlot(13) - gameSlot(8);
        }
        if (gameLat.x * gameLat.x + gameLat.y * gameLat.y < 4.f &&
            slotOk(22) && slotOk(25)) {
            bindLat = bindSlot(25) - bindSlot(22);
            gameLat = gameSlot(25) - gameSlot(22);
        }
        const float bindLenSq = bindLat.x * bindLat.x + bindLat.y * bindLat.y;
        const float gameLenSq = gameLat.x * gameLat.x + gameLat.y * gameLat.y;
        if (bindLenSq > 1e-3f && gameLenSq > 1e-3f) {
            const float bindAng = std::atan2f(bindLat.y, bindLat.x);
            const float gameAng = std::atan2f(gameLat.y, gameLat.x);
            const float dz = gameAng - bindAng;
            cz = std::cosf(dz);
            sz = std::sinf(dz);
        }
    }

    auto rotXY = [&](const Vec3& v) -> Vec3 {
        return { v.x * cz - v.y * sz, v.x * sz + v.y * cz, v.z };
    };

    // Map a vertex bone slot to the limb segment (startSlot → endSlot) whose
    // direction should orient that vertex. Torso/head slots return {-1,-1}.
    auto limbSegment = [](int slot, int& a, int& b) -> bool {
        switch (slot) {
        case 8:  a = 8;  b = 9;  return true;   // L upper arm
        case 9:  a = 9;  b = 11; return true;   // L forearm
        case 11: a = 9;  b = 11; return true;   // L hand
        case 13: a = 13; b = 14; return true;   // R upper arm
        case 14: a = 14; b = 16; return true;   // R forearm
        case 16: a = 14; b = 16; return true;   // R hand
        case 22: a = 22; b = 23; return true;   // L thigh
        case 23: a = 23; b = 24; return true;   // L shin
        case 24: a = 23; b = 24; return true;   // L foot
        case 25: a = 25; b = 26; return true;   // R thigh
        case 26: a = 26; b = 27; return true;   // R shin
        case 27: a = 26; b = 27; return true;   // R foot
        default: a = -1; b = -1; return false;
        }
    };

    for (size_t vi = 0; vi < mesh->vertices.size(); ++vi) {
        const Vec3& bindVert = haveBindPose ? mesh->posedHammer[vi] : mesh->vertices[vi].bindPos;
        const Vec3 rigid =
            gamePelvis + rotXY((bindVert - mesh->bindPelvis) * torsoScale);

        if (!haveBindPose) {
            skinned[vi] = gameFeet + rotXY((bindVert - mesh->bindFoot) * bodyScale);
            continue;
        }

        int slot = 0;
        if (vi < mesh->vertBoneSlot.size())
            slot = mesh->vertBoneSlot[vi];
        if (slot < 0 || slot >= PlayerData::kBoneCount || !slotOk(slot))
            slot = 0;

        int segA = -1;
        int segB = -1;
        if (limbSegment(slot, segA, segB) && slotOk(segA) && slotOk(segB)) {
            // Orient the vertex by the live limb-segment direction so arms and
            // legs follow the pose instead of staying in the bind orientation.
            const Vec3 bindDir = bindSlot(segB) - bindSlot(segA);
            const Vec3 gameDir = gameSlot(segB) - gameSlot(segA);
            Vec3 offset = bindVert - bindSlot(segA);
            // Cap the radial reach so a bad weight can't fling a vertex away.
            const float limbCap = (bindDir.length() + 8.f);
            if (offset.lengthSq() > limbCap * limbCap && offset.lengthSq() > 1e-6f)
                offset = offset * (limbCap / std::sqrtf(offset.lengthSq()));
            const Vec3 posed = gameSlot(segA) + rotateVectorToDir(offset, bindDir, gameDir);
            constexpr float kLimbFollow = 0.85f;
            const Vec3 blended = rigid + (posed - rigid) * kLimbFollow;
            skinned[vi] = clampDisplacement(blended, rigid, maxBoneOffset * 1.6f);
        } else {
            const Vec3 posed = gameSlot(slot) + rotXY(bindVert - bindSlot(slot));
            constexpr float kBoneFollow = 0.55f;
            const Vec3 blended = rigid + (posed - rigid) * kBoneFollow;
            skinned[vi] = clampDisplacement(blended, rigid, maxBoneOffset);
        }
    }

    float screenBodyH = gameHeight;
    Vec2 pelvisSc{}, headSc{};
    if (projectBone(vm, gamePelvis, sw, sh, pelvisSc) && projectBone(vm, gameHead, sw, sh, headSc))
        screenBodyH = std::fabsf(headSc.y - pelvisSc.y);

    const bool playerVis = !g_cfg.visibilityCheckEnabled || !player.visibilityChecked || player.isVisible;
    constexpr size_t kDrawTriStep = 1;
    constexpr float kMaxTriEdge = 40.f;
    constexpr float kMaxTriEdgeSq = kMaxTriEdge * kMaxTriEdge;
    const float maxScreenEdge = (std::max)(screenBodyH * 1.25f, 40.f);
    const float maxScreenEdgeSq = maxScreenEdge * maxScreenEdge;

    auto drawTri = [&](const Vec2& p0, const Vec2& p1, const Vec2& p2, unsigned int col) {
        if (((col >> 24) & 0xFF) == 0)
            return;
        const float tri[] = { p0.x, p0.y, p1.x, p1.y, p2.x, p2.y };
        r.drawFilledConvexPolygon(tri, 3, col);
    };

    struct SilhouetteTri {
        Vec2 p[3];
        unsigned int col = 0;
    };
    thread_local std::vector<SilhouetteTri> silhouetteTris;
    thread_local std::vector<Vec2> silhouettePts;
    if (silhouette2d) {
        silhouetteTris.clear();
        silhouettePts.clear();
    }

    auto triangleVis = [&]() -> bool {
        return playerVis;
    };

    int trianglesDrawn = 0;

    for (size_t ti = 0; ti + 2 < mesh->indices.size(); ti += 3 * kDrawTriStep) {
        const Vec3& a = skinned[mesh->indices[ti + 0]];
        const Vec3& b = skinned[mesh->indices[ti + 1]];
        const Vec3& c = skinned[mesh->indices[ti + 2]];

        const auto edgeLenSq = [](const Vec3& p, const Vec3& q) {
            const float dx = p.x - q.x;
            const float dy = p.y - q.y;
            const float dz = p.z - q.z;
            return dx * dx + dy * dy + dz * dz;
        };
        if (edgeLenSq(a, b) > kMaxTriEdgeSq ||
            edgeLenSq(b, c) > kMaxTriEdgeSq ||
            edgeLenSq(c, a) > kMaxTriEdgeSq)
            continue;

        const Vec3 triCenter = (a + b + c) * (1.f / 3.f);
        const float planarDx = triCenter.x - gamePelvis.x;
        const float planarDy = triCenter.y - gamePelvis.y;
        if (planarDx * planarDx + planarDy * planarDy > maxPlanarSq)
            continue;
        if (triCenter.z < gameFeet.z - footDrop ||
            triCenter.z > gameHead.z + headRise)
            continue;

        Vec2 sa{}, sb{}, sc{};
        if (!projectBone(vm, a, sw, sh, sa) ||
            !projectBone(vm, b, sw, sh, sb) ||
            !projectBone(vm, c, sw, sh, sc))
            continue;

        const auto screenEdgeLenSq = [](const Vec2& p, const Vec2& q) {
            const float dx = p.x - q.x;
            const float dy = p.y - q.y;
            return dx * dx + dy * dy;
        };
        if (screenEdgeLenSq(sa, sb) > maxScreenEdgeSq ||
            screenEdgeLenSq(sb, sc) > maxScreenEdgeSq ||
            screenEdgeLenSq(sc, sa) > maxScreenEdgeSq)
            continue;

        const float area = std::fabsf((sb.x - sa.x) * (sc.y - sa.y) - (sc.x - sa.x) * (sb.y - sa.y));
        if (area < 2.f)
            continue;

        const bool vis = triangleVis();
        unsigned int col = 0;
        if (vis && drawVisible)
            col = visCol;
        else if (!vis && drawOccluded)
            col = occCol;

        if (silhouette2d) {
            if (!col)
                continue;
            SilhouetteTri tri{};
            tri.p[0] = sa;
            tri.p[1] = sb;
            tri.p[2] = sc;
            tri.col = scaleMeshArgbAlpha(col, 0.90f);
            silhouetteTris.push_back(tri);
            silhouettePts.push_back(sa);
            silhouettePts.push_back(sb);
            silhouettePts.push_back(sc);
            ++trianglesDrawn;
            continue;
        }

        if (vis && drawVisible) {
            drawTri(sa, sb, sc, visCol);
            ++trianglesDrawn;
        } else if (!vis && drawOccluded) {
            drawTri(sa, sb, sc, occCol);
            ++trianglesDrawn;
        }
    }

    if (silhouette2d) {
        if (trianglesDrawn < 40)
            return false;

        for (const SilhouetteTri& tri : silhouetteTris)
            drawTri(tri.p[0], tri.p[1], tri.p[2], tri.col);

        std::sort(silhouettePts.begin(), silhouettePts.end(), [](const Vec2& a, const Vec2& b) {
            if (std::fabsf(a.x - b.x) > 0.5f)
                return a.x < b.x;
            return a.y < b.y;
        });
        silhouettePts.erase(std::unique(silhouettePts.begin(), silhouettePts.end(), [](const Vec2& a, const Vec2& b) {
            return std::fabsf(a.x - b.x) <= 0.5f && std::fabsf(a.y - b.y) <= 0.5f;
        }), silhouettePts.end());

        if (silhouettePts.size() >= 3) {
            auto cross = [](const Vec2& o, const Vec2& a, const Vec2& b) {
                return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
            };

            thread_local std::vector<Vec2> hull;
            hull.clear();
            hull.reserve(silhouettePts.size() * 2);
            for (const Vec2& p : silhouettePts) {
                while (hull.size() >= 2 &&
                       cross(hull[hull.size() - 2], hull[hull.size() - 1], p) <= 0.f)
                    hull.pop_back();
                hull.push_back(p);
            }
            const size_t lowerCount = hull.size();
            for (int i = static_cast<int>(silhouettePts.size()) - 2; i >= 0; --i) {
                const Vec2& p = silhouettePts[static_cast<size_t>(i)];
                while (hull.size() > lowerCount &&
                       cross(hull[hull.size() - 2], hull[hull.size() - 1], p) <= 0.f)
                    hull.pop_back();
                hull.push_back(p);
            }
            if (hull.size() > 1)
                hull.pop_back();

            const unsigned int edgeCol = 0xDDFFFFFFu;
            for (size_t i = 0; i < hull.size(); ++i) {
                const Vec2& a = hull[i];
                const Vec2& b = hull[(i + 1) % hull.size()];
                r.drawLine(a.x, a.y, b.x, b.y, edgeCol, 1.6f);
            }
        }
        return true;
    }

    return trianglesDrawn >= 40;
}
