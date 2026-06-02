const fs = require('fs');
let content = fs.readFileSync('src/menu/menu.cpp', 'utf8');

// 1. Serialization
content = content.replace(
    'j["ammoBarPosOccluded"] = cfg.ammoBarPosOccluded;',
    'j["ammoBarPosOccluded"] = cfg.ammoBarPosOccluded;\n    j["espItemOrderVisible"] = cfg.espItemOrderVisible;\n    j["espItemOrderOccluded"] = cfg.espItemOrderOccluded;'
);

content = content.replace(
    'cfg.flagsPosOccluded = clampEspInfoAnchorStored(cfg.flagsPosOccluded);',
    'cfg.flagsPosOccluded = clampEspInfoAnchorStored(cfg.flagsPosOccluded);\n    if (j.contains("espItemOrderVisible")) { for (int i=0; i<10; ++i) cfg.espItemOrderVisible[i] = j["espItemOrderVisible"][i]; }\n    if (j.contains("espItemOrderOccluded")) { for (int i=0; i<10; ++i) cfg.espItemOrderOccluded[i] = j["espItemOrderOccluded"][i]; }'
);

// 2. placeRect Left/Right fix
const oldPlaceRect = `        case 2:
            outX = simBoxX - w - sidePad;
            outY = simBoxY + stack[2];
            stack[2] += h + S(1.f);
            break;
        case 3:
            outX = simBoxX + simBoxW + sidePad;
            outY = simBoxY + stack[3];
            stack[3] += h + S(1.f);
            break;`;

const newPlaceRect = `        case 2:
            outX = simBoxX - w - sidePad - stack[2];
            outY = simBoxY;
            stack[2] += w + S(2.f);
            break;
        case 3:
            outX = simBoxX + simBoxW + sidePad + stack[3];
            outY = simBoxY;
            stack[3] += w + S(2.f);
            break;`;
content = content.replace(oldPlaceRect, newPlaceRect);

// 3. For-loop sorting in drawPlayersEspPreviewWindow
const oldDrawLoop = `for (int i = 0; i < kPreviewCount; ++i) {
        if (defs[i].visAnchor && defs[i].occAnchor) {
            int stored = modeIndex == 0 ? *defs[i].visAnchor : *defs[i].occAnchor;`;

const newDrawLoop = `std::vector<int> drawIndices;
    for (int i = 0; i < kPreviewCount; ++i) drawIndices.push_back(i);
    const auto& orderArr = modeIndex == 0 ? g_cfg.espItemOrderVisible : g_cfg.espItemOrderOccluded;
    std::sort(drawIndices.begin(), drawIndices.end(), [&](int a, int b) {
        bool aFixed = (a < 3);
        bool bFixed = (b < 3);
        if (aFixed && !bFixed) return true;
        if (!aFixed && bFixed) return false;
        if (aFixed && bFixed) return a < b;
        
        int anchorA = modeIndex == 0 ? *defs[a].visAnchor : *defs[a].occAnchor;
        int anchorB = modeIndex == 0 ? *defs[b].visAnchor : *defs[b].occAnchor;
        int aNorm = anchorA < 0 ? 0 : clampAnchorForDef(defs[a], anchorA);
        int bNorm = anchorB < 0 ? 0 : clampAnchorForDef(defs[b], anchorB);
        if (aNorm != bNorm) return aNorm < bNorm;
        return orderArr[a - 3] < orderArr[b - 3];
    });

    for (int i : drawIndices) {
        if (defs[i].visAnchor && defs[i].occAnchor) {
            int stored = modeIndex == 0 ? *defs[i].visAnchor : *defs[i].occAnchor;`;
content = content.replace(oldDrawLoop, newDrawLoop);

