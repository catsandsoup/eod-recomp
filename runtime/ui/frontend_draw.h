// frontend_draw.h - draws the themed front end (frontend::Menu) with ImGui draw lists:
// rusted-metal panels, riveted frames and chains from the game's UIACTION.RES art
// (ui_art.h), with a procedural imitation where an asset is unavailable. No ImGui
// windows or widgets: every pixel is placed here.
#pragma once
#include <string>
#include <vector>

#include "frontend.h"

struct ImDrawList;
struct ImFont;

namespace frontend_draw {

enum HitKind { HitRow, HitTab, HitLeft, HitRight };
struct Hit {
    float x0, y0, x1, y1;
    HitKind kind;
    int index;
};

struct Ctx {
    ImFont *font = nullptr;
    float w = 1280, h = 720;
    frontend::Glyphs glyphs = frontend::Glyphs::PlayStation;
    std::string version;
    std::string busy;  // e.g. "INSTALLING YOUR DISC..." (drawn over the panels)
};

void draw_menu(ImDrawList *dl, frontend::Menu &m, const Ctx &c, std::vector<Hit> &hits);
void draw_fps(ImDrawList *dl, const Ctx &c, const char *text);

}  // namespace frontend_draw
