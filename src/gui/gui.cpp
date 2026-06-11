#include "gui.h"
#include "overlay/renderer.h"
#include "gui/font.h"
#include <cctype>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

struct Hsva {
    float h, s, v, a;
};

// ═════════════════════════════════════════════════════════════════════════════
// Helpers
// ═════════════════════════════════════════════════════════════════════════════

static std::string vkDisplayName(int vk) {
    if (vk == 0) return "None (any)";
    switch (vk) {
        case VK_LBUTTON:  return "LMB";
        case VK_RBUTTON:  return "RMB";
        case VK_MBUTTON:  return "MMB";
        case VK_XBUTTON1: return "Mouse4";
        case VK_XBUTTON2: return "Mouse5";
        case VK_SHIFT:    return "Shift";
        case VK_CONTROL:  return "Ctrl";
        case VK_MENU:     return "Alt";
        case VK_CAPITAL:  return "Caps";
        case VK_TAB:      return "Tab";
        case VK_RETURN:   return "Enter";
        case VK_SPACE:    return "Space";
        case VK_BACK:     return "Back";
        case VK_ESCAPE:   return "Esc";
        case VK_DELETE:   return "Del";
        case VK_INSERT:   return "Ins";
        case VK_HOME:     return "Home";
        case VK_END:      return "End";
        case VK_PRIOR:    return "PgUp";
        case VK_NEXT:     return "PgDn";
        case VK_UP:       return "Up";
        case VK_DOWN:     return "Down";
        case VK_LEFT:     return "Left";
        case VK_RIGHT:    return "Right";
    }
    if (vk >= VK_F1 && vk <= VK_F12)
        return "F" + std::to_string(vk - VK_F1 + 1);
    if (vk >= 'A' && vk <= 'Z')
        return std::string(1, (char)vk);
    if (vk >= '0' && vk <= '9')
        return std::string(1, (char)vk);
    UINT scan = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    if (scan) {
        wchar_t buf[64] = {};
        LONG lp = (scan & 0xFF) << 16;
        if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN ||
            vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT ||
            vk == VK_INSERT || vk == VK_DELETE || vk == VK_RCONTROL || vk == VK_RMENU)
            lp |= 0x1000000;
        if (GetKeyNameTextW(lp, buf, 64)) {
            char mb[64] = {};
            WideCharToMultiByte(CP_UTF8, 0, buf, -1, mb, 64, nullptr, nullptr);
            return std::string(mb);
        }
    }
    return "VK_" + std::to_string(vk);
}

static float textCenterY(const FontAtlas& font, float top, float boxH, float fontSize) {
    return top + (boxH - font.lineHeight(fontSize)) * 0.5f;
}

static unsigned int withAlpha(unsigned int argb, float a01) {
    a01 = (std::max)(0.f, (std::min)(a01, 1.f));
    unsigned int a = (unsigned int)(a01 * 255.f);
    return (argb & 0x00FFFFFFu) | (a << 24);
}

static unsigned int lerpColor(unsigned int c0, unsigned int c1, float t) {
    t = (std::max)(0.f, (std::min)(t, 1.f));
    auto ch = [&](int s) -> unsigned int {
        float a = (float)((c0 >> s) & 0xFF);
        float b = (float)((c1 >> s) & 0xFF);
        return (unsigned int)(a + (b - a) * t + 0.5f);
    };
    return (ch(24) << 24) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

static float g_animFrameDt = 1.f / 60.f;
static int64_t g_animLastTick = 0;
static int64_t g_animFreq = 0;

static void updateAnimationClock() {
    if (g_animFreq == 0) {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        g_animFreq = freq.QuadPart;

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        g_animLastTick = now.QuadPart;
        g_animFrameDt = 1.f / 60.f;
        return;
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = (float)((now.QuadPart - g_animLastTick) / (double)g_animFreq);
    g_animLastTick = now.QuadPart;
    g_animFrameDt = (dt > 0.f && dt < 0.1f) ? dt : (1.f / 60.f);
}

static float animationBlend(float speed) {
    speed = (std::max)(0.01f, (std::min)(speed * 1.35f, 0.98f));
    float frameScale = (std::max)(0.25f, (std::min)(g_animFrameDt * 60.f, 4.f));
    return 1.f - powf(1.f - speed, frameScale);
}

static float animValue(uint32_t id, float target, float speed = 0.2f) {
    static std::unordered_map<uint32_t, float> s_anim;
    float& v = s_anim[id];
    v += (target - v) * animationBlend(speed);
    return v;
}

static float measureTextWidth(const FontAtlas& font, const char* text, float size) {
    if (!text || !*text)
        return 0.f;
    int len = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (len <= 1)
        return 0.f;

    std::wstring wtext(static_cast<std::size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wtext.data(), len);

    const float scale = size / font.renderPx();
    float width = 0.f;
    float spaceAdv = 8.f;
    if (const GlyphInfo* sg = font.glyph(L' '))
        spaceAdv = static_cast<float>(sg->advanceX);

    for (wchar_t ch : wtext) {
        if (ch == L' ') {
            width += spaceAdv * scale;
            continue;
        }
        if (const GlyphInfo* gi = font.glyph(ch))
            width += gi->advanceX * scale;
        else
            width += 8.f * scale;
    }
    return width;
}

static float measureRenderedTextWidth(Renderer& r, const FontAtlas& font, const char* text, float fontSize) {
    if (!text || !*text)
        return 0.f;

    int len = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (len <= 1)
        return 0.f;

    std::wstring wtext(static_cast<std::size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wtext.data(), len);

    float minX = 0.f, minY = 0.f, maxX = 0.f, maxY = 0.f;
    r.measureTextBoundsW(font, wtext.c_str(), fontSize, minX, minY, maxX, maxY);
    return (std::max)(0.f, maxX - minX);
}

static bool utf8ToWide(const char* text, std::wstring& out) {
    out.clear();
    if (!text || !*text)
        return false;

    int len = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (len <= 1)
        return false;

    out.resize(len - 1);
    MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), len);
    return true;
}

static float renderedTextCenterY(Renderer& r, const FontAtlas& font, float top, float boxH,
                                 const char* text, float fontSize) {
    if (!text || !*text)
        return textCenterY(font, top, boxH, fontSize);

    std::wstring wtext;
    if (!utf8ToWide(text, wtext))
        return textCenterY(font, top, boxH, fontSize);

    float minX = 0.f, minY = 0.f, maxX = 0.f, maxY = 0.f;
    r.measureTextBoundsW(font, wtext.c_str(), fontSize, minX, minY, maxX, maxY);
    const float boundsH = maxY - minY;
    if (boundsH <= 0.f)
        return textCenterY(font, top, boxH, fontSize);
    return top + (boxH - boundsH) * 0.5f - minY;
}

static float textVisualCenterY(const FontAtlas& font, float top, float boxH, const wchar_t* text, float fontSize) {
    if (!text || !*text)
        return textCenterY(font, top, boxH, fontSize);

    float scale = fontSize / font.renderPx();
    float minY = (std::numeric_limits<float>::max)();
    float maxY = (std::numeric_limits<float>::lowest)();

    for (const wchar_t* p = text; *p; ++p) {
        if (*p == L' ')
            continue;

        const GlyphInfo* gi = font.glyph(*p);
        if (!gi || gi->width <= 0 || gi->height <= 0)
            continue;

        float glyphTop = gi->bearingY * scale;
        float glyphBottom = glyphTop + gi->height * scale;
        minY = (std::min)(minY, glyphTop);
        maxY = (std::max)(maxY, glyphBottom);
    }

    if (minY == (std::numeric_limits<float>::max)() || maxY == (std::numeric_limits<float>::lowest)())
        return textCenterY(font, top, boxH, fontSize);

    float boundsH = maxY - minY;
    return top + (boxH - boundsH) * 0.5f - minY;
}

