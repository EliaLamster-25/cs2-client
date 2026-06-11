#include "loader_ui.h"
#include "texture_util.h"

#include "overlay/renderer.h"
#include "gui/gui.h"
#include "gui/font.h"
#include "config.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace {

LoaderUi g_ui;

constexpr float kTitlebarH = 32.f;
constexpr float kFormW     = 306.f;
constexpr float kPadX      = 28.f;

constexpr unsigned int kBg       = 0xFF08090E;
constexpr unsigned int kSurface  = 0xFF0D0E13;
constexpr unsigned int kSurface2 = 0xFF12131A;
constexpr unsigned int kBorder   = 0xFF20222C;
constexpr unsigned int kOk       = 0xFF5BC48A;
constexpr unsigned int kBad      = 0xFFB94261;
constexpr unsigned int kWarn     = 0xFFD4A853;
constexpr unsigned int kDim      = 0xFF454862;
constexpr unsigned int kInk      = 0xFFE1E2EE;
constexpr unsigned int kMuted2   = 0xFF85879D;

static unsigned int withAlpha(unsigned int argb, float a01) {
    a01 = (std::max)(0.f, (std::min)(a01, 1.f));
    return (argb & 0x00FFFFFFu) | ((unsigned int)(a01 * 255.f) << 24);
}

static unsigned int lerpColor(unsigned int c0, unsigned int c1, float t) {
    t = (std::max)(0.f, (std::min)(t, 1.f));
    auto ch = [&](int s) -> unsigned int {
        const float a = (float)((c0 >> s) & 0xFF);
        const float b = (float)((c1 >> s) & 0xFF);
        return (unsigned int)(a + (b - a) * t + 0.5f);
    };
    return (ch(24) << 24) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

static float easeOutCubic(float t) {
    t = (std::max)(0.f, (std::min)(t, 1.f));
    const float inv = 1.f - t;
    return 1.f - inv * inv * inv;
}

static float smooth(float cur, float target, float speed, float dt) {
    speed = (std::max)(0.02f, (std::min)(speed, 0.95f));
    const float k = 1.f - std::pow(1.f - speed, (std::max)(0.25f, (std::min)(dt * 60.f, 4.f)));
    return cur + (target - cur) * k;
}

static float measureText(const FontAtlas& font, const char* text, float size) {
    if (!text || !*text) return 0.f;
    int len = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (len <= 1) return 0.f;
    std::wstring w(len - 1, L' ');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, w.data(), len);
    const float scale = size / (float)font.renderPx();
    float x = 0.f;
    for (wchar_t ch : w) {
        if (ch == L' ') { x += 8.f * scale; continue; }
        const GlyphInfo* g = font.glyph(ch);
        x += (g ? (float)g->advanceX : 8.f) * scale;
    }
    return x;
}

static float textCenterY(const FontAtlas& font, float top, float boxH, float fontSize) {
    return top + (boxH - font.lineHeight(fontSize)) * 0.5f;
}

static std::string fitText(const FontAtlas& font, const std::string& text, float size, float maxW) {
    if (measureText(font, text.c_str(), size) <= maxW)
        return text;
    std::string out = text;
    while (out.size() > 3 && measureText(font, (out + "...").c_str(), size) > maxW)
        out.pop_back();
    return out + "...";
}

static const char* injectPhaseLabel(LoaderPhase phase) {
    switch (phase) {
    case LoaderPhase::Boot:      return "booting";
    case LoaderPhase::Login:     return "idle";
    case LoaderPhase::Ready:     return "ready";
    case LoaderPhase::Launching: return "injecting";
    case LoaderPhase::Done:      return "injected";
    case LoaderPhase::Failed:    return "failed";
    default:                     return "idle";
    }
}

static std::string specificEnvMessage(const LoaderApp& app) {
    const auto& errors = app.envErrors();
    if (errors.empty())
        return "Environment check failed.";

    const std::string& first = errors.front();
    if (first.find("FACEIT") != std::string::npos)
        return "Disable/stop FACEIT AC.";
    if (first.find("Vanguard") != std::string::npos || first.find("Riot") != std::string::npos)
        return "Disable Riot Vanguard.";
    if (first.find("Easy Anti-Cheat") != std::string::npos)
        return "Close Easy Anti-Cheat.";
    if (first.find("BattlEye") != std::string::npos)
        return "Close BattlEye service.";
    if (first.find("ESEA") != std::string::npos)
        return "Close ESEA client.";
    return first;
}

} // namespace

