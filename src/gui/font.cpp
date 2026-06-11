#include "font.h"

#include <Windows.h>

#include <iostream>
#include <cstring>
#include <string>

namespace {

std::wstring resolveCs2PanoramaFont(const wchar_t* fileName) {
    auto trySteamRoot = [](HKEY root, const char* subKey, const char* valueName) -> std::string {
        HKEY hk{};
        if (RegOpenKeyExA(root, subKey, 0, KEY_READ, &hk) != ERROR_SUCCESS)
            return {};
        char buf[512]{};
        DWORD sz = sizeof(buf);
        if (RegQueryValueExA(hk, valueName, nullptr, nullptr,
                reinterpret_cast<LPBYTE>(buf), &sz) != ERROR_SUCCESS) {
            RegCloseKey(hk);
            return {};
        }
        RegCloseKey(hk);
        std::string path = buf;
        while (!path.empty() && (path.back() == '\\' || path.back() == '/'))
            path.pop_back();
        return path;
    };

    std::string steamPath = trySteamRoot(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath");
    if (steamPath.empty())
        steamPath = trySteamRoot(HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Valve\\Steam", "InstallPath");
    if (steamPath.empty())
        return {};

    const std::string cs2Root = steamPath +
        "\\steamapps\\common\\Counter-Strike Global Offensive";
    const std::string rel = std::string("\\game\\csgo\\panorama\\fonts\\");
    char narrowName[64]{};
    WideCharToMultiByte(CP_UTF8, 0, fileName, -1, narrowName, sizeof(narrowName), nullptr, nullptr);
    std::string full = cs2Root + rel + narrowName;
    if (GetFileAttributesA(full.c_str()) == INVALID_FILE_ATTRIBUTES)
        return {};

    int wlen = MultiByteToWideChar(CP_UTF8, 0, full.c_str(), -1, nullptr, 0);
    if (wlen <= 0)
        return {};
    std::wstring wide(static_cast<std::size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, full.c_str(), -1, wide.data(), wlen);
    if (!wide.empty() && wide.back() == L'\0')
        wide.pop_back();
    return wide;
}

bool loadPrivateFont(const wchar_t* path) {
    return path && path[0] && AddFontResourceExW(path, FR_PRIVATE, 0) != 0;
}

} // namespace

// --- Row-based packer ---
bool FontAtlas::pack(int w, int h, int& outX, int& outY) {
    if (m_cursorX + w + 1 > kSize) {
        m_cursorX = 1;
        m_cursorY += m_rowH + 1;
        m_rowH = 0;
    }
    if (m_cursorY + h + 1 > kSize) {
        std::cerr << "[Font] Atlas full! (" << kSize << "x" << kSize << ")\n";
        return false;
    }
    outX = m_cursorX;
    outY = m_cursorY;
    m_cursorX += w + 1;
    if (h > m_rowH) m_rowH = h;
    return true;
}

// --- Initialisation ---
bool FontAtlas::init(ID3D11Device* device, const wchar_t* fontName, int renderPx) {
    m_fontName = fontName;
    m_renderPx = renderPx;
    m_atlas.resize(kSize * kSize, 0); // zero-filled = transparent

    // Create GDI font at the requested pixel size.
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) { std::cerr << "[Font] CreateCompatibleDC failed\n"; return false; }

    SetMapMode(dc, MM_TEXT);

    HFONT hFont = CreateFontW(-renderPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH, fontName);
    if (!hFont) { std::cerr << "[Font] CreateFontW failed\n"; return false; }
    SelectObject(dc, hFont);

    // Get ascender / descender for line-height calculation.
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    m_fontHeight = tm.tmHeight;

    MAT2 mat{};
    mat.eM11.value = 1; mat.eM11.fract = 0;
    mat.eM22.value = 1; mat.eM22.fract = 0;

    int skipped = 0, packed = 0;
    for (wchar_t c = 32; c < 128; ++c) {
        GLYPHMETRICS gm{};
        DWORD size = GetGlyphOutlineW(dc, c, GGO_GRAY8_BITMAP, &gm, 0, nullptr, &mat);
        if (size == GDI_ERROR) continue;

        int bw = gm.gmBlackBoxX;
        int bh = gm.gmBlackBoxY;

        // Zero-width / blank glyph: store advance only.
        if (bw <= 0 || bh <= 0 || size == 0) {
            GlyphInfo info{};
            info.advanceX = gm.gmCellIncX;
            m_glyphs[c] = info;
            ++skipped;
            continue;
        }

        // GGO_GRAY8_BITMAP: rows are DWORD-aligned.
        int stride = (bw + 3) & ~3;
        if (size < (DWORD)(bh * stride)) {
            ++skipped;
            continue;
        }

        std::vector<byte> buf(size);
        GetGlyphOutlineW(dc, c, GGO_GRAY8_BITMAP, &gm, size, buf.data(), &mat);

        int px, py;
        if (!pack(bw, bh, px, py)) { ++skipped; continue; }

        for (int y = 0; y < bh; ++y) {
            for (int x = 0; x < bw; ++x) {
                byte v = buf[y * stride + x];
                unsigned av = (unsigned)v * 255u / 64u;
                unsigned idx = (unsigned)(py + y) * kSize + (unsigned)(px + x);
                if (idx < (unsigned)m_atlas.size())
                    m_atlas[idx] = (byte)(av > 255 ? 255 : av);
            }
        }

        GlyphInfo info{};
        float hto = 0.5f; // half-texel inset to prevent bilinear bleed
        info.u0 = ((float)px + hto) / kSize;
        info.v0 = ((float)py + hto) / kSize;
        info.u1 = ((float)(px + bw) - hto) / kSize;
        info.v1 = ((float)(py + bh) - hto) / kSize;
        info.width   = bw;
        info.height  = bh;
        info.bearingX = gm.gmptGlyphOrigin.x;
        info.bearingY = tm.tmAscent - gm.gmptGlyphOrigin.y;
        info.advanceX = gm.gmCellIncX;
        m_glyphs[c] = info;
        ++packed;
    }

    // ── Helper to load a single glyph codepoint from the current DC font ────
    auto loadGlyph = [&](wchar_t c) {
        if (m_glyphs.count(c)) return;
        GLYPHMETRICS gm{};
        DWORD size = GetGlyphOutlineW(dc, c, GGO_GRAY8_BITMAP, &gm, 0, nullptr, &mat);
        if (size == GDI_ERROR) { ++skipped; return; }
        int bw = gm.gmBlackBoxX, bh = gm.gmBlackBoxY;
        if (bw <= 0 || bh <= 0 || size == 0) {
            GlyphInfo info{}; info.advanceX = gm.gmCellIncX;
            m_glyphs[c] = info; ++skipped; return;
        }
        int stride = (bw + 3) & ~3;
        if (size < (DWORD)(bh * stride)) { ++skipped; return; }
        std::vector<byte> buf(size);
        GetGlyphOutlineW(dc, c, GGO_GRAY8_BITMAP, &gm, size, buf.data(), &mat);
        int px, py;
        if (!pack(bw, bh, px, py)) { ++skipped; return; }
        for (int y = 0; y < bh; ++y)
            for (int x = 0; x < bw; ++x) {
                byte v = buf[y * stride + x];
                unsigned av = (unsigned)v * 255u / 64u;
                unsigned idx = (unsigned)(py + y) * kSize + (unsigned)(px + x);
                if (idx < (unsigned)m_atlas.size())
                    m_atlas[idx] = (byte)(av > 255 ? 255 : av);
            }
        GlyphInfo info{};
        float hto = 0.5f;
        info.u0 = ((float)px + hto) / kSize;
        info.v0 = ((float)py + hto) / kSize;
        info.u1 = ((float)(px + bw) - hto) / kSize;
        info.v1 = ((float)(py + bh) - hto) / kSize;
        info.width = bw; info.height = bh;
        info.bearingX = gm.gmptGlyphOrigin.x;
        info.bearingY = tm.tmAscent - gm.gmptGlyphOrigin.y;
        info.advanceX = gm.gmCellIncX;
        m_glyphs[c] = info; ++packed;
    };

    // ── Icon glyphs used in the menu (from Segoe UI via font-linking) ──
    static const wchar_t kIcons[] = {
        0x25A3, 0x25C9, 0x2316, 0x263C, 0x25A0, 0x2699, 0x2795,
        0x270E, 0x2716, 0x25A1, 0x25B6,
        0x2601, 0x2668, 0x25CE
    };
    for (wchar_t c : kIcons) loadGlyph(c);

    // ── CS:GO weapon icons from csgo_icons.ttf ────────────────────────
    {
        wchar_t fontPath[MAX_PATH];
        bool found = false;
        if (GetModuleFileNameW(nullptr, fontPath, MAX_PATH)) {
            wchar_t* slash = wcsrchr(fontPath, L'\\');
            if (slash) {
                size_t remain = MAX_PATH - (size_t)((slash + 1) - fontPath);
                wcscpy_s(slash + 1, remain, L"csgo_icons.ttf");
                found = loadPrivateFont(fontPath);
            }
        }
        if (!found)
            found = loadPrivateFont(L"csgo_icons.ttf");
        if (!found)
            found = loadPrivateFont(L"..\\csgo_icons.ttf");
        if (!found) {
            const std::wstring cs2Font = resolveCs2PanoramaFont(L"csgo_icons.ttf");
            found = loadPrivateFont(cs2Font.c_str());
        }

        if (found) {
            HFONT hIconFont = CreateFontW(-renderPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"csgo_icons");
            if (hIconFont) {
                HFONT oldFont = (HFONT)SelectObject(dc, hIconFont);
                // CS weapon / utility icons in the PUA range (csgo_icons.ttf)
                static const wchar_t kCSGOGlyphs[] = {
                    0xE001, // Deagle
                    0xE002, // Dual Berettas
                    0xE003, // Five-SeveN
                    0xE004, // Glock
                    0xE007, // AK-47
                    0xE008, // AUG
                    0xE009, // AWP
                    0xE00A, // FAMAS
                    0xE00B, // G3SG1
                    0xE00D, // Galil
                    0xE00E, // M4A4
                    0xE010, // M4A1-S
                    0xE011, // MAC-10
                    0xE013, // P2000
                    0xE018, // UMP-45
                    0xE019, // XM1014
                    0xE01A, // PP-Bizon
                    0xE01B, // MAG-7
                    0xE01C, // Negev
                    0xE01D, // Sawed-Off
                    0xE01E, // Tec-9
                    0xE01F, // Zeus x27
                    0xE020, // P250
                    0xE021, // MP7
                    0xE022, // MP9
                    0xE023, // Nova
                    0xE024, // P90
                    0xE026, // SCAR-20
                    0xE027, // SG553
                    0xE028, // SSG-08
                    0xE02A, // CT knife
                    0xE02B, // Flashbang
                    0xE02C, // HE grenade
                    0xE02D, // Smoke grenade
                    0xE02E, // Molotov
                    0xE02F, // Decoy
                    0xE030, // Incendiary
                    0xE031, // C4
                    0xE03B, // T knife
                    0xE03C, // M249
                    0xE03D, // USP-S
                    0xE03F, // CZ75-Auto
                    0xE040, // R8 Revolver
                    0xE064, // Armor
                    0xE065, // Armor + Helmet
                    0xE066, // Defuse kit
                    0xE1F4, // Bayonet
                    0xE1F9, // Flip Knife
                    0xE1FA, // Gut Knife
                    0xE1FB, // Karambit
                    0xE1FC, // M9 Bayonet
                    0xE1FD, // Huntsman
                    0xE200, // Falchion
                    0xE202, // Bowie
                    0xE203, // Butterfly
                    0xE204  // Shadow Daggers
                };
                for (wchar_t c : kCSGOGlyphs) loadGlyph(c);
                SelectObject(dc, oldFont);
                DeleteObject(hIconFont);
            }
        } else {
            std::cerr << "[Font] csgo_icons.ttf not found (weapon/grenade icons disabled)\n";
        }
    }

    std::cout << "[Font] packed=" << packed << " skipped=" << skipped << "\n";

    DeleteObject(hFont);
    DeleteDC(dc);

    std::vector<byte> rgbaAtlas(kSize * kSize * 4, 0);
    for (int i = 0; i < kSize * kSize; ++i) {
        byte alpha = m_atlas[i];
        rgbaAtlas[i * 4 + 0] = alpha;
        rgbaAtlas[i * 4 + 1] = alpha;
        rgbaAtlas[i * 4 + 2] = alpha;
        rgbaAtlas[i * 4 + 3] = alpha;
    }

    // Create D3D11 texture. All channels mirror alpha so both the custom text
    // shader and ImGui's RGBA texture path render glyphs correctly.
    D3D11_TEXTURE2D_DESC td{};
    td.Width  = kSize;
    td.Height = kSize;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = rgbaAtlas.data();
    init.SysMemPitch = kSize * 4;

    HRESULT hr = device->CreateTexture2D(&td, &init, m_texture.GetAddressOf());
    if (FAILED(hr)) { std::cerr << "[Font] CreateTexture2D failed\n"; return false; }

    D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
    svd.Format = td.Format;
    svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    svd.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(m_texture.Get(), &svd, m_srv.GetAddressOf());

    std::cout << "[Font] Atlas ready, " << m_glyphs.size() << " glyphs\n";
    return true;
}

void FontAtlas::destroy() {
    m_srv.Reset();
    m_texture.Reset();
    m_glyphs.clear();
    m_atlas.clear();
}

const GlyphInfo* FontAtlas::glyph(wchar_t c) const {
    auto it = m_glyphs.find(c);
    return it != m_glyphs.end() ? &it->second : nullptr;
}