static float textVisualCenterX(const FontAtlas& font, float left, float boxW, const wchar_t* text, float fontSize) {
    if (!text || !*text)
        return left;

    float scale = fontSize / font.renderPx();
    float cursorX = 0.f;
    float minX = (std::numeric_limits<float>::max)();
    float maxX = (std::numeric_limits<float>::lowest)();
    float spaceAdv = 8.f;
    if (const GlyphInfo* sg = font.glyph(L' '))
        spaceAdv = (float)sg->advanceX;

    for (const wchar_t* p = text; *p; ++p) {
        if (*p == L' ') {
            cursorX += spaceAdv * scale;
            continue;
        }

        const GlyphInfo* gi = font.glyph(*p);
        if (!gi) {
            cursorX += 8.f * scale;
            continue;
        }

        if (gi->width > 0 && gi->height > 0) {
            float glyphLeft = cursorX + gi->bearingX * scale;
            float glyphRight = glyphLeft + gi->width * scale;
            minX = (std::min)(minX, glyphLeft);
            maxX = (std::max)(maxX, glyphRight);
        }

        cursorX += gi->advanceX * scale;
    }

    if (minX == (std::numeric_limits<float>::max)() || maxX == (std::numeric_limits<float>::lowest)())
        return left + (boxW - measureTextWidth(font, "", fontSize)) * 0.5f;

    float boundsW = maxX - minX;
    return left + (boxW - boundsW) * 0.5f - minX;
}

static float textVisualCenterY(const FontAtlas& font, float top, float boxH, const char* text, float fontSize) {
    std::wstring wide;
    if (!utf8ToWide(text, wide))
        return textCenterY(font, top, boxH, fontSize);
    return textVisualCenterY(font, top, boxH, wide.c_str(), fontSize);
}

static float textVisualCenterX(const FontAtlas& font, float left, float boxW, const char* text, float fontSize) {
    std::wstring wide;
    if (!utf8ToWide(text, wide))
        return left;
    return textVisualCenterX(font, left, boxW, wide.c_str(), fontSize);
}

static float textControlCenterY(const FontAtlas& font, float top, float boxH, const char* text, float fontSize) {
    return textVisualCenterY(font, top, boxH, text, fontSize) + fontSize * 0.16f;
}

static float textCompactCenterY(const FontAtlas& font, float top, float boxH, const char* text, float fontSize) {
    return textVisualCenterY(font, top, boxH, text, fontSize) + fontSize * 0.2f;
}

static std::string upperAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return (char)std::toupper(ch);
    });
    return text;
}

static void drawCheckGlyph(Renderer& r, float x, float y, float size, unsigned int color, float thickness = 1.8f) {
    r.drawLine(x + size * 0.20f, y + size * 0.54f,
               x + size * 0.44f, y + size * 0.75f, color, thickness);
    r.drawLine(x + size * 0.44f, y + size * 0.75f,
               x + size * 0.80f, y + size * 0.27f, color, thickness);
}

static Hsva rgbaToHsva(const float c[4]) {
    float r = c[0], g = c[1], b = c[2];
    float mx = (std::max)(r, (std::max)(g, b));
    float mn = (std::min)(r, (std::min)(g, b));
    float d = mx - mn;

    Hsva out{};
    out.v = mx;
    out.s = (mx <= 0.f) ? 0.f : (d / mx);
    out.a = c[3];

    if (d <= 1e-6f) {
        out.h = 0.f;
    } else if (mx == r) {
        out.h = fmodf((g - b) / d, 6.f) / 6.f;
    } else if (mx == g) {
        out.h = (((b - r) / d) + 2.f) / 6.f;
    } else {
        out.h = (((r - g) / d) + 4.f) / 6.f;
    }
    if (out.h < 0.f) out.h += 1.f;
    return out;
}

static void hsvaToRgba(const Hsva& in, float out[4]) {
    float h = in.h;
    float s = in.s;
    float v = in.v;
    h = h - floorf(h);
    s = (std::max)(0.f, (std::min)(s, 1.f));
    v = (std::max)(0.f, (std::min)(v, 1.f));

    float c = v * s;
    float hh = h * 6.f;
    float x = c * (1.f - fabsf(fmodf(hh, 2.f) - 1.f));
    float m = v - c;

    float r = 0.f, g = 0.f, b = 0.f;
    if (hh < 1.f) { r = c; g = x; }
    else if (hh < 2.f) { r = x; g = c; }
    else if (hh < 3.f) { g = c; b = x; }
    else if (hh < 4.f) { g = x; b = c; }
    else if (hh < 5.f) { r = x; b = c; }
    else { r = c; b = x; }

    out[0] = r + m;
    out[1] = g + m;
    out[2] = b + m;
    out[3] = (std::max)(0.f, (std::min)(in.a, 1.f));
}

static unsigned int hsvToArgb(float h, float s, float v, float a = 1.f) {
    Hsva hsva{h, s, v, a};
    float rgba[4];
    hsvaToRgba(hsva, rgba);
    auto b = [](float x) -> unsigned int {
        x = (std::max)(0.f, (std::min)(x, 1.f));
        return (unsigned int)(x * 255.f + 0.5f);
    };
    return (b(rgba[3]) << 24) | (b(rgba[0]) << 16) | (b(rgba[1]) << 8) | b(rgba[2]);
}

uint32_t Gui::hashId(const char* id) const {
    uint32_t h = 5381;
    for (const char* p = id; *p; ++p)
        h = ((h << 5) + h) + (unsigned char)*p;
    return h;
}

bool Gui::isMouseInRect(float x, float y, float w, float h) const {
    if (m_modalInputBlocked)
        return false;
    return m_mouseX >= x && m_mouseX < x + w && m_mouseY >= y && m_mouseY < y + h;
}
bool Gui::isBlockedByPopup(float x, float y, float w, float h) const {
    (void)x; (void)y; (void)w; (void)h;
    return m_modalPopupActive;
}

void Gui::clearPopupState() {
    m_dropdownOpen = false;
    m_dropdownId = -1;
    m_colorPopupId = -1;
    m_modalPopupActive = false;
    m_popupJustOpened = false;
    m_popupKind = PopupKind::None;
    m_modalPopupId = -1;
    m_modalPopupX = 0.f;
    m_modalPopupY = 0.f;
    m_modalPopupW = 0.f;
    m_modalPopupH = 0.f;
    m_popupComboItems = nullptr;
    m_popupComboCount = 0;
    m_popupComboCurrent = nullptr;
    m_popupColor = nullptr;
}

// ═════════════════════════════════════════════════════════════════════════════
// Frame lifecycle
// ═════════════════════════════════════════════════════════════════════════════

void Gui::beginFrame(Renderer& r, const FontAtlas& f, HWND hwnd) {
    m_renderer = &r;
    m_font     = &f;
    m_window   = hwnd;

    updateAnimationClock();

    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(hwnd, &pt);
    m_mouseX = (float)pt.x;
    m_mouseY = (float)pt.y;

    bool nowDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    m_mouseClicked  = nowDown && !m_mouseDown;
    m_mouseReleased = !nowDown && m_mouseDown;
    m_mouseDown = nowDown;

    m_modalInputBlocked = false;

    // Reset cursor for this frame
    m_cursorX = 0;
    m_cursorY = 0;
    m_itemWidth = 200.f;
}

void Gui::endFrame() {
    pollKeyboardTextInput();

    // If the active item was released, clear it
    if (m_mouseReleased && m_activeItem >= 0)
        m_activeItem = -1;

    drawPopupOverlay();
    m_popupJustOpened = false;
}

void Gui::setScale(float scale, float contentScale) {
    m_scale = (std::max)(1.0f, scale);
    m_contentScale = (std::max)(1.0f, contentScale);
}

// ═════════════════════════════════════════════════════════════════════════════
// Cursor
// ═════════════════════════════════════════════════════════════════════════════

void Gui::setCursor(float x, float y) { m_cursorX = x; m_cursorY = y; }
void Gui::advanceY(float dy)          { m_cursorY += dy; }
void Gui::setItemWidth(float w)       { m_itemWidth = w; }

// ═════════════════════════════════════════════════════════════════════════════
// label
// ═════════════════════════════════════════════════════════════════════════════