bool LoaderUi::init(Renderer& renderer, HWND hwnd) {
    m_renderer = &renderer;
    m_hwnd = hwnd;
    static FontAtlas font;
    static Gui gui;
    m_font = &font;
    m_gui = &gui;

    if (!m_font->init(renderer.device(), L"Segoe UI", 80))
        return false;

    loadBrandLogoFromResource(renderer.device(), m_logoSrv, m_logoW, m_logoH);

    g_cfg.espEnabled = false;
    g_cfg.menuVisible = true;
    g_cfg.aaMode = 0;
    return true;
}

void LoaderUi::shutdown() {
    if (m_font)
        m_font->destroy();
    m_logoSrv.Reset();
    m_font = nullptr;
    m_gui = nullptr;
    m_renderer = nullptr;
}

void LoaderUi::drawTitlebar() {
    Renderer& r = *m_renderer;
    const float sw = (float)r.screenWidth();

    r.drawFilledRect(0.f, 0.f, sw, kTitlebarH, 0xFF06070B);
    r.drawFilledRect(0.f, kTitlebarH - 1.f, sw, 1.f, kBorder);
    r.drawText(*m_font, 14.f, 9.f, "crymore.pw", 0xFF55586D, 11.f);

    const float closeS = 26.f;
    const float closeX = sw - closeS - 10.f;
    const float closeY = (kTitlebarH - 22.f) * 0.5f;
    const bool closeHover = m_gui->mouseX() >= closeX && m_gui->mouseX() <= closeX + closeS
        && m_gui->mouseY() >= closeY && m_gui->mouseY() <= closeY + 22.f;
    if (closeHover)
        r.drawRoundedFilledRect(closeX, closeY, closeS, 22.f, 0xFF22131A, 4.f);
    r.drawText(*m_font, closeX + 8.f, closeY + 2.f, "×", closeHover ? kInk : Theme::TEXT_MUTED, 18.f);
    if (closeHover && m_gui->mouseClicked())
        m_closeRequested = true;
}

void LoaderUi::drawLogo(float cx, float cy, float frameSize, float logoSize, float alpha) {
    Renderer& r = *m_renderer;
    const float fx = cx - frameSize * 0.5f;
    const float fy = cy - frameSize * 0.5f;

    r.drawRoundedFilledRect(fx, fy, frameSize, frameSize, withAlpha(0xFF101119, alpha), 10.f);
    r.drawRoundedRect(fx, fy, frameSize, frameSize, withAlpha(0xFF2A2C38, alpha), 10.f, 1.f);

    const float inset = (frameSize - logoSize) * 0.5f;
    if (m_logoSrv)
        r.drawImageRgba(m_logoSrv.Get(), fx + inset, fy + inset, logoSize, logoSize, withAlpha(0xFFFFFFFF, alpha));
    else
        r.drawRoundedFilledRect(fx + inset, fy + inset, logoSize, logoSize, withAlpha(0xFF252242, alpha), 10.f);
}

