#include "ui.h"
#include "loader_app.h"
#include "loader_theme.h"
#include "texture_util.h"

#include <imgui.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <Windows.h>

namespace {

constexpr float kTitleH = 40.f;
constexpr float kRailW = 214.f;
constexpr float kOuterPad = 16.f;

Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> g_logoSrv;
int g_logoW = 0;
int g_logoH = 0;
HWND g_hwnd = nullptr;
ImFont* g_fontBody = nullptr;
ImFont* g_fontTitle = nullptr;
ImFont* g_fontSmall = nullptr;

ImU32 toCol(unsigned int argb) {
    return IM_COL32((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, (argb >> 24) & 0xFF);
}

ImVec4 toVec4(unsigned int argb) {
    return ImColor(toCol(argb));
}

void snap(float& v) { v = std::floor(v + 0.5f); }

ImVec2 snapped(ImVec2 p) {
    snap(p.x);
    snap(p.y);
    return p;
}

void fillRect(const ImVec2& a, const ImVec2& b, ImU32 col) {
    ImGui::GetWindowDrawList()->AddRectFilled(snapped(a), snapped(b), col, 0.f);
}

void strokeRect(const ImVec2& a, const ImVec2& b, ImU32 col, float thickness = 1.f) {
    ImGui::GetWindowDrawList()->AddRect(snapped(a), snapped(b), col, 0.f, 0, thickness);
}

void line(const ImVec2& a, const ImVec2& b, ImU32 col, float thickness = 1.f) {
    ImGui::GetWindowDrawList()->AddLine(snapped(a), snapped(b), col, thickness);
}

void textAt(float x, float y, const char* text, ImU32 col, ImFont* font = nullptr) {
    if (font)
        ImGui::PushFont(font);
    ImGui::GetWindowDrawList()->AddText(snapped({ x, y }), col, text);
    if (font)
        ImGui::PopFont();
}

void applyTheme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0.f;
    s.ChildRounding = 0.f;
    s.FrameRounding = 0.f;
    s.PopupRounding = 0.f;
    s.ScrollbarRounding = 0.f;
    s.GrabRounding = 0.f;
    s.TabRounding = 0.f;
    s.WindowBorderSize = 0.f;
    s.FrameBorderSize = 1.f;
    s.WindowPadding = { 0.f, 0.f };
    s.FramePadding = { 11.f, 8.f };
    s.ItemSpacing = { 0.f, 9.f };
    s.ItemInnerSpacing = { 7.f, 4.f };
    s.ScrollbarSize = 5.f;
    s.AntiAliasedLines = false;
    s.AntiAliasedLinesUseTex = false;
    s.AntiAliasedFill = false;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]       = toVec4(LoaderTheme::BG);
    c[ImGuiCol_Border]         = toVec4(LoaderTheme::BORDER);
    c[ImGuiCol_Text]           = toVec4(LoaderTheme::TEXT);
    c[ImGuiCol_TextDisabled]   = toVec4(LoaderTheme::TEXT_MUTED);
    c[ImGuiCol_FrameBg]        = toVec4(0xFF0D0E14);
    c[ImGuiCol_FrameBgHovered] = toVec4(0xFF11141E);
    c[ImGuiCol_FrameBgActive]  = toVec4(0xFF151827);
    c[ImGuiCol_Button]         = toVec4(0xFF252242);
    c[ImGuiCol_ButtonHovered]  = toVec4(0xFF2D2B55);
    c[ImGuiCol_ButtonActive]   = toVec4(0xFF353268);
    c[ImGuiCol_CheckMark]      = toVec4(LoaderTheme::ACCENT);
    c[ImGuiCol_Separator]      = toVec4(LoaderTheme::BORDER);
}

