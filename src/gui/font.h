#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <Windows.h>

struct GlyphInfo {
    float u0, v0, u1, v1;
    int width, height;
    int bearingX, bearingY;
    int advanceX;
};

class FontAtlas {
public:
    bool init(ID3D11Device* device, const wchar_t* fontName = L"Segoe UI", int renderPx = 64);
    void destroy();

    const GlyphInfo* glyph(wchar_t c) const;
    ID3D11ShaderResourceView* texture() const { return m_srv.Get(); }
    int height() const { return m_fontHeight; }
    int renderPx() const { return m_renderPx; }
    float lineHeight(float fontSize) const { return (float)m_fontHeight * fontSize / (float)m_renderPx; }

private:
    bool pack(int w, int h, int& outX, int& outY);

    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srv;
    std::unordered_map<wchar_t, GlyphInfo> m_glyphs;

    static constexpr int kSize = 1024;
    std::vector<byte> m_atlas; // alpha channel buffer (1 byte per pixel)

    int m_cursorX = 1, m_cursorY = 1, m_rowH = 0;
    int m_fontHeight = 0;
    int m_renderPx = 64;
    std::wstring m_fontName;
};