void LoaderUi::drawLoginPage(LoaderApp& app, float alpha, float slideY) {
    if (alpha <= 0.005f)
        return;

    Renderer& r = *m_renderer;
    const float sw = (float)r.screenWidth();
    const float sh = (float)r.screenHeight();
    const float contentTop = kTitlebarH;
    const float cx = sw * 0.5f;
    const float baseY = contentTop + 12.f + slideY;
    const float fieldW = 470.f;
    const float fieldX = cx - fieldW * 0.5f;

    const float logoCx = cx;
    const float logoCy = baseY + 92.f;
    r.drawFilledCircle(logoCx, logoCy, 156.f, withAlpha(Theme::ACCENT, alpha * 0.035f));
    r.drawFilledCircle(logoCx, logoCy, 96.f, withAlpha(Theme::ACCENT, alpha * 0.045f));
    if (m_logoSrv) {
        r.drawImageRgba(m_logoSrv.Get(), logoCx - 108.f, logoCy - 108.f, 216.f, 216.f,
            withAlpha(0xFFFFFFFF, alpha * 0.050f));
        r.drawImageRgba(m_logoSrv.Get(), logoCx - 76.f, logoCy - 76.f, 152.f, 152.f,
            withAlpha(0xFFFFFFFF, alpha * 0.18f));
        r.drawImageRgba(m_logoSrv.Get(), logoCx - 60.f, logoCy - 60.f, 120.f, 120.f,
            withAlpha(0xFFFFFFFF, alpha));
    } else {
        r.drawRoundedFilledRect(logoCx - 60.f, logoCy - 60.f, 120.f, 120.f,
            withAlpha(0xFF252242, alpha), 18.f);
    }

    const float userY = baseY + 230.f;
    m_gui->setCursor(fieldX, userY);
    m_gui->setItemWidth(fieldW);

    const auto& state = app.uiState();
    m_gui->textField("user", m_username, sizeof(m_username), 42.f, false);
    if (m_username[0] == '\0')
        r.drawText(*m_font, fieldX + 14.f, textCenterY(*m_font, userY, 42.f, 18.f),
            "Username", withAlpha(0xFF666879, alpha), 18.f);

    const float passY = userY + 82.f;
    m_gui->setCursor(fieldX, passY);
    m_gui->setItemWidth(fieldW);
    m_gui->textField("pass", m_password, sizeof(m_password), 42.f, true);
    if (m_password[0] == '\0')
        r.drawText(*m_font, fieldX + 14.f, textCenterY(*m_font, passY, 42.f, 18.f),
            "Password", withAlpha(0xFF666879, alpha), 18.f);

    const bool canLogin = !state.busy && !state.loggedIn;
    const float btnY = passY + 84.f;
    if (drawPrimaryButton("login", state.busy ? "Checking..." : "Login",
            fieldX, btnY, fieldW, 64.f, canLogin, alpha)) {
        app.requestLogin(m_username, m_password);
    }

    if (!state.statusLine.empty() && !state.loggedIn &&
        state.statusLine.find("Sign in") == std::string::npos) {
        const std::string msg = fitText(*m_font, state.statusLine, 12.f, fieldW);
        r.drawText(*m_font, fieldX, btnY + 82.f,
            msg.c_str(), withAlpha(kBad, alpha), 12.f);
    }
}

void LoaderUi::drawStatTile(float x, float y, float w, const char* label, const char* value,
                            unsigned int valueCol, unsigned int dotCol, float dotPulse, float alpha) {
    Renderer& r = *m_renderer;
    const float h = 58.f;

    r.drawRoundedFilledRect(x, y, w, h, withAlpha(kSurface, alpha), 8.f);
    r.drawRoundedRect(x, y, w, h, withAlpha(kBorder, alpha), 8.f, 1.f);
    r.drawText(*m_font, x + 14.f, y + 11.f, label, withAlpha(Theme::TEXT_MUTED, alpha), 10.f);

    const float dotA = dotPulse > 0.f ? (0.55f + 0.45f * std::sin(m_time * 5.f)) : 1.f;
    r.drawFilledCircle(x + 17.f, y + 38.f, 3.5f, withAlpha(dotCol, alpha * dotA));
    r.drawText(*m_font, x + 28.f, y + 31.f, value, withAlpha(valueCol, alpha), 14.f);
}

bool LoaderUi::drawGhostButton(const char* /*id*/, const char* label, float x, float y, float w, float h) {
    Renderer& r = *m_renderer;
    const bool hover = m_gui->mouseX() >= x && m_gui->mouseX() <= x + w
        && m_gui->mouseY() >= y && m_gui->mouseY() <= y + h;

    unsigned int border = hover ? 0xFF252838 : kBorder;
    unsigned int textCol = hover ? Theme::TEXT : Theme::TEXT_MUTED;
    r.drawRoundedRect(x, y, w, h, border, 8.f, 1.f);
    const float tw = measureText(*m_font, label, 13.f);
    r.drawText(*m_font, x + (w - tw) * 0.5f, textCenterY(*m_font, y, h, 13.f),
        label, textCol, 13.f);

    if (hover && m_gui->mouseClicked())
        return true;
    return false;
}

bool LoaderUi::drawPrimaryButton(const char* /*id*/, const char* label, float x, float y, float w, float h,
                                 bool enabled, float alpha) {
    Renderer& r = *m_renderer;
    const bool hover = enabled
        && m_gui->mouseX() >= x && m_gui->mouseX() <= x + w
        && m_gui->mouseY() >= y && m_gui->mouseY() <= y + h;

    unsigned int bg = enabled ? Theme::ACCENT : 0xFF5350A6;
    if (hover)
        bg = 0xFF8583FF;

    r.drawRoundedFilledRect(x, y, w, h, withAlpha(bg, alpha), 8.f);
    r.drawRoundedRect(x, y, w, h, withAlpha(enabled ? 0xFF8B89F8 : 0xFF5F5CB8, alpha), 8.f, 1.f);

    const float fs = h >= 62.f ? 32.f : (h >= 54.f ? 20.f : 15.f);
    const float tw = measureText(*m_font, label, fs);
    r.drawText(*m_font, x + (w - tw) * 0.5f, textCenterY(*m_font, y, h, fs),
        label, withAlpha(enabled ? 0xFFFFFFFF : 0xFFE1E2EE, alpha), fs);

    return hover && m_gui->mouseClicked();
}

