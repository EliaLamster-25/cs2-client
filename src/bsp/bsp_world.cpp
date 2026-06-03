#include "bsp/bsp_world.h"

#include <fstream>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <iostream>
#include <limits>
#include <vector>

// LZ4 decompression (single-file implementation pulled in here)
extern "C" {
#include "lz4.h"
}

// ZSTD decompression (for KV3 v5 compressionMethod=2)
#include "zstd.h"

// ─── BSP on-disk structs ─────────────────────────────────────────────────────
// These match the Valve Source-2 VBSP format used by CS2.
// Brushes / planes / brushsides use the same layout as Source-1 / CS:GO.
#pragma pack(push, 1)

struct DLump {
    int32_t fileofs;
    int32_t filelen;
    int32_t version;
    char    fourCC[4];
};

struct DBSPHeader {
    int32_t ident;        // 'VBSP' = 0x50534256
    int32_t version;      // CS2 = 29
    DLump   lumps[64];
    int32_t mapRevision;
};

struct DPlane {           // lump 1 — 20 bytes each
    float   normal[3];
    float   dist;
    int32_t type;
};

struct DBrush {           // lump 18 — 12 bytes each
    int32_t firstside;
    int32_t numsides;
    int32_t contents;
};

struct DBrushSide {       // lump 19 — 8 bytes each
    uint16_t planenum;
    int16_t  texinfo;
    int16_t  dispinfo;
    int16_t  bevel;
};

#pragma pack(pop)

static constexpr int     LUMP_PLANES     = 1;
static constexpr int     LUMP_BRUSHES    = 18;
static constexpr int     LUMP_BRUSHSIDES = 19;
static constexpr int32_t BSP_IDENT       = 0x50534256;  // 'VBSP'
static constexpr int32_t CONTENTS_SOLID  = 0x1;

// Small outward bias applied at sweep start to avoid re-entering the same
// surface immediately after a bounce.
static constexpr float SWEEP_BIAS = 0.125f;

// ─── Inline dot product ───────────────────────────────────────────────────────
static inline float vdot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

// ─── VPK v2 support ───────────────────────────────────────────────────────────
// CS2 retail ships maps as .vpk packages.  The BSP lives inside the VPK's
// embedded data section under ext="bsp", path="maps", filename=<mapName>.
#pragma pack(push, 1)
struct VpkHeader {
    uint32_t sig;                     // 0x55AA1234
    uint32_t version;                 // 2
    uint32_t treeSize;                // byte size of directory tree
    uint32_t fileDataSize;            // byte size of embedded data section
    uint32_t archiveMD5SectionSize;
    uint32_t otherMD5SectionSize;
    uint32_t signatureSectionSize;
};
struct VpkDirEntry {
    uint32_t crc32;
    uint16_t preloadBytes;
    uint16_t archiveIndex;  // 0x7FFF = embedded in same file
    uint32_t entryOffset;   // offset within embedded data section
    uint32_t entryLength;
    uint16_t terminator;    // 0xFFFF
};
#pragma pack(pop)
static constexpr uint32_t VPK_SIG      = 0x55AA1234u;
static constexpr uint16_t VPK_EMBEDDED = 0x7FFFu;

// Scans the VPK directory tree and returns the absolute file offset + length
// of the embedded BSP (ext="bsp", path="maps", fname=mapName).
static bool findBspInVpk(std::ifstream& f,
                         const std::string& mapName,
                         std::streamoff& bspOffset,
                         uint32_t& bspLength)
{
    f.seekg(0);
    VpkHeader hdr{};
    f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (!f) return false;
    if (hdr.sig != VPK_SIG || hdr.version != 2) return false;

    const std::streamoff treeStart = sizeof(VpkHeader);
    const std::streamoff dataStart = treeStart + (std::streamoff)hdr.treeSize;

    auto readStr = [&]() -> std::string {
        std::string s; char c;
        while (f.get(c) && c != '\0') s += c;
        return s;
    };

    f.seekg(treeStart);
    while (f.good()) {
        std::string ext = readStr();
        if (ext.empty()) break;
        const bool isBsp = (ext == "bsp");
        while (f.good()) {
            std::string path = readStr();
            if (path.empty()) break;
            while (f.good()) {
                std::string fname = readStr();
                if (fname.empty()) break;
                VpkDirEntry entry{};
                f.read(reinterpret_cast<char*>(&entry), sizeof(entry));
                bool match = (ext == "bsp" && path == "maps" && fname == mapName);
                if (entry.preloadBytes && !match)
                    f.seekg((std::streamoff)entry.preloadBytes, std::ios::cur);
                if (!match) continue;
                if (entry.archiveIndex != VPK_EMBEDDED) return false;
                bspOffset = dataStart + (std::streamoff)entry.entryOffset;
                bspLength = entry.entryLength;
                return bspLength > sizeof(DBSPHeader);
            }
        }
    }
    return false;
}

// ─── BspWorld::load ───────────────────────────────────────────────────────────
bool BspWorld::load(const std::string& mapName, const std::string& cs2Path) {
    clear();

    std::string vpkPath = cs2Path + "\\game\\csgo\\maps\\" + mapName + ".vpk";
    std::string bspPath = cs2Path + "\\game\\csgo\\maps\\" + mapName + ".bsp";

    std::ifstream f;
    std::streamoff bspBase = 0;

    // Try VPK first (retail CS2).
    {
        std::ifstream vpk(vpkPath, std::ios::binary);
        if (vpk.good()) {
            std::streamoff off{}; uint32_t len{};
            if (findBspInVpk(vpk, mapName, off, len)) {
                f = std::move(vpk);
                bspBase = off;
                std::cout << "[BspWorld] BSP found in VPK: " << vpkPath << "\n";
            }
        }
    }
    // Fall back to raw .bsp (mods / legacy).
    if (!f.is_open()) {
        f.open(bspPath, std::ios::binary);
        bspBase = 0;
    }
    if (!f) return false;

    f.seekg(bspBase);
    DBSPHeader hdr{};
    f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (!f || hdr.ident != BSP_IDENT) return false;
    std::cout << "[BspWorld] Loaded " << mapName
              << "  (VBSP v" << hdr.version << ")\n";

    // ── 1. Planes ────────────────────────────────────────────────────────────
    {
        const DLump& lump = hdr.lumps[LUMP_PLANES];
        const int count   = lump.filelen / (int)sizeof(DPlane);
        f.seekg(bspBase + (std::streamoff)lump.fileofs);
        m_planes.reserve(count);
        for (int i = 0; i < count; ++i) {
            DPlane dp{};
            f.read(reinterpret_cast<char*>(&dp), sizeof(dp));
            m_planes.push_back({ { dp.normal[0], dp.normal[1], dp.normal[2] },
                                  dp.dist });
        }
    }

    // ── 2. Brushsides (raw, mapped to planes below) ──────────────────────────
    std::vector<DBrushSide> rawSides;
    {
        const DLump& lump = hdr.lumps[LUMP_BRUSHSIDES];
        const int count   = lump.filelen / (int)sizeof(DBrushSide);
        rawSides.resize(count);
        f.seekg(bspBase + (std::streamoff)lump.fileofs);
        f.read(reinterpret_cast<char*>(rawSides.data()),
               count * (int)sizeof(DBrushSide));
    }

    // ── 3. Brushes — keep only CONTENTS_SOLID ────────────────────────────────
    {
        const DLump& lump = hdr.lumps[LUMP_BRUSHES];
        const int count   = lump.filelen / (int)sizeof(DBrush);
        f.seekg(bspBase + (std::streamoff)lump.fileofs);

        const int nPlanes = (int)m_planes.size();
        const int nSides  = (int)rawSides.size();

        for (int i = 0; i < count; ++i) {
            DBrush db{};
            f.read(reinterpret_cast<char*>(&db), sizeof(db));
            if (!(db.contents & CONTENTS_SOLID)) continue;
            if (db.numsides <= 0) continue;
            if (db.firstside < 0 || db.firstside >= nSides) continue;

            BspBrush b;
            b.firstSide = (int)m_sides.size();
            b.numSides  = 0;
            b.mnX = b.mnY = b.mnZ =  std::numeric_limits<float>::max();
            b.mxX = b.mxY = b.mxZ = -std::numeric_limits<float>::max();

            for (int s = 0; s < db.numsides; ++s) {
                const int rawIdx = db.firstside + s;
                if (rawIdx >= nSides) break;
                const int pIdx = (int)rawSides[rawIdx].planenum;
                if (pIdx < 0 || pIdx >= nPlanes) break;

                m_sides.push_back({ pIdx });
                ++b.numSides;

                // Expand conservative AABB.
                // For plane n·x ≤ d (outward normal), the brush is constrained to
                //   x ≤ d/n.x  (when n.x > threshold)
                //   x ≥ d/n.x  (when n.x < -threshold)
                // Threshold 0.15 avoids near-zero divisions that give huge bounds.
                const BspPlane& pl = m_planes[pIdx];
                constexpr float kThr = 0.15f;
                if (pl.n.x >  kThr) b.mxX = std::min(b.mxX, pl.d / pl.n.x);
                if (pl.n.x < -kThr) b.mnX = std::max(b.mnX, pl.d / pl.n.x);
                if (pl.n.y >  kThr) b.mxY = std::min(b.mxY, pl.d / pl.n.y);
                if (pl.n.y < -kThr) b.mnY = std::max(b.mnY, pl.d / pl.n.y);
                if (pl.n.z >  kThr) b.mxZ = std::min(b.mxZ, pl.d / pl.n.z);
                if (pl.n.z < -kThr) b.mnZ = std::max(b.mnZ, pl.d / pl.n.z);
            }

            if (b.numSides < 4) continue;    // degenerate — skip

            // Clamp infinite bounds (brush has no axis-aligned plane for this axis)
            constexpr float kWorld = 32768.f;
            if (b.mnX > b.mxX) { b.mnX = -kWorld; b.mxX = kWorld; }
            if (b.mnY > b.mxY) { b.mnY = -kWorld; b.mxY = kWorld; }
            if (b.mnZ > b.mxZ) { b.mnZ = -kWorld; b.mxZ = kWorld; }

            m_brushes.push_back(b);
        }
    }

    std::cout << "[BspWorld] " << m_brushes.size() << " solid brushes, "
              << m_planes.size() << " planes loaded\n";

    if (m_brushes.empty()) {
        // CS2 VBSP v29 ships no classic brush/plane lumps — all physics is in
        // vmdl_c PHYS blobs.  Return false so the caller falls through to
        // loadFromVpk() which extracts the actual collision hulls and mesh tris.
        std::cout << "[BspWorld] No solid brushes in VBSP — delegating to VPK physics loader\n";
        return false;
    }

    buildBrushGrid();
    m_mapName = mapName;
    m_loaded  = true;
    return true;
}

