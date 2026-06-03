#include "esp_renderer.h"
#include <functional>
#include <vector>
#include "config.h"
#include "gui/gui.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <cstdio>
#include <imgui.h>

// ─── Convex hull (gift-wrapping / Jarvis march) ───────────────────────────────
static int jarvisHull(const Vec2* pts, int n, Vec2* hull) {
    if (n < 3) { for (int i = 0; i < n; ++i) hull[i] = pts[i]; return n; }

    int start = 0;
    for (int i = 1; i < n; ++i)
        if (pts[i].x < pts[start].x ||
            (pts[i].x == pts[start].x && pts[i].y > pts[start].y))
            start = i;

    int curr = start, hc = 0;
    do {
        hull[hc++] = pts[curr];
        int next = (curr + 1) % n;
        for (int i = 0; i < n; ++i) {
            float cross = (pts[next].x - pts[curr].x) * (pts[i].y - pts[curr].y)
                        - (pts[next].y - pts[curr].y) * (pts[i].x - pts[curr].x);
            if (cross < 0.f) next = i;
        }
        curr = next;
    } while (curr != start && hc < n);
    return hc;
}

static wchar_t mapWeaponIconGlyph(const std::string& weaponId) {
    if (weaponId.empty())
        return 0;

    auto has = [&](const char* key) { return weaponId.find(key) != std::string::npos; };
    if (has("deagle")) return 0xE001;
    if (has("elite")) return 0xE002;
    if (has("fiveseven")) return 0xE003;
    if (has("glock")) return 0xE004;
    if (has("ak47")) return 0xE007;
    if (has("aug")) return 0xE008;
    if (has("awp")) return 0xE009;
    if (has("famas")) return 0xE00A;
    if (has("g3sg1")) return 0xE00B;
    if (has("galilar")) return 0xE00D;
    if (has("m4a1_silencer")) return 0xE010;
    if (has("m4a1")) return 0xE00E;
    if (has("mac10")) return 0xE011;
    if (has("hkp2000") || has("p2000")) return 0xE013;
    if (has("ump45")) return 0xE018;
    if (has("xm1014")) return 0xE019;
    if (has("bizon")) return 0xE01A;
    if (has("mag7")) return 0xE01B;
    if (has("negev")) return 0xE01C;
    if (has("sawedoff")) return 0xE01D;
    if (has("tec9")) return 0xE01E;
    if (has("taser")) return 0xE01F;
    if (has("p250")) return 0xE020;
    if (has("mp7")) return 0xE021;
    if (has("mp9")) return 0xE022;
    if (has("nova")) return 0xE023;
    if (has("p90")) return 0xE024;
    if (has("scar20")) return 0xE026;
    if (has("sg556")) return 0xE027;
    if (has("ssg08")) return 0xE028;
    if (has("knife_t")) return 0xE03B;
    if (has("knife") || has("bayonet") || has("knife_css")) return 0xE02A;
    if (has("m249")) return 0xE03C;
    if (has("usp_silencer")) return 0xE03D;
    if (has("cz75a")) return 0xE03F;
    if (has("revolver")) return 0xE040;
    if (has("knife_bayonet")) return 0xE1F4;
    if (has("knife_flip")) return 0xE1F9;
    if (has("knife_gut")) return 0xE1FA;
    if (has("knife_karambit")) return 0xE1FB;
    if (has("knife_m9_bayonet")) return 0xE1FC;
    if (has("knife_tactical")) return 0xE1FD;
    if (has("knife_falchion")) return 0xE200;
    if (has("knife_survival_bowie")) return 0xE202;
    if (has("knife_butterfly")) return 0xE203;
    if (has("knife_push")) return 0xE204;
    if (has("flashbang")) return 0xE02B;
    if (has("hegrenade")) return 0xE02C;
    if (has("smokegrenade")) return 0xE02D;
    if (has("molotov")) return 0xE02E;
    if (has("decoy")) return 0xE02F;
    if (has("incgrenade")) return 0xE030;
    if (has("c4")) return 0xE031;
    return 0;
}

enum class EspAnchor : int {
    Top = 0,
    Bottom = 1,
    Left = 2,
    Right = 3,
    TopLeft = 4,
    TopRight = 5,
    BottomLeft = 6,
    BottomRight = 7
};

static EspAnchor clampEspAnchor(int v) {
    return static_cast<EspAnchor>(std::clamp(v, 0, 7));
}

static EspAnchor resolveEspAnchor(int stored, EspAnchor fallback) {
    if (stored < 0)
        return fallback;
    return clampEspAnchor(stored);
}

static EspAnchor sanitizeBarAnchor(EspAnchor anchor) {
    switch (anchor) {
    case EspAnchor::Top:
    case EspAnchor::Bottom:
    case EspAnchor::Left:
    case EspAnchor::Right:
        return anchor;
    case EspAnchor::TopLeft:
    case EspAnchor::BottomLeft:
        return EspAnchor::Left;
    case EspAnchor::TopRight:
    case EspAnchor::BottomRight:
    default:
        return EspAnchor::Right;
    }
}

static EspAnchor sanitizeInfoAnchor(EspAnchor anchor) {
    switch (anchor) {
    case EspAnchor::Top:
    case EspAnchor::Bottom:
    case EspAnchor::TopLeft:
    case EspAnchor::TopRight:
    case EspAnchor::BottomLeft:
    case EspAnchor::BottomRight:
        return anchor;
    case EspAnchor::Left:
        return EspAnchor::TopLeft;
    case EspAnchor::Right:
    default:
        return EspAnchor::TopRight;
    }
}

static float approxTextWidth(const char* text, float size) {
    if (!text)
        return 0.f;
    return static_cast<float>(std::strlen(text)) * size * 0.56f;
}

// ─── Public ────────────────────────────────────────────────────────────────────

void EspRenderer::render(Renderer& r, const EntityManager& em) {
    const bool renderPlayers = g_cfg.espEnabled && (
        g_cfg.boxEnabled       || g_cfg.boxOccluded
        || g_cfg.hpBarEnabled  || g_cfg.hpBarOccluded
        || g_cfg.hpTextEnabled
        || g_cfg.skeletonEnabled || g_cfg.skeletonOccluded
        || g_cfg.chamsEnabled  || g_cfg.chamsOccluded
        || g_cfg.nameEspEnabled || g_cfg.armorEspEnabled
        || g_cfg.weaponEspEnabled || g_cfg.ammoEspEnabled || g_cfg.flagsEspEnabled);
    const bool renderGrenades = g_cfg.espEnabled && g_cfg.grenadeEnabled;
    const bool renderBomb = g_cfg.bombTimerEnabled;
    const bool renderSpectatorList = g_cfg.spectatorListEnabled;
    const bool renderRadar = g_cfg.espEnabled && g_cfg.radarEnabled;
    if (!renderPlayers && !renderGrenades && !renderBomb && !renderSpectatorList && !renderRadar)
        return;

    r.setImGuiDrawMode(true);

    const auto snap = em.snapshot();

    float sw = static_cast<float>(r.screenWidth());
    float sh = static_cast<float>(r.screenHeight());

    if (renderPlayers) {
        // Take a consistent snapshot so all draw calls use the same frame's data.
        const auto& players = snap.players;
        const auto& vm = snap.viewMatrix;
        const int localTeam = snap.localTeam;

        for (const auto& player : players) {
            if (!player.isValid || !player.isAlive)
                continue;
            if (player.isLocalPlayer)
                continue;
            if (!player.screenRelevant)
                continue;
            if (player.isDormant)
                continue;
            if (g_cfg.enemyOnly && player.teamNum == localTeam)
                continue;

            Vec3 predDelta{};
            if (g_cfg.latencyMode == 2) {
                constexpr float kPredictSec = 0.020f;
                predDelta = player.velocity * kPredictSec;
            }

            Vec2 feetSc, headSc;
            Vec3 projOrigin = player.origin + predDelta;
            Vec3 projHead = player.headPos + predDelta;
            if (!vm.worldToScreen(projOrigin, feetSc, sw, sh)) continue;
            if (!vm.worldToScreen(projHead, headSc, sw, sh)) continue;
            float bodyH = feetSc.y - headSc.y;
            if (bodyH < 5.f) continue;

            const bool occ = g_cfg.visibilityCheckEnabled
                          && player.visibilityChecked && !player.isVisible;

            if (occ ? g_cfg.chamsOccluded : g_cfg.chamsEnabled)
                drawChams(r, player, vm, occ, feetSc, headSc, sw, sh, predDelta);

            if (occ ? g_cfg.skeletonOccluded : g_cfg.skeletonEnabled)
                drawSkeleton(r, player, vm, occ, feetSc, headSc, predDelta);

            drawPlayerBox(r, player, vm, localTeam, occ, feetSc, headSc, sw, sh, predDelta);
        }
    }

    // Draw grenades last so labels appear above player boxes.
    if (renderGrenades)
        drawGrenades(r, snap);

    if (renderBomb)
        drawBomb(r, snap);
    if (renderSpectatorList)
        drawSpectators(r, snap);
    if (renderRadar)
        drawRadar(r, snap);
    r.setImGuiDrawMode(false);
}

// Forward declaration — defined in colour utilities section below.
static const float* pickPlayerColor(const PlayerData& p, int localTeam);

// ─── Per-player AABB box ───────────────────────────────────────────────────────

void EspRenderer::drawPlayerBox(Renderer& r,
                                 const PlayerData& player,
                                 const ViewMatrix& vm,
                                 int localTeam,
                                 bool occ,
                                 const Vec2& feetSc,
                                 const Vec2& headSc,
                                 float sw, float sh,
                                 const Vec3& predDelta)
{
    (void)vm; (void)sw; (void)sh; (void)predDelta;

    float boxH = feetSc.y - headSc.y;
    if (boxH < 5.f) return;

    float boxW = boxH * 0.5f;
    float boxX = feetSc.x - boxW * 0.5f;
    float boxY = headSc.y - boxH * 0.04f;
    boxH += boxH * 0.04f;

    (void)localTeam;
    const float* boxCol = occ ? g_cfg.boxOccludedColor : g_cfg.boxVisibleColor;
    const unsigned int color = rgbaToArgb(boxCol);

    if (occ ? g_cfg.boxOccluded : g_cfg.boxEnabled) {
        r.drawRect(boxX, boxY, boxW, boxH, color, g_cfg.boxThickness);
    }

    drawPlayerInfo(r, player, boxX, boxY, boxW, boxH, occ);
}

// ─── drawChams ──────────────────────────────────────────────────────────────────────────
void EspRenderer::drawChams(Renderer& r,
                             const PlayerData& player,
                             const ViewMatrix& vm,
                             bool occ,
                             const Vec2& feetSc,
                             const Vec2& headSc,
                             float sw, float sh,
                             const Vec3& predDelta)
{
    float bh = feetSc.y - headSc.y;
    if (bh < 5.f) return;
    float bw = bh * 0.5f;
    float bx = feetSc.x - bw * 0.5f;
    float by = headSc.y;

    const float* base = occ ? g_cfg.chamsOccludedColor : g_cfg.chamsVisibleColor;
    const float alpha = std::clamp(base[3] * g_cfg.chamsAlpha, 0.f, 1.f);
    float colRgba[4] = { base[0], base[1], base[2], alpha };
    unsigned int col = rgbaToArgb(colRgba);

    if (player.bonesValid) {
        static constexpr int kHullBones[] = {
            0, 2, 4, 5, 6,
            8, 9, 11,
            13, 14, 16,
            22, 23, 24,
            25, 26, 27
        };
        const float mx = bw * 1.15f;
        const float my = bh * 0.28f;

        Vec2 raw[PlayerData::kBoneCount];
        int nRaw = 0;
        raw[nRaw++] = headSc;
        raw[nRaw++] = Vec2(feetSc.x - bw * 0.22f, feetSc.y);
        raw[nRaw++] = Vec2(feetSc.x + bw * 0.22f, feetSc.y);
        for (int boneIndex : kHullBones) {
            Vec3 d = player.bones[boneIndex] - player.origin;
            if (d.x*d.x + d.y*d.y + d.z*d.z > 80.f * 80.f) continue;
            Vec2 sp;
            Vec3 boneWorld = player.bones[boneIndex] + predDelta;
            if (!vm.worldToScreen(boneWorld, sp, sw, sh)) continue;
            if (sp.x < bx - mx || sp.x > bx + bw + mx) continue;
            if (sp.y < by - my || sp.y > by + bh + my) continue;
            raw[nRaw++] = Vec2(sp.x, sp.y);
        }

        if (nRaw >= 3) {
            Vec2 hull[PlayerData::kBoneCount];
            int hc = jarvisHull(raw, nRaw, hull);

            if (hc >= 3) {
                float hcx = 0.f, hcy = 0.f;
                for (int i = 0; i < hc; ++i) { hcx += hull[i].x; hcy += hull[i].y; }
                hcx /= (float)hc; hcy /= (float)hc;

                float kExpand = std::clamp(bh * 0.028f, 5.f, 11.f);
                for (int i = 0; i < hc; ++i) {
                    float dx = hull[i].x - hcx, dy = hull[i].y - hcy;
                    float len = sqrtf(dx*dx + dy*dy);
                    if (len > 0.5f) {
                        hull[i].x += dx / len * kExpand;
                        hull[i].y += dy / len * kExpand;
                    }
                }

                float hullPts[PlayerData::kBoneCount * 2];
                for (int i = 0; i < hc; ++i) {
                    hullPts[i*2]   = hull[i].x;
                    hullPts[i*2+1] = hull[i].y;
                }
                r.drawFilledConvexPolygon(hullPts, hc, col);
                return;
            }
        }
    }

    r.drawFilledRect(bx, by, bw, bh, col);
}