void setupFonts() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    io.Fonts->Flags |= ImFontAtlasFlags_NoBakedLines;

    ImFontConfig cfg{};
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    cfg.PixelSnapH = true;

    const char* segoe = "C:\\Windows\\Fonts\\segoeui.ttf";
    g_fontBody = io.Fonts->AddFontFromFileTTF(segoe, 15.f, &cfg);
    g_fontTitle = io.Fonts->AddFontFromFileTTF(segoe, 22.f, &cfg);
    g_fontSmall = io.Fonts->AddFontFromFileTTF(segoe, 13.f, &cfg);

    if (!g_fontBody)
        g_fontBody = io.Fonts->AddFontDefault(&cfg);
    if (!g_fontTitle)
        g_fontTitle = g_fontBody;
    if (!g_fontSmall)
        g_fontSmall = g_fontBody;
}

bool closeButton(float x, float y) {
    ImGui::SetCursorPos({ x, y });
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.f);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toVec4(0xFF22131A));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, toVec4(LoaderTheme::DESTRUCTIVE));
    const bool pressed = ImGui::Button("X", { 26.f, 22.f });
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
    return pressed;
}

void drawTitleBar(float w) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    fillRect(p, { p.x + w, p.y + kTitleH }, toCol(0xFF080910));
    line({ p.x, p.y + kTitleH - 1.f }, { p.x + w, p.y + kTitleH - 1.f }, toCol(LoaderTheme::BORDER));
    fillRect({ p.x, p.y + kTitleH - 3.f }, { p.x + kRailW, p.y + kTitleH - 1.f }, toCol(LoaderTheme::ACCENT));

    textAt(p.x + 14.f, p.y + 10.f, "crymore.pw", toCol(LoaderTheme::TEXT_MUTED), g_fontSmall);
    textAt(p.x + kRailW + 16.f, p.y + 10.f, "loader", toCol(0xFF454862), g_fontSmall);

    if (closeButton(w - 36.f, 7.f) && g_hwnd)
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);

    ImGui::Dummy({ 0.f, kTitleH });
}

void drawRailBlock(float x, float y, float w, const char* label, const char* value, ImU32 valueCol) {
    fillRect({ x, y }, { x + w, y + 58.f }, toCol(0xFF0D0E14));
    strokeRect({ x, y }, { x + w, y + 58.f }, toCol(LoaderTheme::BORDER));
    textAt(x + 14.f, y + 10.f, label, toCol(LoaderTheme::TEXT_MUTED), g_fontSmall);
    textAt(x + 14.f, y + 31.f, value, valueCol, g_fontBody);
}

void drawRail(LoaderApp& app, LoaderUiState& state, float x, float y, float h) {
    fillRect({ x, y }, { x + kRailW, y + h }, toCol(0xFF090A10));
    line({ x + kRailW, y }, { x + kRailW, y + h }, toCol(LoaderTheme::BORDER));

    if (g_logoSrv) {
        ImGui::SetCursorPos({ x + 20.f, y + 22.f });
        ImGui::Image(reinterpret_cast<ImTextureID>(g_logoSrv.Get()), { 46.f, 46.f });
    } else {
        fillRect({ x + 20.f, y + 22.f }, { x + 66.f, y + 68.f }, toCol(0xFF252242));
        strokeRect({ x + 20.f, y + 22.f }, { x + 66.f, y + 68.f }, toCol(LoaderTheme::ACCENT));
    }

    textAt(x + 78.f, y + 25.f, "crymore.pw", toCol(LoaderTheme::TEXT), g_fontBody);
    textAt(x + 78.f, y + 48.f, "rebirth", toCol(LoaderTheme::TEXT_MUTED), g_fontSmall);

    const char* session = state.loggedIn ? "authorized" : "offline";
    const ImU32 sessionCol = toCol(state.loggedIn ? LoaderTheme::SUCCESS : LoaderTheme::TEXT_MUTED);
    drawRailBlock(x + 18.f, y + 104.f, kRailW - 36.f, "SESSION", session, sessionCol);
    drawRailBlock(x + 18.f, y + 174.f, kRailW - 36.f, "ENVIRONMENT",
        state.envOk ? "clear" : "blocked",
        toCol(state.envOk ? LoaderTheme::SUCCESS : LoaderTheme::DESTRUCTIVE));

    const LoaderPhase phase = app.phase();
    const char* phaseText = "idle";
    if (phase == LoaderPhase::Launching)
        phaseText = "launching";
    else if (phase == LoaderPhase::Failed)
        phaseText = "failed";
    else if (phase == LoaderPhase::Done)
        phaseText = "done";
    drawRailBlock(x + 18.f, y + 244.f, kRailW - 36.f, "STATE", phaseText, toCol(LoaderTheme::TEXT));

    textAt(x + 18.f, y + h - 50.f, "api: pending", toCol(0xFF454862), g_fontSmall);
    textAt(x + 18.f, y + h - 28.f, "build: release", toCol(0xFF454862), g_fontSmall);
}