// ─── BspWorld::clear ─────────────────────────────────────────────────────────
void BspWorld::clear() {
    m_planes.clear();
    m_sides.clear();
    m_brushes.clear();
    m_tris.clear();
    m_triGrid.clear();
    m_brushGrid.clear();
    m_loaded  = false;
    m_mapName.clear();
}

// ─── BspWorld::buildTriGrid ───────────────────────────────────────────────────
// Rebuilds m_triGrid from whatever is currently in m_tris.
void BspWorld::buildTriGrid() {
    m_triGrid.clear();
    for (uint32_t ti = 0; ti < static_cast<uint32_t>(m_tris.size()); ++ti) {
        const MeshTri& tri = m_tris[ti];
        const float mnX = std::min({ tri.p[0].x, tri.p[1].x, tri.p[2].x });
        const float mxX = std::max({ tri.p[0].x, tri.p[1].x, tri.p[2].x });
        const float mnY = std::min({ tri.p[0].y, tri.p[1].y, tri.p[2].y });
        const float mxY = std::max({ tri.p[0].y, tri.p[1].y, tri.p[2].y });
        const int cx0 = static_cast<int>(std::floorf(mnX / kTriGridCell));
        const int cx1 = static_cast<int>(std::floorf(mxX / kTriGridCell));
        const int cy0 = static_cast<int>(std::floorf(mnY / kTriGridCell));
        const int cy1 = static_cast<int>(std::floorf(mxY / kTriGridCell));
        for (int cx = cx0; cx <= cx1; ++cx) {
            for (int cy = cy0; cy <= cy1; ++cy) {
                const int64_t key =
                    (static_cast<int64_t>(static_cast<int32_t>(cx)) << 32)
                    | static_cast<uint32_t>(static_cast<int32_t>(cy));
                m_triGrid[key].push_back(ti);
            }
        }
    }
    std::cout << "[MeshGrid] " << m_triGrid.size() << " cells for "
              << m_tris.size() << " triangles\n";
}

// ─── BspWorld::buildBrushGrid ────────────────────────────────────────────────
// Rebuilds m_brushGrid from whatever is currently in m_brushes.
// Uses AABB overlap (same approach as buildTriGrid) to insert each brush
// into every cell its bounding box touches. Cell size = 512 units.
void BspWorld::buildBrushGrid() {
    m_brushGrid.clear();
    for (uint32_t bi = 0; bi < static_cast<uint32_t>(m_brushes.size()); ++bi) {
        const BspBrush& br = m_brushes[bi];
        const int cx0 = static_cast<int>(std::floorf(br.mnX / kBrushGridCell));
        const int cx1 = static_cast<int>(std::floorf(br.mxX / kBrushGridCell));
        const int cy0 = static_cast<int>(std::floorf(br.mnY / kBrushGridCell));
        const int cy1 = static_cast<int>(std::floorf(br.mxY / kBrushGridCell));
        for (int cx = cx0; cx <= cx1; ++cx) {
            for (int cy = cy0; cy <= cy1; ++cy) {
                const int64_t key =
                    (static_cast<int64_t>(static_cast<int32_t>(cx)) << 32)
                    | static_cast<uint32_t>(static_cast<int32_t>(cy));
                m_brushGrid[key].push_back(bi);
            }
        }
    }
    std::cout << "[BrushGrid] " << m_brushGrid.size() << " cells for "
              << m_brushes.size() << " brushes\n";
}

// ─── BspWorld::loadTri ────────────────────────────────────────────────────────
// Loads pre-extracted solid collision triangles from a raw .tri binary file.
// Format: no header; N × 9 × float32 little-endian (v0.xyz, v1.xyz, v2.xyz).
bool BspWorld::loadTri(const std::string& triPath) {
    std::ifstream f(triPath, std::ios::binary | std::ios::ate);
    if (!f) return false;

    const std::streamsize fileSize = f.tellg();
    if (fileSize <= 0 || fileSize % 36 != 0) return false;

    f.seekg(0);
    const size_t triCount = static_cast<size_t>(fileSize) / 36;

    clear();
    m_tris.reserve(triCount);

    float v[9];
    for (size_t i = 0; i < triCount; ++i) {
        f.read(reinterpret_cast<char*>(v), 36);
        if (!f) break;

        // normal = normalize(cross(e1, e2))
        const float e1x = v[3]-v[0], e1y = v[4]-v[1], e1z = v[5]-v[2];
        const float e2x = v[6]-v[0], e2y = v[7]-v[1], e2z = v[8]-v[2];
        float nx = e1y*e2z - e1z*e2y;
        float ny = e1z*e2x - e1x*e2z;
        float nz = e1x*e2y - e1y*e2x;
        const float len = std::sqrtf(nx*nx + ny*ny + nz*nz);
        if (len < 1e-6f) continue;  // degenerate triangle – skip

        MeshTri tri;
        tri.p[0] = {v[0], v[1], v[2]};
        tri.p[1] = {v[3], v[4], v[5]};
        tri.p[2] = {v[6], v[7], v[8]};
        tri.n    = {nx/len, ny/len, nz/len};
        m_tris.push_back(tri);
    }

    if (m_tris.empty()) return false;

    std::cout << "[BspWorld] Loaded " << m_tris.size()
              << " triangles from " << triPath << "\n";

    buildTriGrid();
    // No brush grid needed — .tri files contain only triangle data.
    m_mapName = triPath;
    m_loaded  = true;
    return true;
}