static float measureTextNarrow(const FontAtlas* font, const char* text, float size) {
    if (!font || !text || !text[0]) return 0.f;
    float w = 0.f;
    const float scale = size / static_cast<float>(font->renderPx());
    while (*text) {
        const auto* g = font->glyph(static_cast<wchar_t>(*text));
        if (g) w += g->advanceX * scale;
        ++text;
    }
    return w;
}

void EspRenderer::drawPlayerInfo(Renderer& r,
                                  const PlayerData& player,
                                  float boxX, float boxY,
                                  float boxW, float boxH,
                                  bool occ)
{
    if (!m_font)
        return;

    const bool drawHpBar = g_cfg.hpBarEnabled
        && (occ ? g_cfg.hpBarOccluded : true)
        && ((occ ? g_cfg.hpBarPosOccluded : g_cfg.hpBarPosVisible) >= 0);
    const bool drawHpText = g_cfg.hpTextEnabled
        && (occ ? g_cfg.hpTextOccludedEnabled : g_cfg.hpTextVisibleEnabled)
        && ((occ ? g_cfg.hpTextPosOccluded : g_cfg.hpTextPosVisible) >= 0);
    const bool drawName = g_cfg.nameEspEnabled && !player.name.empty()
        && ((occ ? g_cfg.namePosOccluded : g_cfg.namePosVisible) >= 0);
    const bool weaponModeEnabled = g_cfg.weaponEspEnabled
        && (occ ? g_cfg.weaponOccludedEnabled : g_cfg.weaponVisibleEnabled);
    const bool drawWeaponText = weaponModeEnabled && g_cfg.weaponTextEnabled && !player.weaponName.empty()
        && ((occ ? g_cfg.weaponTextPosOccluded : g_cfg.weaponTextPosVisible) >= 0);
    const bool drawWeaponIcon = weaponModeEnabled && g_cfg.weaponIconEnabled && !player.weaponId.empty()
        && ((occ ? g_cfg.weaponIconPosOccluded : g_cfg.weaponIconPosVisible) >= 0);
    const bool valueModeEnabledArmor = g_cfg.armorEspEnabled
        && (occ ? g_cfg.armorOccludedEnabled : g_cfg.armorVisibleEnabled);
    const bool valueModeEnabledAmmo = g_cfg.ammoEspEnabled
        && (occ ? g_cfg.ammoOccludedEnabled : g_cfg.ammoVisibleEnabled)
        && player.ammoClip >= 0;
    const bool drawArmorText = valueModeEnabledArmor && g_cfg.armorTextEnabled
        && ((occ ? g_cfg.armorTextPosOccluded : g_cfg.armorTextPosVisible) >= 0);
    const bool drawArmorBar = valueModeEnabledArmor && g_cfg.armorBarEnabled
        && ((occ ? g_cfg.armorBarPosOccluded : g_cfg.armorBarPosVisible) >= 0);
    const bool drawAmmoText = valueModeEnabledAmmo && g_cfg.ammoTextEnabled
        && ((occ ? g_cfg.ammoTextPosOccluded : g_cfg.ammoTextPosVisible) >= 0);
    const bool drawAmmoBar = valueModeEnabledAmmo && g_cfg.ammoBarEnabled
        && ((occ ? g_cfg.ammoBarPosOccluded : g_cfg.ammoBarPosVisible) >= 0);
    const bool drawFlags = g_cfg.flagsEspEnabled
        && (occ ? g_cfg.flagsOccludedEnabled : g_cfg.flagsVisibleEnabled)
        && ((occ ? g_cfg.flagsPosOccluded : g_cfg.flagsPosVisible) >= 0);

    if (!drawHpBar && !drawHpText && !drawName && !drawWeaponText && !drawWeaponIcon
        && !drawArmorText && !drawArmorBar && !drawAmmoText && !drawAmmoBar && !drawFlags)
        return;

    const unsigned int txtColor = rgbaToArgb(g_cfg.infoTextColor);
    const unsigned int armorBarColor = rgbaToArgb(g_cfg.armorBarColor);
    const unsigned int ammoBarColor = rgbaToArgb(g_cfg.ammoBarColor);
    constexpr unsigned int shadowColor = 0xAA000000u;
    const float textSizeInfo = std::clamp(g_cfg.infoTextSize, 10.f, 24.f);
    const float textSizeName = textSizeInfo + 1.f;
    const float leftX = boxX;

    auto drawOutlined = [&](float x, float y, const char* text, float size) {
        if (!text || !text[0])
            return;
        r.drawText(*m_font, x + 1.f, y + 1.f, text, shadowColor, size);
        r.drawText(*m_font, x, y, text, txtColor, size);
    };

    auto drawBar = [&](float x, float y, float w, float h, float fraction, unsigned int fillColor) {
        fraction = std::clamp(fraction, 0.f, 1.f);
        r.drawFilledRect(x, y, w, h, 0x88000000u);
        r.drawRect(x, y, w, h, 0xB0000000u, 1.f);
        const float fw = (std::max)(1.f, (w - 2.f) * fraction);
        if (fw > 1.f)
            r.drawFilledRect(x + 1.f, y + 1.f, fw, h - 2.f, fillColor);
    };

    struct AnchorStack {
        float top = 0.f;
        float bottom = 0.f;
        float left = 0.f;
        float right = 0.f;
        float topLeft = 0.f;
        float topRight = 0.f;
        float bottomLeft = 0.f;
        float bottomRight = 0.f;
    } stack;

    auto placeRect = [&](EspAnchor anchor, float w, float h, float& outX, float& outY) {
        const float gap = 3.f;
        const float sidePad = 6.f;
        switch (anchor) {
        case EspAnchor::Top:
            outX = boxX + boxW * 0.5f - w * 0.5f;
            outY = boxY - h - gap - stack.top;
            stack.top += h + 1.f;
            break;
        case EspAnchor::Bottom:
            outX = boxX + boxW * 0.5f - w * 0.5f;
            outY = boxY + boxH + gap + stack.bottom;
            stack.bottom += h + 1.f;
            break;
        case EspAnchor::Left:
            outX = boxX - w - sidePad - stack.left;
            outY = boxY;
            stack.left += w + 2.f;
            break;
        case EspAnchor::Right:
            outX = boxX + boxW + sidePad + stack.right;
            outY = boxY;
            stack.right += w + 2.f;
            break;
        case EspAnchor::TopLeft:
            outX = boxX - w - 4.f;
            outY = boxY - h - gap - stack.topLeft;
            stack.topLeft += h + 1.f;
            break;
        case EspAnchor::TopRight:
            outX = boxX + boxW + 4.f;
            outY = boxY - h - gap - stack.topRight;
            stack.topRight += h + 1.f;
            break;
        case EspAnchor::BottomLeft:
            outX = boxX - w - 4.f;
            outY = boxY + boxH + gap + stack.bottomLeft;
            stack.bottomLeft += h + 1.f;
            break;
        case EspAnchor::BottomRight:
        default:
            outX = boxX + boxW + 4.f;
            outY = boxY + boxH + gap + stack.bottomRight;
            stack.bottomRight += h + 1.f;
            break;
        }

        const int ai = std::clamp(static_cast<int>(anchor), 0, 7);
        outX += g_cfg.espAnchorOffsetX[ai];
        outY += g_cfg.espAnchorOffsetY[ai];
    };

    struct RenderItem {
        int id;
        EspAnchor anchor;
        int order;
    };
    RenderItem items[10]{};
    int itemCount = 0;
    const auto& orderArr = occ ? g_cfg.espItemOrderOccluded : g_cfg.espItemOrderVisible;
    auto addItem = [&](int id, EspAnchor anchor) {
        items[itemCount++] = { id, anchor, orderArr[id] };
    };

    if (drawHpBar) {
        addItem(0, sanitizeBarAnchor(resolveEspAnchor(occ ? g_cfg.hpBarPosOccluded : g_cfg.hpBarPosVisible, EspAnchor::Left)));
    }

    if (drawHpText) {
        addItem(1, sanitizeInfoAnchor(resolveEspAnchor(occ ? g_cfg.hpTextPosOccluded : g_cfg.hpTextPosVisible, EspAnchor::TopRight)));
    }

    if (drawName) {
        addItem(2, sanitizeInfoAnchor(resolveEspAnchor(occ ? g_cfg.namePosOccluded : g_cfg.namePosVisible, EspAnchor::Top)));
    }

    if (drawWeaponText) {
        addItem(3, sanitizeInfoAnchor(resolveEspAnchor(occ ? g_cfg.weaponTextPosOccluded : g_cfg.weaponTextPosVisible, EspAnchor::BottomLeft)));
    }

    if (drawWeaponIcon) {
        addItem(4, sanitizeInfoAnchor(resolveEspAnchor(occ ? g_cfg.weaponIconPosOccluded : g_cfg.weaponIconPosVisible, EspAnchor::BottomRight)));
    }

    if (drawArmorBar) {
        addItem(5, sanitizeBarAnchor(resolveEspAnchor(occ ? g_cfg.armorBarPosOccluded : g_cfg.armorBarPosVisible, EspAnchor::Left)));
    }

    if (drawArmorText) {
        addItem(6, sanitizeInfoAnchor(resolveEspAnchor(occ ? g_cfg.armorTextPosOccluded : g_cfg.armorTextPosVisible, EspAnchor::BottomLeft)));
    }

    if (drawAmmoBar) {
        addItem(7, sanitizeBarAnchor(resolveEspAnchor(occ ? g_cfg.ammoBarPosOccluded : g_cfg.ammoBarPosVisible, EspAnchor::Right)));
    }

    if (drawAmmoText) {
        addItem(8, sanitizeInfoAnchor(resolveEspAnchor(occ ? g_cfg.ammoTextPosOccluded : g_cfg.ammoTextPosVisible, EspAnchor::BottomRight)));
    }

    if (drawFlags) {
        addItem(9, sanitizeInfoAnchor(resolveEspAnchor(occ ? g_cfg.flagsPosOccluded : g_cfg.flagsPosVisible, EspAnchor::TopRight)));
    }

    std::sort(items, items + itemCount, [](const RenderItem& a, const RenderItem& b) {
        if (a.anchor != b.anchor)
            return a.anchor < b.anchor;
        return a.order < b.order;
    });

    for (int i = 0; i < itemCount; ++i) {
        const int id = items[i].id;
        const EspAnchor anchor = items[i].anchor;
        switch (id) {
        case 0: {
            const bool horizontal = (anchor == EspAnchor::Top || anchor == EspAnchor::Bottom);
            const float bw = horizontal ? (std::max)(28.f, boxW) : (std::max)(2.f, g_cfg.hpBarWidth);
            const float bh = horizontal ? (std::max)(3.f, g_cfg.hpBarWidth) : (std::max)(26.f, boxH);
            float x = leftX;
            float y = boxY;
            placeRect(anchor, bw, bh, x, y);
            
            float hpFraction = std::clamp(player.health / 100.f, 0.f, 1.f);
            const float* base = occ ? g_cfg.hpBarOccludedColor : g_cfg.hpBarVisibleColor;
            float outCol[4] = {
                std::clamp(base[0] * (0.40f + hpFraction * 0.60f), 0.f, 1.f),
                std::clamp(base[1] * (0.40f + hpFraction * 0.60f), 0.f, 1.f),
                std::clamp(base[2] * (0.40f + hpFraction * 0.60f), 0.f, 1.f),
                base[3]
            };
            unsigned int fillColor = rgbaToArgb(outCol);
            
            if (horizontal) {
                drawBar(x, y, bw, bh, hpFraction, fillColor);
            } else {
                r.drawFilledRect(x, y, bw, bh, 0x88000000u);
                r.drawRect(x, y, bw, bh, 0xB0000000u, 1.f);
                const float fh = (std::max)(1.f, (bh - 2.f) * hpFraction);
                r.drawFilledRect(x + 1.f, y + (bh - 1.f - fh), bw - 2.f, fh, fillColor);
            }
            break;
        }
        case 1: {
            char hpLabel[16]{};
            std::snprintf(hpLabel, sizeof(hpLabel), "HP: %d", std::clamp(player.health, 0, 100));
            float x = leftX;
            float y = boxY;
            float actualW = measureTextNarrow(m_font, hpLabel, textSizeInfo);
            placeRect(anchor, approxTextWidth(hpLabel, textSizeInfo), textSizeInfo + 1.f, x, y);
            if (anchor == EspAnchor::Top || anchor == EspAnchor::Bottom)
                x = boxX + boxW * 0.5f - actualW * 0.5f;
            drawOutlined(x, y, hpLabel, textSizeInfo);
            break;
        }
        case 2: {
            std::string displayName = player.name;
            if (player.isBot && !displayName.empty()) {
                if (_strnicmp(displayName.c_str(), "bot ", 4) != 0)
                    displayName = "BOT " + displayName;
            }
            float x = leftX;
            float y = boxY;
            float actualW = measureTextNarrow(m_font, displayName.c_str(), textSizeName);
            placeRect(anchor, approxTextWidth(displayName.c_str(), textSizeName), textSizeName + 1.f, x, y);
            if (anchor == EspAnchor::Top || anchor == EspAnchor::Bottom)
                x = boxX + boxW * 0.5f - actualW * 0.5f;
            drawOutlined(x, y, displayName.c_str(), textSizeName);
            break;
        }
        case 3: {
            float x = leftX;
            float y = boxY;
            float actualW = measureTextNarrow(m_font, player.weaponName.c_str(), textSizeInfo);
            placeRect(anchor, approxTextWidth(player.weaponName.c_str(), textSizeInfo), textSizeInfo + 1.f, x, y);
            if (anchor == EspAnchor::Top || anchor == EspAnchor::Bottom)
                x = boxX + boxW * 0.5f - actualW * 0.5f;
            drawOutlined(x, y, player.weaponName.c_str(), textSizeInfo);
            break;
        }
        case 4: {
            const wchar_t glyph[2] = { mapWeaponIconGlyph(player.weaponId), 0 };
            if (glyph[0] != 0) {
                const float iconSize = textSizeInfo + 3.f;
                float x = leftX;
                float y = boxY;
                float actualW = 0.f;
                const auto* g = m_font->glyph(glyph[0]);
                if (g) actualW = g->advanceX * (iconSize / static_cast<float>(m_font->renderPx()));
                placeRect(anchor, iconSize * 0.85f, iconSize, x, y);
                if (anchor == EspAnchor::Top || anchor == EspAnchor::Bottom)
                    x = boxX + boxW * 0.5f - actualW * 0.5f;
                r.drawTextW(*m_font, x + 1.f, y + 1.f, glyph, shadowColor, iconSize);
                r.drawTextW(*m_font, x, y, glyph, txtColor, iconSize);
            }
            break;
        }
        case 5: {
            const int armor = std::clamp(player.armor, 0, 100);
            const bool horizontal = (anchor == EspAnchor::Top || anchor == EspAnchor::Bottom);
            const float bw = horizontal ? (std::max)(28.f, boxW) : 5.f;
            const float bh = horizontal ? 5.f : (std::max)(26.f, boxH);
            float x = leftX;
            float y = boxY;
            placeRect(anchor, bw, bh, x, y);
            if (horizontal) {
                drawBar(x, y, bw, bh, armor / 100.f, armorBarColor);
            } else {
                const float frac = std::clamp(armor / 100.f, 0.f, 1.f);
                r.drawFilledRect(x, y, bw, bh, 0x88000000u);
                r.drawRect(x, y, bw, bh, 0xB0000000u, 1.f);
                const float fh = (std::max)(1.f, (bh - 2.f) * frac);
                r.drawFilledRect(x + 1.f, y + (bh - 1.f - fh), bw - 2.f, fh, armorBarColor);
            }
            break;
        }
        case 6: {
            const int armor = std::clamp(player.armor, 0, 100);
            char armorLabel[32]{};
            std::snprintf(armorLabel, sizeof(armorLabel), "Armor: %d", armor);
            float x = leftX;
            float y = boxY;
            float actualW = measureTextNarrow(m_font, armorLabel, textSizeInfo);
            placeRect(anchor, approxTextWidth(armorLabel, textSizeInfo), textSizeInfo + 1.f, x, y);
            if (anchor == EspAnchor::Top || anchor == EspAnchor::Bottom)
                x = boxX + boxW * 0.5f - actualW * 0.5f;
            drawOutlined(x, y, armorLabel, textSizeInfo);
            break;
        }
        case 7: {
            const int ammoClip = (std::max)(0, player.ammoClip);
            const int ammoMax = player.ammoMaxClip > 0 ? player.ammoMaxClip : (std::max)(ammoClip, 1);
            const bool horizontal = (anchor == EspAnchor::Top || anchor == EspAnchor::Bottom);
            const float bw = horizontal ? (std::max)(28.f, boxW) : 5.f;
            const float bh = horizontal ? 5.f : (std::max)(26.f, boxH);
            float x = leftX;
            float y = boxY;
            placeRect(anchor, bw, bh, x, y);
            if (horizontal) {
                drawBar(x, y, bw, bh, static_cast<float>(ammoClip) / static_cast<float>(ammoMax), ammoBarColor);
            } else {
                const float frac = std::clamp(static_cast<float>(ammoClip) / static_cast<float>(ammoMax), 0.f, 1.f);
                r.drawFilledRect(x, y, bw, bh, 0x88000000u);
                r.drawRect(x, y, bw, bh, 0xB0000000u, 1.f);
                const float fh = (std::max)(1.f, (bh - 2.f) * frac);
                r.drawFilledRect(x + 1.f, y + (bh - 1.f - fh), bw - 2.f, fh, ammoBarColor);
            }
            break;
        }
        case 8: {
            const int ammoClip = (std::max)(0, player.ammoClip);
            const int ammoMax = player.ammoMaxClip > 0 ? player.ammoMaxClip : (std::max)(ammoClip, 1);
            char ammoLabel[32]{};
            std::snprintf(ammoLabel, sizeof(ammoLabel), "Ammo: %d/%d", ammoClip, ammoMax);
            float x = leftX;
            float y = boxY;
            float actualW = measureTextNarrow(m_font, ammoLabel, textSizeInfo);
            placeRect(anchor, approxTextWidth(ammoLabel, textSizeInfo), textSizeInfo + 1.f, x, y);
            if (anchor == EspAnchor::Top || anchor == EspAnchor::Bottom)
                x = boxX + boxW * 0.5f - actualW * 0.5f;
            drawOutlined(x, y, ammoLabel, textSizeInfo);
            break;
        }
        case 9: {
            if (g_cfg.flagFlashedEnabled && player.isFlashed) {
                float x = boxX;
                float y = boxY;
                float actualW = measureTextNarrow(m_font, "FLASHED", textSizeInfo);
                placeRect(anchor, approxTextWidth("FLASHED", textSizeInfo), textSizeInfo + 1.f, x, y);
                if (anchor == EspAnchor::Top || anchor == EspAnchor::Bottom)
                    x = boxX + boxW * 0.5f - actualW * 0.5f;
                drawOutlined(x, y, "FLASHED", textSizeInfo);
            }
            if (g_cfg.flagDefusingEnabled && player.isDefusing) {
                float x = boxX;
                float y = boxY;
                float actualW = measureTextNarrow(m_font, "DEFUSING", textSizeInfo);
                placeRect(anchor, approxTextWidth("DEFUSING", textSizeInfo), textSizeInfo + 1.f, x, y);
                if (anchor == EspAnchor::Top || anchor == EspAnchor::Bottom)
                    x = boxX + boxW * 0.5f - actualW * 0.5f;
                drawOutlined(x, y, "DEFUSING", textSizeInfo);
            }
            if (g_cfg.flagScopedEnabled && player.isScoped) {
                float x = boxX;
                float y = boxY;
                float actualW = measureTextNarrow(m_font, "SCOPED", textSizeInfo);
                placeRect(anchor, approxTextWidth("SCOPED", textSizeInfo), textSizeInfo + 1.f, x, y);
                if (anchor == EspAnchor::Top || anchor == EspAnchor::Bottom)
                    x = boxX + boxW * 0.5f - actualW * 0.5f;
                drawOutlined(x, y, "SCOPED", textSizeInfo);
            }
            if (g_cfg.flagDefuseKitEnabled && player.teamNum == 3 && player.hasDefuseKit) {
                const wchar_t kitGlyph[2] = { 0xE066, 0 };
                const float kitSize = textSizeInfo + 1.f;
                float x = boxX;
                float y = boxY;
                float actualW = 0.f;
                const auto* g = m_font->glyph(kitGlyph[0]);
                if (g) actualW = g->advanceX * (kitSize / static_cast<float>(m_font->renderPx()));
                placeRect(anchor, kitSize * 0.85f, kitSize + 1.f, x, y);
                if (anchor == EspAnchor::Top || anchor == EspAnchor::Bottom)
                    x = boxX + boxW * 0.5f - actualW * 0.5f;
                r.drawTextW(*m_font, x + 1.f, y + 1.f, kitGlyph, shadowColor, kitSize);
                r.drawTextW(*m_font, x, y, kitGlyph, txtColor, kitSize);
            }
            break;
        }
        default:
            break;
        }
    }
}
// ─── Pick player colour based on visibility state ─────────────────────────────