void pageHeader(float x, float y, const char* eyebrow, const char* title, const char* subtitle) {
    textAt(x, y, eyebrow, toCol(LoaderTheme::ACCENT), g_fontSmall);
    textAt(x, y + 24.f, title, toCol(LoaderTheme::TEXT), g_fontTitle);
    textAt(x, y + 52.f, subtitle, toCol(LoaderTheme::TEXT_MUTED), g_fontSmall);
}

void drawLabel(const char* text) {
    ImGui::PushFont(g_fontSmall);
    ImGui::TextColored(toVec4(LoaderTheme::TEXT_MUTED), "%s", text);
    ImGui::PopFont();
    ImGui::Dummy({ 0.f, 2.f });
}

bool primaryButton(const char* id, const char* label, float w, float h, bool enabled) {
    ImGui::PushID(id);
    if (!enabled)
        ImGui::BeginDisabled();

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.f);
    ImGui::PushStyleColor(ImGuiCol_Border, toVec4(LoaderTheme::BORDER));
    ImGui::PushStyleColor(ImGuiCol_Button, toVec4(0xFF29274D));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toVec4(0xFF343163));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, toVec4(0xFF403B77));
    const bool pressed = ImGui::Button(label, { w, h });
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 b = ImGui::GetItemRectMax();
    fillRect({ a.x, b.y - 3.f }, { b.x, b.y }, toCol(LoaderTheme::ACCENT));
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();

    if (!enabled)
        ImGui::EndDisabled();
    ImGui::PopID();
    return pressed && enabled;
}

void drawWorkspace(float x, float y, float w, float h) {
    fillRect({ x, y }, { x + w, y + h }, toCol(LoaderTheme::SURFACE));
    strokeRect({ x, y }, { x + w, y + h }, toCol(LoaderTheme::BORDER));
    fillRect({ x, y }, { x + w, y + 3.f }, toCol(0xFF1A1C29));
}

void drawInfoStrip(float x, float y, float w, LoaderUiState& state) {
    fillRect({ x, y }, { x + w, y + 42.f }, toCol(0xFF0D0E14));
    strokeRect({ x, y }, { x + w, y + 42.f }, toCol(LoaderTheme::BORDER));
    fillRect({ x + 14.f, y + 17.f }, { x + 22.f, y + 25.f },
        toCol(state.envOk ? LoaderTheme::SUCCESS : LoaderTheme::DESTRUCTIVE));
    textAt(x + 34.f, y + 12.f, state.envOk ? "environment clear" : "environment blocked",
        toCol(state.envOk ? LoaderTheme::SUCCESS : LoaderTheme::DESTRUCTIVE), g_fontSmall);
    textAt(x + w - 122.f, y + 12.f, "sharp layout", toCol(0xFF454862), g_fontSmall);
}

