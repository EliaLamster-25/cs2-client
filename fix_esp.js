const fs = require('fs');
let content = fs.readFileSync('src/esp/esp_renderer.cpp', 'utf8');

if (!content.includes('<functional>')) {
    content = content.replace('#include "esp_renderer.h"', '#include "esp_renderer.h"\n#include <functional>\n#include <vector>');
}

const oldStruct = `    struct RenderItem {
        int id;
        EspAnchor anchor;
        int order;
        std::function<void()> draw;
    };`;

const newStruct = `    struct RenderItem {
        int id;
        EspAnchor anchor;
        int order;
        std::function<void()> draw;
        RenderItem(int i, EspAnchor a, int o, std::function<void()> d) : id(i), anchor(a), order(o), draw(d) {}
    };`;

content = content.replace(oldStruct, newStruct);

fs.writeFileSync('src/esp/esp_renderer.cpp', content, 'utf8');