/// Returns the RGBA colour array for a player, choosing visible/occluded/dormant
/// variants based on the current visibility check result and config toggles.
static const float* pickPlayerColor(const PlayerData& p, int localTeam) {
    const bool occluded = g_cfg.visibilityCheckEnabled
                       && p.visibilityChecked && !p.isVisible;
    if (p.teamNum != localTeam)
        return occluded ? g_cfg.enemyOccludedColor : g_cfg.enemyVisibleColor;
    else
        return occluded ? g_cfg.teamOccludedColor : g_cfg.teamVisibleColor;
}

// ─── Colour utilities ──────────────────────────────────────────────────────────

unsigned int EspRenderer::lerpColor(unsigned int a, unsigned int b, float t) {
    t = std::clamp(t, 0.f, 1.f);

    auto lerp = [](int c0, int c1, float t) -> int {
        return static_cast<int>(c0 + (c1 - c0) * t);
    };

    int aA = (a >> 24) & 0xFF, aR = (a >> 16) & 0xFF,
        aG = (a >>  8) & 0xFF, aB = (a >>  0) & 0xFF;
    int bA = (b >> 24) & 0xFF, bR = (b >> 16) & 0xFF,
        bG = (b >>  8) & 0xFF, bB = (b >>  0) & 0xFF;

    int rA = lerp(aA, bA, t), rR = lerp(aR, bR, t),
        rG = lerp(aG, bG, t), rB = lerp(aB, bB, t);

    return (rA << 24) | (rR << 16) | (rG << 8) | rB;
}

static float getBombMapRadius(const std::string& mapName);