void drawLoginPage(LoaderApp& app, LoaderUiState& state, float x, float y, float w, float h) {
    drawWorkspace(x, y, w, h);
    drawInfoStrip(x + 22.f, y + 22.f, w - 44.f, state);
    pageHeader(x + 34.f, y + 88.f, "AUTHORIZATION", "Sign in", "Enter your account credentials to continue.");

    const float formX = x + 34.f;
    const float formY = y + 176.f;
    const float formW = w - 68.f;
    const float formH = 246.f;
    fillRect({ formX, formY }, { formX + formW, formY + formH }, toCol(0xFF0D0E14));
    strokeRect({ formX, formY }, { formX + formW, formY + formH }, toCol(LoaderTheme::BORDER));
    fillRect({ formX, formY }, { formX + 3.f, formY + formH }, toCol(0xFF252242));

    ImGui::SetCursorPos({ formX + 22.f, formY + 20.f });
    ImGui::BeginGroup();
    ImGui::PushItemWidth(formW - 44.f);

    drawLabel("USERNAME");
    ImGui::InputTextWithHint("##user", "demo", state.username, sizeof(state.username));

    ImGui::Dummy({ 0.f, 8.f });
    drawLabel("PASSWORD");
    ImGui::InputTextWithHint("##pass", "demo", state.password, sizeof(state.password),
        state.showPassword ? 0 : ImGuiInputTextFlags_Password);

    ImGui::Dummy({ 0.f, 2.f });
    ImGui::PushFont(g_fontSmall);
    ImGui::Checkbox("Show password", &state.showPassword);
    ImGui::PopFont();

    ImGui::Dummy({ 0.f, 14.f });
    if (primaryButton("login", state.busy ? "Signing in..." : "Authorize", formW - 44.f, 40.f,
                      !state.busy && !state.loggedIn))
        app.requestLogin();

    if (!state.statusLine.empty() && !state.loggedIn &&
        state.statusLine.find("Sign in") == std::string::npos) {
        ImGui::Dummy({ 0.f, 10.f });
        ImGui::PushFont(g_fontSmall);
        ImGui::PushStyleColor(ImGuiCol_Text, toVec4(LoaderTheme::DESTRUCTIVE));
        ImGui::TextWrapped("%s", state.statusLine.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }

    ImGui::Dummy({ 0.f, 12.f });
    ImGui::PushFont(g_fontSmall);
    ImGui::TextColored(toVec4(0xFF454862), "offline auth: demo / demo");
    ImGui::PopFont();

    ImGui::PopItemWidth();
    ImGui::EndGroup();
}

void drawLaunchPage(LoaderApp& app, LoaderUiState& state, float x, float y, float w, float h) {
    drawWorkspace(x, y, w, h);
    drawInfoStrip(x + 22.f, y + 22.f, w - 44.f, state);

    char title[128]{};
    std::snprintf(title, sizeof(title), "Welcome, %s", app.session().username.c_str());
    pageHeader(x + 34.f, y + 88.f, "DASHBOARD", title, "The overlay is staged and ready to launch.");

    const float cardX = x + 34.f;
    const float cardY = y + 176.f;
    const float cardW = w - 68.f;
    fillRect({ cardX, cardY }, { cardX + cardW, cardY + 86.f }, toCol(0xFF0D0E14));
    strokeRect({ cardX, cardY }, { cardX + cardW, cardY + 86.f }, toCol(LoaderTheme::BORDER));
    fillRect({ cardX, cardY }, { cardX + 3.f, cardY + 86.f },
        toCol(state.envOk ? LoaderTheme::SUCCESS : LoaderTheme::DESTRUCTIVE));
    textAt(cardX + 20.f, cardY + 16.f, "Environment", toCol(LoaderTheme::TEXT), g_fontBody);
    textAt(cardX + 20.f, cardY + 45.f,
        state.envOk ? "No protected services detected." : "Close FACEIT / Vanguard / EAC first.",
        toCol(state.envOk ? LoaderTheme::TEXT_MUTED : LoaderTheme::DESTRUCTIVE), g_fontSmall);

    ImGui::SetCursorPos({ cardX, cardY + 108.f });
    if (primaryButton("launch", state.busy ? "Launching..." : "Launch overlay", cardW, 42.f,
                      state.envOk && !state.busy))
        app.requestLaunch();

    ImGui::SetCursorPos({ cardX + (cardW - 70.f) * 0.5f, cardY + 164.f });
    ImGui::PushFont(g_fontSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, toVec4(LoaderTheme::TEXT_LINK));
    if (ImGui::Selectable("Sign out", false, 0, { 70.f, 18.f }))
        app.requestLogout();
    ImGui::PopStyleColor();
    ImGui::PopFont();

    if (!state.logLines.empty()) {
        const float logY = y + h - 104.f;
        fillRect({ cardX, logY }, { cardX + cardW, logY + 74.f }, toCol(0xFF0A0B10));
        strokeRect({ cardX, logY }, { cardX + cardW, logY + 74.f }, toCol(LoaderTheme::BORDER));
        textAt(cardX + 10.f, logY + 8.f, "LOG", toCol(LoaderTheme::TEXT_MUTED), g_fontSmall);
        ImGui::SetCursorPos({ cardX + 10.f, logY + 28.f });
        ImGui::BeginChild("log", { cardW - 20.f, 38.f }, false, ImGuiWindowFlags_NoScrollbar);
        ImGui::PushFont(g_fontSmall);
        int shown = 0;
        for (auto it = state.logLines.rbegin(); it != state.logLines.rend() && shown < 2; ++it) {
            if (it->rfind("[launch]", 0) == 0 || it->rfind("[env]", 0) == 0 ||
                it->rfind("[warn]", 0) == 0 || it->rfind("[auth]", 0) == 0) {
                ImGui::TextUnformatted(it->c_str());
                ++shown;
            }
        }
        ImGui::PopFont();
        ImGui::EndChild();
    }
}

} // namespace