// 4. Drag logic inside `if (!m_gui.mouseDown()) {`
const oldDropLogic = `            } else if (previewHovered) {
                if (defs[m_espPreviewDragElement].visAnchor && defs[m_espPreviewDragElement].occAnchor) {
                    s_animPos[modeIndex][m_espPreviewDragElement][0] = m_gui.mouseX();
                    s_animPos[modeIndex][m_espPreviewDragElement][1] = m_gui.mouseY();
                }
                setPlaced(m_espPreviewDragElement, modeIndex, bestAnchor);
            } else if (!s_dragFromPreview) {`;

const newDropLogic = `            } else if (previewHovered) {
                if (defs[m_espPreviewDragElement].visAnchor && defs[m_espPreviewDragElement].occAnchor) {
                    s_animPos[modeIndex][m_espPreviewDragElement][0] = m_gui.mouseX();
                    s_animPos[modeIndex][m_espPreviewDragElement][1] = m_gui.mouseY();
                }

                int oldAnchor = (modeIndex == 0 ? *defs[m_espPreviewDragElement].visAnchor : *defs[m_espPreviewDragElement].occAnchor);
                int newAnchor = clampAnchorForDef(defs[m_espPreviewDragElement], bestAnchor);
                
                if (m_espPreviewDragElement >= 3) {
                    auto& ord = modeIndex == 0 ? g_cfg.espItemOrderVisible : g_cfg.espItemOrderOccluded;
                    if (oldAnchor >= 0 && clampAnchorForDef(defs[m_espPreviewDragElement], oldAnchor) == newAnchor) {
                        std::vector<int> siblingIndices;
                        for(int idx = 3; idx < kPreviewCount; ++idx) {
                            if (idx == m_espPreviewDragElement) continue;
                            if (!isModeEnabled(idx, modeIndex)) continue;
                            int a = (modeIndex == 0 ? *defs[idx].visAnchor : *defs[idx].occAnchor);
                            if (a >= 0 && clampAnchorForDef(defs[idx], a) == newAnchor) {
                                siblingIndices.push_back(idx);
                            }
                        }
                        std::sort(siblingIndices.begin(), siblingIndices.end(), [&](int a, int b) {
                            return ord[a-3] < ord[b-3];
                        });
                        
                        int insertPos = 0;
                        float dropCoord = (newAnchor == 0 || newAnchor == 1 || newAnchor == 4 || newAnchor == 5 || newAnchor == 6 || newAnchor == 7) ? m_gui.mouseY() : m_gui.mouseX();
                        for(int sib : siblingIndices) {
                            float sibCoord = (newAnchor == 0 || newAnchor == 1 || newAnchor == 4 || newAnchor == 5 || newAnchor == 6 || newAnchor == 7) ? dragRects[sib].y + dragRects[sib].h*0.5f : dragRects[sib].x + dragRects[sib].w*0.5f;
                            // For bottom anchors (1, 6, 7), higher Y means further down the stack. For top (0, 4, 5), lower Y means further down the stack.
                            // Actually, stack grows outward. Let's just compare dropCoord and sibCoord based on mouse pos directly. 
                            // But dragRects are absolute coords, so their absolute order reflects the visual stack order.
                            if (dropCoord > sibCoord) insertPos++;
                        }
                        
                        siblingIndices.insert(siblingIndices.begin() + insertPos, m_espPreviewDragElement);
                        for(int k=0; k<siblingIndices.size(); ++k) {
                            ord[siblingIndices[k]-3] = k;
                        }
                    } else {
                        ord[m_espPreviewDragElement-3] = 99; 
                    }
                    
                    std::vector<int> allIndices;
                    for(int idx = 3; idx < kPreviewCount; ++idx) allIndices.push_back(idx);
                    std::sort(allIndices.begin(), allIndices.end(), [&](int a, int b) {
                        return ord[a-3] < ord[b-3];
                    });
                    for(int k=0; k<allIndices.size(); ++k) ord[allIndices[k]-3] = k;
                }

                setPlaced(m_espPreviewDragElement, modeIndex, bestAnchor);
            } else if (!s_dragFromPreview) {`;

content = content.replace(oldDropLogic, newDropLogic);
fs.writeFileSync('src/menu/menu.cpp', content, 'utf8');