void EspRenderer::drawRadar(Renderer& r, const EntityManager::Snapshot& snap) {
    const auto& players = snap.players;
    const PlayerData* local = nullptr;
    for (const auto& p : players) {
        if (p.isValid && p.isAlive && p.isLocalPlayer) {
            local = &p;
            break;
        }
    }
    if (!local)
        return;

    const float sw = static_cast<float>(r.screenWidth());
    const float sh = static_cast<float>(r.screenHeight());
    const bool overlayMode = (g_cfg.radarMode == 1);
    const float size = overlayMode ? 365.f : std::clamp(g_cfg.radarSize, 120.f, 420.f);
    const float range = overlayMode ? 1167.f : std::clamp(g_cfg.radarRange, 400.f, 5000.f);
    const float x = overlayMode ? 18.f : std::clamp(g_cfg.radarPosX, 0.f, 1.f) * (sw - size - 36.f) + 18.f;
    const float y = overlayMode ? 18.f : std::clamp(g_cfg.radarPosY, 0.f, 1.f) * (sh - size - 36.f) + 18.f;
    const float cx = x + size * 0.5f;
    const float cy = y + size * 0.5f;
    const float radius = size * 0.42f;
    const float bgOpacity = overlayMode ? 0.f : std::clamp(g_cfg.radarBgOpacity, 0.f, 1.f);

    auto applyAlpha = [](unsigned int argb, float alpha) {
        alpha = std::clamp(alpha, 0.f, 1.f);
        return (argb & 0x00FFFFFFu) | (static_cast<unsigned int>(alpha * 255.f + 0.5f) << 24);
    };

    if (!overlayMode) {
        // Soft shadow stack to avoid visible opacity steps.
        for (int i = 0; i < 4; ++i) {
            float spread = 4.f + i * 5.5f;
            unsigned int alpha = static_cast<unsigned int>(24.f - i * 4.5f);
            r.drawRoundedFilledRect(x - spread, y - spread, size + spread * 2.f, size + spread * 2.f,
                                    (0x000000u | (alpha << 24)), 12.f + spread * 0.6f);
        }

        r.drawRoundedFilledRect(x, y, size, size, applyAlpha(0xFF0A0F19u, bgOpacity), 12.f);
        r.drawRoundedRect(x, y, size, size, 0xFF2B3041, 12.f, 1.f);

        // Radar grid.
        r.drawCircle(cx, cy, radius, 0x553C4560, 1.f);
        r.drawCircle(cx, cy, radius * 0.66f, 0x44333D57, 1.f);
        r.drawCircle(cx, cy, radius * 0.33f, 0x44333D57, 1.f);
        r.drawLine(cx - radius, cy, cx + radius, cy, 0x44343E58, 1.f);
        r.drawLine(cx, cy - radius, cx, cy + radius, 0x44343E58, 1.f);
    }

    const float yawRad = local->eyeYaw * (3.14159265f / 180.f);
    const Vec2 fwd{ std::cos(yawRad), std::sin(yawRad) };
    const Vec2 right{ fwd.y, -fwd.x };
    const int localTeam = snap.localTeam;

    if (snap.bomb.isPlanted) {
        const float maxBlastWU = getBombMapRadius(snap.currentMapName);
        Vec3 db = snap.bomb.origin - local->origin;
        const float localDist2D = std::sqrt(db.x * db.x + db.y * db.y);
        if (localDist2D <= maxBlastWU) {
        float bombForward = db.x * fwd.x + db.y * fwd.y;
        float bombSide = db.x * right.x + db.y * right.y;

        float bx = cx + (bombSide / range) * radius;
        float by = cy - (bombForward / range) * radius;

        float bdx = bx - cx;
        float bdy = by - cy;
        float bdist = std::sqrt(bdx * bdx + bdy * bdy);
        if (bdist > radius - 3.f && bdist > 0.001f) {
            float bscale = (radius - 3.f) / bdist;
            bx = cx + bdx * bscale;
            by = cy + bdy * bscale;
        }

        const float minBlastWU = maxBlastWU * 0.35f;
        const float midBlastWU = maxBlastWU * 0.70f;
        auto wuToRadar = [&](float wu) {
            return std::clamp((wu / range) * radius, 2.f, radius * 0.95f);
        };
        const float rMin = wuToRadar(minBlastWU);
        const float rMid = wuToRadar(midBlastWU);
        const float rMax = wuToRadar(maxBlastWU);

        float panicAlpha = 0.22f;
        if (snap.bomb.timeRemaining > 0.f) {
            const float normalized = std::clamp((40.f - snap.bomb.timeRemaining) / 40.f, 0.f, 1.f);
            panicAlpha = 0.18f + normalized * 0.28f;
        }

        r.drawFilledCircle(bx, by, rMax, applyAlpha(0xFFDE3A2Bu, panicAlpha * 0.35f));
        r.drawFilledCircle(bx, by, rMid, applyAlpha(0xFFF57F29u, panicAlpha * 0.35f));
        r.drawFilledCircle(bx, by, rMin, applyAlpha(0xFFF7D34Fu, panicAlpha * 0.45f));

        r.drawCircle(bx, by, rMax, applyAlpha(0xFFFF4E4Eu, 0.75f), 1.6f);
        r.drawCircle(bx, by, rMid, applyAlpha(0xFFFFB347u, 0.82f), 1.6f);
        r.drawCircle(bx, by, rMin, applyAlpha(0xFFFFFF99u, 0.90f), 1.6f);
        r.drawFilledCircle(bx, by, 3.2f, 0xFFFF4B4Bu);
        }
    }

    for (const auto& p : players) {
        if (!p.isValid || !p.isAlive || p.isLocalPlayer)
            continue;
        if (p.isDormant)
            continue;
        if (g_cfg.enemyOnly && p.teamNum == localTeam)
            continue;

        Vec3 d3 = p.origin - local->origin;
        float forward = d3.x * fwd.x + d3.y * fwd.y;
        float side = d3.x * right.x + d3.y * right.y;

        float px = cx + (side / range) * radius;
        float py = cy - (forward / range) * radius;

        float dx = px - cx;
        float dy = py - cy;
        float dist = std::sqrt(dx * dx + dy * dy);
        if (dist > radius - 3.f && dist > 0.001f) {
            float scale = (radius - 3.f) / dist;
            px = cx + dx * scale;
            py = cy + dy * scale;
        }

        unsigned int col;
        if (p.teamNum != localTeam) col = rgbaToArgb(pickPlayerColor(p, localTeam));
        else col = 0xFFFFD94Au;

        float blipR = std::clamp(g_cfg.radarBlipSize, 2.f, 8.f);
        r.drawFilledCircle(px, py, blipR, col);
        r.drawCircle(px, py, std::clamp(blipR + 1.4f, 3.5f, 10.f), 0xB0FFFFFF, 1.f);

        Vec2 pFwd{ std::cos(p.eyeYaw * (3.14159265f / 180.f)), std::sin(p.eyeYaw * (3.14159265f / 180.f)) };
        float arrowX = pFwd.x * right.x + pFwd.y * right.y;
        float arrowY = -(pFwd.x * fwd.x + pFwd.y * fwd.y);
        float arrowLen = std::sqrt(arrowX * arrowX + arrowY * arrowY);
        if (arrowLen > 0.001f) {
            arrowX /= arrowLen;
            arrowY /= arrowLen;
            float tipDist = blipR + 6.5f;
            float tipX = px + arrowX * tipDist;
            float tipY = py + arrowY * tipDist;
            float baseX = px + arrowX * (blipR + 2.0f);
            float baseY = py + arrowY * (blipR + 2.0f);
            float perpX = -arrowY;
            float perpY = arrowX;
            float wing = 2.2f;
            r.drawFilledTriangle(
                tipX, tipY,
                baseX + perpX * wing, baseY + perpY * wing,
                baseX - perpX * wing, baseY - perpY * wing,
                col
            );
        }
    }

    if (!overlayMode) {
        // Local indicator (upward triangle).
        r.drawFilledTriangle(cx, cy - 8.f, cx - 6.f, cy + 6.f, cx + 6.f, cy + 6.f, Theme::ACCENT);
        r.drawCircle(cx, cy, 6.f, 0xAAFFFFFF, 1.f);
    }

    if (g_cfg.menuVisible) {
        ImGuiIO& io = ImGui::GetIO();
        const bool hovered = io.MousePos.x >= x && io.MousePos.x <= x + size
            && io.MousePos.y >= y && io.MousePos.y <= y + size;
        static bool s_dragRadar = false;
        static float s_dragOffsetX = 0.f;
        static float s_dragOffsetY = 0.f;
        if (!s_dragRadar && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left, false)) {
            s_dragRadar = true;
            s_dragOffsetX = io.MousePos.x - x;
            s_dragOffsetY = io.MousePos.y - y;
        }
        if (s_dragRadar) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                float nx = std::clamp(io.MousePos.x - s_dragOffsetX, 0.f, sw - size);
                float ny = std::clamp(io.MousePos.y - s_dragOffsetY, 0.f, sh - size);
                g_cfg.radarPosX = (sw - size > 0.f) ? (nx / (sw - size)) : 0.f;
                g_cfg.radarPosY = (sh - size > 0.f) ? (ny / (sh - size)) : 0.f;
                if (overlayMode) g_cfg.radarMode = 1;
            } else {
                s_dragRadar = false;
            }
        }
    }
}
//
//  Projects each CS2 bone world-position to screen and draws line segments
//  between them.  Bone indices (as seen in the CS2 skeleton, 2024-2025 era):
//    0=pelvis  2=spine  4=chest  5=neck  6=head
//    8=Lshoulder  9=Lelbow  11=Lhand
//    13=Rshoulder  14=Relbow  16=Rhand
//    22=Lhip  23=Lknee  24=Lankle
//    25=Rhip  26=Rknee  27=Rankle

void EspRenderer::drawSkeleton(Renderer& r,
                                const PlayerData& player,
                                const ViewMatrix& vm,
                                bool occ,
                                const Vec2& feetSc,
                                const Vec2& headSc,
                                const Vec3& predDelta)
{
    float sw = static_cast<float>(r.screenWidth());
    float sh = static_cast<float>(r.screenHeight());
    unsigned int col = rgbaToArgb(occ ? g_cfg.skeletonOccludedColor : g_cfg.skeletonVisibleColor);

    float bodyH = feetSc.y - headSc.y;
    if (bodyH < 5.f) return;
    float boxW = bodyH * 0.5f;
    float maxScreenSeg = std::clamp(bodyH * 0.58f, 36.f, 120.f);
    float lineThickness = std::clamp(bodyH / 150.f, 1.35f, 2.25f);

    auto drawFallbackSkeleton = [&]() {
        float cx = feetSc.x;
        float topY = headSc.y;

        Vec2 head  { cx, topY };
        Vec2 neck  { cx, topY + bodyH * 0.12f };
        Vec2 chest { cx, topY + bodyH * 0.28f };
        Vec2 pelvis{ cx, topY + bodyH * 0.52f };

        Vec2 lShoulder{ cx - boxW * 0.30f, topY + bodyH * 0.30f };
        Vec2 rShoulder{ cx + boxW * 0.30f, topY + bodyH * 0.30f };
        Vec2 lElbow   { cx - boxW * 0.44f, topY + bodyH * 0.42f };
        Vec2 rElbow   { cx + boxW * 0.44f, topY + bodyH * 0.42f };
        Vec2 lHand    { cx - boxW * 0.36f, topY + bodyH * 0.58f };
        Vec2 rHand    { cx + boxW * 0.36f, topY + bodyH * 0.58f };

        Vec2 lHip     { cx - boxW * 0.16f, pelvis.y };
        Vec2 rHip     { cx + boxW * 0.16f, pelvis.y };
        Vec2 lKnee    { cx - boxW * 0.12f, topY + bodyH * 0.77f };
        Vec2 rKnee    { cx + boxW * 0.12f, topY + bodyH * 0.77f };
        Vec2 lAnkle   { cx - boxW * 0.10f, topY + bodyH * 0.98f };
        Vec2 rAnkle   { cx + boxW * 0.10f, topY + bodyH * 0.98f };

        auto seg2d = [&](const Vec2& a, const Vec2& b) {
            float dx = a.x - b.x;
            float dy = a.y - b.y;
            float lenSq = dx * dx + dy * dy;
            if (lenSq < 1.f || lenSq > maxScreenSeg * maxScreenSeg)
                return;
            r.drawLine(a.x, a.y, b.x, b.y, col, lineThickness);
        };

        seg2d(head, neck);
        seg2d(neck, chest);
        seg2d(chest, pelvis);

        seg2d(chest, lShoulder);
        seg2d(lShoulder, lElbow);
        seg2d(lElbow, lHand);

        seg2d(chest, rShoulder);
        seg2d(rShoulder, rElbow);
        seg2d(rElbow, rHand);

        seg2d(pelvis, lHip);
        seg2d(lHip, lKnee);
        seg2d(lKnee, lAnkle);

        seg2d(pelvis, rHip);
        seg2d(rHip, rKnee);
        seg2d(rKnee, rAnkle);
    };

    auto drawLiveSkeleton = [&]() -> bool {
        if (!player.bonesValid)
            return false;

        static constexpr int kRenderBones[] = {
            0, 2, 4, 5, 6,
            8, 9, 11,
            13, 14, 16,
            22, 23, 24,
            25, 26, 27,
        };
        static constexpr int kConnections[][2] = {
            { 0, 2 }, { 2, 4 }, { 4, 5 }, { 5, 6 },
            { 5, 8 }, { 8, 9 }, { 9, 11 },
            { 5, 13 }, { 13, 14 }, { 14, 16 },
            { 0, 22 }, { 22, 23 }, { 23, 24 },
            { 0, 25 }, { 25, 26 }, { 26, 27 },
        };

        Vec2 projected[PlayerData::kBoneCount]{};
        bool projectedOk[PlayerData::kBoneCount]{};

        const float minX = feetSc.x - boxW * 1.0f;
        const float maxX = feetSc.x + boxW * 1.0f;
        const float minY = headSc.y - bodyH * 0.18f;
        const float maxY = feetSc.y + bodyH * 0.08f;

        auto projectBone = [&](int boneIndex) {
            if (boneIndex < 0 || boneIndex >= PlayerData::kBoneCount)
                return;

            const Vec3& bone = player.bones[boneIndex];
            if (!std::isfinite(bone.x) || !std::isfinite(bone.y) || !std::isfinite(bone.z))
                return;

            Vec3 boneWorld = bone + predDelta;
            Vec3 delta = bone - player.origin;

            Vec2 screen;
            if (!vm.worldToScreen(boneWorld, screen, sw, sh))
                return;
            if (screen.x < minX || screen.x > maxX || screen.y < minY || screen.y > maxY)
                return;

            projected[boneIndex] = screen;
            projectedOk[boneIndex] = true;
        };

        for (int boneIndex : kRenderBones)
            projectBone(boneIndex);

        int drawnSegments = 0;
        for (const auto& connection : kConnections) {
            int first = connection[0];
            int second = connection[1];
            if (!projectedOk[first] || !projectedOk[second])
                continue;

            float dx = projected[first].x - projected[second].x;
            float dy = projected[first].y - projected[second].y;
            float lenSq = dx * dx + dy * dy;
            if (lenSq < 4.f || lenSq > maxScreenSeg * maxScreenSeg)
                continue;

            r.drawLine(projected[first].x,
                       projected[first].y,
                       projected[second].x,
                       projected[second].y,
                       col,
                       lineThickness);
            ++drawnSegments;
        }

        return drawnSegments >= 8;
    };

    if (!drawLiveSkeleton())
        drawFallbackSkeleton();
}