// ─── Source-2 / CS2 physics loader (VPK → vmdl_c → PHYS → KV3 v5) ──────────
//
// CS2 retail maps are shipped as a VPK.  Each model's collision data lives in
// a .vmdl_c resource file inside the same VPK.  The PHYS block of a .vmdl_c
// is a KV3 v5 binary blob that encodes the physics aggregate (hull planes,
// mesh triangles, etc.) using a multi-stream, LZ4-compressed format.
//
// We only need the *binary blobs* section, which contains the raw RnPlane_t
// arrays.  We skip full KV3 tree parsing and instead scan the decompressed
// binary-blob bytes for 16-byte-aligned {nx, ny, nz, d} float tuples where
// |normal| ≈ 1.
//
// KV3 v5 file layout (all integers little-endian):
//   [0 ] magic       uint32  0x4B563305
//   [4 ] formatGuid  [16]    (ignored)
//   [20] comprMethod uint32  1 = LZ4
//   [24] dictId      uint16
//   [26] frameSize   uint16  16384
//   [28] cntBytes1   int32
//   [32] cntBytes4   int32
//   [36] cntBytes8   int32
//   [40] cntTypes    int32
//   [44] cntObjects  uint16
//   [46] cntArrays   uint16
//   [48] uncTotal    int32   total uncompressed (buffer1+buffer2)
//   [52] cmpTotal    int32
//   [56] cntBlocks   int32   number of binary-blob LZ4 frames
//   [60] blobBytes   int32   total uncompressed bytes of all binary blobs
//   -- v4+ (always present for v5):
//   [64] cntBytes2   int32
//   [68] blkCmpSzBytes int32 (informational only)
//   -- v5:
//   [72] uncBuf1     int32   uncompressed size of buffer1
//   [76] cmpBuf1     int32   compressed size of buffer1 (follows header)
//   [80] uncBuf2     int32   uncompressed size of buffer2
//   [84] cmpBuf2     int32   compressed size of buffer2
//   [88] cb1_buf2    int32
//   [92] cb2_buf2    int32
//   [96] cb4_buf2    int32
//   [100] cb8_buf2   int32
//   [104] unk13      int32
//   [108] cntObj2    int32
//   [112] cntArr2    int32
//   [116] unk16      int32
//   = 120 bytes total
//
// Immediately follows: cmpBuf1 bytes (LZ4 → uncBuf1 bytes)
//                      cmpBuf2 bytes (LZ4 → uncBuf2 bytes)
//                      binary-blob LZ4 frames  (uint16-sized frames read from
//                        decompressed buffer2 tail, see extractBlobFrameSizes)
//                      0xFFEEDD00 trailer uint32
//
// Decompressed buffer2 layout:
//   [0 .. cntObj2*4)         ObjectLengths
//   alignUp4 bytes1_buf2     Bytes1
//   alignUp4 bytes4_buf2*4   Bytes4
//   alignUp8 bytes8_buf2*8   Bytes8
//   cntTypes bytes            Types
//   cntBlocks*4              BinaryBlobLengths (uint32 each = uncompressed blob size)
//   4                        trailer 0xFFEEDD00
//   cntBlocks*2              uint16 per-frame compressed sizes

static constexpr uint32_t KV3_MAGIC5        = 0x4B563305u;
static constexpr uint32_t KV3_TRAILER       = 0xFFEEDD00u;
static constexpr uint32_t KV3_COMPR_LZ4     = 1u;
static constexpr uint32_t KV3_COMPR_ZSTD    = 2u;
static constexpr int      KV3_HEADER_SIZE   = 120;   // v5 fixed

#pragma pack(push,1)
struct Kv3v5Header {
    uint32_t magic;
    uint8_t  formatGuid[16];
    uint32_t compressionMethod;
    uint16_t comprDictId;
    uint16_t comprFrameSize;
    int32_t  cntBytes1;
    int32_t  cntBytes4;
    int32_t  cntBytes8;
    int32_t  cntTypes;
    uint16_t cntObjects;
    uint16_t cntArrays;
    int32_t  sizeUncTotal;
    int32_t  sizeCmpTotal;
    int32_t  cntBlocks;
    int32_t  sizeBlobBytes;
    // v4+
    int32_t  cntBytes2;
    int32_t  blkCmpSzBytes;
    // v5
    int32_t  sizeUncBuf1;
    int32_t  sizeCmpBuf1;
    int32_t  sizeUncBuf2;
    int32_t  sizeCmpBuf2;
    int32_t  cb1Buf2;
    int32_t  cb2Buf2;
    int32_t  cb4Buf2;
    int32_t  cb8Buf2;
    int32_t  unk13;
    int32_t  cntObjBuf2;
    int32_t  cntArrBuf2;
    int32_t  unk16;
};
static_assert(sizeof(Kv3v5Header) == 120, "KV3 v5 header must be 120 bytes");
#pragma pack(pop)

// Align 'v' up to the given power-of-2 boundary.
static inline int alignUp(int v, int align) {
    return (v + align - 1) & ~(align - 1);
}

// Compute the byte offset within decompressed buf2 where BinaryBlobLengths start.
static int buf2BlobLenOff(const Kv3v5Header& h)
{
    int off = h.cntObjBuf2 * 4;
    off += h.cb1Buf2;
    off  = alignUp(off, 4);
    off += h.cb2Buf2 * 2;
    off  = alignUp(off, 4);
    off += h.cb4Buf2 * 4;
    off  = alignUp(off, 8);
    off += h.cb8Buf2 * 8;
    off += h.cntTypes;
    return off;
}

// Extract per-blob uncompressed byte lengths (uint32[cntBlocks]) from buf2.
// These lengths let us scan each individual physics blob independently.
static bool extractBlobLengths(
    const uint8_t* buf2Data, int buf2Len,
    const Kv3v5Header& h,
    std::vector<uint32_t>& blobLengthsOut)
{
    int off = buf2BlobLenOff(h);
    int needed = off + h.cntBlocks * 4;
    if (needed > buf2Len) return false;
    blobLengthsOut.resize(h.cntBlocks);
    std::memcpy(blobLengthsOut.data(), buf2Data + off,
                static_cast<size_t>(h.cntBlocks) * 4);
    // Sanity: sum must equal sizeBlobBytes.
    uint64_t total = 0;
    for (uint32_t v : blobLengthsOut) total += v;
    if (total != static_cast<uint64_t>(h.sizeBlobBytes)) {
        blobLengthsOut.clear();
        return false;
    }
    return true;
}

// Given the fully decompressed buffer2 and the KV3 v5 header, return a pointer
// to the first uint16 frame-compressed-size and store the count in outCount.
// Returns nullptr on layout mismatch.
static const uint16_t* extractBlobFrameSizes(
    const std::vector<uint8_t>& buf2,
    const Kv3v5Header& h,
    int& outCount)
{
    int off        = buf2BlobLenOff(h);
    int afterBlobs = off + h.cntBlocks * 4 + 4;   // +4 for 0xFFEEDD00 trailer
    int szFrames   = h.cntBlocks * 2;
    if (afterBlobs + szFrames > (int)buf2.size()) return nullptr;
    outCount = h.cntBlocks;
    return reinterpret_cast<const uint16_t*>(buf2.data() + afterBlobs);
}