bool loaderUiInit(ID3D11Device* device) {
    setupFonts();
    applyTheme();
    loadBrandLogoFromResource(device, g_logoSrv, g_logoW, g_logoH);
    return true;
}

void loaderUiSetWindow(HWND hwnd) { g_hwnd = hwnd; }

void loaderUiShutdown() {
    g_logoSrv.Reset();
    g_hwnd = nullptr;
    g_fontBody = g_fontTitle = g_fontSmall = nullptr;
}

Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> loaderBrandLogoSrv() { return g_logoSrv; }
int loaderBrandLogoW() { return g_logoW; }
int loaderBrandLogoH() { return g_logoH; }

void loaderUiFrame(LoaderApp& app, LoaderUiState& state, float dt) {
    (void)dt;
    ImGuiIO& io = ImGui::GetIO();
    if (g_fontBody)
        ImGui::PushFont(g_fontBody);

    ImGui::SetNextWindowPos({ 0, 0 });
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("crymore_loader_root", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    const float w = io.DisplaySize.x;
    const float h = io.DisplaySize.y;
    const ImVec2 root = ImGui::GetWindowPos();
    fillRect(root, { root.x + w, root.y + h }, toCol(LoaderTheme::BG));
    strokeRect(root, { root.x + w, root.y + h }, toCol(LoaderTheme::BORDER));

    drawTitleBar(w);

    const float bodyY = kTitleH;
    drawRail(app, state, 0.f, bodyY, h - bodyY);

    const LoaderPhase phase = app.phase();
    const bool onLaunchPage = state.loggedIn &&
        (phase == LoaderPhase::Ready || phase == LoaderPhase::Launching || phase == LoaderPhase::Failed);

    const float workX = kRailW + kOuterPad;
    const float workY = bodyY + kOuterPad;
    const float workW = w - workX - kOuterPad;
    const float workH = h - workY - kOuterPad;

    if (onLaunchPage)
        drawLaunchPage(app, state, workX, workY, workW, workH);
    else
        drawLoginPage(app, state, workX, workY, workW, workH);

    ImGui::End();
    if (g_fontBody)
        ImGui::PopFont();
}