// ─── World-to-screen with viewport edge clamping ──────────────────────────────────────────────────────
static bool worldToScreenClamped(const ViewMatrix& vm, const Vec3& world,
                                  float sw, float sh, Vec2& edgePt)
{
    Vec2 sc;
    if (vm.worldToScreen(world, sc, sw, sh) &&
        sc.x >= 0.f && sc.x <= sw && sc.y >= 0.f && sc.y <= sh)
    {
        edgePt = sc;
        return true;
    }
    const auto& m = vm.m;
    float w  = m[3][0]*world.x + m[3][1]*world.y + m[3][2]*world.z + m[3][3];
    float cx = m[0][0]*world.x + m[0][1]*world.y + m[0][2]*world.z + m[0][3];
    float cy = m[1][0]*world.x + m[1][1]*world.y + m[1][2]*world.z + m[1][3];
    (void)w;
    float sdx =  cx * (sw * 0.5f);
    float sdy = -cy * (sh * 0.5f);
    float len = std::sqrtf(sdx*sdx + sdy*sdy);
    if (len < 0.001f) { edgePt = Vec2(sw * 0.5f, sh * 0.5f); return false; }
    sdx /= len; sdy /= len;
    constexpr float kMargin = 80.f;
    float tx = std::fabsf(sdx) > 0.001f ? (sw * 0.5f - kMargin) / std::fabsf(sdx) : 1e9f;
    float ty = std::fabsf(sdy) > 0.001f ? (sh * 0.5f - kMargin) / std::fabsf(sdy) : 1e9f;
    float t  = tx < ty ? tx : ty;
    edgePt = Vec2(sw * 0.5f + sdx * t, sh * 0.5f + sdy * t);
    return false;
}

// --- Text measurement helper -------------------------------------------------
static float measureTextW(const FontAtlas& font, const wchar_t* text, float size) {
    float scale = size / font.renderPx();
    float w = 0.f;
    for (const wchar_t* p = text; *p; ++p) {
        if (*p == L' ') {
            w += 8.f * scale;
        } else if (const GlyphInfo* gi = font.glyph(*p)) {
            w += gi->advanceX * scale;
        } else {
            w += 8.f * scale;
        }
    }
}

static float textCenterY(const FontAtlas& font, float top, float boxH, float fontSize) {
    return top + (boxH - font.lineHeight(fontSize)) * 0.5f;
}

