#pragma once

#include <Windows.h>
#include <string>
#include <cstdint>

class Renderer;
class FontAtlas;

// ─── Theme colours (ARGB) ────────────────────────────────────────────────────
namespace Theme {
    constexpr unsigned int BG          = 0xFF0B0C12;
    constexpr unsigned int BORDER      = 0xFF1C1E28;
    constexpr unsigned int NAV_ACTIVE  = 0xFF242246;
    constexpr unsigned int ACCENT      = 0xFF7674F4;
    constexpr unsigned int TEXT        = 0xFFD8D9E9;
    constexpr unsigned int TEXT_MUTED  = 0xFF6E7087;
    constexpr unsigned int TEXT_LINK   = 0xFF8B89F8;
    constexpr unsigned int DESTRUCTIVE = 0xFFB94261;
    constexpr unsigned int SURFACE     = 0xFF151622;

    constexpr float CORNER_RADIUS = 16.f;
    constexpr float PADDING       = 24.f;
    constexpr float ITEM_SPACING  = 10.f;
}

class Gui {
public:
    // Called each frame before/after widget rendering.
    void beginFrame(Renderer& r, const FontAtlas& f, HWND hwnd);
    void endFrame();
    void setScale(float scale, float contentScale = 1.f);

    // ── Cursor / sizing ──────────────────────────────────────────────────
    void setCursor(float x, float y);
    void advanceY(float dy);
    void sameLine(float x = 0); // reset cursor.x (if x=0, uses previous item's end + spacing)
    float cursorX() const { return m_cursorX; }
    float cursorY() const { return m_cursorY; }
    void setItemWidth(float w);
    float itemWidth() const { return m_itemWidth; }

    // ── Widgets ──────────────────────────────────────────────────────────
    void label(const char* text, unsigned int color = Theme::TEXT, float fontSize = 14.f);
    void label(const wchar_t* text, unsigned int color = Theme::TEXT, float fontSize = 14.f);
    bool button(const char* label, float w, float h = 32.f);
    bool checkbox(const char* label, bool* value);
    bool sliderFloat(const char* id, float* value, float min, float max, const char* fmt = "%.0f");
    bool sliderInt(const char* id, int* value, int min, int max, const char* fmt = "%d");
    bool comboBox(const char* id, const char* const* items, int count, int* current);
    bool comboField(const char* id, const char* label, const char* const* items, int count, int* current, float h = 48.f);
    bool keybindButton(const char* label, int* targetKey);

    // Reusable Busan-style components
    bool accentButton(const char* id, const char* text, float w = 0.f, float h = 38.f);
    bool toggleCheckbox(const char* id, const char* label, bool* value, float h = 38.f);
    bool sliderFloatValue(const char* id, const char* label, float* value,
                          float min, float max, const char* fmt = "%.0f", float h = 38.f);
    bool keybindCard(const char* id, const char* label, int* targetKey, float h = 38.f);
    void separator();
    void dummy(float h);
    void colorEdit4(const char* id, float color[4]);

    // ── Tab / header helpers ─────────────────────────────────────────────
    bool tabButton(const char* label, float w, float h, bool active);

    // ── Accessors ────────────────────────────────────────────────────────
    float mouseX() const { return m_mouseX; }
    float mouseY() const { return m_mouseY; }
    bool  mouseDown() const { return m_mouseDown; }
    bool  mouseClicked() const { return m_mouseClicked; }
    Renderer&  renderer() const { return *m_renderer; }
    const FontAtlas& font() const { return *m_font; }

private:
    enum class PopupKind {
        None,
        Combo,
        Color,
    };

    uint32_t hashId(const char* id) const;
    bool isMouseInRect(float x, float y, float w, float h) const;
    bool isBlockedByPopup(float x, float y, float w, float h) const;
    void clearPopupState();
    void drawPopupOverlay();
    void drawSliderGrab(float cx, float cy, float size, unsigned int color = Theme::ACCENT);

    Renderer*         m_renderer = nullptr;
    const FontAtlas*  m_font     = nullptr;
    HWND              m_window   = nullptr;

    float m_mouseX = 0, m_mouseY = 0;
    bool  m_mouseDown = false, m_mouseClicked = false, m_mouseReleased = false;

    // Immediate-mode interaction state
    int m_hotItem     = -1;  // widget under cursor
    int m_activeItem  = -1;  // widget being clicked/dragged

    // Layout cursor
    float m_cursorX   = 0;
    float m_cursorY   = 0;
    float m_itemWidth = 0;
    float m_scale     = 1.f;
    float m_contentScale = 1.f;

    // Drop-down state (for comboBox)
    bool  m_dropdownOpen = false;
    int   m_dropdownId   = -1;
    int   m_colorPopupId = -1;
    bool  m_modalPopupActive = false;
    bool  m_popupJustOpened = false;
    PopupKind m_popupKind = PopupKind::None;
    int   m_modalPopupId = -1;
    float m_modalPopupX = 0.f;
    float m_modalPopupY = 0.f;
    float m_modalPopupW = 0.f;
    float m_modalPopupH = 0.f;
    const char* const* m_popupComboItems = nullptr;
    int m_popupComboCount = 0;
    int* m_popupComboCurrent = nullptr;
    int m_comboPopupResult = 0;
    int m_comboPopupResultId = -1;
    bool m_comboPopupResultValid = false;
    float* m_popupColor = nullptr;
};
