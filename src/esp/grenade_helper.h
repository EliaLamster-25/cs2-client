#pragma once

#include "overlay/renderer.h"
#include "game/entity_manager.h"
#include "gui/font.h"

class GrenadeHelper {
public:
    void render(Renderer& r, const EntityManager::Snapshot& snap,
                const ViewMatrix& vm, const FontAtlas* font);
};
