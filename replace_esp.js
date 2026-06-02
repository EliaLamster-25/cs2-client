const fs = require('fs');

let content = fs.readFileSync('src/esp/esp_renderer.cpp', 'utf8');

const targetStr = content.substring(
    content.indexOf('// ─── drawHealthBar'),
    content.indexOf('void EspRenderer::drawVoteRevealer')
);

const replacement = `static float measureTextNarrow(const FontAtlas* font, const char* text, float size) {
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
    };

    struct RenderItem {
        int id;
        EspAnchor anchor;
        int order;
        std::function<void()> draw;
    };
    std::vector<RenderItem> items;
    items.reserve(10);
    const auto& orderArr = occ ? g_cfg.espItemOrderOccluded : g_cfg.espItemOrderVisible;

    if (drawHpBar) {
        items.push_back({0, sanitizeBarAnchor(resolveEspAnchor(occ ? g_cfg.hpBarPosOccluded : g_cfg.hpBarPosVisible, EspAnchor::Left)), orderArr[0], [&]() {
            const EspAnchor anchor = sanitizeBarAnchor(resolveEspAnchor(occ ? g_cfg.hpBarPosOccluded : g_cfg.hpBarPosVisible, EspAnchor::Left));
            const bool horizontal = (anchor == EspAnchor::Top || anchor == EspAnchor::Bottom);
            const float bw = horizontal ? (std::max)(28.f, boxW) : (std::max)(2.f, g_cfg.hpBarWidth);
            const float bh = horizontal ? (std::max)(3.f, g_cfg.hpBarWidth) : (std::max)(26.f, boxH * 0.34f);
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
        }});
    }

    if (drawHpText) {
        items.push_back({1, sanitizeInfoAnchor(resolveEspAnchor(occ ? g_cfg.hpTextPosOccluded : g_cfg.hpTextPosVisible, EspAnchor::TopRight)), orderArr[1], [&]() {
            const EspAnchor anchor = sanitizeInfoAnchor(resolveEspAnchor(occ ? g_cfg.hpTextPosOccluded : g_cfg.hpTextPosVisible, EspAnchor::TopRight));
            char hpLabel[16]{};
            std::snprintf(hpLabel, sizeof(hpLabel), "HP: %d", std::clamp(player.health, 0, 100));
            float x = leftX;
            float y = boxY;
            float actualW = measureTextNarrow(m_font, hpLabel, textSizeInfo);
            placeRect(anchor, approxTextWidth(hpLabel, textSizeInfo), textSizeInfo + 1.f, x, y);
            if (anchor == EspAnchor::Top || anchor == EspAnchor::Bottom)
                x = boxX + boxW * 0.5f - actualW * 0.5f;
            drawOutlined(x, y, hpLabel, textSizeInfo);
        }});
    }

    if (drawName) {
        items.push_back({2, sanitizeInfoAnchor(resolveEspAnchor(occ ? g_cfg.namePosOccluded : g_cfg.namePosVisible, EspAnchor::Top)), orderArr[2], [&]() {
            const EspAnchor anchor = sanitizeInfoAnchor(resolveEspAnchor(occ ? g_cfg.namePosOccluded : g_cfg.namePosVisible, EspAnchor::Top));
            std::string displayName = player.name;
            if (player.isBot && !displayName.empty()) {
                std::string lower = displayName;
                for (char& ch : lower)
                    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (lower.rfind("bot ", 0) != 0)
                    displayName = "BOT " + displayName;
            }
            float x = leftX;
            float y = boxY;
            float actualW = measureTextNarrow(m_font, displayName.c_str(), textSizeName);
            placeRect(anchor, approxTextWidth(displayName.c_str(), textSizeName), textSizeName + 1.f, x, y);
            if (anchor == EspAnchor::Top || anchor == EspAnchor::Bottom)
                x = boxX + boxW * 0.5f - actualW * 0.5f;
            drawOutlined(x, y, displayName.c_str(), textSizeName);
        }});
    }

    if (drawWeaponText) {
        items.push_back({3, sanitizeInfoAnchor(resolveEspAnchor(occ ? g_cfg.weaponTextPosOccluded : g_cfg.weaponTextPosVisible, EspAnchor::BottomLeft)), orderArr[3], [&]() {
            const EspAnchor anchor = sanitizeInfoAnchor(resolveEspAnchor(occ ? g_cfg.weaponTextPosOccluded : g_cfg.weaponTextPosVisible, EspAnchor::BottomLeft));
            float x = leftX;
            float y = boxY;
            float actualW = measureTextNarrow(m_font, player.weaponName.c_str(), textSizeInfo);
            placeRect(anchor, approxTextWidth(player.weaponName.c_str(), textSizeInfo), textSizeInfo + 1.f, x, y);
            if (anchor == EspAnchor::Top || anchor == EspAnchor::Bottom)
                x = boxX + boxW * 0.5f - actualW * 0.5f;
            drawOutlined(x, y, player.weaponName.c_str(), textSizeInfo);
        }});
    }

    if (drawWeaponIcon) {
        items.push_back({4, sanitizeInfoAnchor(resolveEspAnchor(occ ? g_cfg.weaponIconPosOccluded : g_cfg.weaponIconPosVisible, EspAnchor::BottomRight)), orderArr[4], [&]() {
            const EspAnchor anchor = sanitizeInfoAnchor(resolveEspAnchor(occ ? g_cfg.weaponIconPosOccluded : g_cfg.weaponIconPosVisible, EspAnchor::BottomRight));
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
        }});
    }

    if (drawArmorBar) {
        items.push_back({5, sanitizeBarAnchor(resolveEspAnchor(occ ? g_cfg.armorBarPosOccluded : g_cfg.armorBarPosVisible, EspAnchor::Left)), orderArr[5], [&]() {
            const EspAnchor anchor = sanitizeBarAnchor(resolveEspAnchor(occ ? g_cfg.armorBarPosOccluded : g_cfg.armorBarPosVisible, EspAnchor::Left));
            const int armor = std::clamp(player.armor, 0, 100);
            const bool horizontal = (anchor == EspAnchor::Top || anchor == EspAnchor::Bottom);
            const float bw = horizontal ? (std::max)(28.f, boxW) : 5.f;
            const float bh = horizontal ? 5.f : (std::max)(26.f, boxH * 0.34f);
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
        }});
    }

    if (drawArmorText) {
        items.push_back({6, sanitizeInfoAnchor(resolveEspAnchor(occ ? g_cfg.armorTextPosOccluded : g_cfg.armorTextPosVisible, EspAnchor::BottomLeft)), orderArr[6], [&]() {
            const EspAnchor anchor = sanitizeInfoAnchor(resolveEspAnchor(occ ? g_cfg.armorTextPosOccluded : g_cfg.armorTextPosVisible, EspAnchor::BottomLeft));
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
        }});
    }

    if (drawAmmoBar) {
        items.push_back({7, sanitizeBarAnchor(resolveEspAnchor(occ ? g_cfg.ammoBarPosOccluded : g_cfg.ammoBarPosVisible, EspAnchor::Right)), orderArr[7], [&]() {
            const EspAnchor anchor = sanitizeBarAnchor(resolveEspAnchor(occ ? g_cfg.ammoBarPosOccluded : g_cfg.ammoBarPosVisible, EspAnchor::Right));
            const int ammoClip = (std::max)(0, player.ammoClip);
            const int ammoMax = player.ammoMaxClip > 0 ? player.ammoMaxClip : (std::max)(ammoClip, 1);
            const bool horizontal = (anchor == EspAnchor::Top || anchor == EspAnchor::Bottom);
            const float bw = horizontal ? (std::max)(28.f, boxW) : 5.f;
            const float bh = horizontal ? 5.f : (std::max)(26.f, boxH * 0.34f);
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
        }});
    }

    if (drawAmmoText) {
        items.push_back({8, sanitizeInfoAnchor(resolveEspAnchor(occ ? g_cfg.ammoTextPosOccluded : g_cfg.ammoTextPosVisible, EspAnchor::BottomRight)), orderArr[8], [&]() {
            const EspAnchor anchor = sanitizeInfoAnchor(resolveEspAnchor(occ ? g_cfg.ammoTextPosOccluded : g_cfg.ammoTextPosVisible, EspAnchor::BottomRight));
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
        }});
    }

    if (drawFlags) {
        items.push_back({9, sanitizeInfoAnchor(resolveEspAnchor(occ ? g_cfg.flagsPosOccluded : g_cfg.flagsPosVisible, EspAnchor::TopRight)), orderArr[9], [&]() {
            const EspAnchor anchor = sanitizeInfoAnchor(resolveEspAnchor(occ ? g_cfg.flagsPosOccluded : g_cfg.flagsPosVisible, EspAnchor::TopRight));
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
        }});
    }

    std::sort(items.begin(), items.end(), [](const RenderItem& a, const RenderItem& b) {
        if (a.anchor != b.anchor)
            return a.anchor < b.anchor;
        return a.order < b.order;
    });

    for (const auto& item : items) {
        item.draw();
    }
}
`;

content = content.replace(targetStr, replacement);
fs.writeFileSync('src/esp/esp_renderer.cpp', content, 'utf8');