// Decompress a KV3 v5 PHYS block and return the raw binary-blob bytes.
// Also populates blobLengthsOut with the uncompressed size of each individual
// blob, enabling per-blob scanning (prevents cross-blob hull contamination).
static bool decompressKv3v5Blobs(
    const uint8_t* phys, int physLen,
    std::vector<uint8_t>&  blobsOut,
    std::vector<uint32_t>& blobLengthsOut)
{
    if (physLen < KV3_HEADER_SIZE) return false;

    const Kv3v5Header* h = reinterpret_cast<const Kv3v5Header*>(phys);
    if (h->magic         != KV3_MAGIC5) return false;
    if (h->cntBlocks     <= 0)          return false;
    if (h->sizeBlobBytes <= 0)          return false;

    // ── ZSTD compression (comprMethod == 2) ─────────────────────────────────
    // Layout: header(120) | zstd(buf1) | zstd(buf2) | zstd(blobData) | trailer(4)
    if (h->compressionMethod == KV3_COMPR_ZSTD) {
        const int buf2Off = KV3_HEADER_SIZE + h->sizeCmpBuf1;
        if (buf2Off + h->sizeCmpBuf2 > physLen) return false;

        // Decompress buf2 to extract per-blob byte lengths.
        {
            std::vector<uint8_t> buf2Unc(h->sizeUncBuf2);
            size_t r2 = ZSTD_decompress(
                buf2Unc.data(), static_cast<size_t>(h->sizeUncBuf2),
                phys + buf2Off, static_cast<size_t>(h->sizeCmpBuf2));
            if (!ZSTD_isError(r2))
                extractBlobLengths(buf2Unc.data(), (int)r2, *h, blobLengthsOut);
        }

        // Decompress buf3 = the concatenated blob data.
        const int buf3Off = buf2Off + h->sizeCmpBuf2;
        int cmpBuf3 = physLen - buf3Off - 4;   // −4 for 0xFFEEDD00 trailer
        if (cmpBuf3 <= 0) return false;

        blobsOut.resize(h->sizeBlobBytes);
        size_t r3 = ZSTD_decompress(
            blobsOut.data(), static_cast<size_t>(h->sizeBlobBytes),
            phys + buf3Off, static_cast<size_t>(cmpBuf3));
        if (ZSTD_isError(r3)) {
            std::cout << "[KV3] ZSTD blob decomp error: "
                      << ZSTD_getErrorName(r3) << "\n";
            return false;
        }
        return true;
    }

    // ── LZ4 compression (comprMethod == 1) ──────────────────────────────────
    if (h->compressionMethod != KV3_COMPR_LZ4) return false;

    int off = KV3_HEADER_SIZE + h->sizeCmpBuf1;
    if (off + h->sizeCmpBuf2 > physLen) return false;

    // Decompress buffer2 to get both per-frame LZ4 sizes and per-blob lengths.
    std::vector<uint8_t> buf2(h->sizeUncBuf2);
    {
        const char* src = reinterpret_cast<const char*>(phys + off);
        char*       dst = reinterpret_cast<char*>(buf2.data());
        int written = LZ4_decompress_safe(src, dst, h->sizeCmpBuf2, h->sizeUncBuf2);
        if (written != h->sizeUncBuf2) return false;
    }
    off += h->sizeCmpBuf2;

    // Extract uncompressed per-blob lengths from buf2.
    extractBlobLengths(buf2.data(), h->sizeUncBuf2, *h, blobLengthsOut);

    // Extract LZ4 frame compressed sizes (for decompressing blob data).
    int frameCount = 0;
    const uint16_t* frameSizes = extractBlobFrameSizes(buf2, *h, frameCount);
    if (!frameSizes || frameCount != h->cntBlocks) return false;

    // Chain-decompress binary-blob LZ4 frames into one contiguous buffer.
    blobsOut.resize(h->sizeBlobBytes);
    LZ4_streamDecode_t lz4State;
    LZ4_setStreamDecode(&lz4State, nullptr, 0);

    int writePos = 0;
    for (int i = 0; i < frameCount; ++i) {
        int cmpFrameSize = static_cast<int>(frameSizes[i]);
        if (off + cmpFrameSize > physLen) return false;

        int remaining    = h->sizeBlobBytes - writePos;
        int frameUncSize = std::min(remaining, (int)h->comprFrameSize);

        const char* src = reinterpret_cast<const char*>(phys + off);
        char*       dst = reinterpret_cast<char*>(blobsOut.data() + writePos);

        int written = LZ4_decompress_safe_continue(
            &lz4State, src, dst, cmpFrameSize, frameUncSize);
        if (written < 1) return false;

        writePos += written;
        off      += cmpFrameSize;
    }

    return (writePos == h->sizeBlobBytes);
}

// Scan one binary blob for contiguous RnPlane_t arrays {nx,ny,nz,dist} (16 bytes).
// Each contiguous run of ≥2 valid planes is split into convex hulls using
// a greedy AABB accumulator.  quiet=true suppresses diagnostic output (used
// when scanning thousands of small per-blob chunks from world_physics).
static void scanHullsFromBlobs(
    const uint8_t*         blobData,
    int                    blobLen,
    std::vector<BspPlane>&      planesOut,
    std::vector<BspBrushSide>&  sidesOut,
    std::vector<BspBrush>&      brushesOut,
    bool                   quiet = false)
{
    const int n = blobLen;
    if (n < 32) return;  // need at least 2 planes = 32 bytes

    // Pass 1 – mark positions that look like a valid RnPlane_t {nx,ny,nz,dist}.
    const int maxSlot = n / 4;
    std::vector<bool> valid(maxSlot + 1, false);

    for (int i = 0; i + 16 <= n; i += 4) {
        float nx, ny, nz, d;
        std::memcpy(&nx, blobData + i,      4);
        std::memcpy(&ny, blobData + i + 4,  4);
        std::memcpy(&nz, blobData + i + 8,  4);
        std::memcpy(&d,  blobData + i + 12, 4);

        if (!std::isfinite(nx) || !std::isfinite(ny) ||
            !std::isfinite(nz) || !std::isfinite(d))  continue;
        const float lenSq = nx*nx + ny*ny + nz*nz;
        if (lenSq < 0.9998f || lenSq > 1.0002f)       continue;
        if (d < -32768.f || d > 32768.f)               continue;
        valid[i / 4] = true;
    }

    if (!quiet) {
        int vc = 0;
        for (int k = 0; k < maxSlot; ++k) if (valid[k]) ++vc;
        float nx0=0, ny0=0, nz0=0, d0=0;
        if (n >= 16) {
            std::memcpy(&nx0, blobData,      4);
            std::memcpy(&ny0, blobData + 4,  4);
            std::memcpy(&nz0, blobData + 8,  4);
            std::memcpy(&d0,  blobData + 12, 4);
        }
        std::cout << "[HullScan] blob=" << n << "B  valid=" << vc
                  << "  p0=(" << nx0 << "," << ny0 << "," << nz0
                  << " d=" << d0 << ")\n";
    }

    // Pass 2 – scan all four byte-alignment offsets (mod 16) for runs of
    // consecutive valid planes at exactly 16-byte stride.
    // Require ≥2 planes per hull (a 1-plane brush is still an unbounded half-space).
    // The 'used' flag prevents the same data from being emitted at multiple alignments.
    std::vector<bool> used(maxSlot + 1, false);

    int runsFound = 0;
    for (int align = 0; align < 16; align += 4) {
        for (int i = align; i + 16 <= n; i += 16) {   // just need room for ≥1 plane
            if (!valid[i / 4] || used[i / 4]) continue;

            // Measure the contiguous run at 16-byte stride.
            int count = 0;
            for (int j = i; j + 16 <= n && valid[j / 4]; j += 16)
                ++count;

            if (count < 2) continue;  // single planes remain unbounded — skip

            if (!quiet) {
                std::cout << "[HullScan]  run align=" << align << " off=" << i
                          << " count=" << count << "\n";
            }

            // Mark all positions so other alignments don't re-emit them.
            for (int k = 0; k < count; k++)
                used[(i + k * 16) / 4] = true;

            ++runsFound;
            // Greedily split the run into individual convex hulls.
            // Within a single hull the AABB only ever tightens — it never flips
            // sign (mnX >= mxX).  A sign-flip means the new plane came from a
            // different hull, so we cut here and start a new accumulator.
            struct HullAcc {
                // Use ±1e9 sentinels (not map-extent ±32768) so we can tell
                // whether each bound was ever constrained by a real plane.
                // An axis where both bounds remain at their initial sentinel
                // means no axis-dominant plane existed for that direction,
                // making the hull unbounded (map-extent AABB) → false positives.
                float mnX=-1e9f, mnY=-1e9f, mnZ=-1e9f;
                float mxX= 1e9f, mxY= 1e9f, mxZ= 1e9f;
                int   firstSide = 0;
                int   cnt       = 0;
            };

            // Apply one plane's contribution to an AABB (axis-dominant only).
            auto applyP = [](float nx, float ny, float nz, float d,
                              float& mnX, float& mnY, float& mnZ,
                              float& mxX, float& mxY, float& mxZ) {
                if (std::fabsf(nx) >= 0.7f) {
                    if (nx > 0.f) mxX = std::min(mxX, d / nx);
                    else          mnX = std::max(mnX, d / nx);
                }
                if (std::fabsf(ny) >= 0.7f) {
                    if (ny > 0.f) mxY = std::min(mxY, d / ny);
                    else          mnY = std::max(mnY, d / ny);
                }
                if (std::fabsf(nz) >= 0.7f) {
                    if (nz > 0.f) mxZ = std::min(mxZ, d / nz);
                    else          mnZ = std::max(mnZ, d / nz);
                }
            };

            auto commitH = [&](HullAcc& ha) {
                // ── Validity: need ≥4 planes AND all 6 half-space bounds actually
                // constrained (sentinel ±1e9 means no axis-dominant plane ever
                // touched that bound → the hull would span the entire map → discard).
                // This eliminates "phantom hulls" built from oblique-normal-only data
                // (e.g. mesh vertex normals) that otherwise produce false sweep hits
                // throughout open air and kill grenade velocity in a few steps.
                const bool bounded =
                    ha.mnX > -1e8f && ha.mxX < 1e8f &&
                    ha.mnY > -1e8f && ha.mxY < 1e8f &&
                    ha.mnZ > -1e8f && ha.mxZ < 1e8f;
                if (ha.cnt < 4 || !bounded ||
                    ha.mnX >= ha.mxX || ha.mnY >= ha.mxY || ha.mnZ >= ha.mxZ) {
                    planesOut.resize(planesOut.size() - ha.cnt);
                    sidesOut.resize(sidesOut.size()  - ha.cnt);
                    return;
                }
                BspBrush br;
                br.firstSide = ha.firstSide;
                br.numSides  = ha.cnt;
                br.mnX = ha.mnX - 2.f;  br.mnY = ha.mnY - 2.f;  br.mnZ = ha.mnZ - 2.f;
                br.mxX = ha.mxX + 2.f;  br.mxY = ha.mxY + 2.f;  br.mxZ = ha.mxZ + 2.f;
                brushesOut.push_back(br);
            };

            HullAcc cur;
            cur.firstSide = (int)sidesOut.size();

            for (int k = 0; k < count; k++) {
                const int off = i + k * 16;
                float nx, ny, nz, d;
                std::memcpy(&nx, blobData + off,      4);
                std::memcpy(&ny, blobData + off + 4,  4);
                std::memcpy(&nz, blobData + off + 8,  4);
                std::memcpy(&d,  blobData + off + 12, 4);

                // Would adding this plane invalidate the running AABB?
                float tnX=cur.mnX, tnY=cur.mnY, tnZ=cur.mnZ;
                float txX=cur.mxX, txY=cur.mxY, txZ=cur.mxZ;
                applyP(nx, ny, nz, d, tnX, tnY, tnZ, txX, txY, txZ);
                const bool flips = (tnX >= txX || tnY >= txY || tnZ >= txZ);

                if (flips) {
                    // Hull boundary detected — finalize current accumulator.
                    commitH(cur);
                    cur = HullAcc{};
                    cur.firstSide = (int)sidesOut.size();
                    // Reset AABB and seed with this plane only.
                    applyP(nx, ny, nz, d, cur.mnX, cur.mnY, cur.mnZ,
                                          cur.mxX, cur.mxY, cur.mxZ);
                } else {
                    cur.mnX=tnX; cur.mnY=tnY; cur.mnZ=tnZ;
                    cur.mxX=txX; cur.mxY=txY; cur.mxZ=txZ;
                }

                planesOut.push_back({{nx, ny, nz}, d});
                sidesOut.push_back({(int)planesOut.size() - 1});
                ++cur.cnt;
            }
            commitH(cur);
        }
    }
}