void Gui::label(const char* text, unsigned int color, float fontSize) {
    const float scaledSize = fontSize * m_contentScale;
    m_renderer->drawText(*m_font, m_cursorX, m_cursorY, text, color, scaledSize);
    m_cursorY += scaledSize + 4.f * m_scale;
}

void Gui::label(const wchar_t* text, unsigned int color, float fontSize) {
    const float scaledSize = fontSize * m_contentScale;
    m_renderer->drawTextW(*m_font, m_cursorX, m_cursorY, text, color, scaledSize);
    m_cursorY += scaledSize + 4.f * m_scale;
}

// ═════════════════════════════════════════════════════════════════════════════
// button
// ═════════════════════════════════════════════════════════════════════════════

bool Gui::button(const char* label, float w, float h) {
    return accentButton(label, label, w, h);
}

bool Gui::accentButton(const char* id, const char* text, float w, float h) {
    uint32_t idh = hashId(id);
    const float s = m_scale;
    const float c = m_contentScale;
    float bw = (w > 0.f) ? w * s : m_itemWidth;
    float x = m_cursorX, y = m_cursorY;

    h = (std::max)(h, 46.f) * s;
    bool hovered = !isBlockedByPopup(x, y, bw, h) && isMouseInRect(x, y, bw, h);
    float hovA = animValue(idh ^ 0xA11CEu, hovered ? 1.f : 0.f, 0.22f);
    float prsA = animValue(idh ^ 0xA11CFu, (m_activeItem == (int)idh && m_mouseDown) ? 1.f : 0.f, 0.28f);
    unsigned int bg = lerpColor(Theme::ACCENT, 0xFF8B89F8, hovA * 0.65f);
    bg = lerpColor(bg, 0xFF6C6AF0, prsA * 0.8f);

    m_renderer->drawRoundedFilledRect(x, y, bw, h, bg, 11.f * s);
    const float fontSize = 16.f * c;
    const float tw = m_renderer
        ? measureRenderedTextWidth(*m_renderer, *m_font, text, fontSize)
        : measureTextWidth(*m_font, text, fontSize);
    const float tx = x + (bw - tw) * 0.5f;
    const float ty = m_renderer
        ? renderedTextCenterY(*m_renderer, *m_font, y, h, text, fontSize)
        : textVisualCenterY(*m_font, y, h, text, fontSize);
    m_renderer->drawText(*m_font, tx, ty, text, 0xFFFFFFFF, fontSize);

    if (hovered && m_mouseClicked) m_activeItem = (int)idh;
    bool clicked = (m_activeItem == (int)idh && m_mouseReleased && hovered);
    if (m_activeItem == (int)idh && m_mouseReleased) m_activeItem = -1;

    m_cursorY += h + Theme::ITEM_SPACING * s;
    return clicked;
}

bool Gui::textField(const char* id, char* buf, std::size_t bufSize, float h, bool password) {
    if (!buf || bufSize == 0)
        return false;

    const uint32_t idh = hashId(id);
    const float s = m_scale;
    const float c = m_contentScale;
    const float x = m_cursorX;
    const float y = m_cursorY;
    const float w = m_itemWidth;
    h = (std::max)(h, 38.f) * s;

    const bool hovered = !isBlockedByPopup(x, y, w, h) && isMouseInRect(x, y, w, h);
    const bool focused = (m_activeTextFieldId == idh);
    const float hovA = animValue(idh ^ 0x7E57u, (hovered || focused) ? 1.f : 0.f, 0.22f);

    unsigned int bg = lerpColor(0xFF0D0E14, 0xFF12141E, hovA);
    m_renderer->drawRoundedFilledRect(x, y, w, h, bg, 10.f * s);
    m_renderer->drawRoundedRect(x, y, w, h,
        focused ? Theme::ACCENT : 0xFF1D1F2D, 10.f * s, (std::max)(1.f, s));

    if (hovered && m_mouseClicked) {
        m_activeTextFieldId = idh;
        m_activeTextFieldBuf = buf;
        m_activeTextFieldSize = bufSize;
    }

    const float padX = 14.f * s;
    const float fontSize = 15.f * c;
    const float textY = textControlCenterY(*m_font, y, h, "Ag", fontSize);
    std::string display = buf;
    if (password && !display.empty())
        display.assign(display.size(), '*');

    const unsigned int textCol = display.empty() ? Theme::TEXT_MUTED : Theme::TEXT;
    if (!display.empty())
        m_renderer->drawText(*m_font, x + padX, textY, display.c_str(), textCol, fontSize);

    if (focused) {
        m_textCaretBlink += g_animFrameDt;
        if (fmodf(m_textCaretBlink, 1.f) < 0.55f) {
            const float tw = (!display.empty() && m_renderer)
                ? measureRenderedTextWidth(*m_renderer, *m_font, display.c_str(), fontSize)
                : 0.f;
            const float caretX = x + padX + tw + 1.f;
            m_renderer->drawFilledRect(caretX, y + 10.f * s, (std::max)(1.f, s), h - 20.f * s, Theme::ACCENT);
        }
    }

    m_cursorY += h + Theme::ITEM_SPACING * s;
    return focused;
}

static void sanitizeAsciiTokenInPlace(char* buf, std::size_t bufSize) {
    if (!buf || bufSize == 0)
        return;

    std::size_t w = 0;
    for (std::size_t r = 0; buf[r] != '\0' && r + 1 < bufSize; ++r) {
        const unsigned char c = static_cast<unsigned char>(buf[r]);
        if (std::isspace(c) != 0)
            continue;
        if (c < 0x21 || c > 0x7E)
            continue;
        buf[w++] = static_cast<char>(c);
    }
    buf[w] = '\0';
}

void Gui::pasteClipboardText() {
    if (!m_activeTextFieldBuf || m_activeTextFieldSize == 0)
        return;
    if (!OpenClipboard(m_window))
        return;

    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (data) {
        const wchar_t* wtext = static_cast<const wchar_t*>(GlobalLock(data));
        if (wtext) {
            std::string chunk;
            chunk.reserve(128);
            for (const wchar_t* p = wtext; *p; ++p) {
                if (*p == L'\r' || *p == L'\n' || *p == L'\t' || *p == L' ')
                    continue;
                if (*p == 0xFEFF || *p == 0x200B || *p == 0x200C || *p == 0x200D || *p == 0x2060)
                    continue;

                char utf8[8]{};
                const int n = WideCharToMultiByte(CP_UTF8, 0, p, 1, utf8, 8, nullptr, nullptr);
                if (n <= 0)
                    continue;
                chunk.append(utf8, static_cast<std::size_t>(n));
            }
            GlobalUnlock(data);

            const std::size_t used = std::strlen(m_activeTextFieldBuf);
            const std::size_t room = m_activeTextFieldSize > 0 ? m_activeTextFieldSize - 1 : 0;
            const std::size_t copyLen = (std::min)(chunk.size(), room > used ? room - used : 0);
            if (copyLen > 0) {
                std::memcpy(m_activeTextFieldBuf + used, chunk.data(), copyLen);
                m_activeTextFieldBuf[used + copyLen] = '\0';
            }
            sanitizeAsciiTokenInPlace(m_activeTextFieldBuf, m_activeTextFieldSize);
        }
    }
    CloseClipboard();
}