static float textVisualCenterY(const FontAtlas& font, float top, float boxH,
                               const wchar_t* text, float fontSize) {
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

static float textVisualCenterX(const FontAtlas& font, float left, float boxW,
                               const wchar_t* text, float fontSize) {
    if (!text || !*text)
        return left;

    float scale = fontSize / font.renderPx();
    float cursorX = 0.f;
    float minX = (std::numeric_limits<float>::max)();
    float maxX = (std::numeric_limits<float>::lowest)();
    float spaceAdv = 8.f;
    if (const GlyphInfo* sg = font.glyph(L' '))
        spaceAdv = static_cast<float>(sg->advanceX);

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
        return left + (boxW - measureTextW(font, text, fontSize)) * 0.5f;

    float boundsW = maxX - minX;
    return left + (boxW - boundsW) * 0.5f - minX;
}

static float measureTextActualWidth(const Renderer& rn, const FontAtlas& font,
                                    const wchar_t* text, float fontSize) {
    float minX = 0.f;
    float minY = 0.f;
    float maxX = 0.f;
    float maxY = 0.f;
    rn.measureTextBoundsW(font, text, fontSize, minX, minY, maxX, maxY);
    return maxX - minX;
}

static float textActualCenterX(const Renderer& rn, const FontAtlas& font,
                               float left, float boxW,
                               const wchar_t* text, float fontSize) {
    float minX = 0.f;
    float minY = 0.f;
    float maxX = 0.f;
    float maxY = 0.f;
    rn.measureTextBoundsW(font, text, fontSize, minX, minY, maxX, maxY);
    float boundsW = maxX - minX;
    return left + (boxW - boundsW) * 0.5f - minX;
}

static float textActualCenterY(const Renderer& rn, const FontAtlas& font,
                               float top, float boxH,
                               const wchar_t* text, float fontSize) {
    float minX = 0.f;
    float minY = 0.f;
    float maxX = 0.f;
    float maxY = 0.f;
    rn.measureTextBoundsW(font, text, fontSize, minX, minY, maxX, maxY);
    float boundsH = maxY - minY;
    return top + (boxH - boundsH) * 0.5f - minY;
}

static float textHudCenterY(const Renderer& rn, const FontAtlas& font, float top, float boxH,
                            const wchar_t* text, float fontSize) {
    return textActualCenterY(rn, font, top, boxH, text, fontSize) + fontSize * 0.10f;
}

static float textHudCompactCenterY(const Renderer& rn, const FontAtlas& font, float top, float boxH,
                                   const wchar_t* text, float fontSize) {
    return textActualCenterY(rn, font, top, boxH, text, fontSize) + fontSize * 0.02f;
}

static float textHudClockCenterY(const Renderer& rn, const FontAtlas& font, float top, float boxH,
                                 const wchar_t* text, float fontSize) {
    return textActualCenterY(rn, font, top, boxH, text, fontSize) - fontSize * 0.01f;
}

// --- Centred single-glyph text helper --------------------------------------------
static void drawCenteredIcon(Renderer& rn, const FontAtlas& font, float cx, float cy,
                              wchar_t ch, float size, unsigned int color) {
    float scale = size / font.renderPx();
    const GlyphInfo* gi = font.glyph(ch);
    if (!gi || gi->width <= 0 || gi->height <= 0) return;
    float cursorX = cx - (gi->bearingX + gi->width * 0.5f) * scale;
    float baselineY = cy - (gi->bearingY + gi->height * 0.5f) * scale;
    wchar_t str[2] = { ch, 0 };
    rn.drawTextW(font, cursorX, baselineY, str, color, size);
}

static unsigned int withAlpha(unsigned int color, unsigned int alpha) {
    return (color & 0x00FFFFFFu) | (alpha << 24);
}

static const char* spectatorModeLabel(int mode) {
    switch (mode) {
        case 2: return "1ST";
        case 3: return "3RD";
        case 4: return "FREE";
        default: return "OBS";
    }
}

static bool projectWorldToScreenRaw(const ViewMatrix& vm, const Vec3& world,
                                    float sw, float sh, Vec2& screen) {
    const auto& m = vm.m;
    float clipX = m[0][0]*world.x + m[0][1]*world.y + m[0][2]*world.z + m[0][3];
    float clipY = m[1][0]*world.x + m[1][1]*world.y + m[1][2]*world.z + m[1][3];
    float clipW = m[3][0]*world.x + m[3][1]*world.y + m[3][2]*world.z + m[3][3];
    if (clipW <= 0.001f)
        return false;

    float invW = 1.f / clipW;
    float ndcX = clipX * invW;
    float ndcY = clipY * invW;
    screen.x = (ndcX * 0.5f + 0.5f) * sw;
    screen.y = (-ndcY * 0.5f + 0.5f) * sh;
    return true;
}

static int clipCode(float x, float y, float minX, float minY,
                    float maxX, float maxY) {
    int code = 0;
    if (x < minX) code |= 1;
    else if (x > maxX) code |= 2;
    if (y < minY) code |= 4;
    else if (y > maxY) code |= 8;
    return code;
}

static bool clipLineToRect(float& x0, float& y0, float& x1, float& y1,
                           float minX, float minY, float maxX, float maxY) {
    int code0 = clipCode(x0, y0, minX, minY, maxX, maxY);
    int code1 = clipCode(x1, y1, minX, minY, maxX, maxY);

    for (;;) {
        if (!(code0 | code1))
            return true;
        if (code0 & code1)
            return false;

        int outCode = code0 ? code0 : code1;
        float x = 0.f;
        float y = 0.f;

        if (outCode & 8) {
            if (std::fabs(y1 - y0) < 0.001f) return false;
            x = x0 + (x1 - x0) * (maxY - y0) / (y1 - y0);
            y = maxY;
        } else if (outCode & 4) {
            if (std::fabs(y1 - y0) < 0.001f) return false;
            x = x0 + (x1 - x0) * (minY - y0) / (y1 - y0);
            y = minY;
        } else if (outCode & 2) {
            if (std::fabs(x1 - x0) < 0.001f) return false;
            y = y0 + (y1 - y0) * (maxX - x0) / (x1 - x0);
            x = maxX;
        } else {
            if (std::fabs(x1 - x0) < 0.001f) return false;
            y = y0 + (y1 - y0) * (minX - x0) / (x1 - x0);
            x = minX;
        }
        if (outCode == code0) {
            x0 = x;
            y0 = y;
            code0 = clipCode(x0, y0, minX, minY, maxX, maxY);
        } else {
            x1 = x;
            y1 = y;
            code1 = clipCode(x1, y1, minX, minY, maxX, maxY);
        }
    }
}

static void drawSmoothRing(Renderer& rn, float cx, float cy, float outerR,
                           float fraction, unsigned int activeColor,
                           unsigned int trackColor) {
    constexpr float kPi = 3.14159265f;

    if (outerR < 0.5f)
        return;

    fraction = std::clamp(fraction, 0.f, 1.f);
    rn.drawFilledCircle(cx, cy, outerR, trackColor);
    if (fraction <= 0.f)
        return;
    if (fraction >= 0.999f) {
        rn.drawFilledCircle(cx, cy, outerR, activeColor);
        return;
    }

    rn.drawFilledPie(cx, cy, outerR,
                     -kPi * 0.5f,
                     -kPi * 0.5f + fraction * kPi * 2.f,
                     activeColor);
}

static void drawTrajectoryPath(Renderer& rn, const ViewMatrix& vm,
                               const Vec3* points, int count,
                               float sw, float sh,
                               unsigned int color,
                               const Vec3* trimWorld = nullptr) {
    if (!points || count < 2)
        return;

    unsigned int lineCol = withAlpha(color, 0xF8);
    constexpr float kThickness = 1.05f;
    constexpr float kInset = 1.0f;
    int segmentStep = 1;
    if (count > 160) segmentStep = 4;
    else if (count > 96) segmentStep = 3;
    else if (count > 48) segmentStep = 2;

    int startIndex = 0;
    Vec3 trimStart = points[0];
    bool useTrimStart = false;
    if (trimWorld) {
        float bestDistSq = (std::numeric_limits<float>::max)();
        int bestSeg = 0;
        float bestT = 0.f;
        for (int i = 0; i < count - 1; ++i) {
            Vec3 seg{ points[i + 1].x - points[i].x,
                      points[i + 1].y - points[i].y,
                      points[i + 1].z - points[i].z };
            float segLenSq = seg.x*seg.x + seg.y*seg.y + seg.z*seg.z;
            float t = 0.f;
            if (segLenSq > 1.0e-4f) {
                Vec3 rel{ trimWorld->x - points[i].x,
                          trimWorld->y - points[i].y,
                          trimWorld->z - points[i].z };
                t = (rel.x*seg.x + rel.y*seg.y + rel.z*seg.z) / segLenSq;
                t = std::clamp(t, 0.f, 1.f);
            }
            Vec3 candidate{ points[i].x + seg.x * t,
                            points[i].y + seg.y * t,
                            points[i].z + seg.z * t };
            Vec3 d{ candidate.x - trimWorld->x,
                    candidate.y - trimWorld->y,
                    candidate.z - trimWorld->z };
            float distSq = d.x*d.x + d.y*d.y + d.z*d.z;
            if (distSq < bestDistSq) {
                bestDistSq = distSq;
                bestSeg = i;
                bestT = t;
                trimStart = candidate;
            }
        }
        if (bestT >= 0.995f)
            startIndex = bestSeg + 1;
        else {
            startIndex = bestSeg;
            useTrimStart = true;
        }
        if (startIndex >= count - 1)
            return;
    }

    auto drawSegment = [&](const Vec3& worldA, const Vec3& worldB) {
        Vec2 p0{};
        Vec2 p1{};
        if (!projectWorldToScreenRaw(vm, worldA, sw, sh, p0) ||
            !projectWorldToScreenRaw(vm, worldB, sw, sh, p1))
            return;

        float x0 = p0.x;
        float y0 = p0.y;
        float x1 = p1.x;
        float y1 = p1.y;
        if (!clipLineToRect(x0, y0, x1, y1, kInset, kInset, sw - kInset, sh - kInset))
            return;

        float dx = x1 - x0;
        float dy = y1 - y0;
        if ((dx * dx + dy * dy) < 0.25f)
            return;

        rn.drawLine(x0, y0, x1, y1, lineCol, kThickness);
    };

    if (useTrimStart)
        drawSegment(trimStart, points[startIndex + 1]);

    for (int i = startIndex + (useTrimStart ? 1 : 0); i < count - 1; i += segmentStep) {
        int nextIndex = (std::min)(i + segmentStep, count - 1);
        drawSegment(points[i], points[nextIndex]);
    }
}

// --- Timer ring landing indicator ---------------------------------------------------------
// Font-based grenade icons from csgo_icons.ttf (PUA codepoints)
static wchar_t kGrenadeIconChar[] = {
    0xE02C,  // HE
    0xE02D,  // Smoke
    0xE02B,  // Flash
    0xE02E,  // Molotov
    0xE02F,  // Decoy
};

static void drawGrenadeIcon(Renderer& rn, const FontAtlas& font, float cx, float cy,
                             float size, GrenadeType type, unsigned int col) {
    int idx = static_cast<int>(type);
    if (idx < 0 || idx >= 5) return;
    drawCenteredIcon(rn, font, cx, cy, kGrenadeIconChar[idx], size, col);
}

// --- Timer ring landing indicator ---------------------------------------------------------
static void drawTimerRing(Renderer& rn, const FontAtlas& font, float cx, float cy,
                           bool hasFuse, float fuseTime, float timeAlive,
                           unsigned int typeCol, bool inDanger, bool isPreThrow,
                           GrenadeType type,
                           float outerR = 26.f, float innerR = 24.f)
{
    bool showTimer = hasFuse && fuseTime > 0.f && !isPreThrow;
    unsigned int alertCol = 0xFFFF5151;
    unsigned int accentCol = inDanger ? alertCol : typeCol;
    float alertPulse = inDanger ? (0.55f + 0.45f * std::sin(timeAlive * 9.0f)) : 0.f;

    float remaining = fuseTime;
    float fraction  = 1.f;
    if (showTimer) {
        remaining = fuseTime - timeAlive;
        if (remaining < 0.f) remaining = 0.f;
        fraction  = remaining / fuseTime;
        if (fraction < 0.f) fraction = 0.f;
        if (fraction > 1.f) fraction = 1.f;
    }

    float badgeR = (std::max)(4.f, outerR - 1.2f);
    float ringOuterR = badgeR - 2.0f;
    float ringInnerR = badgeR - (inDanger ? 4.6f : 4.2f);
    float iconY  = cy;
    float iconSize = innerR * 0.84f;

    unsigned int shellCol = inDanger ? 0xF1280D14 : 0xCC101317;
    unsigned int coreCol = inDanger ? 0xFF351218 : 0xF014181C;
    unsigned int trackCol = inDanger ? 0x8E59161F : 0x44343A42;

    if (inDanger) {
        float glowR = badgeR + 2.0f + alertPulse * 2.5f;
        unsigned int glowCol = withAlpha(alertCol, static_cast<unsigned int>(0x1E + alertPulse * 0x20));
        rn.drawFilledCircle(cx, cy, glowR, glowCol);
    }

    rn.drawFilledCircle(cx, cy, badgeR, shellCol);

    if (showTimer) {
        drawSmoothRing(rn, cx, cy, ringOuterR, fraction,
                       withAlpha(accentCol, inDanger ? static_cast<unsigned int>(0xF0 + alertPulse * 0x0F) : 0xF4),
                       trackCol);
        iconY = cy - badgeR * 0.22f;
        iconSize = innerR * 0.90f;
    } else {
        drawSmoothRing(rn, cx, cy, ringOuterR, 1.f,
                       withAlpha(accentCol, 0xDE),
                       withAlpha(accentCol, 0xDE));
    }

    rn.drawFilledCircle(cx, cy, ringInnerR, coreCol);

    drawGrenadeIcon(rn, font, cx, iconY, iconSize, type, withAlpha(accentCol, 0xF6));

    if (showTimer) {
        wchar_t buf[16];
        if (remaining >= 10.f)
            swprintf(buf, 16, L"%d", static_cast<int>(remaining + 0.5f));
        else
            swprintf(buf, 16, L"%.1f", remaining);
        float textSize = std::clamp(badgeR * 0.52f, 14.0f, 21.f);
        float textBandTop = cy + badgeR * 0.14f;
        float textBandHeight = badgeR * 0.50f;
        float tx = textActualCenterX(rn, font, cx - badgeR, badgeR * 2.f, buf, textSize);
        float ty = textActualCenterY(rn, font, textBandTop, textBandHeight, buf, textSize);
        unsigned int tcol = inDanger ? 0xFFFFD4D4 : 0xE8F4F4F6;
        rn.drawTextW(font, tx, ty, buf, withAlpha(tcol, 0xC8), textSize);
        rn.drawTextW(font, tx + 0.6f, ty, buf, tcol, textSize);
    }
}

static void drawBombPanelIcon(Renderer& rn, const FontAtlas& font, float x, float y, float size)
{
    unsigned int tileFill = 0xFF171A28;
    unsigned int tileInset = withAlpha(Theme::ACCENT, 0x1A);
    unsigned int tileBorder = 0xFF2A2D42;
    unsigned int iconCol = 0xFFF6F7FA;
    float tileR = size * 0.22f;

    rn.drawRoundedFilledRect(x, y, size, size, tileFill, tileR);
    rn.drawRoundedFilledRect(x + size * 0.10f,
                             y + size * 0.10f,
                             size * 0.80f,
                             size * 0.80f,
                             tileInset,
                             size * 0.18f);
    rn.drawRoundedRect(x, y, size, size, tileBorder, tileR, 1.0f);

    drawCenteredIcon(rn, font,
                     x + size * 0.5f,
                     y + size * 0.5f,
                     0xE031,
                     size * 0.56f,
                     iconCol);
}

static void drawBombPanelClock(Renderer& rn, const FontAtlas& font,
                               float cx, float cy,
                               float totalTime, float remainingTime,
                               float radius,
                               unsigned int accentCol)
{
    totalTime = std::clamp(totalTime, 0.5f, 60.f);
    remainingTime = std::clamp(remainingTime, 0.f, totalTime);
    float fraction = remainingTime / totalTime;

    float outerR = radius;
    float ringOuter = outerR - 0.75f;
    float ringInner = outerR - 3.6f;
    rn.drawFilledCircle(cx, cy, outerR + 1.7f, withAlpha(accentCol, 0x12));
    rn.drawFilledCircle(cx, cy, outerR, 0xFF161924);
    drawSmoothRing(rn, cx, cy, ringOuter, fraction, withAlpha(accentCol, 0xF8), 0xFF2B2E44);
    rn.drawFilledCircle(cx, cy, ringInner, 0xFF10131B);

    wchar_t timerBuf[16];
    swprintf(timerBuf, 16, L"%.1f", remainingTime);

    float timerSize = std::clamp(outerR * 0.66f, 16.f, 22.f);
    float timerBoxLeft = cx - ringInner;
    float timerBoxTop = cy - ringInner;
    float timerBoxSize = ringInner * 2.f;
    float timerTextX = textActualCenterX(rn, font, timerBoxLeft, timerBoxSize, timerBuf, timerSize);
    float timerTextY = textHudClockCenterY(rn, font, timerBoxTop, timerBoxSize, timerBuf, timerSize);
    rn.drawTextW(font,
                 timerTextX,
                 timerTextY,
                 timerBuf,
                 Theme::TEXT,
                 timerSize);
}

static void drawBombStatusPanel(Renderer& rn, const FontAtlas& font,
                                float sw, float sh,
                                const BombData& bomb,
                                int localHealth,
                                int localArmor,
                                float predictedDamage)
{
    const bool showPlanting = bomb.isPlanting && bomb.plantRemaining > 0.f;
    const bool showPlanted = bomb.isPlanted && bomb.isTicking && bomb.timeRemaining > 0.f;
    if (!showPlanting && !showPlanted)
        return;

    const bool showDefuse = showPlanted && bomb.isBeingDefused && bomb.defuseRemaining > 0.f;
    const bool hasDamage = showPlanted && localHealth > 0 && predictedDamage >= 0.f;
    const int roundedDamage = hasDamage ? static_cast<int>(predictedDamage + 0.5f) : 0;

    wchar_t siteBuf[8];
    swprintf(siteBuf, 8, L"%lc", bomb.site == 1 ? L'B' : L'A');

    const wchar_t* row2Label = L"Damage:";
    unsigned int row2ValueCol = 0xFFFF5A64;
    bool row2UsesLargeValue = true;
    wchar_t row2ValueBuf[24];
    if (showPlanting) {
        row2Label = L"Status:";
        row2ValueCol = Theme::TEXT;
        row2UsesLargeValue = false;
        swprintf(row2ValueBuf, 24, L"Planting");
    } else if (showDefuse) {
        row2Label = L"Defuse:";
        row2ValueCol = 0xFF84E8B0;
        row2UsesLargeValue = false;
        swprintf(row2ValueBuf, 24, L"%.1f", bomb.defuseRemaining);
    } else {
        swprintf(row2ValueBuf, 24, L"%d", roundedDamage);
    }

    const wchar_t* row1Label = L"Site:";

    float scale = std::clamp(sh / 1080.f, 1.16f, 1.24f);
    float padX = 11.5f * scale;
    float rightPad = 10.0f * scale;
    float iconSize = 34.f * scale;
    float gapAfterIcon = 10.f * scale;
    float rowLabelSize = 14.5f * scale;
    float rowValueSize = 14.5f * scale;
    float damageValueSize = rowValueSize;
    float labelValueGap = 6.f * scale;
    float textRingGap = 12.f * scale;
    float clockR = 23.f * scale;
    float sitePillPadX = 8.f * scale;
    float row2PillPadX = sitePillPadX;
    float sitePillH = 18.f * scale;
    float row1H = 18.f * scale;
    float row2ValueSize = row2UsesLargeValue ? damageValueSize : rowValueSize;
    float row2PillH = sitePillH;
    float row2H = row2PillH;
    float rowGap = 6.f * scale;
    float textBlockH = row1H + rowGap + row2H;
    float panelH = (std::max)(60.f * scale, textBlockH + 16.f * scale);

    float row1LabelW = measureTextActualWidth(rn, font, row1Label, rowLabelSize);
    float row2LabelW = measureTextActualWidth(rn, font, row2Label, rowLabelSize);
    float sitePillW = measureTextActualWidth(rn, font, siteBuf, rowValueSize) + sitePillPadX * 2.f;
    float row2PillW = measureTextActualWidth(rn, font, row2ValueBuf, row2ValueSize) + row2PillPadX * 2.f;
    float row1W = row1LabelW + labelValueGap + sitePillW;
    float row2W = row2LabelW + labelValueGap + row2PillW;
    float textBlockW = (std::max)(row1W, row2W);
    float panelW = padX + iconSize + gapAfterIcon + textBlockW + textRingGap + clockR * 2.f + rightPad;

    float defaultX = sw - panelW - 34.f * scale;
    float defaultY = 120.f * scale;
    float availX = (std::max)(0.f, sw - panelW);
    float availY = (std::max)(0.f, sh - panelH);
    float x = defaultX;
    float y = defaultY;
    if (g_cfg.bombTimerPosX >= 0.f && g_cfg.bombTimerPosY >= 0.f) {
        x = availX > 0.f ? std::clamp(g_cfg.bombTimerPosX, 0.f, 1.f) * availX : defaultX;
        y = availY > 0.f ? std::clamp(g_cfg.bombTimerPosY, 0.f, 1.f) * availY : defaultY;
    }

    static bool s_dragBombTimer = false;
    static float s_dragOffsetX = 0.f;
    static float s_dragOffsetY = 0.f;
    if (g_cfg.menuVisible) {
        ImGuiIO& io = ImGui::GetIO();
        const bool hovered = io.MousePos.x >= x && io.MousePos.x <= x + panelW
            && io.MousePos.y >= y && io.MousePos.y <= y + panelH;
        if (!s_dragBombTimer && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left, false)) {
            s_dragBombTimer = true;
            s_dragOffsetX = io.MousePos.x - x;
            s_dragOffsetY = io.MousePos.y - y;
        }
        if (s_dragBombTimer) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                x = std::clamp(io.MousePos.x - s_dragOffsetX, 0.f, availX);
                y = std::clamp(io.MousePos.y - s_dragOffsetY, 0.f, availY);
                g_cfg.bombTimerPosX = availX > 0.f ? (x / availX) : 1.f;
                g_cfg.bombTimerPosY = availY > 0.f ? (y / availY) : 0.f;
            } else {
                s_dragBombTimer = false;
            }
        }
    } else {
        s_dragBombTimer = false;
    }

    unsigned int shellCol = Theme::BG;
    unsigned int innerCol = Theme::SURFACE;
    unsigned int borderCol = 0xFF26293B;
    unsigned int labelCol = Theme::TEXT;
    unsigned int sitePillBg = 0xFF1B1C28;
    unsigned int sitePillBorder = 0xFF312D57;
    unsigned int siteTextCol = Theme::TEXT_LINK;
    unsigned int damageTextCol = 0xFFFF6B76;
    unsigned int damagePillBg = 0xFF24141A;
    unsigned int damagePillBorder = 0xFF4A2932;
    unsigned int plantPillBg = 0xFF1A1C2C;
    unsigned int plantPillBorder = 0xFF312D57;
    unsigned int defuseCol = 0xFF84E8B0;
    unsigned int defusePillBg = 0xFF15261E;
    unsigned int defusePillBorder = 0xFF27503E;
    unsigned int timerCol = Theme::ACCENT;
    float corner = 12.f * scale;

    for (int i = 0; i < 5; ++i) {
        float t = static_cast<float>(i + 1) / 5.f;
        float spread = (2.5f + t * t * 15.f) * scale;
        float growX = spread * 0.62f;
        float growY = spread * 0.50f;
        float offsetY = (0.8f + t * 3.1f) * scale;
        float alphaT = 1.f - t;
        unsigned int shadowAlpha = static_cast<unsigned int>(2.f + alphaT * alphaT * 11.f);
        rn.drawRoundedFilledRect(x - growX,
                                 y + offsetY - growY * 0.42f,
                                 panelW + growX * 2.f,
                                 panelH + growY * 1.22f,
                                 withAlpha(0xFF000000, shadowAlpha),
                                 corner + spread * 0.46f);
    }

    rn.drawRoundedFilledRect(x, y, panelW, panelH, shellCol, corner);
    rn.drawRoundedFilledRect(x + scale,
                             y + scale,
                             panelW - scale * 2.f,
                             panelH - scale * 2.f,
                             innerCol,
                             (std::max)(0.f, corner - scale * 1.5f));
    rn.drawRoundedRect(x, y, panelW, panelH, borderCol, corner, 1.0f);

    float textBlockTop = y + (panelH - textBlockH) * 0.5f;
    float row1Y = textBlockTop;
    float row2Y = row1Y + row1H + rowGap;
    float iconX = x + padX;
    float iconY = y + (panelH - iconSize) * 0.5f;
    float textX = iconX + iconSize + gapAfterIcon;

    auto drawPill = [&](float px, float py, float pw, float ph,
                        const wchar_t* text, float fontSize,
                        unsigned int fillCol, unsigned int pillBorderCol, unsigned int textCol) {
        float pillCorner = ph * 0.38f;
        rn.drawRoundedFilledRect(px, py, pw, ph, fillCol, pillCorner);
        rn.drawRoundedRect(px, py, pw, ph, pillBorderCol, pillCorner, 1.0f);
        rn.drawTextW(font,
                     textActualCenterX(rn, font, px, pw, text, fontSize),
                     textHudCompactCenterY(rn, font, py, ph, text, fontSize),
                     text,
                     textCol,
                     fontSize);
    };

    drawBombPanelIcon(rn, font, iconX, iconY, iconSize);

    float mainRemaining = showPlanting ? bomb.plantRemaining : bomb.timeRemaining;
    float clockCx = x + panelW - rightPad - clockR;
    float clockCy = y + panelH * 0.5f;
    drawBombPanelClock(rn, font, clockCx, clockCy,
                       showPlanting ? (bomb.plantLength > 0.1f ? bomb.plantLength : 3.2f)
                                    : (bomb.timerLength > 0.1f ? bomb.timerLength : 40.f),
                       mainRemaining,
                       clockR,
                       timerCol);

    rn.drawTextW(font,
                 textX,
                 textHudCenterY(rn, font, row1Y, row1H, row1Label, rowLabelSize),
                 row1Label,
                 labelCol,
                 rowLabelSize);

    float siteValueX = textX + row1LabelW + labelValueGap;
    float sitePillY = row1Y + (row1H - sitePillH) * 0.5f;
    drawPill(siteValueX,
             sitePillY,
             sitePillW,
             sitePillH,
             siteBuf,
             rowValueSize,
             sitePillBg,
             sitePillBorder,
             siteTextCol);

    rn.drawTextW(font,
                 textX,
                 textHudCenterY(rn, font, row2Y, row2H, row2Label, rowLabelSize),
                 row2Label,
                 labelCol,
                 rowLabelSize);

    float row2ValueX = textX + row2LabelW + labelValueGap;
    unsigned int row2PillBg = damagePillBg;
    unsigned int row2PillBorder = damagePillBorder;
    unsigned int row2TextCol = damageTextCol;
    if (showPlanting) {
        row2PillBg = plantPillBg;
        row2PillBorder = plantPillBorder;
        row2TextCol = row2ValueCol;
    } else if (showDefuse) {
        row2PillBg = defusePillBg;
        row2PillBorder = defusePillBorder;
        row2TextCol = defuseCol;
    }

    float row2PillY = row2Y + (row2H - row2PillH) * 0.5f;
    drawPill(row2ValueX,
             row2PillY,
             row2PillW,
             row2PillH,
             row2ValueBuf,
             row2ValueSize,
             row2PillBg,
             row2PillBorder,
             row2TextCol);
}