// ─── Triangle mesh extraction from world_physics blob data ───────────────────
// CS2 RnMesh_t shapes appear in the blob stream as consecutive pairs:
//   Blob A: vertex array   — N×12 bytes  (float3 world-space positions)
//   Blob B: triangle array — M×8  bytes  (uint16[3] indices + 2 bytes flags)
//
// Detection heuristic: A is a vertex array if size%12==0 and all floats finite.
// B is a triangle array for A if size%8==0 and every uint16 index in bytes[0..5]
// per 8-byte group is strictly < vertCount(A).  This index-range check has an
// essentially zero false-positive rate because random float bit-patterns produce
// uint16 values >> typical vertex counts (< 65536/256 ≈ 256 per small shape).
static void scanMeshesFromBlobs(
    const uint8_t*               blobData,
    size_t                       blobLen,
    const std::vector<uint32_t>& blobLengths,
    std::vector<MeshTri>&        trisOut)
{
    size_t       offset = 0;
    const size_t N      = blobLengths.size();

    for (size_t i = 0; i < N; ) {
        const uint32_t blenA = blobLengths[i];
        if (offset + blenA > blobLen) break;

        // Blob A must be a multiple of 12 bytes with ≥3 vertices.
        if (blenA < 36 || blenA % 12 != 0) {
            offset += blenA;
            ++i;
            continue;
        }
        const int      vertCount = static_cast<int>(blenA / 12);
        const uint8_t* blobA     = blobData + offset;

        // All float3 groups must be finite world-space coordinates.
        bool allFinite = true;
        for (int v = 0; v < vertCount && allFinite; ++v) {
            float x, y, z;
            std::memcpy(&x, blobA + v * 12,     4);
            std::memcpy(&y, blobA + v * 12 + 4, 4);
            std::memcpy(&z, blobA + v * 12 + 8, 4);
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
                allFinite = false;
        }
        if (!allFinite) {
            offset += blenA;
            ++i;
            continue;
        }

        // Look-ahead at blob B (must exist and be a multiple of 8 bytes).
        if (i + 1 >= N) { offset += blenA; ++i; break; }
        const uint32_t blenB = blobLengths[i + 1];
        if (offset + blenA + blenB > blobLen || blenB < 8 || blenB % 8 != 0) {
            offset += blenA;
            ++i;
            continue;
        }
        const int      triCount = static_cast<int>(blenB / 8);
        const uint8_t* blobB    = blobData + offset + blenA;

        // All 3 uint16 vertex indices per 8-byte group must be < vertCount.
        bool indicesOk = true;
        for (int t = 0; t < triCount && indicesOk; ++t) {
            uint16_t ia, ib, ic;
            std::memcpy(&ia, blobB + t * 8,     2);
            std::memcpy(&ib, blobB + t * 8 + 2, 2);
            std::memcpy(&ic, blobB + t * 8 + 4, 2);
            if (ia >= static_cast<uint16_t>(vertCount) ||
                ib >= static_cast<uint16_t>(vertCount) ||
                ic >= static_cast<uint16_t>(vertCount))
                indicesOk = false;
        }
        if (!indicesOk) {
            offset += blenA;
            ++i;
            continue;
        }

        // Valid (vertex, triangle) pair — extract geometry.
        std::vector<Vec3> verts(vertCount);
        for (int v = 0; v < vertCount; ++v) {
            std::memcpy(&verts[v].x, blobA + v * 12,     4);
            std::memcpy(&verts[v].y, blobA + v * 12 + 4, 4);
            std::memcpy(&verts[v].z, blobA + v * 12 + 8, 4);
        }

        for (int t = 0; t < triCount && trisOut.size() < 2000000u; ++t) {
            uint16_t ia, ib, ic;
            std::memcpy(&ia, blobB + t * 8,     2);
            std::memcpy(&ib, blobB + t * 8 + 2, 2);
            std::memcpy(&ic, blobB + t * 8 + 4, 2);
            if (ia == ib || ib == ic || ia == ic) continue;  // degenerate

            const Vec3& p0 = verts[ia];
            const Vec3& p1 = verts[ib];
            const Vec3& p2 = verts[ic];

            float e1x = p1.x - p0.x, e1y = p1.y - p0.y, e1z = p1.z - p0.z;
            float e2x = p2.x - p0.x, e2y = p2.y - p0.y, e2z = p2.z - p0.z;
            float nx  = e1y * e2z - e1z * e2y;
            float ny  = e1z * e2x - e1x * e2z;
            float nz  = e1x * e2y - e1y * e2x;
            float len = std::sqrtf(nx * nx + ny * ny + nz * nz);
            if (len < 1e-6f) continue;  // zero-area triangle
            nx /= len;  ny /= len;  nz /= len;

            MeshTri tri;
            tri.p[0] = p0;  tri.p[1] = p1;  tri.p[2] = p2;
            tri.n    = { nx, ny, nz };
            trisOut.push_back(tri);
        }

        // Consume both blobs and advance.
        offset += blenA + blenB;
        i      += 2;
    }
}

// VPK2 directory-tree scanner that collects all vmdl_c entries instead of
// searching for a single match.
struct VpkEntry {
    uint32_t    offset;   // within VPK embedded-data section
    uint32_t    length;
    std::string vpkPath;  // full path string within the VPK (for filtering)
};

static bool gatherVmdlcEntries(std::ifstream& f,
                                std::streamoff dataStart,
                                std::vector<VpkEntry>& entries)
{
    f.seekg(sizeof(VpkHeader));
    if (!f) return false;

    auto readStr = [&]() -> std::string {
        std::string s; char c;
        while (f.get(c) && c != '\0') s += c;
        return s;
    };

    while (f.good()) {
        std::string ext = readStr();
        if (ext.empty()) break;
        const bool isVmdlc = (ext == "vmdl_c");
        while (f.good()) {
            std::string path = readStr();
            if (path.empty()) break;
            while (f.good()) {
                std::string fname = readStr();
                if (fname.empty()) break;
                VpkDirEntry de{};
                f.read(reinterpret_cast<char*>(&de), sizeof(de));
                if (de.preloadBytes) {
                    f.seekg((std::streamoff)de.preloadBytes, std::ios::cur);
                }
                if (!isVmdlc) continue;
                if (de.archiveIndex != VPK_EMBEDDED) continue;
                if (de.entryLength  == 0)             continue;
                entries.push_back({ de.entryOffset, de.entryLength, path + "/" + fname });
            }
        }
    }
    return !entries.empty();
}