void Gui::pollKeyboardTextInput() {
    if (!m_activeTextFieldBuf || m_activeTextFieldSize == 0)
        return;

    bool nowDown[256]{};
    for (int vk = 0; vk < 256; ++vk)
        nowDown[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;

    auto keyEdge = [&](int vk) -> bool {
        const std::size_t idx = static_cast<std::size_t>(vk & 0xFF);
        return nowDown[idx] && !m_keyPrev[idx];
    };

    if ((nowDown[VK_CONTROL] || nowDown[VK_LCONTROL] || nowDown[VK_RCONTROL]) && keyEdge('V')) {
        for (int vk = 0; vk < 256; ++vk)
            m_keyPrev[vk] = nowDown[vk];
        pasteClipboardText();
        return;
    }

    if (keyEdge(VK_BACK)) {
        onKeyDown(VK_BACK);
        for (int vk = 0; vk < 256; ++vk)
            m_keyPrev[vk] = nowDown[vk];
        return;
    }
    if (keyEdge(VK_ESCAPE)) {
        onKeyDown(VK_ESCAPE);
        for (int vk = 0; vk < 256; ++vk)
            m_keyPrev[vk] = nowDown[vk];
        return;
    }
    if (keyEdge(VK_RETURN)) {
        onKeyDown(VK_RETURN);
        for (int vk = 0; vk < 256; ++vk)
            m_keyPrev[vk] = nowDown[vk];
        return;
    }

    if (nowDown[VK_CONTROL] || nowDown[VK_LCONTROL] || nowDown[VK_RCONTROL]
        || nowDown[VK_MENU] || nowDown[VK_LMENU] || nowDown[VK_RMENU]
        || nowDown[VK_LWIN] || nowDown[VK_RWIN]) {
        for (int vk = 0; vk < 256; ++vk)
            m_keyPrev[vk] = nowDown[vk];
        return;
    }

    BYTE keyboardState[256]{};
    if (GetKeyboardState(keyboardState)) {
        const HKL layout = GetKeyboardLayout(0);
        for (int vk = 1; vk < 256; ++vk) {
            if (!keyEdge(vk))
                continue;
            if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT
                || vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL
                || vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU
                || vk == VK_TAB || vk == VK_CAPITAL)
                continue;

            const UINT scan = MapVirtualKey(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
            if (scan == 0)
                continue;

            wchar_t chars[4]{};
            const int count = ToUnicodeEx(static_cast<UINT>(vk), scan, keyboardState, chars, 4, 0, layout);
            if (count == 1 && chars[0] >= 32 && chars[0] != 127)
                onChar(chars[0]);
        }
    }

    for (int vk = 0; vk < 256; ++vk)
        m_keyPrev[vk] = nowDown[vk];
}

void Gui::onChar(wchar_t ch) {
    if (!m_activeTextFieldBuf || m_activeTextFieldSize == 0)
        return;
    if (ch < 32 || ch == 127)
        return;

    char utf8[8]{};
    const int n = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, utf8, 8, nullptr, nullptr);
    if (n <= 0)
        return;

    const std::size_t len = std::strlen(m_activeTextFieldBuf);
    if (len + static_cast<std::size_t>(n) >= m_activeTextFieldSize)
        return;

    std::memcpy(m_activeTextFieldBuf + len, utf8, static_cast<std::size_t>(n));
    m_activeTextFieldBuf[len + static_cast<std::size_t>(n)] = '\0';
}