static float applyBombArmor(float damage, int armor) {
    if (damage <= 0.f || armor <= 0)
        return damage;

    constexpr float kArmorRatio = 0.5f;
    constexpr float kArmorBonus = 0.5f;

    float reducedDamage = damage * kArmorRatio;
    float armorDamage = (damage - reducedDamage) * kArmorBonus;

    if (armorDamage > static_cast<float>(armor)) {
        armorDamage = static_cast<float>(armor) * (1.f / kArmorBonus);
        reducedDamage = damage - armorDamage;
    }

    return reducedDamage;
}

static float estimateBombRawDamageAtPoint(const Vec3& samplePos,
                                         const Vec3& bombPos,
                                         float damageRadius) {
    Vec3 delta{ samplePos.x - bombPos.x, samplePos.y - bombPos.y, samplePos.z - bombPos.z };
    const double distance = std::sqrt(static_cast<double>(delta.x) * static_cast<double>(delta.x)
                                      + static_cast<double>(delta.y) * static_cast<double>(delta.y)
                                      + static_cast<double>(delta.z) * static_cast<double>(delta.z));
    
    if (distance >= static_cast<double>(damageRadius))
        return 0.f;

    const double sigma = static_cast<double>(damageRadius) / 3.0;
    const double exponent = -(distance * distance) / (2.0 * sigma * sigma);
    const float rawDamage = 500.f * static_cast<float>(std::exp(exponent));
    return rawDamage >= 0.5f ? rawDamage : 0.f;
}

static float getBombMapRadius(const std::string& mapName) {
    std::string lower = mapName;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    auto hasMap = [&lower](const char* token) {
        return lower == token || lower.find(token) != std::string::npos;
    };

    if (hasMap("de_dust2")) {
        return 1750.f;
    } else if (hasMap("de_ancient")) {
        return 2275.f;
    } else if (hasMap("de_anubis")) {
        return 1575.f;
    } else if (hasMap("de_inferno")) {
        return 2170.f;
    } else if (hasMap("de_mirage")) {
        return 2275.f;
    } else if (hasMap("de_nuke")) {
        return 2275.f;
    } else if (hasMap("de_overpass")) {
        return 2275.f;
    } else if (hasMap("de_vertigo")) {
        return 1750.f;
    }

    return 2275.f;
}

static float estimateBombDamage(const Vec3& localOrigin,
                                const Vec3& localHead,
                                const Vec3& bombPos,
                                const std::string& mapName,
                                int armor) {
    const float damageRadius = getBombMapRadius(mapName);
    Vec3 resolvedHead = localHead.lengthSq() > 1.f ? localHead : (localOrigin + Vec3{ 0.f, 0.f, 64.f });
    float standingHeight = resolvedHead.z - localOrigin.z;
    if (standingHeight < 48.f)
        standingHeight = 64.f;

    Vec3 torso = localOrigin + Vec3{ 0.f, 0.f, standingHeight * 0.55f };
    Vec3 upperTorso = localOrigin + Vec3{ 0.f, 0.f, standingHeight * 0.78f };
    constexpr float kHullRadius = 14.f;
    const Vec3 samplePoints[] = {
        localOrigin + Vec3{ 0.f, 0.f, 6.f },
        localOrigin + Vec3{ 0.f, 0.f, standingHeight * 0.35f },
        torso,
        upperTorso,
        resolvedHead,
        torso + Vec3{ kHullRadius, 0.f, 0.f },
        torso + Vec3{ -kHullRadius, 0.f, 0.f },
        torso + Vec3{ 0.f, kHullRadius, 0.f },
        torso + Vec3{ 0.f, -kHullRadius, 0.f },
        upperTorso + Vec3{ kHullRadius * 0.8f, 0.f, 0.f },
        upperTorso + Vec3{ -kHullRadius * 0.8f, 0.f, 0.f },
        upperTorso + Vec3{ 0.f, kHullRadius * 0.8f, 0.f },
        upperTorso + Vec3{ 0.f, -kHullRadius * 0.8f, 0.f },
    };

    float rawDamage = 0.f;
    for (const Vec3& samplePoint : samplePoints)
        rawDamage = (std::max)(rawDamage, estimateBombRawDamageAtPoint(samplePoint, bombPos, damageRadius));

    if (rawDamage < 0.5f)
        return 0.f;

    float damage = applyBombArmor(rawDamage, armor);
    return std::floor((std::max)(0.f, damage));
}

// ─── Grenades ──────────────────────────────────────────────────────────────────────────────────

