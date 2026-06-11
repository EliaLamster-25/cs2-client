#include "loader_font.h"

#include <imgui.h>
#include <Windows.h>
#include <vector>

namespace {

struct GdiFace {
    HDC dc = nullptr;
    HFONT font = nullptr;

    ~GdiFace() {
        if (font)
            DeleteObject(font);
        if (dc)
            DeleteDC(dc);
    }

    bool init(const wchar_t* face, int px) {
        dc = CreateCompatibleDC(nullptr);
        if (!dc)
            return false;
        font = CreateFontW(-px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
            NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
        if (!font)
            return false;
        SelectObject(dc, font);
        return true;
    }

    bool glyph(wchar_t ch, GLYPHMETRICS& gm, std::vector<unsigned char>& bits) const {
        MAT2 mat{};
        mat.eM11.value = 1;
        mat.eM22.value = 1;
        const DWORD size = GetGlyphOutlineW(dc, ch, GGO_BITMAP, &gm, 0, nullptr, &mat);
        if (size == GDI_ERROR)
            return false;
        bits.clear();
        if (size == 0)
            return true;
        bits.resize(size);
        return GetGlyphOutlineW(dc, ch, GGO_BITMAP, &gm, size, bits.data(), &mat) != GDI_ERROR;
    }
};

struct PendingGlyph {
    int rect = -1;
    int w = 0;
    int h = 0;
    std::vector<unsigned char> bits;
};

void blitMono(ImFontAtlas* atlas, int tx, int ty, int bw, int bh, const unsigned char* packed) {
    if (!atlas->TexPixelsAlpha8 || bw <= 0 || bh <= 0)
        return;
    const int stride = ((bw + 31) / 32) * 4;
    unsigned char* dst = atlas->TexPixelsAlpha8;
    const int atlasW = atlas->TexWidth;
    for (int y = 0; y < bh; ++y) {
        for (int x = 0; x < bw; ++x) {
            const int byteIdx = y * stride + (x / 8);
            const int bit = 7 - (x % 8);
            dst[(ty + y) * atlasW + (tx + x)] = (packed[byteIdx] & (1 << bit)) ? 255 : 0;
        }
    }
}

ImFont* queueSize(ImFontAtlas* atlas,
                  GdiFace& gdi,
                  float sizePx,
                  std::vector<PendingGlyph>& pending) {
    const int px = static_cast<int>(sizePx + 0.5f);
    if (!gdi.init(L"Segoe UI", px))
        return nullptr;

    ImFontConfig cfg{};
    cfg.SizePixels = static_cast<float>(px);
    cfg.OversampleH = 1;
    cfg.OversampleV = 1;
    cfg.PixelSnapH = true;
    cfg.RasterizerMultiply = 1.f;
    ImFont* font = atlas->AddFontDefault(&cfg);
    if (!font)
        return nullptr;

    for (wchar_t ch = 32; ch < 127; ++ch) {
        GLYPHMETRICS gm{};
        std::vector<unsigned char> bits;
        if (!gdi.glyph(ch, gm, bits))
            continue;

        const int bw = gm.gmBlackBoxX;
        const int bh = gm.gmBlackBoxY;
        if (bw <= 0 || bh <= 0)
            continue;
        const float advance = static_cast<float>(gm.gmCellIncX);
        const ImVec2 offset(static_cast<float>(gm.gmptGlyphOrigin.x),
            static_cast<float>(-gm.gmptGlyphOrigin.y));

        PendingGlyph pg{};
        pg.rect = atlas->AddCustomRectFontGlyph(font, ch, bw, bh, advance, offset);
        pg.w = bw;
        pg.h = bh;
        pg.bits = std::move(bits);
        pending.push_back(std::move(pg));
    }

    return font;
}

} // namespace

bool loaderBuildGdiFonts(ImFontAtlas* atlas, LoaderFonts& out) {
    if (!atlas)
        return false;

    atlas->Clear();
    atlas->Flags |= ImFontAtlasFlags_NoBakedLines;
    atlas->TexGlyphPadding = 0;

    std::vector<PendingGlyph> pending;
    pending.reserve(96 * 3);

    GdiFace bodyFace;
    GdiFace titleFace;
    GdiFace captionFace;
    out.body = queueSize(atlas, bodyFace, 14.f, pending);
    out.title = queueSize(atlas, titleFace, 18.f, pending);
    out.caption = queueSize(atlas, captionFace, 12.f, pending);

    if (!out.body) {
        out.body = atlas->AddFontDefault();
        out.title = out.body;
        out.caption = out.body;
        return out.body != nullptr;
    }

    if (!out.title)
        out.title = out.body;
    if (!out.caption)
        out.caption = out.body;

    if (!atlas->Build())
        return true;

    for (const PendingGlyph& pg : pending) {
        ImFontAtlasCustomRect* r = atlas->GetCustomRectByIndex(pg.rect);
        if (!r || pg.bits.empty())
            continue;
        blitMono(atlas, r->X, r->Y, pg.w, pg.h, pg.bits.data());
    }

    return true;
}