void LoaderUi::drawStatusPage(LoaderApp& app, float alpha, float slideY) {
    if (alpha <= 0.005f)
        return;

    Renderer& r = *m_renderer;
    const auto& state = app.uiState();
    const LoaderPhase phase = app.phase();
    const bool busy = state.busy;
    const float sw = (float)r.screenWidth();
    const float sh = (float)r.screenHeight();
    const float y0 = kTitlebarH + 24.f + slideY;
    const float innerW = sw - kPadX * 2.f;
    const bool envOk = state.envOk;
    const bool injecting = phase == LoaderPhase::Launching || busy;
    const bool failed = phase == LoaderPhase::Failed;
    const bool done = phase == LoaderPhase::Done;

    char userLine[128]{};
    std::snprintf(userLine, sizeof(userLine), "%s",
        app.session().username.empty() ? "User" : app.session().username.c_str());

    const float bodyY = kTitlebarH + 24.f + slideY;
    const float leftX = 72.f;
    const float leftW = 280.f;
    const float rightX = 408.f;
    const float rightW = sw - rightX - 64.f;
    const float progressH = 368.f;
    const float profileH = 90.f;

    // Progress card
    r.drawRoundedFilledRect(leftX, bodyY, leftW, progressH, withAlpha(kSurface, alpha), 4.f);
    r.drawText(*m_font, leftX + 20.f, bodyY + 18.f, "Progress", withAlpha(kInk, alpha), 18.f);

    const float stepX = leftX + 20.f;
    float stepY = bodyY + 105.f;
    auto progressStep = [&](const char* label, unsigned int col, bool active) {
        const float pulse = active ? (0.55f + 0.45f * std::sin(m_time * 5.f)) : 1.f;
        r.drawFilledCircle(stepX, stepY + 7.f, 4.5f, withAlpha(col, alpha * pulse));
        r.drawText(*m_font, stepX + 18.f, stepY - 2.f, label, withAlpha(col, alpha), 14.f);
        stepY += 86.f;
    };

    progressStep("Logged in", kOk, false);
    progressStep("Loading", injecting ? kWarn : (done ? kOk : kMuted2), injecting);
    progressStep("Loaded", done ? kOk : (failed ? kBad : kMuted2), false);

    // Profile card
    const float profileY = bodyY + progressH + 32.f;
    r.drawRoundedFilledRect(leftX, profileY, leftW, profileH, withAlpha(kSurface, alpha), 4.f);
    drawLogo(leftX + 44.f, profileY + profileH * 0.5f, 56.f, 42.f, alpha);
    r.drawText(*m_font, leftX + 96.f, profileY + 24.f,
        fitText(*m_font, userLine, 16.f, leftW - 112.f).c_str(), withAlpha(kInk, alpha), 16.f);
    const std::string remaining = app.session().expiresAt.empty()
        ? "Subscribed until active"
        : ("Subscribed until " + app.session().expiresAt);
    r.drawText(*m_font, leftX + 96.f, profileY + 50.f,
        fitText(*m_font, remaining, 12.f, leftW - 112.f).c_str(), withAlpha(kMuted2, alpha), 12.f);

    // Launch card
    const float actionY = bodyY;
    const float actionH = 220.f;
    r.drawRoundedFilledRect(rightX, actionY, rightW, actionH, withAlpha(kSurface, alpha), 4.f);

    const std::string envDetail = !envOk ? specificEnvMessage(app) : "";
    const char* title = !envOk ? "Launch blocked"
        : injecting ? "Loading"
        : done ? "Loaded"
        : failed ? "Launch failed"
        : "Ready to launch";
    r.drawText(*m_font, rightX + 48.f, actionY + 28.f, title, withAlpha(kInk, alpha), 18.f);

    const std::string desc = !envOk ? envDetail
        : injecting ? "Starting overlay payload."
        : done ? "Overlay is running."
        : failed ? (state.statusLine.empty() ? "Check logs for details." : state.statusLine)
        : "All checks passed.";
    r.drawText(*m_font, rightX + 48.f, actionY + 74.f,
        fitText(*m_font, desc, 13.f, rightW - 96.f).c_str(),
        withAlpha(!envOk || failed ? kBad : kMuted2, alpha), 13.f);

    const bool canLaunch = envOk && !busy && phase != LoaderPhase::Done;
    const char* btnText = injecting ? "Loading..." : (done ? "Loaded" : "Inject");
    const float btnW = 268.f;
    if (drawPrimaryButton("launch", btnText, rightX + (rightW - btnW) * 0.5f,
            actionY + 136.f, btnW, 58.f, canLaunch, alpha)) {
        app.requestLaunch();
    }

    // Logs card
    const float logY = actionY + actionH + 36.f;
    const float logH = sh - logY - 26.f;
    r.drawRoundedFilledRect(rightX, logY, rightW, logH, withAlpha(kSurface, alpha), 4.f);
    r.drawText(*m_font, rightX + 20.f, logY + 18.f, "Logs", withAlpha(kInk, alpha), 14.f);

    float lineY = logY + 52.f;
    const float lineH = 15.f;
    const int maxLines = (std::max)(0, (int)((logH - 64.f) / lineH));
    int shown = 0;
    r.setClipRect(rightX + 20.f, logY + 50.f, rightW - 40.f, logH - 58.f);
    for (auto it = state.logLines.rbegin(); it != state.logLines.rend() && shown < maxLines; ++it) {
        if (it->rfind("[launch]", 0) == 0 || it->rfind("[env]", 0) == 0 ||
            it->rfind("[warn]", 0) == 0 || it->rfind("[auth]", 0) == 0 ||
            it->rfind("[boot]", 0) == 0) {
            unsigned int col = 0xFF8B8DA8;
            if (it->rfind("[warn]", 0) == 0) col = kWarn;
            if (it->rfind("[env]", 0) == 0 && it->find("blocked") != std::string::npos) col = kBad;
            std::string line = *it;
            if (line.rfind("[auth] welcome", 0) == 0)
                line = "Logged in successfully";
            const std::string fitted = fitText(*m_font, line, 11.f, rightW - 48.f);
            r.drawText(*m_font, rightX + 20.f, lineY, fitted.c_str(), withAlpha(col, alpha), 11.f);
            lineY += lineH;
            ++shown;
        }
    }
    r.clearClipRect();
}