void Gui::onKeyDown(WPARAM vk) {
    if (!m_activeTextFieldBuf || m_activeTextFieldSize == 0)
        return;

    const std::size_t len = std::strlen(m_activeTextFieldBuf);
    if (vk == VK_BACK) {
        if (len == 0)
            return;
        m_activeTextFieldBuf[len - 1] = '\0';
        return;
    }
    if (vk == VK_ESCAPE) {
        m_activeTextFieldId = 0;
        m_activeTextFieldBuf = nullptr;
        m_activeTextFieldSize = 0;
        return;
    }
    if (vk == VK_RETURN) {
        m_activeTextFieldId = 0;
        m_activeTextFieldBuf = nullptr;
        m_activeTextFieldSize = 0;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// checkbox
// ═════════════════════════════════════════════════════════════════════════════

bool Gui::checkbox(const char* label, bool* value) {
    return toggleCheckbox(label, label, value, 38.f);
}

bool Gui::toggleCheckbox(const char* id, const char* label, bool* value, float h) {
    uint32_t idh = hashId(id);
    const float s = m_scale;
    const float c = m_contentScale;
    float x = m_cursorX, y = m_cursorY;
    float w = m_itemWidth;
    h = (std::max)(h, 44.f) * s;
    bool hovered = !isBlockedByPopup(x, y, w, h) && isMouseInRect(x, y, w, h);

    float hovA = animValue(idh ^ 0xC4B000u, hovered ? 1.f : 0.f, 0.2f);
    float onA = animValue(idh ^ 0xC4B001u, *value ? 1.f : 0.f, 0.32f);
    float pressA = animValue(idh ^ 0xC4B002u, (hovered && m_mouseDown) ? 1.f : 0.f, 0.4f);

    unsigned int rowBg = lerpColor(Theme::SURFACE, 0xFF181927, hovA * 0.35f + onA * 0.12f + pressA * 0.18f);
    if (hovA > 0.01f || onA > 0.01f) {
        m_renderer->drawRoundedFilledRect(
            x - 1.f * s,
            y - 1.f * s,
            w + 2.f * s,
            h + 2.f * s,
            withAlpha(Theme::ACCENT, 0.03f + hovA * 0.03f + onA * 0.05f),
            14.f * s
        );
    }
    m_renderer->drawRoundedFilledRect(x, y, w, h, rowBg, 11.f * s);
    m_renderer->drawRoundedRect(x, y, w, h, 0xFF1D1F2D, 11.f * s, (std::max)(1.f, s));

    m_renderer->drawText(*m_font, x + 16.f * s, textControlCenterY(*m_font, y, h, label, 16.f * c), label, Theme::TEXT, 16.f * c);

    const float box = 22.f * c;
    float bx = x + w - box - 16.f * s;
    float by = y + (h - box) * 0.5f;
    unsigned int boxBg = lerpColor(lerpColor(0xFF1B1D2B, 0xFF141622, pressA * 0.25f), Theme::ACCENT, onA);
    unsigned int boxBr = lerpColor(0xFF23253A, Theme::ACCENT, onA * 0.7f);
    if (onA > 0.01f || hovA > 0.01f) {
        m_renderer->drawFilledCircle(bx + box * 0.5f, by + box * 0.5f, box * 0.46f,
                                     withAlpha(Theme::ACCENT, 0.05f + hovA * 0.04f + onA * 0.14f));
    }
    m_renderer->drawRoundedFilledRect(bx, by, box, box, boxBg, 5.5f * s);
    m_renderer->drawRoundedRect(bx, by, box, box, boxBr, 5.5f * s, (std::max)(1.f, s));
    if (onA > 0.05f)
        drawCheckGlyph(*m_renderer, bx, by, box, withAlpha(0xFFFFFFFF, onA), 1.8f * c);

    if (hovered && m_mouseClicked) m_activeItem = (int)idh;
    bool clicked = (m_activeItem == (int)idh && m_mouseReleased && hovered);
    if (clicked) *value = !*value;
    if (m_activeItem == (int)idh && m_mouseReleased) m_activeItem = -1;

    m_cursorY += h + Theme::ITEM_SPACING * s;
    return clicked;
}

// ═════════════════════════════════════════════════════════════════════════════
// sliderFloat / sliderInt
// ═════════════════════════════════════════════════════════════════════════════

void Gui::drawSliderGrab(float cx, float cy, float size, unsigned int color) {
    m_renderer->drawRoundedFilledRect(cx - size * 0.5f, cy - size * 0.25f, size, size * 0.5f, color, 2.f);
}

static bool keyPressedOnce(int vk) {
    static bool prev[256]{};
    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool edge = down && !prev[vk];
    prev[vk] = down;
    return edge;
}

static void appendSliderEditChar(char* buf, std::size_t cap, char ch) {
    const std::size_t len = std::strlen(buf);
    if (len + 1 >= cap)
        return;
    buf[len] = ch;
    buf[len + 1] = '\0';
}

static bool parseSliderEditValue(const char* text, float& out) {
    if (!text || !*text)
        return false;
    char* end = nullptr;
    const float v = std::strtof(text, &end);
    if (end == text || !std::isfinite(v))
        return false;
    out = v;
    return true;
}

bool Gui::sliderFloat(const char* id, float* value, float min, float max, const char* fmt) {
    return sliderFloatValue(id, id, value, min, max, fmt, 38.f);
}

bool Gui::sliderFloatValue(const char* id, const char* label, float* value,
                           float min, float max, const char* fmt, float h) {
    uint32_t idh = hashId(id);
    const int editId = static_cast<int>(idh);
    const float s = m_scale;
    const float c = m_contentScale;
    float x = m_cursorX, y = m_cursorY;
    float w = m_itemWidth;
    h = (std::max)(h, 72.f) * s;

    const bool blockedByOtherEdit = m_sliderEditActive && m_sliderEditId != editId;

    bool hovered = !blockedByOtherEdit && !isBlockedByPopup(x, y, w, h) && isMouseInRect(x, y, w, h);
    const float hovA = hovered ? 1.f : 0.f;
    bool active = !blockedByOtherEdit && (m_activeItem == (int)idh && m_mouseDown && !m_sliderEditActive);
    const float activeA = active ? 1.f : 0.f;

    m_renderer->drawRoundedFilledRect(x, y, w, h, lerpColor(Theme::SURFACE, 0xFF171928, hovA * 0.32f + activeA * 0.14f), 12.f * s);
    m_renderer->drawRoundedRect(x, y, w, h, 0xFF1D1F2D, 12.f * s, (std::max)(1.f, s));

    const unsigned int labelCol = blockedByOtherEdit ? Theme::TEXT_MUTED : Theme::TEXT;
    const unsigned int valueCol = blockedByOtherEdit ? Theme::TEXT_MUTED : Theme::ACCENT;

    float labelX = x + 16.f * s;
    float labelY = y + 16.f * s;
    m_renderer->drawText(*m_font, labelX, labelY, label, labelCol, 16.f * c);

    const bool editing = m_sliderEditActive && m_sliderEditId == editId;
    char buf[64];
    if (editing) {
        std::snprintf(buf, sizeof(buf), "%s", m_sliderEditBuf);
    } else {
        snprintf(buf, sizeof(buf), fmt, *value);
    }

    float valueTextW = measureTextWidth(*m_font, buf, 15.f * c);
    float valueBoxW = (std::max)(38.f * s, valueTextW + 14.f * s);
    float valueBoxH = 26.f * c;
    float vx = x + w - valueBoxW - 14.f * s;
    float vy = y + 13.f * s;
    const bool valueHovered = isMouseInRect(vx, vy, valueBoxW, valueBoxH);

    unsigned int valueBg = editing ? 0xFF2A2548 : (valueHovered ? 0xFF2A2D45 : 0xFF21233A);
    unsigned int valueBorder = editing ? Theme::ACCENT : 0xFF21233A;
    m_renderer->drawRoundedFilledRect(vx, vy, valueBoxW, valueBoxH, valueBg, 7.f * s);
    if (editing || valueHovered)
        m_renderer->drawRoundedRect(vx, vy, valueBoxW, valueBoxH, valueBorder, 7.f * s, (std::max)(1.f, s));
    m_renderer->drawText(*m_font, textVisualCenterX(*m_font, vx, valueBoxW, buf, 15.f * c), textCompactCenterY(*m_font, vy, valueBoxH, buf, 15.f * c), buf, valueCol, 15.f * c);

    float tx = x + 16.f * s;
    float tw = w - 32.f * s;
    float th = 4.f * s;
    float ty = y + h - 22.f * s;

    m_renderer->drawRoundedFilledRect(tx, ty, tw, th, 0xFF22253A, 2.f * s);
    float targetT = (*value - min) / (max - min);
    if (targetT < 0.f) targetT = 0.f;
    if (targetT > 1.f) targetT = 1.f;
    const float t = targetT;
    m_renderer->drawRoundedFilledRect(tx, ty, tw * t, th, blockedByOtherEdit ? Theme::TEXT_MUTED : Theme::ACCENT, 2.f * s);

    float gx = tx + tw * t;
    float gy = ty + th * 0.5f;
    float thumbR = 7.f * c + activeA * 1.2f * s;
    m_renderer->drawFilledCircle(gx, gy, thumbR, 0xFFFFFFFF);
    m_renderer->drawCircle(gx, gy, thumbR, Theme::ACCENT, (std::max)(1.f, s));
    m_renderer->drawFilledCircle(gx, gy, 2.1f * c, Theme::ACCENT);

    bool changed = false;

    if (editing) {
        if (keyPressedOnce(VK_ESCAPE)) {
            m_sliderEditActive = false;
            m_sliderEditId = -1;
            m_sliderEditValue = nullptr;
        } else if (keyPressedOnce(VK_RETURN)) {
            float parsed = 0.f;
            if (parseSliderEditValue(m_sliderEditBuf, parsed)) {
                parsed = (std::max)(min, (std::min)(max, parsed));
                if (*value != parsed) {
                    *value = parsed;
                    changed = true;
                }
            }
            m_sliderEditActive = false;
            m_sliderEditId = -1;
            m_sliderEditValue = nullptr;
        } else {
            if (keyPressedOnce(VK_BACK)) {
                const std::size_t len = std::strlen(m_sliderEditBuf);
                if (len > 0)
                    m_sliderEditBuf[len - 1] = '\0';
            }
            for (int vk = '0'; vk <= '9'; ++vk) {
                if (keyPressedOnce(vk))
                    appendSliderEditChar(m_sliderEditBuf, sizeof(m_sliderEditBuf), (char)vk);
            }
            if (keyPressedOnce(VK_OEM_PERIOD) || keyPressedOnce(VK_DECIMAL))
                appendSliderEditChar(m_sliderEditBuf, sizeof(m_sliderEditBuf), '.');
            if (keyPressedOnce(VK_SUBTRACT) || keyPressedOnce(VK_OEM_MINUS))
                appendSliderEditChar(m_sliderEditBuf, sizeof(m_sliderEditBuf), '-');
        }
    } else if (!blockedByOtherEdit) {
        if (valueHovered && m_mouseClicked) {
            m_sliderEditActive = true;
            m_sliderEditId = editId;
            m_sliderEditMin = min;
            m_sliderEditMax = max;
            m_sliderEditValue = value;
            std::snprintf(m_sliderEditBuf, sizeof(m_sliderEditBuf), fmt, *value);
            m_activeItem = -1;
        } else {
            if (hovered && m_mouseClicked && !valueHovered)
                m_activeItem = (int)idh;
            if (m_activeItem == (int)idh && m_mouseDown) {
                float nt = (m_mouseX - tx) / tw;
                if (nt < 0.f) nt = 0.f;
                if (nt > 1.f) nt = 1.f;
                *value = min + nt * (max - min);
            }
            changed = (m_activeItem == (int)idh && m_mouseReleased);
            if (m_activeItem == (int)idh && m_mouseReleased)
                m_activeItem = -1;
        }
    }

    m_cursorY += h + Theme::ITEM_SPACING * s;
    return changed;
}

bool Gui::sliderInt(const char* id, int* value, int min, int max, const char* fmt) {
    float fv = (float)*value;
    bool changed = sliderFloat(id, &fv, (float)min, (float)max, fmt);
    *value = (int)(fv + 0.5f);
    if (*value < min) *value = min;
    if (*value > max) *value = max;
    return changed;
}

// ═════════════════════════════════════════════════════════════════════════════
// comboBox
// ═════════════════════════════════════════════════════════════════════════════

bool Gui::comboBox(const char* id, const char* const* items, int count, int* current) {
    uint32_t idh = hashId(id);
    const float s = m_scale;
    const float c = m_contentScale;

    // Apply deferred popup result from previous frame's drawPopupOverlay.
    // The popup runs in endFrame AFTER this function returns, so *current
    // (which may be a local) is a dangling pointer by then. Instead we store
    // the chosen index in drawPopupOverlay and apply it here, next frame,
    // where *current is guaranteed valid. Returns true on change so the caller
    // can persist the value to config.
    bool changed = false;
    if (m_comboPopupResultValid && m_comboPopupResultId == (int)idh) {
        *current = m_comboPopupResult;
        m_comboPopupResultValid = false;
        changed = true;
    }

    float x = m_cursorX, y = m_cursorY;
    float w = m_itemWidth, h = 48.f * s;
    bool hovered = !isBlockedByPopup(x, y, w, h) && isMouseInRect(x, y, w, h);
    float hovA = animValue(idh ^ 0xD22000u, hovered ? 1.f : 0.f, 0.2f);
    float openA = animValue(idh ^ 0xD22001u, (m_dropdownOpen && m_dropdownId == (int)idh) ? 1.f : 0.f, 0.24f);

    unsigned int bg = lerpColor(Theme::SURFACE, 0xFF171928, hovA * 0.35f + openA * 0.16f);
    m_renderer->drawRoundedFilledRect(x, y, w, h, bg, 12.f * s);
    m_renderer->drawRoundedRect(x, y, w, h, 0xFF1D1F2D, 12.f * s, (std::max)(1.f, s));
    m_renderer->drawText(*m_font, x + 16.f * s, textControlCenterY(*m_font, y, h, items[*current], 15.f * c), items[*current], Theme::TEXT, 15.f * c);

    float arrowX = x + w - 20.f * s, arrowY = y + h * 0.5f + 1.f * s;
    float arrowWing = 4.4f * c;
    float arrowLift = 3.2f * c;
    float arrowApexY = arrowY + (1.f - openA * 2.f) * arrowLift;
    float arrowSideY = arrowY - (1.f - openA * 2.f) * (arrowLift * 0.38f);
    unsigned int arrowColor = lerpColor(Theme::TEXT_MUTED, Theme::ACCENT, hovA * 0.2f + openA * 0.35f);
    m_renderer->drawLine(arrowX - arrowWing, arrowSideY, arrowX, arrowApexY, arrowColor, 1.25f * s);
    m_renderer->drawLine(arrowX + arrowWing, arrowSideY, arrowX, arrowApexY, arrowColor, 1.25f * s);

    if (hovered && m_mouseClicked) {
        m_dropdownOpen = !m_dropdownOpen;
        m_dropdownId = (int)idh;
        if (m_dropdownOpen)
            m_popupJustOpened = true;
    }

    // Dropdown list
    if (m_dropdownOpen && m_dropdownId == (int)idh) {
        float listY = y + h + 6.f * s;
        float ih = 34.f * s;
        float listH = count * ih + 10.f * s;
        m_modalPopupActive = true;
        m_popupKind = PopupKind::Combo;
        m_modalPopupId = (int)idh;
        m_modalPopupX = x;
        m_modalPopupY = listY;
        m_modalPopupW = w;
        m_modalPopupH = listH;
        m_popupComboItems = items;
        m_popupComboCount = count;
        m_popupComboCurrent = current;
    } else if (!m_dropdownOpen && m_modalPopupId == (int)idh && m_popupKind == PopupKind::Combo) {
        clearPopupState();
    }

    m_cursorY += h + Theme::ITEM_SPACING * s;
    return changed;
}

bool Gui::comboField(const char* id, const char* label, const char* const* items, int count, int* current, float h) {
    uint32_t idh = hashId(id);
    const float s = m_scale;
    const float c = m_contentScale;

    bool changed = false;
    if (m_comboPopupResultValid && m_comboPopupResultId == (int)idh) {
        *current = m_comboPopupResult;
        m_comboPopupResultValid = false;
        changed = true;
    }

    float x = m_cursorX, y = m_cursorY;
    float w = m_itemWidth;
    h = (std::max)(h, 48.f) * s;
    bool hovered = !isBlockedByPopup(x, y, w, h) && isMouseInRect(x, y, w, h);
    float hovA = animValue(idh ^ 0xD22800u, hovered ? 1.f : 0.f, 0.2f);
    float openA = animValue(idh ^ 0xD22801u, (m_dropdownOpen && m_dropdownId == (int)idh) ? 1.f : 0.f, 0.24f);

    unsigned int bg = lerpColor(Theme::SURFACE, 0xFF171928, hovA * 0.45f + openA * 0.2f);
    m_renderer->drawRoundedFilledRect(x, y, w, h, bg, 12.f * s);
    m_renderer->drawRoundedRect(x, y, w, h, 0xFF1D1F2D, 12.f * s, (std::max)(1.f, s));

    float labelW = measureTextWidth(*m_font, label, 12.f * c);
    float labelX = x + 16.f * s;
    float dividerX = labelX + labelW + 16.f * s;
    m_renderer->drawText(*m_font, labelX, textControlCenterY(*m_font, y, h, label, 12.f * c), label, Theme::TEXT_MUTED, 12.f * c);
    m_renderer->drawFilledRect(dividerX, y + 14.f * s, (std::max)(1.f, s), h - 28.f * s, 0xFF222538);
    m_renderer->drawText(*m_font, dividerX + 14.f * s, textControlCenterY(*m_font, y, h, items[*current], 15.f * c), items[*current], Theme::TEXT, 15.f * c);

    float arrowX = x + w - 20.f * s, arrowY = y + h * 0.5f + 1.f * s;
    float arrowWing = 4.4f * c;
    float arrowLift = 3.2f * c;
    float arrowApexY = arrowY + (1.f - openA * 2.f) * arrowLift;
    float arrowSideY = arrowY - (1.f - openA * 2.f) * (arrowLift * 0.38f);
    unsigned int arrowColor = lerpColor(Theme::TEXT_MUTED, Theme::ACCENT, hovA * 0.2f + openA * 0.35f);
    m_renderer->drawLine(arrowX - arrowWing, arrowSideY, arrowX, arrowApexY, arrowColor, 1.25f * s);
    m_renderer->drawLine(arrowX + arrowWing, arrowSideY, arrowX, arrowApexY, arrowColor, 1.25f * s);

    if (hovered && m_mouseClicked) {
        m_dropdownOpen = !m_dropdownOpen;
        m_dropdownId = (int)idh;
        if (m_dropdownOpen)
            m_popupJustOpened = true;
    }

    if (m_dropdownOpen && m_dropdownId == (int)idh) {
        float listY = y + h + 6.f * s;
        float ih = 34.f * s;
        float listH = count * ih + 10.f * s;
        m_modalPopupActive = true;
        m_popupKind = PopupKind::Combo;
        m_modalPopupId = (int)idh;
        m_modalPopupX = x;
        m_modalPopupY = listY;
        m_modalPopupW = w;
        m_modalPopupH = listH;
        m_popupComboItems = items;
        m_popupComboCount = count;
        m_popupComboCurrent = current;
    } else if (!m_dropdownOpen && m_modalPopupId == (int)idh && m_popupKind == PopupKind::Combo) {
        clearPopupState();
    }

    m_cursorY += h + Theme::ITEM_SPACING * s;
    return changed;
}

// ═════════════════════════════════════════════════════════════════════════════
// keybindButton
// ═════════════════════════════════════════════════════════════════════════════

bool Gui::keybindButton(const char* label, int* targetKey) {
    return keybindCard(label, label, targetKey, 38.f);
}

bool Gui::keybindCard(const char* id, const char* label, int* targetKey, float h) {
    uint32_t idh = hashId(id);
    const float s = m_scale;
    const float c = m_contentScale;
    float x = m_cursorX, y = m_cursorY;
    float w = m_itemWidth;
    h = (std::max)(h, 58.f) * s;

    // Static capture state (only one capture at a time across all keybindCards)
    static int captureId = -1;
    static bool prevDown[256] = {};
    static int settleCount = 0;

    bool isCapturing = (captureId == (int)idh);

    bool hov = !isBlockedByPopup(x, y, w, h) && isMouseInRect(x, y, w, h);
    float hovA = animValue(idh ^ 0xB10D00u, hov ? 1.f : 0.f, 0.2f);
    unsigned int bg = lerpColor(Theme::SURFACE, 0xFF171928, hovA * 0.45f);
    m_renderer->drawRoundedFilledRect(x, y, w, h, bg, 12.f * s);
    m_renderer->drawRoundedRect(x, y, w, h, 0xFF1D1F2D, 12.f * s, (std::max)(1.f, s));

    m_renderer->drawText(*m_font, x + 16.f * s, textControlCenterY(*m_font, y, h, label, 16.f * c), label, Theme::TEXT, 16.f * c);

    std::string chipText = isCapturing ? std::string("PRESS") : upperAscii(vkDisplayName(*targetKey));
    float chipTextW = measureTextWidth(*m_font, chipText.c_str(), 13.f * c);
    float chipW = (std::max)(64.f * s, chipTextW + 24.f * s);
    float chipH = 30.f * c;
    float chipX = x + w - chipW - 14.f * s;
    float chipY = y + (h - chipH) * 0.5f;
    unsigned int chipBg = isCapturing ? withAlpha(Theme::ACCENT, 0.18f) : 0xFF1A1C2C;
    unsigned int chipBr = isCapturing ? withAlpha(Theme::ACCENT, 0.65f) : 0xFF262941;
    unsigned int chipTc = isCapturing ? Theme::ACCENT : Theme::TEXT_MUTED;
    m_renderer->drawRoundedFilledRect(chipX, chipY, chipW, chipH, chipBg, 7.f * s);
    m_renderer->drawRoundedRect(chipX, chipY, chipW, chipH, chipBr, 7.f * s, (std::max)(1.f, s));
    m_renderer->drawText(*m_font, textVisualCenterX(*m_font, chipX, chipW, chipText.c_str(), 13.f * c),
                         textCompactCenterY(*m_font, chipY, chipH, chipText.c_str(), 13.f * c), chipText.c_str(), chipTc, 13.f * c);

    if (isCapturing) {
        if (settleCount > 0) {
            --settleCount;
            if (settleCount == 0) {
                for (int vk = 1; vk < 256; ++vk)
                    prevDown[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
            }
            m_cursorY += h + Theme::ITEM_SPACING * s;
            return false;
        }

        for (int vk = 1; vk < 256; ++vk) {
            bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
            bool pressed = down && !prevDown[vk];
            prevDown[vk] = down;
            if (!pressed) continue;

            if (vk == VK_ESCAPE || vk == VK_DELETE || vk == VK_BACK) {
                *targetKey = 0;
            } else {
                *targetKey = vk;
            }

            captureId = -1;
            settleCount = 0;
            memset(prevDown, 0, sizeof(prevDown));
            break;
        }
    } else if (hov && m_mouseClicked) {
        captureId = (int)idh;
        settleCount = 2; // skip 2 frames for LMB release
        memset(prevDown, 0, sizeof(prevDown));
    }

    m_cursorY += h + Theme::ITEM_SPACING * s;
    return false;
}

// ═════════════════════════════════════════════════════════════════════════════
// separator / dummy
// ═════════════════════════════════════════════════════════════════════════════

void Gui::separator() {
    const float s = m_scale;
    m_renderer->drawFilledRect(m_cursorX, m_cursorY + 8.f * s, m_itemWidth, (std::max)(1.f, s), 0xFF1D1F2D);
    m_cursorY += 22.f * s;
}

void Gui::dummy(float h) {
    m_cursorY += h * m_scale;
}

// ═════════════════════════════════════════════════════════════════════════════
// colorEdit4
// ═════════════════════════════════════════════════════════════════════════════

void Gui::colorEdit4(const char* id, float color[4]) {
    uint32_t idh = hashId(id);
    const float s = m_scale;
    const float c = m_contentScale;
    float x = m_cursorX, y = m_cursorY;
    float w = m_itemWidth;
    const float rowH = 38.f * s;
    bool hovered = !isBlockedByPopup(x, y, w, rowH) && isMouseInRect(x, y, w, rowH);

    float hovA = animValue(idh ^ 0xC01000u, hovered ? 1.f : 0.f, 0.2f);
    unsigned int rowBg = lerpColor(Theme::SURFACE, 0xFF17171D, hovA);
    m_renderer->drawRoundedFilledRect(x, y, w, rowH, rowBg, 10.f * s);
    m_renderer->drawRoundedRect(x, y, w, rowH, Theme::BORDER, 10.f * s, (std::max)(1.f, s));

    auto b = [](float v) -> unsigned int {
        if (v < 0.f) v = 0.f;
        if (v > 1.f) v = 1.f;
        return (unsigned int)(v * 255.f + 0.5f);
    };
    unsigned int argb = (b(color[3]) << 24) | (b(color[0]) << 16) | (b(color[1]) << 8) | b(color[2]);

    m_renderer->drawText(*m_font, x + 10.f * s, textControlCenterY(*m_font, y, rowH, id, 16.f * c), id, Theme::TEXT_MUTED, 16.f * c);

    char rgbaTxt[48];
    snprintf(rgbaTxt, sizeof(rgbaTxt), "%u %u %u %u", b(color[0]), b(color[1]), b(color[2]), b(color[3]));
    float rgbaW = measureTextWidth(*m_font, rgbaTxt, 14.f * c);
    float swatch = 22.f * c;
    float sx = x + w - swatch - 10.f * s;
    float sy = y + (rowH - swatch) * 0.5f;
    m_renderer->drawText(*m_font, sx - rgbaW - 10.f * s, textControlCenterY(*m_font, y, rowH, rgbaTxt, 14.f * c), rgbaTxt, Theme::TEXT_MUTED, 14.f * c);

    m_renderer->drawRoundedFilledRect(sx, sy, swatch, swatch, argb, 6.f * s);
    m_renderer->drawRoundedRect(sx, sy, swatch, swatch, Theme::BORDER, 6.f * s, (std::max)(1.f, s));

    bool rowClicked = hovered && m_mouseClicked;
    if (rowClicked) {
        bool opening = (m_colorPopupId != (int)idh);
        m_colorPopupId = (m_colorPopupId == (int)idh) ? -1 : (int)idh;
        if (opening)
            m_popupJustOpened = true;
    }

    bool opened = (m_colorPopupId == (int)idh);
    if (opened) {
        const float popupPad = 12.f * s;
        const float svSize = 190.f * s;
        const float sliderH = 16.f * s;
        const float sliderGap = 10.f * s;
        const float popupH = popupPad + svSize + sliderGap + sliderH + 8.f * s + sliderH + 12.f * s + 36.f * s + popupPad;
        const float popupW = popupPad * 2.f + svSize;
        float px = x + w + 30.f * s;
        float py = sy - 10.f * s;

        const float screenW = (float)m_renderer->screenWidth();
        const float screenH = (float)m_renderer->screenHeight();
        if (px + popupW > screenW - 6.f * s)
            px = screenW - popupW - 6.f * s;
        if (py < 6.f * s) py = 6.f * s;
        if (py + popupH > screenH - 6.f * s) py = screenH - popupH - 6.f * s;
        if (px < 6.f * s) px = 6.f * s;

        m_modalPopupActive = true;
        m_popupKind = PopupKind::Color;
        m_modalPopupId = (int)idh;
        m_modalPopupX = px;
        m_modalPopupY = py;
        m_modalPopupW = popupW;
        m_modalPopupH = popupH;
        m_popupColor = color;
    } else if (m_modalPopupId == (int)idh && m_popupKind == PopupKind::Color) {
        clearPopupState();
    }

    m_cursorY += rowH + Theme::ITEM_SPACING * s;
}

void Gui::drawPopupOverlay() {
    if (!m_modalPopupActive)
        return;

    const float s = m_scale;
    const float c = m_contentScale;

    if (m_popupKind == PopupKind::Combo) {
        float x = m_modalPopupX;
        float popupA = animValue((uint32_t)m_modalPopupId ^ 0xD22010u, 1.f, 0.26f);
        float y = m_modalPopupY + (1.f - popupA) * 8.f * s;
        float w = m_modalPopupW;
        float h = m_modalPopupH;
        float ih = 34.f * s;

        m_renderer->clearClipRect();
        m_renderer->drawRoundedFilledRect(x, y, w, h, withAlpha(0xFF121420, popupA), 12.f * s);
        m_renderer->drawRoundedRect(x, y, w, h, withAlpha(0xFF222538, popupA), 12.f * s, (std::max)(1.f, s));

        for (int i = 0; i < m_popupComboCount; ++i) {
            float iy = y + 5.f * s + i * ih + (1.f - popupA) * 4.f * s;
            bool ihov = isMouseInRect(x + 4.f * s, iy, w - 8.f * s, ih);
            if (ihov)
                m_renderer->drawRoundedFilledRect(x + 4.f * s, iy, w - 8.f * s, ih, withAlpha(0xFF1B1E2D, popupA), 8.f * s);
            unsigned int tc = (i == *m_popupComboCurrent) ? Theme::ACCENT : Theme::TEXT;
            tc = withAlpha(tc, popupA);
            m_renderer->drawText(*m_font, x + 14.f * s, textControlCenterY(*m_font, iy, ih, m_popupComboItems[i], 14.f * c), m_popupComboItems[i], tc, 14.f * c);
            if (ihov && m_mouseClicked) {
                m_comboPopupResult = i;
                m_comboPopupResultId = m_modalPopupId;
                m_comboPopupResultValid = true;
                clearPopupState();
                return;
            }
        }

        if (!m_popupJustOpened && m_mouseClicked && !isMouseInRect(x, y, w, h)) {
            clearPopupState();
        }
    } else if (m_popupKind == PopupKind::Color && m_popupColor) {
        float x = m_modalPopupX;
        float y = m_modalPopupY;
        float w = m_modalPopupW;
        float h = m_modalPopupH;

        m_renderer->clearClipRect();
        m_renderer->drawRoundedFilledRect(x, y, w, h, Theme::SURFACE, 10.f * s);
        m_renderer->drawRoundedRect(x, y, w, h, Theme::BORDER, 10.f * s, (std::max)(1.f, s));

        Hsva hsv = rgbaToHsva(m_popupColor);
        const float popupPad = 12.f * s;
        const float svSize = 190.f * s;
        const float sliderH = 16.f * s;
        const float sliderGap = 10.f * s;

        float svX = x + popupPad;
        float svY = y + popupPad;
        unsigned int hueCol = hsvToArgb(hsv.h, 1.f, 1.f, 1.f);
        m_renderer->drawGradientRectH(svX, svY, svSize, svSize, 0xFFFFFFFF, hueCol);
        m_renderer->drawGradientRect(svX, svY, svSize, svSize, 0x00000000, 0xFF000000);
        m_renderer->drawRoundedRect(svX, svY, svSize, svSize, Theme::BORDER, 6.f, 1.f);

        bool svHover = isMouseInRect(svX, svY, svSize, svSize);
        if (svHover && m_mouseClicked) m_activeItem = 0xC02010;
        if (m_activeItem == 0xC02010 && m_mouseDown) {
            float ns = (m_mouseX - svX) / svSize;
            float nv = 1.f - (m_mouseY - svY) / svSize;
            hsv.s = (std::max)(0.f, (std::min)(ns, 1.f));
            hsv.v = (std::max)(0.f, (std::min)(nv, 1.f));
            hsvaToRgba(hsv, m_popupColor);
        }
        float cx = svX + hsv.s * svSize;
        float cy = svY + (1.f - hsv.v) * svSize;
        m_renderer->drawCircle(cx, cy, 8.f, 0xFFFFFFFF, 1.4f);

        float hueY = svY + svSize + sliderGap;
        float hueW = svSize;
        m_renderer->drawGradientRectH(svX, hueY, hueW / 6.f, sliderH, 0xFFFF0000, 0xFFFFFF00);
        m_renderer->drawGradientRectH(svX + hueW / 6.f, hueY, hueW / 6.f, sliderH, 0xFFFFFF00, 0xFF00FF00);
        m_renderer->drawGradientRectH(svX + hueW * 2.f / 6.f, hueY, hueW / 6.f, sliderH, 0xFF00FF00, 0xFF00FFFF);
        m_renderer->drawGradientRectH(svX + hueW * 3.f / 6.f, hueY, hueW / 6.f, sliderH, 0xFF00FFFF, 0xFF0000FF);
        m_renderer->drawGradientRectH(svX + hueW * 4.f / 6.f, hueY, hueW / 6.f, sliderH, 0xFF0000FF, 0xFFFF00FF);
        m_renderer->drawGradientRectH(svX + hueW * 5.f / 6.f, hueY, hueW / 6.f, sliderH, 0xFFFF00FF, 0xFFFF0000);
        m_renderer->drawRoundedRect(svX, hueY, hueW, sliderH, Theme::BORDER, 6.f, 1.f);

        bool hueHover = isMouseInRect(svX, hueY, hueW, sliderH);
        if (hueHover && m_mouseClicked) m_activeItem = 0xC02011;
        if (m_activeItem == 0xC02011 && m_mouseDown) {
            float nh = (m_mouseX - svX) / hueW;
            hsv.h = (std::max)(0.f, (std::min)(nh, 1.f));
            hsvaToRgba(hsv, m_popupColor);
        }
        float hx = svX + hsv.h * hueW;
        m_renderer->drawFilledCircle(hx, hueY + sliderH * 0.5f, 7.f, 0xFFE9E9EF);
        m_renderer->drawCircle(hx, hueY + sliderH * 0.5f, 7.f, 0xFF9A9AA3, 1.f);

        float alphaY = hueY + sliderH + 8.f;
        unsigned int alphaFrom = (0x00FFFFFFu) | 0x00000000u;
        unsigned int alphaTo = (0xFF000000u) | 0x00FFFFFFu;
        m_renderer->drawGradientRectH(svX, alphaY, hueW, sliderH, alphaFrom, alphaTo);
        m_renderer->drawRoundedRect(svX, alphaY, hueW, sliderH, Theme::BORDER, 6.f, 1.f);

        bool alphaHover = isMouseInRect(svX, alphaY, hueW, sliderH);
        if (alphaHover && m_mouseClicked) m_activeItem = 0xC02012;
        if (m_activeItem == 0xC02012 && m_mouseDown) {
            float na = (m_mouseX - svX) / hueW;
            m_popupColor[3] = (std::max)(0.f, (std::min)(na, 1.f));
        }
        float ax = svX + m_popupColor[3] * hueW;
        m_renderer->drawFilledCircle(ax, alphaY + sliderH * 0.5f, 7.f, 0xFFE9E9EF);
        m_renderer->drawCircle(ax, alphaY + sliderH * 0.5f, 7.f, 0xFF9A9AA3, 1.f);

        char hex[16];
        auto b = [](float v) -> unsigned int {
            if (v < 0.f) v = 0.f;
            if (v > 1.f) v = 1.f;
            return (unsigned int)(v * 255.f + 0.5f);
        };
        snprintf(hex, sizeof(hex), "%02X%02X%02X", b(m_popupColor[0]), b(m_popupColor[1]), b(m_popupColor[2]));
        float hxBoxY = alphaY + sliderH + 12.f;
        m_renderer->drawRoundedFilledRect(svX + 32.f, hxBoxY, svSize - 64.f, 36.f, 0xFF1A1A1D, 6.f);
        m_renderer->drawRoundedRect(svX + 32.f, hxBoxY, svSize - 64.f, 36.f, 0xFFBDBDC4, 6.f, 1.f);
        float hexW = measureTextWidth(*m_font, hex, 22.f);
        m_renderer->drawText(*m_font, svX + 32.f + ((svSize - 64.f) - hexW) * 0.5f,
            textControlCenterY(*m_font, hxBoxY, 36.f, hex, 22.f), hex, 0xFFD0D3DD, 22.f);

        if (!m_popupJustOpened && m_mouseClicked && !isMouseInRect(x, y, w, h)) {
            clearPopupState();
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// tabButton (sidebar navigation)
// ═════════════════════════════════════════════════════════════════════════════

bool Gui::tabButton(const char* label, float w, float h, bool active) {
    uint32_t idh = hashId(label);
    float x = m_cursorX, y = m_cursorY;
    bool hovered = isMouseInRect(x, y, w, h);

    unsigned int bg = Theme::BG;
    if (active) bg = Theme::NAV_ACTIVE;
    else if (hovered) bg = 0xFF1A1A24;

    if (active || hovered)
        m_renderer->drawRoundedFilledRect(x, y, w, h, bg, 6.f);

    unsigned int tc = active ? Theme::TEXT : Theme::TEXT_MUTED;
    m_renderer->drawText(*m_font, x + 16.f, textControlCenterY(*m_font, y, h, label, 19.f), label, tc, 19.f);

    m_cursorY += h;
    return hovered && m_mouseClicked;
}