// Read the PHYS block bytes from a vmdl_c resource inside the VPK.
// vmdl_c header: 8-byte magic, then block entries of
//   { char[4] name; uint32 version; uint32 relOff; uint32 size }
// followed by the blocks themselves.
static bool extractPhysBlock(const uint8_t* data, int len,
                              std::vector<uint8_t>& physOut)
{
    // Source 2 resource file header (16 bytes):
    //   [0-3]   fileSize     uint32
    //   [4-5]   headerVer    uint16  (12 for CS2)
    //   [6-7]   resourceType uint16
    //   [8-11]  fileRevision uint32
    //   [12-15] blockCount   uint32
    if (len < 16) return false;

    uint32_t blockCount;
    std::memcpy(&blockCount, data + 12, 4);
    if (blockCount == 0 || blockCount > 64) return false;

    // Block descriptors at offset 16, each 12 bytes:
    //   [0-3]  name    char[4]   (ASCII, e.g. "PHYS")
    //   [4-7]  relOff  int32     (relative to the address of THIS field)
    //   [8-11] size    uint32
    //
    // absOff = (descStart + 4) + relOff
    for (uint32_t i = 0; i < blockCount; ++i) {
        int descStart = 16 + (int)i * 12;
        if (descStart + 12 > len) return false;

        char     name[4];
        int32_t  relOff;
        uint32_t size;
        std::memcpy(name,    data + descStart,     4);
        std::memcpy(&relOff, data + descStart + 4, 4);
        std::memcpy(&size,   data + descStart + 8, 4);

        if (name[0]!='P'||name[1]!='H'||name[2]!='Y'||name[3]!='S') continue;

        int absOff = (descStart + 4) + relOff;
        if (absOff < 0 || (int)size == 0 || absOff + (int)size > len) return false;
        physOut.assign(data + absOff, data + absOff + size);
        return true;
    }
    return false;
}

bool BspWorld::loadFromVpk(const std::string& mapName,
                            const std::string& cs2Path)
{
    const std::string vpkPath =
        cs2Path + "\\game\\csgo\\maps\\" + mapName + ".vpk";

    std::ifstream f(vpkPath, std::ios::binary);
    if (!f) return false;

    VpkHeader hdr{};
    f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (!f || hdr.sig != VPK_SIG || hdr.version != 2) return false;

    const std::streamoff dataStart =
        (std::streamoff)sizeof(VpkHeader) + (std::streamoff)hdr.treeSize;

    // Collect all embedded vmdl_c entries.
    std::vector<VpkEntry> entries;
    gatherVmdlcEntries(f, dataStart, entries);
    if (entries.empty()) return false;

    std::cout << "[BspWorld] " << entries.size()
              << " vmdl_c entries found, parsing PHYS blocks...\n";

    // Read each vmdl_c, extract PHYS block, decompress binary blobs, scan planes.
    int physParsed = 0;
    std::vector<uint8_t> entryBuf;
    std::vector<uint8_t> physBuf;
    std::vector<uint8_t> blobBuf;

    for (const VpkEntry& ve : entries) {
        std::streamoff fileOff = dataStart + (std::streamoff)ve.offset;
        f.seekg(fileOff);
        if (!f) continue;

        entryBuf.resize(ve.length);
        f.read(reinterpret_cast<char*>(entryBuf.data()), ve.length);
        if (!f) continue;

        physBuf.clear();
        if (!extractPhysBlock(entryBuf.data(), (int)ve.length, physBuf)) continue;

        blobBuf.clear();
        std::vector<uint32_t> blobLengths;
        if (!decompressKv3v5Blobs(physBuf.data(), (int)physBuf.size(),
                                   blobBuf, blobLengths)) continue;

        // Per-blob scanning: each blob is one physics shape (hull or mesh).
        // Scanning blobs individually prevents runs from crossing blob boundaries,
        // which would merge planes from physically separate hulls into one big
        // (geometrically wrong) brush that passes through open air.
        const bool isWorldPhysics = (ve.vpkPath.find("world_physics") != std::string::npos);
        if (!blobLengths.empty()) {
            const bool quiet = (blobLengths.size() > 20);
            uint64_t blobOff = 0;
            int hullsBefore = (int)m_brushes.size();
            for (uint32_t blen : blobLengths) {
                if (blobOff + blen > blobBuf.size()) break;
                if (blen >= 32)
                    scanHullsFromBlobs(blobBuf.data() + blobOff, (int)blen,
                                       m_planes, m_sides, m_brushes, quiet);
                blobOff += blen;
            }
            if (quiet) {
                std::cout << "[HullScan] " << blobLengths.size() << " blobs → "
                          << (m_brushes.size() - hullsBefore) << " hulls\n";
            }
            // Extract triangle meshes only from the world physics (world-space coords).
            if (isWorldPhysics) {
                size_t trisBefore = m_tris.size();
                scanMeshesFromBlobs(blobBuf.data(), blobBuf.size(), blobLengths, m_tris);
                std::cout << "[MeshScan] world_physics → "
                          << (m_tris.size() - trisBefore) << " triangles\n";
            }
        } else {
            scanHullsFromBlobs(blobBuf.data(), (int)blobBuf.size(),
                               m_planes, m_sides, m_brushes, false);
        }
        ++physParsed;
    }

    std::cout << "[BspWorld] Parsed " << physParsed << " PHYS blocks → "
              << m_brushes.size() << " convex hulls, "
              << m_planes.size()  << " planes, "
              << m_tris.size()    << " mesh triangles\n";

    // Build spatial hash grid for triangle mesh queries.
    if (!m_tris.empty())
        buildTriGrid();
    if (!m_brushes.empty())
        buildBrushGrid();

    if (m_brushes.empty() && m_tris.empty()) return false;

    m_mapName = mapName;
    m_loaded  = true;
    return true;
}

// ─── Per-brush convex sweep ───────────────────────────────────────────────────
// Classic Source-engine "enter/exit" sweep against a convex set of half-spaces.
// The segment must start outside solid (guaranteed after a bounce + bias push).
bool BspWorld::sweepBrush(const Vec3& a, const Vec3& b,
                          const BspBrush& br,
                          float& t_out, Vec3& n_out) const {
    float tEnter  = 0.f;
    float tExit   = 1.f;
    Vec3  enterN  { 0.f, 0.f, 1.f };
    bool  hadEntry = false;  // must have a real outside→inside crossing

    for (int i = 0; i < br.numSides; ++i) {
        const BspPlane& pl = m_planes[m_sides[br.firstSide + i].planeIdx];

        // Signed distances from the plane  (positive = outside the brush side)
        const float dA = vdot(a, pl.n) - pl.d + SWEEP_BIAS;
        const float dB = vdot(b, pl.n) - pl.d + SWEEP_BIAS;

        if (dA > 0.f && dB > 0.f) return false;   // segment entirely outside this half-space
        if (dA <= 0.f && dB <= 0.f) continue;      // already inside — skip

        const float denom = dA - dB;
        if (fabsf(denom) < 1e-8f) {
            if (dA > 0.f) return false;
            continue;
        }

        const float t = dA / denom;
        if (dA > dB) {            // entering brush through this plane (outside→inside)
            if (t > tEnter) { tEnter = t; enterN = pl.n; }
            hadEntry = true;
        } else {                  // exiting brush through this plane
            if (t < tExit) tExit = t;
        }

        if (tEnter >= tExit) return false;  // missed
    }

    // Require an actual crossing: start must have been outside at least one plane.
    // Without this, single-plane brushes where the start is already on the solid
    // side return t=0 (false hit) because tEnter stays at its initial 0.
    if (hadEntry && tEnter < tExit && tEnter >= 0.f) {
        t_out = tEnter;
        n_out = enterN;
        return true;
    }
    return false;
}

// ─── BspWorld::sweepTris ─────────────────────────────────────────────────────
// Segment [a→b] treated as a sphere sweep against all extracted mesh triangles.
// Only hits from the OUTSIDE are reported (grenade approaches from positive-normal side).
// Radius = 2 matches the actual game hull: UTIL_TraceHull mins(-2,-2,-2) maxs(2,2,2).
// Using 5 caused false positives on thin geometry (railings, props) that grenades
// pass through in reality.
static constexpr float kSphereR = 2.0f;