void LoaderUi::render(LoaderApp& app, float dt) {
    if (!m_renderer || !m_gui || !m_font)
        return;

    m_time += dt;

    const bool onStatus = app.uiState().loggedIn;
    m_pageAnim = smooth(m_pageAnim, onStatus ? 1.f : 0.f, 0.18f, dt);

    m_renderer->beginFrame();
    m_gui->beginFrame(*m_renderer, *m_font, m_hwnd);
    m_gui->setScale(1.f, 1.f);

    Renderer& r = *m_renderer;
    r.drawFilledRect(0.f, 0.f, (float)r.screenWidth(), (float)r.screenHeight(), kBg);
    r.drawRect(0.f, 0.f, (float)r.screenWidth(), (float)r.screenHeight(), kBorder, 1.f);

    drawTitlebar();

    const float t = easeOutCubic(m_pageAnim);
    const float loginA = 1.f - t;
    const float statusA = t;
    const float loginSlide = -28.f * t;
    const float statusSlide = 28.f * (1.f - t);

    drawLoginPage(app, loginA, loginSlide);
    drawStatusPage(app, statusA, statusSlide);

    m_gui->endFrame();
    m_renderer->endFrame();
}

void LoaderUi::onChar(wchar_t ch) {
    if (m_gui) m_gui->onChar(ch);
}

void LoaderUi::onKeyDown(WPARAM vk) {
    if (m_gui) m_gui->onKeyDown(vk);
}

bool loaderUiInit(Renderer& renderer, HWND hwnd) { return g_ui.init(renderer, hwnd); }
void loaderUiShutdown() { g_ui.shutdown(); }
void loaderUiRender(LoaderApp& app, float dt) { g_ui.render(app, dt); }
void loaderUiOnChar(wchar_t ch) { g_ui.onChar(ch); }
void loaderUiOnKeyDown(WPARAM vk) { g_ui.onKeyDown(vk); }
bool loaderUiCloseRequested() { return g_ui.closeRequested(); }