void EspRenderer::drawGrenades(Renderer& r, const EntityManager::Snapshot& snap) {
    if (!m_font) return;

    const auto& vm = snap.viewMatrix;
    const auto& grenades = snap.grenades;
    float sw = static_cast<float>(r.screenWidth());
    float sh = static_cast<float>(r.screenHeight());

    Vec3 localPos{};
    {
        for (const auto& p : snap.players)
            if (p.isLocalPlayer && p.isAlive) { localPos = p.origin; break; }
    }

    struct GrenadeStyle { unsigned int argb; const char* label; };
    static const GrenadeStyle kStyles[] = {
        { 0xFFFF3C3C, "HE"      },
        { 0xFFAAAAAA, "Smoke"   },
        { 0xFFFFFFC8, "Flash"   },
        { 0xFFFF8C00, "Molotov" },
        { 0xFFC8C800, "Decoy"   },
    };

    constexpr float kLabelSize = 20.5f;
    constexpr float kWorldIconSize = 28.f;
    constexpr float kWorldIconOffsetX = 14.f;
    constexpr float kWorldIconOffsetY = -14.f;
    constexpr float kLabelOffsetY = 10.0f;

    auto labelWidth = [&](const char* label) {
        if (std::strcmp(label, "HE") == 0) return measureTextW(*m_font, L"HE", kLabelSize);
        if (std::strcmp(label, "Flash") == 0) return measureTextW(*m_font, L"Flash", kLabelSize);
        if (std::strcmp(label, "Molotov") == 0) return measureTextW(*m_font, L"Molotov", kLabelSize);
        if (std::strcmp(label, "Decoy") == 0) return measureTextW(*m_font, L"Decoy", kLabelSize);
        return measureTextW(*m_font, L"Smoke", kLabelSize);
    };

    auto drawBolderLabel = [&](float x, float y, const char* label, unsigned int col) {
        r.drawText(*m_font, x, y, label, withAlpha(col, 0xBC), kLabelSize);
        r.drawText(*m_font, x + 0.9f, y, label, col, kLabelSize);
    };

    if (g_cfg.grenadeTrajectory) {
        const auto& preThrow = snap.preThrow;
        if (preThrow.isActive && preThrow.predCount > 1) {
            unsigned int kPreThrowCol = 0xDC50FF78;
            drawTrajectoryPath(r, vm, preThrow.predPoints, preThrow.predCount, sw, sh, kPreThrowCol, &preThrow.predPoints[0]);
            Vec2 preLandPt;
            if (worldToScreenClamped(vm, preThrow.predPoints[preThrow.predCount - 1], sw, sh, preLandPt))
                drawTimerRing(r, *m_font, preLandPt.x, preLandPt.y,
                              preThrow.fuseTime > 0.f, preThrow.fuseTime, 0.f,
                              kPreThrowCol, false, true, preThrow.type);
        }
    }

    if (!g_cfg.grenadeEnabled)
        return;

    for (const auto& g : grenades) {
        if (!g.isValid) continue;

        Vec2 screen;
        bool onScreenNow = vm.worldToScreen(g.origin, screen, sw, sh);

        if (g.isDeployed) {
            int idx = static_cast<int>(g.type);
            const GrenadeStyle& st = kStyles[idx];

            constexpr float kBurnTotal = 7.f;
            constexpr float kSmokeTotal = 18.f;
            float burnTotal = (g.type == GrenadeType::Smoke) ? kSmokeTotal : kBurnTotal;

            Vec2 deployPt;
            bool deployOnScreen = worldToScreenClamped(vm, g.deployPos, sw, sh, deployPt);

            float elapsed = burnTotal - g.burnRemaining;
            if (elapsed < 0.f) elapsed = 0.f;

            if (deployOnScreen) {
                drawTimerRing(r, *m_font, deployPt.x, deployPt.y, true, burnTotal, elapsed,
                              st.argb, false, false, g.type);
            } else {
                drawTimerRing(r, *m_font, deployPt.x, deployPt.y, true, burnTotal, elapsed,
                              st.argb, false, false, g.type, 40.f, 38.f);
            }
            continue;
        }

        if (!onScreenNow) continue;

        int idx = static_cast<int>(g.type);
        const GrenadeStyle& st = kStyles[idx];

        float iconCx = screen.x + kWorldIconOffsetX;
        float iconCy = screen.y + kWorldIconOffsetY;
        drawGrenadeIcon(r, *m_font, iconCx, iconCy, kWorldIconSize, g.type, st.argb);

        if (g_cfg.grenadeTrajectory && g.predCount > 1) {
            drawTrajectoryPath(r, vm, g.predPoints, g.predCount, sw, sh, st.argb, &g.origin);

            const Vec3& landWorld = g.predPoints[g.predCount - 1];
            Vec2 edgePt;
            bool lpOnScreen = worldToScreenClamped(vm, landWorld, sw, sh, edgePt);
            if (lpOnScreen) {
                bool inDanger = false;
                if (g.hasFuse && g.damageRadius > 0.f) {
                    Vec3 d{ localPos.x - landWorld.x,
                            localPos.y - landWorld.y,
                            localPos.z - landWorld.z };
                    inDanger = (d.x*d.x + d.y*d.y + d.z*d.z) < g.damageRadius * g.damageRadius;
                }
                drawTimerRing(r, *m_font, edgePt.x, edgePt.y, g.hasFuse, g.fuseTime, g.timeAlive,
                              st.argb, inDanger, false, g.type);
            } else if (g.hasFuse) {
                bool inDanger = false;
                if (g.damageRadius > 0.f) {
                    Vec3 d{ localPos.x - landWorld.x,
                            localPos.y - landWorld.y,
                            localPos.z - landWorld.z };
                    inDanger = (d.x*d.x + d.y*d.y + d.z*d.z) < g.damageRadius * g.damageRadius;
                }
                drawTimerRing(r, *m_font, edgePt.x, edgePt.y, g.hasFuse, g.fuseTime, g.timeAlive,
                              st.argb, inDanger, false, g.type, 40.f, 38.f);
            }
        }
    }
}

void EspRenderer::drawBomb(Renderer& r, const EntityManager::Snapshot& snap) {
    if (!m_font)
        return;

    const BombData& bomb = snap.bomb;
    const std::string& currentMapName = snap.currentMapName;
    const auto& players = snap.players;
    Vec3 localPos{};
    Vec3 localHeadPos{};
    int localHealth = 0;
    int localArmor = 0;
    for (const auto& p : players) {
        if (!p.isLocalPlayer || !p.isAlive)
            continue;
        localPos = p.origin;
        localHeadPos = p.headPos;
        localHealth = p.health;
        localArmor = p.armor;
        break;
    }

    float predictedDamage = -1.f;
    if (bomb.isPlanted && bomb.isTicking && bomb.origin.lengthSq() > 1.f && localHealth > 0)
        predictedDamage = estimateBombDamage(localPos, localHeadPos, bomb.origin, currentMapName, localArmor);

    drawBombStatusPanel(r, *m_font,
                        static_cast<float>(r.screenWidth()),
                        static_cast<float>(r.screenHeight()),
                        bomb,
                        localHealth,
                        localArmor,
                        predictedDamage);
}

void EspRenderer::drawSpectators(Renderer& r, const EntityManager::Snapshot& snap) {
    if (!m_font || !g_cfg.spectatorListEnabled)
        return;

    const auto& spectators = snap.spectators;
    int spectatorCount = 0;
    for (const auto& spectator : spectators) {
        if (spectator.isValid)
            ++spectatorCount;
    }
    if (spectatorCount <= 0 && !g_cfg.menuVisible)
        return;

    float sw = static_cast<float>(r.screenWidth());
    float sh = static_cast<float>(r.screenHeight());
    float scale = std::clamp(sh / 1080.f, 1.0f, 1.22f);
    float x = 34.f * scale;
    float y = 122.f * scale;
    float pad = 18.f * scale;
    float rowH = 36.f * scale;
    float panelW = (std::min)(296.f * scale, sw - 24.f * scale);
    float panelH = 56.f * scale + (spectatorCount > 0 ? spectatorCount * rowH : 30.f * scale);

    float availX = (std::max)(0.f, sw - panelW);
    float availY = (std::max)(0.f, sh - panelH);
    if (g_cfg.spectatorPosX >= 0.f && g_cfg.spectatorPosY >= 0.f) {
        x = availX > 0.f ? std::clamp(g_cfg.spectatorPosX, 0.f, 1.f) * availX : x;
        y = availY > 0.f ? std::clamp(g_cfg.spectatorPosY, 0.f, 1.f) * availY : y;
    }

    if (g_cfg.menuVisible) {
        ImGuiIO& io = ImGui::GetIO();
        const bool hovered = io.MousePos.x >= x && io.MousePos.x <= x + panelW
            && io.MousePos.y >= y && io.MousePos.y <= y + panelH;
        static bool s_dragSpectators = false;
        static float s_dragOffsetX = 0.f;
        static float s_dragOffsetY = 0.f;
        if (!s_dragSpectators && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left, false)) {
            s_dragSpectators = true;
            s_dragOffsetX = io.MousePos.x - x;
            s_dragOffsetY = io.MousePos.y - y;
        }
        if (s_dragSpectators) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                x = std::clamp(io.MousePos.x - s_dragOffsetX, 0.f, availX);
                y = std::clamp(io.MousePos.y - s_dragOffsetY, 0.f, availY);
                g_cfg.spectatorPosX = availX > 0.f ? (x / availX) : 0.f;
                g_cfg.spectatorPosY = availY > 0.f ? (y / availY) : 0.f;
            } else {
                s_dragSpectators = false;
            }
        }
    }

    r.drawRoundedFilledRect(x, y, panelW, panelH, Theme::BG, 14.f * scale);
    r.drawRoundedRect(x, y, panelW, panelH, Theme::BORDER, 14.f * scale, 1.0f);

    float titlePillW = 116.f * scale;
    float titlePillH = 28.f * scale;
    r.drawRoundedFilledRect(x + pad, y + pad, titlePillW, titlePillH, 0xFF11131C, 8.f * scale);
    r.drawText(*m_font,
               x + pad + 11.f * scale,
               textCenterY(*m_font, y + pad, titlePillH, 14.f * scale),
               "Spectators",
               Theme::TEXT,
               14.f * scale);

    wchar_t countBuf[8];
    swprintf(countBuf, 8, L"%d", spectatorCount);
    float countPillW = spectatorCount >= 10 ? 34.f * scale : 30.f * scale;
    float countPillH = 24.f * scale;
    float countPillX = x + panelW - pad - countPillW;
    float countPillY = y + pad + 2.f * scale;
    r.drawRoundedFilledRect(countPillX, countPillY, countPillW, countPillH, 0xFF171927, 7.f * scale);
    r.drawTextW(*m_font,
                countPillX + 10.f * scale,
                textCenterY(*m_font, countPillY, countPillH, 13.f * scale),
                countBuf,
                Theme::TEXT_LINK,
                13.f * scale);

    float rowY = y + 52.f * scale;
    if (spectatorCount <= 0) {
        r.drawText(*m_font,
                   x + pad,
                   textCenterY(*m_font, rowY, 18.f * scale, 12.5f * scale),
                   "No one spectating you",
                   Theme::TEXT_MUTED,
                   12.5f * scale);
        return;
    }

    for (const auto& spectator : spectators) {
        if (!spectator.isValid)
            continue;

        r.drawRoundedFilledRect(x + pad, rowY, panelW - pad * 2.f, 30.f * scale, 0xFF11131C, 8.f * scale);

        std::string label = spectator.name;
        if (label.size() > 22)
            label = label.substr(0, 21) + "...";

        r.drawText(*m_font,
                   x + pad + 10.f * scale,
                   textCenterY(*m_font, rowY + 2.f * scale, 12.f * scale, 13.5f * scale),
                   label.c_str(),
                   Theme::TEXT,
                   13.5f * scale);

        std::string targetLine;
        if (spectator.watchingLocal || spectator.targetName == "You") {
            targetLine = "Watching you";
        } else if (spectator.targetName == "Free roam") {
            targetLine = "Free roam";
        } else if (!spectator.targetName.empty() && spectator.targetName != "Unknown") {
            targetLine = "Watching " + spectator.targetName;
        } else {
            targetLine = "Dead / No target";
        }
        if (targetLine.size() > 24)
            targetLine = targetLine.substr(0, 23) + "...";

        r.drawText(*m_font,
                   x + pad + 10.f * scale,
                   textCenterY(*m_font, rowY + 16.f * scale, 10.f * scale, 11.5f * scale),
                   targetLine.c_str(),
                   spectator.watchingLocal ? Theme::TEXT_LINK : Theme::TEXT_MUTED,
                   11.5f * scale);

        const char* modeLabel = spectatorModeLabel(spectator.mode);
        float modePillW = spectator.mode == 4 ? 46.f * scale : 38.f * scale;
        float modePillH = 18.f * scale;
        float modePillX = x + panelW - pad - modePillW - 8.f * scale;
        float modePillY = rowY + 6.f * scale;
        r.drawRoundedFilledRect(modePillX, modePillY, modePillW, modePillH, 0xFF1B1D2A, 6.f * scale);
        r.drawText(*m_font,
                   modePillX + 8.f * scale,
                   textCenterY(*m_font, modePillY, modePillH, 11.5f * scale),
                   modeLabel,
                   Theme::TEXT_MUTED,
                   11.5f * scale);

        rowY += rowH;
    }
}