bool BspWorld::sweepTris(const Vec3& a, const Vec3& b,
                          float& t_hit, Vec3& norm) const
{
    if (m_tris.empty()) return false;

    const float dirX = b.x - a.x, dirY = b.y - a.y, dirZ = b.z - a.z;

    // Broad AABB of the query segment (padded by sphere radius).
    const float smnX = std::min(a.x, b.x) - kSphereR;
    const float smxX = std::max(a.x, b.x) + kSphereR;
    const float smnY = std::min(a.y, b.y) - kSphereR;
    const float smxY = std::max(a.y, b.y) + kSphereR;
    const float smnZ = std::min(a.z, b.z) - kSphereR;
    const float smxZ = std::max(a.z, b.z) + kSphereR;

    const int cx0 = static_cast<int>(std::floorf(smnX / kTriGridCell));
    const int cx1 = static_cast<int>(std::floorf(smxX / kTriGridCell));
    const int cy0 = static_cast<int>(std::floorf(smnY / kTriGridCell));
    const int cy1 = static_cast<int>(std::floorf(smxY / kTriGridCell));

    float bestT   = 2.f;
    int   bestIdx = -1;

    for (int cx = cx0; cx <= cx1; ++cx) {
        for (int cy = cy0; cy <= cy1; ++cy) {
            const int64_t key = (static_cast<int64_t>(static_cast<int32_t>(cx)) << 32)
                              | static_cast<uint32_t>(static_cast<int32_t>(cy));
            const auto it = m_triGrid.find(key);
            if (it == m_triGrid.end()) continue;

            for (uint32_t ti : it->second) {
                const MeshTri& tri = m_tris[ti];

                // Z-range reject against padded query Z.
                const float triMnZ = std::min({ tri.p[0].z, tri.p[1].z, tri.p[2].z });
                const float triMxZ = std::max({ tri.p[0].z, tri.p[1].z, tri.p[2].z });
                if (triMxZ + kSphereR < smnZ || triMnZ - kSphereR > smxZ) continue;

                // Segment must be approaching from the positive-normal side.
                const float dDotN = dirX * tri.n.x + dirY * tri.n.y + dirZ * tri.n.z;
                if (dDotN >= -1e-6f) continue;

                // Signed distance of start point from the triangle plane.
                const float dA = (a.x - tri.p[0].x) * tri.n.x
                               + (a.y - tri.p[0].y) * tri.n.y
                               + (a.z - tri.p[0].z) * tri.n.z;
                // Sphere expansion: hit when dA reaches kSphereR from inside.
                if (dA <= kSphereR) continue;  // already inside sphere range

                const float t = (dA - kSphereR) / (-dDotN);
                if (t <= 0.f || t >= bestT) continue;

                // Contact point on the triangle plane (sphere centre − n * R).
                const float Px = a.x + t * dirX - tri.n.x * kSphereR;
                const float Py = a.y + t * dirY - tri.n.y * kSphereR;
                const float Pz = a.z + t * dirZ - tri.n.z * kSphereR;

                // Inside-triangle test: cross product of each edge × (P − vertex)
                // must align with the triangle normal.
                {
                    float ex = tri.p[1].x - tri.p[0].x;
                    float ey = tri.p[1].y - tri.p[0].y;
                    float ez = tri.p[1].z - tri.p[0].z;
                    float vx = Px - tri.p[0].x;
                    float vy = Py - tri.p[0].y;
                    float vz = Pz - tri.p[0].z;
                    if ((ey*vz - ez*vy)*tri.n.x + (ez*vx - ex*vz)*tri.n.y + (ex*vy - ey*vx)*tri.n.z < 0.f)
                        continue;
                }
                {
                    float ex = tri.p[2].x - tri.p[1].x;
                    float ey = tri.p[2].y - tri.p[1].y;
                    float ez = tri.p[2].z - tri.p[1].z;
                    float vx = Px - tri.p[1].x;
                    float vy = Py - tri.p[1].y;
                    float vz = Pz - tri.p[1].z;
                    if ((ey*vz - ez*vy)*tri.n.x + (ez*vx - ex*vz)*tri.n.y + (ex*vy - ey*vx)*tri.n.z < 0.f)
                        continue;
                }
                {
                    float ex = tri.p[0].x - tri.p[2].x;
                    float ey = tri.p[0].y - tri.p[2].y;
                    float ez = tri.p[0].z - tri.p[2].z;
                    float vx = Px - tri.p[2].x;
                    float vy = Py - tri.p[2].y;
                    float vz = Pz - tri.p[2].z;
                    if ((ey*vz - ez*vy)*tri.n.x + (ez*vx - ex*vz)*tri.n.y + (ex*vy - ey*vx)*tri.n.z < 0.f)
                        continue;
                }

                bestT   = t;
                bestIdx = static_cast<int>(ti);
            }
        }
    }

    if (bestIdx >= 0) {
        t_hit = bestT;
        norm  = m_tris[bestIdx].n;
        return true;
    }
    return false;
}

// ─── BspWorld::sweep ─────────────────────────────────────────────────────────
uint32_t& BspWorld::sweepCounter() {
    thread_local uint32_t counter = 0;
    return counter;
}

std::vector<uint32_t>& BspWorld::triVisited() {
    thread_local std::vector<uint32_t> v;
    return v;
}

// Shared triangle-sweep implementation with per-sweep dedup via visitId.
bool BspWorld::sweepTrisInternal(const Vec3& a, const Vec3& b,
                                  float& t_hit, Vec3& norm,
                                  uint32_t visitId, bool anyHit) const {
    if (m_tris.empty()) return false;

    const float dirX = b.x - a.x, dirY = b.y - a.y, dirZ = b.z - a.z;

    const float smnX = std::min(a.x, b.x) - kSphereR;
    const float smxX = std::max(a.x, b.x) + kSphereR;
    const float smnY = std::min(a.y, b.y) - kSphereR;
    const float smxY = std::max(a.y, b.y) + kSphereR;
    const float smnZ = std::min(a.z, b.z) - kSphereR;
    const float smxZ = std::max(a.z, b.z) + kSphereR;

    const int cx0 = static_cast<int>(std::floorf(smnX / kTriGridCell));
    const int cx1 = static_cast<int>(std::floorf(smxX / kTriGridCell));
    const int cy0 = static_cast<int>(std::floorf(smnY / kTriGridCell));
    const int cy1 = static_cast<int>(std::floorf(smxY / kTriGridCell));

    // Per-sweep triangle dedup — prevents re-checking the same triangle across
    // grid cells.  A single floor triangle can span hundreds of units and land in
    // dozens of cells; without dedup it would be tested that many times per sweep.
    std::vector<uint32_t>& visited = triVisited();
    if (visited.size() < m_tris.size())
        visited.resize(m_tris.size());

    float bestT   = 2.f;
    int   bestIdx = -1;

    for (int cx = cx0; cx <= cx1; ++cx) {
        for (int cy = cy0; cy <= cy1; ++cy) {
            const int64_t key = (static_cast<int64_t>(static_cast<int32_t>(cx)) << 32)
                              | static_cast<uint32_t>(static_cast<int32_t>(cy));
            const auto it = m_triGrid.find(key);
            if (it == m_triGrid.end()) continue;

            for (uint32_t ti : it->second) {
                if (visited[ti] == visitId) continue;  // dedup across cells
                visited[ti] = visitId;

                const MeshTri& tri = m_tris[ti];

                const float triMnZ = std::min({ tri.p[0].z, tri.p[1].z, tri.p[2].z });
                const float triMxZ = std::max({ tri.p[0].z, tri.p[1].z, tri.p[2].z });
                if (triMxZ + kSphereR < smnZ || triMnZ - kSphereR > smxZ) continue;

                const float dDotN = dirX * tri.n.x + dirY * tri.n.y + dirZ * tri.n.z;
                if (dDotN >= -1e-6f) continue;

                const float dA = (a.x - tri.p[0].x) * tri.n.x
                               + (a.y - tri.p[0].y) * tri.n.y
                               + (a.z - tri.p[0].z) * tri.n.z;
                if (dA <= kSphereR) continue;

                const float t = (dA - kSphereR) / (-dDotN);
                if (t <= 0.f || t >= bestT) continue;

                const float Px = a.x + t * dirX - tri.n.x * kSphereR;
                const float Py = a.y + t * dirY - tri.n.y * kSphereR;
                const float Pz = a.z + t * dirZ - tri.n.z * kSphereR;

                // Inside-triangle test — three edge checks.
                {
                    float ex = tri.p[1].x - tri.p[0].x;
                    float ey = tri.p[1].y - tri.p[0].y;
                    float ez = tri.p[1].z - tri.p[0].z;
                    float vx = Px - tri.p[0].x;
                    float vy = Py - tri.p[0].y;
                    float vz = Pz - tri.p[0].z;
                    if ((ey*vz - ez*vy)*tri.n.x + (ez*vx - ex*vz)*tri.n.y + (ex*vy - ey*vx)*tri.n.z < 0.f)
                        continue;
                }
                {
                    float ex = tri.p[2].x - tri.p[1].x;
                    float ey = tri.p[2].y - tri.p[1].y;
                    float ez = tri.p[2].z - tri.p[1].z;
                    float vx = Px - tri.p[1].x;
                    float vy = Py - tri.p[1].y;
                    float vz = Pz - tri.p[1].z;
                    if ((ey*vz - ez*vy)*tri.n.x + (ez*vx - ex*vz)*tri.n.y + (ex*vy - ey*vx)*tri.n.z < 0.f)
                        continue;
                }
                {
                    float ex = tri.p[0].x - tri.p[2].x;
                    float ey = tri.p[0].y - tri.p[2].y;
                    float ez = tri.p[0].z - tri.p[2].z;
                    float vx = Px - tri.p[2].x;
                    float vy = Py - tri.p[2].y;
                    float vz = Pz - tri.p[2].z;
                    if ((ey*vz - ez*vy)*tri.n.x + (ez*vx - ex*vz)*tri.n.y + (ex*vy - ey*vx)*tri.n.z < 0.f)
                        continue;
                }

                // For any-hit mode: return immediately on first blocking hit.
                if (anyHit) {
                    t_hit = t;
                    norm  = tri.n;
                    return true;
                }

                bestT   = t;
                bestIdx = static_cast<int>(ti);
            }
        }
    }

    if (bestIdx >= 0) {
        t_hit = bestT;
        norm  = m_tris[bestIdx].n;
        return true;
    }
    return false;
}

