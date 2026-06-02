#pragma once

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <Windows.h>
#include <vector>

class FontAtlas;
struct ImFont;

class Renderer {
public:
    bool init(HWND hwnd, int width, int height);
    void beginFrame();
    void endFrame();
    void resize(int width, int height);
    void setImGuiDrawMode(bool enabled);
    bool isImGuiDrawMode() const { return m_useImGuiDrawMode; }

    // Convenience: correct Y-offset to centre text of given fontSize in a box of given height
    float centerY(float boxHeight, float fontSize) const;

    // Flat-colour primitives
    void drawRect      (float x, float y, float w, float h, unsigned int color, float thickness = 1.0f);
    void drawFilledRect(float x, float y, float w, float h, unsigned int color);
    void drawLine      (float x1, float y1, float x2, float y2, unsigned int color, float thickness = 1.0f);
    void drawRoundedRect(float x, float y, float w, float h, unsigned int color, float r, float thickness = 1.0f);
    void drawRoundedFilledRect(float x, float y, float w, float h, unsigned int color, float r);
    void drawGradientRect(float x, float y, float w, float h, unsigned int topColor, unsigned int botColor);
    void drawGradientRectH(float x, float y, float w, float h, unsigned int leftColor, unsigned int rightColor);
    void drawCircle    (float cx, float cy, float r, unsigned int color, float thickness = 1.0f);
    void drawFilledCircle(float cx, float cy, float r, unsigned int color);
    void drawFilledTriangle(float x1, float y1, float x2, float y2, float x3, float y3, unsigned int color);
    void drawFilledConvexPolygon(const float* pointsXY, int count, unsigned int color);
    void drawFilledPie(float cx, float cy, float r, float startAngle, float endAngle, unsigned int color);
    void setClipRect(float x, float y, float w, float h);
    void clearClipRect();
    void getClipRect(float& x, float& y, float& w, float& h) const;

    // Textured text rendering
    void drawText(const FontAtlas& font, float x, float y, const char* text,
                  unsigned int color, float size);
    void drawTextW(const FontAtlas& font, float x, float y, const wchar_t* text,
                   unsigned int color, float size);
    void measureTextBoundsW(const FontAtlas& font, const wchar_t* text, float size,
                            float& minX, float& minY, float& maxX, float& maxY) const;
    void drawImage(ID3D11ShaderResourceView* texture, float x, float y, float w, float h,
                   unsigned int color = 0xFFFFFFFFu);

    ID3D11Device* device() const { return m_device.Get(); }
    int screenWidth()  const { return m_width;  }
    int screenHeight() const { return m_height; }

private:
    bool createRenderTarget();
    void releaseRenderTarget();
    bool initShaders();
    bool initTexturedShaders();
    bool initPostProcessShaders();
    bool initBlendState();
    bool initRasterizerState();
    bool initWhiteTexture();
    void runPostProcessAA();
    void useTexturedPipeline();
    void useFlatPipeline();
    void flushBatch();
    void drawVertices(const float* verts, int vertexCount, D3D11_PRIMITIVE_TOPOLOGY topo);
    void drawTexturedQuads(const float* verts, int quadCount, ID3D11ShaderResourceView* tex);

    struct TexturedVertex { float x, y, u, v, r, g, b, a; };

    static constexpr int kBatchMaxVerts = 16384;

    Microsoft::WRL::ComPtr<ID3D11Device>           m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>     m_context;
    Microsoft::WRL::ComPtr<IDXGISwapChain>          m_swapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView>  m_rtv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          m_sceneColor;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView>   m_sceneRTV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_sceneSRV;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          m_msaaColor;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView>   m_msaaRTV;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState>   m_rasterizerState;
    Microsoft::WRL::ComPtr<ID3D11BlendState>        m_blendState;
    Microsoft::WRL::ComPtr<ID3D11Buffer>             m_cbScreen;

    // Flat-colour pipeline
    Microsoft::WRL::ComPtr<ID3D11VertexShader>      m_vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>       m_ps;
    Microsoft::WRL::ComPtr<ID3D11InputLayout>        m_inputLayout;
    Microsoft::WRL::ComPtr<ID3D11Buffer>             m_vertexBuffer;
    std::vector<float>                               m_batchBuf;
    int                                               m_batchCount = 0;

    // Textured pipeline
    Microsoft::WRL::ComPtr<ID3D11VertexShader>      m_txVS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>       m_txPS;
    Microsoft::WRL::ComPtr<ID3D11VertexShader>      m_postVS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>       m_postPS;
    Microsoft::WRL::ComPtr<ID3D11InputLayout>        m_txInputLayout;
    Microsoft::WRL::ComPtr<ID3D11Buffer>             m_txVertexBuffer;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>       m_sampler;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_whiteSRV;
    UINT m_msaaCount = 1;
    UINT m_msaaQuality = 0;
    D3D11_RECT m_clipRect{ 0, 0, 0, 0 };
    bool m_useImGuiDrawMode = false;
    bool m_imguiClipPushed = false;
    ImFont* m_imguiTextFont = nullptr;

    int m_width  = 0;
    int m_height = 0;
};
