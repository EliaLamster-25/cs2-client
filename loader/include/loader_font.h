#pragma once

struct ImFont;
struct ImFontAtlas;

struct LoaderFonts {
    ImFont* body = nullptr;
    ImFont* title = nullptr;
    ImFont* caption = nullptr;
};

bool loaderBuildGdiFonts(ImFontAtlas* atlas, LoaderFonts& out);