bool BspWorld::sweepTris(const Vec3& a, const Vec3& b,
                          float& t_hit, Vec3& norm) const {
    uint32_t& vid = sweepCounter();
    ++vid;
    return sweepTrisInternal(a, b, t_hit, norm, vid, false);
}

bool BspWorld::sweepTrisAny(const Vec3& a, const Vec3& b, uint32_t visitId) const {
    float t;
    Vec3  n;
    return sweepTrisInternal(a, b, t, n, visitId, true);
}

bool BspWorld::sweep(const Vec3& a, const Vec3& b,
                     float& t_hit, Vec3& norm) const {
    if (!m_loaded) return false;

    // AABB of this segment (with 1-unit padding)
    const float smnX = std::min(a.x, b.x) - 1.f,  smxX = std::max(a.x, b.x) + 1.f;
    const float smnY = std::min(a.y, b.y) - 1.f,  smxY = std::max(a.y, b.y) + 1.f;
    const float smnZ = std::min(a.z, b.z) - 1.f,  smxZ = std::max(a.z, b.z) + 1.f;

    float bestT = 2.f;
    Vec3  bestN;
    bool  hit   = false;

    // Brush sweep via spatial grid — reuse sweepId for triangle dedup below.
    uint32_t& sweepId = sweepCounter();
    ++sweepId;

    // ── Brush sweep via spatial grid (O(1) per query instead of O(n)) ──────
    if (!m_brushGrid.empty()) {
        thread_local std::vector<uint32_t> visited;
        if (visited.size() < m_brushes.size())
            visited.resize(m_brushes.size());

        const int cx0 = static_cast<int>(std::floorf(smnX / kBrushGridCell));
        const int cx1 = static_cast<int>(std::floorf(smxX / kBrushGridCell));
        const int cy0 = static_cast<int>(std::floorf(smnY / kBrushGridCell));
        const int cy1 = static_cast<int>(std::floorf(smxY / kBrushGridCell));

        for (int cx = cx0; cx <= cx1; ++cx) {
            for (int cy = cy0; cy <= cy1; ++cy) {
                const int64_t key = (static_cast<int64_t>(static_cast<int32_t>(cx)) << 32)
                                  | static_cast<uint32_t>(static_cast<int32_t>(cy));
                const auto it = m_brushGrid.find(key);
                if (it == m_brushGrid.end()) continue;

                for (uint32_t bi : it->second) {
                    if (visited[bi] == sweepId) continue;  // dedup
                    visited[bi] = sweepId;

                    const BspBrush& br = m_brushes[bi];
                    // Broad-phase AABB reject
                    if (br.mxX < smnX || br.mnX > smxX) continue;
                    if (br.mxY < smnY || br.mnY > smxY) continue;
                    if (br.mxZ < smnZ || br.mnZ > smxZ) continue;

                    float t;
                    Vec3  n;
                    if (sweepBrush(a, b, br, t, n) && t < bestT) {
                        bestT = t;
                        bestN = n;
                        hit   = true;
                    }
                }
            }
        }
    } else {
        // Fallback: linear scan (used when grid hasn't been built yet)
        for (const BspBrush& br : m_brushes) {
            if (br.mxX < smnX || br.mnX > smxX) continue;
            if (br.mxY < smnY || br.mnY > smxY) continue;
            if (br.mxZ < smnZ || br.mnZ > smxZ) continue;

            float t;
            Vec3  n;
            if (sweepBrush(a, b, br, t, n) && t < bestT) {
                bestT = t;
                bestN = n;
                hit   = true;
            }
        }
    }

    // Also test against extracted mesh triangles (walls, ramps, floors).
    // Pass sweepId for triangle dedup (prevent re-test across grid cells).
    {
        float triT;
        Vec3  triN;
        if (sweepTrisInternal(a, b, triT, triN, sweepId, false) && triT < bestT) {
            bestT = triT;
            bestN = triN;
            hit   = true;
        }
    }

    if (hit) { t_hit = bestT; norm = bestN; }
    return hit;
}

// ─── BspWorld::sweepAny ──────────────────────────────────────────────────────
// Fast any-hit query for visibility (line-of-sight) checks.
// Early-exits on FIRST hit — no closest-hit search.  Huge speedup for blocked
// rays (the common case near walls).
bool BspWorld::sweepAny(const Vec3& a, const Vec3& b) const {
    if (!m_loaded) return false;

    const float smnX = std::min(a.x, b.x) - 1.f,  smxX = std::max(a.x, b.x) + 1.f;
    const float smnY = std::min(a.y, b.y) - 1.f,  smxY = std::max(a.y, b.y) + 1.f;
    const float smnZ = std::min(a.z, b.z) - 1.f,  smxZ = std::max(a.z, b.z) + 1.f;

    uint32_t& sweepId = sweepCounter();
    ++sweepId;

    // ── Brush sweep — early-exit on first hit ───────────────────────────
    if (!m_brushGrid.empty()) {
        thread_local std::vector<uint32_t> visited;
        if (visited.size() < m_brushes.size())
            visited.resize(m_brushes.size());

        const int cx0 = static_cast<int>(std::floorf(smnX / kBrushGridCell));
        const int cx1 = static_cast<int>(std::floorf(smxX / kBrushGridCell));
        const int cy0 = static_cast<int>(std::floorf(smnY / kBrushGridCell));
        const int cy1 = static_cast<int>(std::floorf(smxY / kBrushGridCell));

        for (int cx = cx0; cx <= cx1; ++cx) {
            for (int cy = cy0; cy <= cy1; ++cy) {
                const int64_t key = (static_cast<int64_t>(static_cast<int32_t>(cx)) << 32)
                                  | static_cast<uint32_t>(static_cast<int32_t>(cy));
                const auto it = m_brushGrid.find(key);
                if (it == m_brushGrid.end()) continue;

                for (uint32_t bi : it->second) {
                    if (visited[bi] == sweepId) continue;
                    visited[bi] = sweepId;

                    const BspBrush& br = m_brushes[bi];
                    if (br.mxX < smnX || br.mnX > smxX) continue;
                    if (br.mxY < smnY || br.mnY > smxY) continue;
                    if (br.mxZ < smnZ || br.mnZ > smxZ) continue;

                    float t;
                    Vec3  n;
                    if (sweepBrush(a, b, br, t, n))
                        return true;  // any hit = blocked
                }
            }
        }
    } else {
        for (const BspBrush& br : m_brushes) {
            if (br.mxX < smnX || br.mnX > smxX) continue;
            if (br.mxY < smnY || br.mnY > smxY) continue;
            if (br.mxZ < smnZ || br.mnZ > smxZ) continue;
            float t;
            Vec3  n;
            if (sweepBrush(a, b, br, t, n))
                return true;
        }
    }

    // Triangle mesh — any-hit with dedup.
    return sweepTrisAny(a, b, sweepId);
}
