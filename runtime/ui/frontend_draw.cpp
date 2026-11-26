// frontend_draw.cpp - the themed front end (see frontend_draw.h).
//
// Layout follows the owner's mockups (FRONTEND_REFERENCES/owner-mockup-*.webp):
// dimmed game behind, chains hanging from the screen edges, a title plate with the
// version, an L1 | tabs | R1 strip, two riveted rust-metal panels (rows on the left,
// details on the right) and a bottom prompt bar with glyphs for the active device.
#include "frontend_draw.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../../third_party/imgui/imgui.h"
#include "ui_art.h"
#include "ui_preview.h"

namespace frontend_draw {
namespace {

using ui_art::Spr;
using frontend::Glyphs;

const ImU32 kOrange = IM_COL32(232, 134, 12, 255);
const ImU32 kOrangeDim = IM_COL32(232, 134, 12, 70);
const ImU32 kWhite = IM_COL32(236, 232, 224, 255);
const ImU32 kGrey = IM_COL32(150, 146, 140, 255);
const ImU32 kDisabled = IM_COL32(110, 106, 100, 255);
const ImU32 kRed = IM_COL32(230, 60, 46, 255);
const ImU32 kGreen = IM_COL32(110, 210, 90, 255);

struct D {
    ImDrawList *dl;
    ImFont *font;
    float s;  // scale: 1.0 at 720 pixels high
};

ImVec2 text_size(const D &d, float px, const char *t) { return d.font->CalcTextSizeA(px, 1e9f, 0, t); }

void text(const D &d, float px, float x, float y, ImU32 col, const char *t, bool shadow = true) {
    if (shadow) d.dl->AddText(d.font, px, ImVec2(x + 1.5f * d.s, y + 1.5f * d.s), IM_COL32(0, 0, 0, 200), t);
    d.dl->AddText(d.font, px, ImVec2(x, y), col, t);
}

// fits text into width max_w by shrinking it (condensed caps should stay on one line)
float fit(const D &d, float px, float max_w, const char *t) {
    const float w = text_size(d, px, t).x;
    return w > max_w && w > 0 ? std::max(px * 0.6f, px * max_w / w) : px;
}

void image(const D &d, Spr s, ImVec2 a, ImVec2 b, ImU32 tint = IM_COL32_WHITE) {
    ui_art::Sprite sp = ui_art::sprite(s);
    if (!sp.ok()) return;
    d.dl->AddImage((ImTextureID)sp.tex, a, b, ImVec2(sp.u0, sp.v0), ImVec2(sp.u1, sp.v1), tint);
}

// Riveted rust-metal panel: game art (metal fill, bars, rivet corners) or the procedural imitation.
void panel(const D &d, ImVec2 a, ImVec2 b, float fill_alpha = 0.92f) {
    const float s = d.s;
    if (ui_art::sprite(Spr::Metal).ok()) {
        d.dl->AddRectFilled(a, b, IM_COL32(10, 9, 8, 255));
        image(d, Spr::Metal, a, b, IM_COL32(120, 110, 100, (int)(255 * fill_alpha)));
        d.dl->AddRectFilledMultiColor(a, b, IM_COL32(0, 0, 0, 40), IM_COL32(0, 0, 0, 40), IM_COL32(0, 0, 0, 120), IM_COL32(0, 0, 0, 120));
    } else {
        d.dl->AddRectFilledMultiColor(a, b, IM_COL32(52, 44, 38, 240), IM_COL32(46, 40, 36, 240), IM_COL32(28, 24, 22, 240),
                                      IM_COL32(34, 28, 24, 240));
    }
    const float bar = 9 * s;
    if (ui_art::sprite(Spr::BarH).ok() && ui_art::sprite(Spr::BarV).ok()) {
        image(d, Spr::BarH, ImVec2(a.x, a.y), ImVec2(b.x, a.y + bar));
        image(d, Spr::BarH, ImVec2(a.x, b.y - bar), ImVec2(b.x, b.y));
        image(d, Spr::BarV, ImVec2(a.x, a.y), ImVec2(a.x + bar, b.y));
        image(d, Spr::BarV, ImVec2(b.x - bar, a.y), ImVec2(b.x, b.y));
    } else {
        const ImU32 rust = IM_COL32(112, 78, 54, 255), dark = IM_COL32(30, 24, 20, 255);
        d.dl->AddRect(a, b, dark, 0, 0, bar);
        d.dl->AddRect(ImVec2(a.x + 2 * s, a.y + 2 * s), ImVec2(b.x - 2 * s, b.y - 2 * s), rust, 0, 0, 3 * s);
    }
    const float c = 26 * s;
    if (ui_art::sprite(Spr::CornerTL).ok()) {
        image(d, Spr::CornerTL, ImVec2(a.x - 4 * s, a.y - 4 * s), ImVec2(a.x - 4 * s + c, a.y - 4 * s + c));
        image(d, Spr::CornerTR, ImVec2(b.x + 4 * s - c, a.y - 4 * s), ImVec2(b.x + 4 * s, a.y - 4 * s + c));
        image(d, Spr::CornerBL, ImVec2(a.x - 4 * s, b.y + 4 * s - c), ImVec2(a.x - 4 * s + c, b.y + 4 * s));
        image(d, Spr::CornerBR, ImVec2(b.x + 4 * s - c, b.y + 4 * s - c), ImVec2(b.x + 4 * s, b.y + 4 * s));
    } else {
        for (ImVec2 p : {ImVec2(a.x + 7 * s, a.y + 7 * s), ImVec2(b.x - 7 * s, a.y + 7 * s), ImVec2(a.x + 7 * s, b.y - 7 * s),
                         ImVec2(b.x - 7 * s, b.y - 7 * s)}) {
            d.dl->AddCircleFilled(p, 4.5f * s, IM_COL32(150, 140, 128, 255));
            d.dl->AddCircleFilled(ImVec2(p.x - 1 * s, p.y - 1 * s), 2 * s, IM_COL32(220, 214, 200, 255));
        }
    }
}

// A chain from p to q (the game's chain strip, rotated along the segment).
void chain(const D &d, ImVec2 p, ImVec2 q) {
    ui_art::Sprite sp = ui_art::sprite(Spr::Chain);
    const float dx = q.x - p.x, dy = q.y - p.y, len = std::sqrt(dx * dx + dy * dy);
    if (len < 1) return;
    const float th = 13 * d.s, nx = -dy / len * th * 0.5f, ny = dx / len * th * 0.5f;
    if (!sp.ok()) {
        const int links = (int)(len / (14 * d.s));
        for (int i = 0; i < links; i++) {
            const float t = (i + 0.5f) / links;
            ImVec2 c(p.x + dx * t, p.y + dy * t);
            d.dl->AddEllipse(c, ImVec2(8 * d.s, (i & 1) ? 3 * d.s : 5 * d.s), IM_COL32(70, 62, 56, 255), std::atan2(dy, dx), 0, 2.5f * d.s);
        }
        return;
    }
    // repeat the strip along the segment
    const float seg = sp.w * th / sp.h;
    float t0 = 0;
    while (t0 < len) {
        const float t1 = std::min(len, t0 + seg), f = (t1 - t0) / seg;
        ImVec2 a(p.x + dx * t0 / len, p.y + dy * t0 / len), b(p.x + dx * t1 / len, p.y + dy * t1 / len);
        const float u1 = sp.u0 + (sp.u1 - sp.u0) * f;
        d.dl->AddImageQuad((ImTextureID)sp.tex, ImVec2(a.x + nx, a.y + ny), ImVec2(b.x + nx, b.y + ny), ImVec2(b.x - nx, b.y - ny),
                           ImVec2(a.x - nx, a.y - ny), ImVec2(sp.u0, sp.v0), ImVec2(u1, sp.v0), ImVec2(u1, sp.v1),
                           ImVec2(sp.u0, sp.v1), IM_COL32(200, 190, 180, 255));
        t0 = t1;
    }
}

enum class G { Navigate, Accept, Back, Details, Reset, PrevTab, NextTab, Move };

// One prompt glyph at (x, y) with height hgt; returns its width.
float glyph(const D &d, Glyphs style, G g, float x, float y, float hgt) {
    const float s = d.s;
    auto keycap = [&](const char *label) {
        const float px = hgt * 0.62f;
        const float w = std::max(hgt, text_size(d, px, label).x + hgt * 0.5f);
        d.dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + hgt), IM_COL32(222, 218, 210, 255), 4 * s);
        d.dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + hgt), IM_COL32(60, 56, 52, 255), 4 * s, 0, 1.5f * s);
        d.dl->AddText(d.font, px, ImVec2(x + (w - text_size(d, px, label).x) * 0.5f, y + (hgt - px) * 0.5f + 1 * s), IM_COL32(20, 20, 20, 255), label);
        return w;
    };
    auto face = [&](ImU32 col, const char *label) {
        const float r = hgt * 0.5f;
        ImVec2 c(x + r, y + r);
        d.dl->AddCircleFilled(c, r, IM_COL32(30, 30, 32, 255));
        d.dl->AddCircleFilled(c, r * 0.86f, col);
        const float px = hgt * 0.7f;
        d.dl->AddText(d.font, px, ImVec2(c.x - text_size(d, px, label).x * 0.5f, c.y - px * 0.5f + 1 * s), IM_COL32(255, 255, 255, 255), label);
        return hgt;
    };
    auto bumper = [&](const char *label) {
        const float w = hgt * 1.5f, px = hgt * 0.6f;
        d.dl->AddRectFilled(ImVec2(x, y + hgt * 0.1f), ImVec2(x + w, y + hgt * 0.9f), IM_COL32(56, 56, 60, 255), hgt * 0.3f);
        d.dl->AddRect(ImVec2(x, y + hgt * 0.1f), ImVec2(x + w, y + hgt * 0.9f), IM_COL32(170, 170, 175, 255), hgt * 0.3f, 0, 1.5f * s);
        d.dl->AddText(d.font, px, ImVec2(x + (w - text_size(d, px, label).x) * 0.5f, y + (hgt - px) * 0.5f + 1 * s), kWhite, label);
        return w;
    };
    auto sprite = [&](Spr sp, float aspect) {
        if (!ui_art::sprite(sp).ok()) return -1.0f;
        const float w = hgt * aspect;
        image(d, sp, ImVec2(x, y), ImVec2(x + w, y + hgt));
        return w;
    };
    auto dpad = [&]() {
        float w = sprite(Spr::PadDpad, 1.0f);
        if (w > 0) return w;
        const float t = hgt / 3;
        d.dl->AddRectFilled(ImVec2(x + t, y), ImVec2(x + 2 * t, y + hgt), IM_COL32(70, 70, 74, 255), 2 * s);
        d.dl->AddRectFilled(ImVec2(x, y + t), ImVec2(x + hgt, y + 2 * t), IM_COL32(70, 70, 74, 255), 2 * s);
        return hgt;
    };
    if (g == G::Navigate) {
        if (style == Glyphs::Keyboard) return keycap("ARROWS");
        return dpad();
    }
    if (style == Glyphs::Keyboard) {
        switch (g) {
        case G::Accept: return keycap("ENTER");
        case G::Back: return keycap("ESC");
        case G::Details: return keycap("SPACE");
        case G::Reset: return keycap("R");
        case G::PrevTab: return keycap("Q");
        case G::NextTab: return keycap("E");
        case G::Move: return keycap("PG UP/DN");
        default: return 0;
        }
    }
    if (style == Glyphs::Xbox) {
        switch (g) {
        case G::Accept: return face(IM_COL32(84, 160, 44, 255), "A");
        case G::Back: return face(IM_COL32(200, 40, 36, 255), "B");
        case G::Details: return face(IM_COL32(36, 100, 210, 255), "X");
        case G::Reset: return face(IM_COL32(220, 180, 20, 255), "Y");
        case G::PrevTab: return bumper("LB");
        case G::NextTab: return bumper("RB");
        case G::Move: return bumper("LT/RT");
        default: return 0;
        }
    }
    float w = -1;
    switch (g) {  // PlayStation: the game's own ps2 button art
    case G::Accept: w = sprite(Spr::PadCross, 1); if (w < 0) w = face(IM_COL32(60, 90, 200, 255), "X"); break;
    case G::Back: w = sprite(Spr::PadCircle, 1); if (w < 0) w = face(IM_COL32(200, 50, 60, 255), "O"); break;
    case G::Details: w = sprite(Spr::PadSquare, 1); if (w < 0) w = face(IM_COL32(200, 80, 170, 255), "[]"); break;
    case G::Reset: w = sprite(Spr::PadTriangle, 1); if (w < 0) w = face(IM_COL32(40, 170, 140, 255), "^"); break;
    case G::PrevTab: w = sprite(Spr::PadL1, 32.0f / 19); if (w < 0) w = bumper("L1"); break;
    case G::NextTab: w = sprite(Spr::PadR1, 32.0f / 19); if (w < 0) w = bumper("R1"); break;
    case G::Move: w = bumper("L2/R2"); break;
    default: w = 0;
    }
    return w;
}

std::string upper(std::string t) {
    for (char &c : t) c = (char)toupper((unsigned char)c);
    return t;
}

// word-wrapped paragraph; returns the height used
float paragraph(const D &d, float px, float x, float y, float w, ImU32 col, const std::string &t) {
    d.dl->AddText(d.font, px, ImVec2(x, y), col, t.c_str(), nullptr, w);
    return d.font->CalcTextSizeA(px, 1e9f, w, t.c_str()).y;
}

}  // namespace

void draw_menu(ImDrawList *dl, frontend::Menu &m, const Ctx &c, std::vector<Hit> &hits) {
    hits.clear();
    D d{dl, c.font ? c.font : ImGui::GetFont(), c.h / 720.0f};
    const float s = d.s, W = c.w, H = c.h;
    // fit a 16:9 layout box in the window (wider screens keep it centred)
    const float bw = std::min(W, H * 16.0f / 9.0f), bx = (W - bw) * 0.5f;
    const float k = bw / 1280.0f;  // horizontal unit
    // dim the game
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), IM_COL32(0, 0, 0, 150));
    const bool err = m.error_active();
    const auto &rows = m.rows();
    const char *online_modal = m.online_modal_title();

    // Screen D: Translucent In-Match Quick Pause Rail (~32% left rail over live 3D gameplay)
    if (!err && m.quick_rail_open()) {
        dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), IM_COL32(0, 0, 0, 55));
        const float rail_w = std::clamp(W * 0.32f, 340.0f * s, 460.0f * s);
        const ImVec2 qa(24 * s, 24 * s), qb(24 * s + rail_w, H - 78 * s);
        dl->AddRectFilled(qa, qb, IM_COL32(14, 12, 11, 215), 6 * s);
        dl->AddRect(qa, qb, IM_COL32(110, 96, 84, 235), 6 * s, 0, 2.0f * s);
        dl->AddRectFilled(qa, ImVec2(qa.x + 5 * s, qb.y), kOrange, 2 * s);

        game_frontend::Snapshot snap;
        if (m.network()) snap = m.network()->snapshot();
        const bool online_live = snap.phase == game_frontend::Snapshot::Phase::Racing ||
                                 snap.phase == game_frontend::Snapshot::Phase::Loading;
        const char *mode_hdr = online_live ? "ONLINE  •  WORLD LIVE"
                             : (snap.ee_state >= 5 && snap.ee_state <= 7) ? "CAREER  •  PAUSED"
                             : snap.ee_state == 3 ? "SPLIT-SCREEN  •  PAUSED"
                             : "SOLO MATCH  •  PAUSED";
        text(d, 22 * s, qa.x + 18 * s, qa.y + 14 * s, online_live ? kGreen : kOrange, mode_hdr);
        dl->AddLine(ImVec2(qa.x + 14 * s, qa.y + 44 * s), ImVec2(qb.x - 14 * s, qa.y + 44 * s), IM_COL32(232, 134, 12, 130), 1.5f * s);

        const float qx0 = qa.x + 18 * s, qx1 = qb.x - 18 * s, qy0 = qa.y + 54 * s, qy1 = qb.y - 14 * s;
        const float qrh = 38 * s;
        const int qvis = std::max(1, (int)((qy1 - qy0) / qrh));
        int sel = m.selected();
        int scroll = m.scroll();
        if (sel < scroll) scroll = sel;
        if (sel >= scroll + qvis) scroll = sel - qvis + 1;
        scroll = std::clamp(scroll, 0, std::max(0, (int)rows.size() - qvis));
        m.set_scroll(scroll);

        dl->PushClipRect(ImVec2(qa.x, qy0), ImVec2(qb.x, qy1), true);
        for (int i = scroll; i < (int)rows.size() && i < scroll + qvis; ++i) {
            const auto &r = rows[i];
            const float y = qy0 + (i - scroll) * qrh;
            if (r.kind == frontend::RowKind::Header) {
                text(d, 17 * s, qx0, y + 10 * s, kOrange, r.label.c_str());
                continue;
            }
            const bool on = (i == sel);
            if (on) {
                dl->AddRectFilled(ImVec2(qx0 - 6 * s, y + 2 * s), ImVec2(qx1 + 6 * s, y + qrh - 2 * s), kOrangeDim, 4 * s);
                dl->AddRect(ImVec2(qx0 - 6 * s, y + 2 * s), ImVec2(qx1 + 6 * s, y + qrh - 2 * s), kOrange, 4 * s, 0, 2.0f * s);
            }
            hits.push_back({qx0 - 6 * s, y, qx1 + 6 * s, y + qrh, HitRow, i});
            const float lpx = fit(d, 20 * s, (qx1 - qx0) * 0.68f, r.label.c_str());
            text(d, lpx, qx0, y + 8 * s, on ? kOrange : kWhite, r.label.c_str());
            const float vpx = fit(d, 16 * s, (qx1 - qx0) * 0.30f, r.value.c_str());
            text(d, vpx, qx1 - text_size(d, vpx, r.value.c_str()).x, y + 10 * s, on ? kWhite : kGrey, r.value.c_str());
        }
        dl->PopClipRect();

        // Optional Right Translucent Glass Card: Live Scoreboard & Active Rules / Career Snapshot
        if (m.quick_scoreboard_open()) {
            const ImVec2 sa(qb.x + 20 * s, 24 * s), sb(W - 24 * s, H - 78 * s);
            dl->AddRectFilled(sa, sb, IM_COL32(14, 12, 11, 200), 6 * s);
            dl->AddRect(sa, sb, IM_COL32(110, 96, 84, 220), 6 * s, 0, 1.5f * s);
            float sy = sa.y + 16 * s;
            const float sx0 = sa.x + 18 * s, sx1 = sb.x - 18 * s;
            text(d, 24 * s, sx0, sy, kOrange, "LIVE SCOREBOARD & ACTIVE RULES");
            sy += 34 * s;
            if (!snap.scoreboard.empty()) {
                for (const auto &sr : snap.scoreboard) {
                    if (sy + 24 * s > sb.y - 20 * s) break;
                    char line[128];
                    if (sr.is_tagger) {
                        snprintf(line, sizeof line, "#%02u  %-18s  [TAGGER %.1fs]  SCORE %d", sr.slot + 1, sr.name.c_str(), sr.fuse_sec, sr.score);
                    } else {
                        snprintf(line, sizeof line, "#%02u  %-18s  %-10s  SCORE %d  %s", sr.slot + 1, sr.name.c_str(), sr.role.c_str(), sr.score, sr.alive ? "ALIVE" : "OUT");
                    }
                    text(d, fit(d, 18 * s, sx1 - sx0, line), sx0, sy, sr.is_tagger ? kOrange : sr.alive ? kWhite : kDisabled, line);
                    sy += 24 * s;
                }
            } else if (snap.career.loaded) {
                char line[128];
                snprintf(line, sizeof line, "RANK #%d   |   CASH $%d   |   GARAGE %d/%d   |   EVES %d/%d",
                         snap.career.rank, snap.career.money, snap.career.garage_count, snap.career.garage_cap,
                         snap.career.events_done, snap.career.events_total);
                text(d, fit(d, 20 * s, sx1 - sx0, line), sx0, sy, kWhite, line);
                sy += 32 * s;
            } else {
                text(d, 19 * s, sx0, sy, kGrey, "Match telemetry active. Live scores appear once cars grid up.");
                sy += 30 * s;
            }
            const auto &draft = !snap.mutator_draft.name.empty() ? snap.mutator_draft : m.mutator_draft();
            if (!draft.name.empty()) {
                sy += 8 * s;
                dl->AddLine(ImVec2(sx0, sy), ImVec2(sx1, sy), IM_COL32(232, 134, 12, 100), 1.5f * s);
                sy += 12 * s;
                text(d, 20 * s, sx0, sy, kOrange, ("MODE: " + upper(draft.name)).c_str());
                sy += 26 * s;
                text(d, 18 * s, sx0, sy, draft.uses_weapons ? kGreen : kRed, draft.uses_weapons ? "WEAPONS: ON" : "WEAPONS: OFF");
            }
        }

        // Compact bottom glyph bar
        const ImVec2 ba(24 * s, H - 64 * s), bb(W - 24 * s, H - 18 * s);
        dl->AddRectFilled(ba, bb, IM_COL32(14, 12, 11, 215), 8 * s);
        dl->AddRect(ba, bb, IM_COL32(100, 88, 76, 220), 8 * s, 0, 1.5f * s);
        const float gh = 26 * s, px = 20 * s;
        float gx = ba.x + 20 * s;
        const float gy = (ba.y + bb.y - gh) * 0.5f;
        gx += glyph(d, c.glyphs, G::Navigate, gx, gy, gh) + 6 * s;
        text(d, px, gx, gy + 3 * s, kWhite, "NAVIGATE"); gx += 120 * s;
        gx += glyph(d, c.glyphs, G::Accept, gx, gy, gh) + 6 * s;
        text(d, px, gx, gy + 3 * s, kWhite, "SELECT"); gx += 100 * s;
        gx += glyph(d, c.glyphs, G::Back, gx, gy, gh) + 6 * s;
        text(d, px, gx, gy + 3 * s, kWhite, "RESUME");
        return;
    }
    const bool play_3col = !err && !online_modal && !m.quick_rail_open() &&
                           m.tab() == frontend::Tab::Play &&
                           m.sub_screen() == frontend::Menu::SubScreen::None;
    const bool is_browser = !err && !online_modal && !m.quick_rail_open() &&
                            m.tab() == frontend::Tab::Play &&
                            (m.sub_screen() == frontend::Menu::SubScreen::ModeBrowser ||
                             m.sub_screen() == frontend::Menu::SubScreen::MapBrowser ||
                             m.sub_screen() == frontend::Menu::SubScreen::CarBrowser);
    const bool is_rules = !err && !online_modal && !m.quick_rail_open() &&
                          m.tab() == frontend::Tab::Play &&
                          m.sub_screen() == frontend::Menu::SubScreen::RulesDrawer;

    // panels' geometry
    const float top = ((play_3col || is_rules) ? 174 : 150) * s, bottom = H - 96 * s;
    const ImVec2 la(bx + (play_3col ? 42 : 70) * k, top), lb(bx + (play_3col ? 422 : 668) * k, bottom);
    const ImVec2 ca(bx + 436 * k, top), cb(bx + 866 * k, bottom);
    const ImVec2 ra(bx + (play_3col ? 880 : 690) * k, top), rb(bx + (play_3col ? 1238 : 1210) * k, bottom);
    // chains: from the screen's top corners down the sides, and side links to the panels
    chain(d, ImVec2(bx + 18 * k, 0), ImVec2(bx + 22 * k, H));
    chain(d, ImVec2(bx + bw - 18 * k, 0), ImVec2(bx + bw - 22 * k, H));
    chain(d, ImVec2(bx + 22 * k, top + 40 * s), ImVec2(la.x, top + 30 * s));
    chain(d, ImVec2(bx + 22 * k, bottom - 50 * s), ImVec2(la.x, bottom - 40 * s));
    chain(d, ImVec2(rb.x, top + 30 * s), ImVec2(bx + bw - 22 * k, top + 40 * s));
    chain(d, ImVec2(rb.x, bottom - 40 * s), ImVec2(bx + bw - 22 * k, bottom - 50 * s));
    chain(d, ImVec2(bx + 470 * k, 0), ImVec2(bx + 430 * k, 24 * s));
    chain(d, ImVec2(bx + 810 * k, 0), ImVec2(bx + 850 * k, 24 * s));

    // title plate
    {
        const ImVec2 a(bx + 400 * k, 18 * s), b(bx + 880 * k, 72 * s);
        if (ui_art::sprite(Spr::Plate).ok()) {
            image(d, Spr::Plate, a, b, IM_COL32(120, 104, 92, 255));
            dl->AddRectFilled(ImVec2(a.x + 12 * s, a.y + 9 * s), ImVec2(b.x - 12 * s, b.y - 9 * s), IM_COL32(16, 14, 12, 225), 4 * s);
            dl->AddRect(ImVec2(a.x + 12 * s, a.y + 9 * s), ImVec2(b.x - 12 * s, b.y - 9 * s), IM_COL32(90, 80, 70, 255), 4 * s, 0, 2 * s);
        } else {
            panel(d, a, b);
        }
        const std::string title = "TEST DRIVE: EOD - RECOMP ";
        const std::string ver = "v" + c.version;
        float px = 34 * s;
        const float tw = text_size(d, px, title.c_str()).x, vw = text_size(d, px * 0.9f, ver.c_str()).x;
        float x = (a.x + b.x - tw - vw) * 0.5f - 6 * s;
        text(d, px, x, a.y + 11 * s, kWhite, title.c_str());
        text(d, px * 0.9f, x + tw, a.y + 13 * s, kOrange, ver.c_str());
        dl->AddCircleFilled(ImVec2(b.x - 22 * s, (a.y + b.y) * 0.5f), 5 * s, IM_COL32(255, 150, 40, 255));
        dl->AddCircleFilled(ImVec2(b.x - 22 * s, (a.y + b.y) * 0.5f), 9 * s, IM_COL32(255, 150, 40, 60));
    }

    // tab strip (or the error title)
    {
        const ImVec2 a(bx + 250 * k, 84 * s), b(bx + 1030 * k, 128 * s);
        dl->AddRectFilled(a, b, IM_COL32(14, 12, 11, 235), 6 * s);
        dl->AddRect(a, b, IM_COL32(110, 98, 86, 255), 6 * s, 0, 2 * s);
        if (err || online_modal) {
            const std::string t = err ? upper(m.error_title()) : online_modal;
            float px = fit(d, 30 * s, b.x - a.x - 30 * s, t.c_str());
            text(d, px, (a.x + b.x - text_size(d, px, t.c_str()).x) * 0.5f, a.y + 7 * s, err ? kRed : kOrange, t.c_str());
        } else {
            const int n = (int)frontend::Tab::Count;
            const float tw = (b.x - a.x) / n;
            for (int i = 0; i < n; i++) {
                const char *name = frontend::tab_name((frontend::Tab)i);
                const bool on = (int)m.tab() == i;
                float px = 30 * s;
                const float x0 = a.x + tw * i;
                text(d, px, x0 + (tw - text_size(d, px, name).x) * 0.5f, a.y + 7 * s, on ? kOrange : kWhite, name);
                if (i) dl->AddLine(ImVec2(x0, a.y + 10 * s), ImVec2(x0, b.y - 10 * s), IM_COL32(150, 140, 130, 200), 2 * s);
                hits.push_back({x0, a.y, x0 + tw, b.y, HitTab, i});
            }
            const float gh = 30 * s;
            glyph(d, c.glyphs, G::PrevTab, a.x - 70 * s, a.y + 7 * s, gh);
            glyph(d, c.glyphs, G::NextTab, b.x + 14 * s, a.y + 7 * s, gh);
        }
    }

    // Screen A top utility bar (when in 3-column PLAY Hub)
    if (play_3col) {
        const ImVec2 ua(bx + 42 * k, 134 * s), ub(bx + 1238 * k, 166 * s);
        dl->AddRectFilled(ua, ub, IM_COL32(16, 14, 12, 235), 4 * s);
        dl->AddRect(ua, ub, IM_COL32(100, 88, 76, 255), 4 * s, 0, 1.5f * s);
        game_frontend::Snapshot snap;
        if (m.network()) snap = m.network()->snapshot();
        using SK = game_frontend::SessionKind;
        const bool net_live = snap.phase != game_frontend::Snapshot::Phase::Offline &&
                              snap.phase != game_frontend::Snapshot::Phase::Disconnected &&
                              snap.phase != game_frontend::Snapshot::Phase::Error;
        const char *badge = net_live ? (snap.is_host ? "HOSTING ONLINE" : "ONLINE LOBBY")
                          : m.session_kind() == SK::Career ? "CAREER MODE"
                          : m.session_kind() == SK::SplitScreen ? "LOCAL SPLIT-SCREEN (2P-4P)"
                          : m.session_kind() == SK::Online ? "ONLINE MULTIPLAYER"
                          : "SOLO QUICK PLAY";
        dl->AddCircleFilled(ImVec2(ua.x + 16 * s, (ua.y + ub.y) * 0.5f), 5 * s, net_live ? kGreen : kOrange);
        text(d, 20 * s, ua.x + 28 * s, ua.y + 6 * s, kWhite, badge);
        if (!snap.code.empty()) {
            const std::string code_txt = "GARAGE CODE: " + snap.code;
            text(d, 20 * s, ua.x + 290 * k, ua.y + 6 * s, kOrange, code_txt.c_str());
        }
        const std::string social = "CREW: " + std::to_string(snap.crew.size()) + " ONLINE   |   LAN: " + std::to_string(snap.lan.size()) + " ROOMS";
        const float sw = text_size(d, 19 * s, social.c_str()).x;
        text(d, 19 * s, ub.x - sw - 16 * s, ua.y + 6 * s, snap.crew.empty() && snap.lan.empty() ? kGrey : kGreen, social.c_str());
    }

    // Screen C: 4-Subtab Bar for Customize Rules Drawer
    if (is_rules) {
        const ImVec2 ua(bx + 70 * k, 134 * s), ub(bx + 1210 * k, 166 * s);
        static const char *kRuleTabs[4] = {"MODE RULES", "FEATS & WEAPONS", "PHYSICS & SCALE", "BOTS & ROLES"};
        const float tw = (ub.x - ua.x) / 4.0f;
        for (int i = 0; i < 4; ++i) {
            const bool active = (m.rules_tab() == i);
            const ImVec2 t0(ua.x + i * tw + 3 * s, ua.y), t1(ua.x + (i + 1) * tw - 3 * s, ub.y);
            dl->AddRectFilled(t0, t1, active ? kOrangeDim : IM_COL32(18, 16, 14, 230), 4 * s);
            dl->AddRect(t0, t1, active ? kOrange : IM_COL32(95, 86, 76, 255), 4 * s, 0, active ? 2 * s : 1.2f * s);
            const float fpx = fit(d, 19 * s, (t1.x - t0.x) - 10 * s, kRuleTabs[i]);
            text(d, fpx, (t0.x + t1.x - text_size(d, fpx, kRuleTabs[i]).x) * 0.5f, t0.y + 6 * s, active ? kOrange : kWhite, kRuleTabs[i]);
            hits.push_back({t0.x, t0.y, t1.x, t1.y, HitSubTab, i});
        }
    }

    // Screen B: Trackmania-style Visual Mode & Map Browser (3 Hero Cards + Categorized Visual Grid)
    if (is_browser) {
        const ImVec2 ba(bx + 42 * k, 136 * s), bb(bx + 1238 * k, H - 96 * s);
        panel(d, ba, bb);
        const float px0 = ba.x + 20 * s, px1 = bb.x - 20 * s;
        float py = ba.y + 14 * s;
        const bool map_mode = (m.sub_screen() == frontend::Menu::SubScreen::MapBrowser);
        const bool car_mode = (m.sub_screen() == frontend::Menu::SubScreen::CarBrowser);
        const char *mode_cats[4] = {"ALL", "RETAIL", "PARTY & ARENA", "MY CUSTOM MODES"};
        const char *map_cats[3] = {"ALL MAPS", "RETAIL TRACKS", "CUSTOM MAPS (.EODMAP)"};
        const char *car_cats[7] = {"ALL", "COMPACT", "MIDSIZE", "MUSCLE", "SPECIAL", "TRAILERS", "MODDED"};
        const int ncats = car_mode ? 7 : map_mode ? 3 : 4;
        const float cat_w = (px1 - px0) / ncats;
        for (int i = 0; i < ncats; ++i) {
            const bool active = (m.browser_category() == i);
            const ImVec2 c0(px0 + i * cat_w + 3 * s, py), c1(px0 + (i + 1) * cat_w - 3 * s, py + 32 * s);
            dl->AddRectFilled(c0, c1, active ? kOrangeDim : IM_COL32(20, 18, 16, 220), 4 * s);
            dl->AddRect(c0, c1, active ? kOrange : IM_COL32(90, 82, 74, 255), 4 * s, 0, active ? 2 * s : 1.2f * s);
            const char *lbl = car_mode ? car_cats[i] : map_mode ? map_cats[i] : mode_cats[i];
            const float fpx = fit(d, (car_mode ? 17.5f : 20.0f) * s, (c1.x - c0.x) - 10 * s, lbl);
            text(d, fpx, (c0.x + c1.x - text_size(d, fpx, lbl).x) * 0.5f, c0.y + 6 * s, active ? kOrange : kWhite, lbl);
            hits.push_back({c0.x, c0.y, c1.x, c1.y, HitBrowserCat, i});
        }
        py += 44 * s;

        const auto items = m.browser_items();
        const int sel_idx = std::clamp(m.browser_selected(), 0, std::max(0, (int)items.size() - 1));
        const int cols = 3;
        const float gap = 14 * s;
        const float card_w = (px1 - px0 - gap * (cols - 1)) / cols;
        const float hero_h = 144 * s;
        const float tile_h = car_mode ? 132.0f * s : 116.0f * s;
        const int start_row = std::max(0, (sel_idx / cols) - 1);

        dl->PushClipRect(ImVec2(ba.x + 8 * s, py), ImVec2(bb.x - 8 * s, bb.y - 12 * s), true);
        for (int i = start_row * cols; i < (int)items.size(); ++i) {
            const int r_vis = (i / cols) - start_row;
            const int col = i % cols;
            const bool is_hero_row = (!map_mode && !car_mode && m.browser_category() == 0 && (i / cols) == 0);
            const float h = is_hero_row ? hero_h : tile_h;
            float y = py;
            for (int rr = start_row; rr < (i / cols); ++rr) {
                y += ((!map_mode && !car_mode && m.browser_category() == 0 && rr == 0) ? hero_h : tile_h) + gap;
            }
            if (y > bb.y - 16 * s) break;
            const float x = px0 + col * (card_w + gap);
            const ImVec2 t0(x, y), t1(x + card_w, y + h);
            const bool on = (i == sel_idx);
            const auto &it = items[i];

            dl->AddRectFilled(t0, t1, IM_COL32(22, 19, 17, 240), 4 * s);
            if (it.is_car && it.car_num) {
                const auto cpic = ui_preview::car(it.car_num);
                if (cpic.ok()) {
                    const float avail_h = h - 46 * s;
                    float pw = card_w - 16 * s, ph = pw / cpic.aspect;
                    if (ph > avail_h) { ph = avail_h; pw = ph * cpic.aspect; }
                    const float px_car = t0.x + (card_w - pw) * 0.5f;
                    const float py_car = t0.y + 6 * s + (avail_h - ph) * 0.5f;
                    dl->AddImage((ImTextureID)cpic.tex, ImVec2(px_car, py_car), ImVec2(px_car + pw, py_car + ph),
                                 ImVec2(cpic.u0, cpic.v0), ImVec2(cpic.u1, cpic.v1),
                                 on ? IM_COL32(255, 255, 255, 255) : IM_COL32(220, 215, 210, 225));
                }
            } else if (!it.track_art.empty()) {
                const auto pic = ui_preview::track(it.track_art);
                if (pic.ok()) {
                    dl->AddImage((ImTextureID)pic.tex, t0, t1, ImVec2(pic.u0, pic.v0), ImVec2(pic.u1, pic.v1),
                                 on ? IM_COL32(255, 255, 255, 235) : IM_COL32(185, 180, 175, 200));
                }
            }
            if ((it.featured || it.is_custom_map || it.is_modded) && ui_art::sprite(Spr::Hazard).ok()) {
                image(d, Spr::Hazard, t0, ImVec2(t1.x, t0.y + 12 * s), IM_COL32(255, 255, 255, 180));
            }
            // Dark bottom scrim for crisp 10-foot TV legibility
            dl->AddRectFilledMultiColor(ImVec2(t0.x, t1.y - 62 * s), t1,
                                        IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
                                        IM_COL32(0, 0, 0, 235), IM_COL32(0, 0, 0, 235));
            dl->AddRect(t0, t1, on ? kOrange : IM_COL32(95, 86, 76, 230), 4 * s, 0, on ? 3.0f * s : 1.5f * s);

            // Top-right badge pill
            if (!it.badge.empty()) {
                const float bpx = 14 * s;
                const float bw2 = text_size(d, bpx, it.badge.c_str()).x + 12 * s;
                const ImVec2 bg0(t1.x - bw2 - 8 * s, t0.y + 16 * s), bg1(t1.x - 8 * s, t0.y + 34 * s);
                dl->AddRectFilled(bg0, bg1, it.featured ? kOrange : it.is_custom_map ? IM_COL32(46, 130, 220, 230) : IM_COL32(20, 20, 20, 210), 3 * s);
                text(d, bpx, bg0.x + 6 * s, bg0.y + 2 * s, it.featured ? IM_COL32(16, 14, 12, 255) : kWhite, it.badge.c_str(), false);
            }
            const float title_px = fit(d, (is_hero_row ? 23 : 20) * s, card_w - 16 * s, it.title.c_str());
            text(d, title_px, t0.x + 10 * s, t1.y - 44 * s, on ? kOrange : kWhite, it.title.c_str());
            const float sub_px = fit(d, 14.5f * s, card_w - 16 * s, it.subtitle.c_str());
            text(d, sub_px, t0.x + 10 * s, t1.y - 20 * s, kGrey, it.subtitle.c_str());
            hits.push_back({t0.x, t0.y, t1.x, t1.y, HitBrowserTile, i});
            (void)r_vis;
        }
        dl->PopClipRect();
    } else {
    panel(d, la, lb);
    if (play_3col) panel(d, ca, cb);
    panel(d, ra, rb);

    // left: rows
    const float pad = 22 * s, rh = 46 * s;
    const float lx0 = la.x + pad, lx1 = lb.x - pad, ly0 = la.y + pad, ly1 = lb.y - pad;
    const int visible = std::max(1, (int)((ly1 - ly0) / rh));
    int sel = m.selected();
    int scroll = m.scroll();
    if (sel < scroll) scroll = sel;
    if (sel >= scroll + visible) scroll = sel - visible + 1;
    // show the section header above a selected first row
    if (scroll > 0 && scroll == sel && rows[scroll - 1].kind == frontend::RowKind::Header) scroll--;
    scroll = std::clamp(scroll, 0, std::max(0, (int)rows.size() - visible));
    m.set_scroll(scroll);
    dl->PushClipRect(ImVec2(la.x, ly0 - 4 * s), ImVec2(lb.x, ly1 + 4 * s), true);
    for (int i = scroll; i < (int)rows.size() && i < scroll + visible; i++) {
        const frontend::Row &r = rows[i];
        const float y = ly0 + (i - scroll) * rh;
        const bool on = i == sel && r.kind != frontend::RowKind::Header;
        const float px = 27 * s;
        if (r.kind == frontend::RowKind::Header) {
            text(d, 22 * s, lx0, y + 14 * s, kOrange, r.label.c_str());
            dl->AddLine(ImVec2(lx0 + text_size(d, 22 * s, r.label.c_str()).x + 12 * s, y + 27 * s), ImVec2(lx1, y + 27 * s),
                        IM_COL32(232, 134, 12, 110), 1.5f * s);
            continue;
        }
        if (on) {
            dl->AddRectFilled(ImVec2(lx0 - 8 * s, y + 2 * s), ImVec2(lx1 + 8 * s, y + rh - 2 * s), kOrangeDim, 4 * s);
            dl->AddRect(ImVec2(lx0 - 8 * s, y + 2 * s), ImVec2(lx1 + 8 * s, y + rh - 2 * s), kOrange, 4 * s, 0, 2.5f * s);
        }
        hits.push_back({lx0 - 8 * s, y, lx1 + 8 * s, y + rh, HitRow, i});
        const ImU32 lc = !r.enabled ? kDisabled : r.warn ? kRed : kWhite;
        float x = lx0;
        if (r.kind == frontend::RowKind::Check) {  // mod checkbox
            const float b = 26 * s, cy = y + (rh - b) * 0.5f;
            dl->AddRectFilled(ImVec2(x, cy), ImVec2(x + b, cy + b), IM_COL32(20, 18, 16, 255));
            dl->AddRect(ImVec2(x, cy), ImVec2(x + b, cy + b), r.checked ? kOrange : kGrey, 2 * s, 0, 2 * s);
            if (r.checked) text(d, 26 * s, x + 6 * s, cy + 1 * s, kOrange, "X", false);
            x += b + 14 * s;
            std::string label = r.label + (r.value.empty() ? "" : " " + r.value);
            const float avail = lx1 - x - (r.note.empty() ? 0 : 110 * s);
            text(d, fit(d, px, avail, label.c_str()), x, y + 9 * s, lc, label.c_str());
            if (!r.note.empty()) text(d, 18 * s, lx1 - text_size(d, 18 * s, r.note.c_str()).x, y + 15 * s, kGrey, r.note.c_str());
            if (r.warn) dl->AddCircleFilled(ImVec2(lx1 + 2 * s, y + rh * 0.5f), 5 * s, kRed);
            continue;
        }
        const float label_w = (lx1 - lx0) * 0.46f;
        text(d, fit(d, px, label_w, r.label.c_str()), x, y + 9 * s, lc, r.label.c_str());
        // value: "< value >" for adjustable rows
        const float vx0 = lx0 + label_w + 8 * s, vx1 = lx1;
        const bool adjustable = r.kind == frontend::RowKind::Choice || r.kind == frontend::RowKind::Toggle;
        ImU32 vc = !r.enabled ? kDisabled : r.warn ? kRed : r.good ? kGreen : (on ? kOrange : kWhite);
        float vpx = 24 * s;
        const float inner = vx1 - vx0 - (adjustable ? 44 * s : 0);
        vpx = fit(d, vpx, inner, r.value.c_str());
        const float tw = text_size(d, vpx, r.value.c_str()).x;
        if (adjustable) {
            text(d, 26 * s, vx0, y + 9 * s, on ? kOrange : kGrey, "<");
            text(d, 26 * s, vx1 - 12 * s, y + 9 * s, on ? kOrange : kGrey, ">");
            hits.push_back({vx0 - 6 * s, y, vx0 + 24 * s, y + rh, HitLeft, i});
            hits.push_back({vx1 - 24 * s, y, vx1 + 6 * s, y + rh, HitRight, i});
            if (r.slider_fraction >= 0) {
                const float left = vx0 + 28*s, right = vx1 - 28*s;
                const float mid = y + 35*s, thumb = left + (right-left)*r.slider_fraction;
                dl->AddLine(ImVec2(left, mid), ImVec2(right, mid), kGrey, 3*s);
                dl->AddLine(ImVec2(left, mid), ImVec2(thumb, mid), kOrange, 3*s);
                dl->AddCircleFilled(ImVec2(thumb, mid), 5*s, vc);
                hits.push_back({left, y, right, y+rh, HitSlider, i});
                text(d, vpx, (vx0 + vx1 - tw)*0.5f, y + 3*s, vc, r.value.c_str());
            } else {
                text(d, vpx, (vx0 + vx1 - tw) * 0.5f, y + 11 * s + (24 * s - vpx) * 0.5f, vc, r.value.c_str());
            }
        } else {
            text(d, vpx, vx1 - tw, y + 11 * s + (24 * s - vpx) * 0.5f, vc, r.value.c_str());
        }
    }
    dl->PopClipRect();
    // scroll bar
    if ((int)rows.size() > visible) {
        const float x = lb.x - 14 * s, h = ly1 - ly0;
        dl->AddRectFilled(ImVec2(x, ly0), ImVec2(x + 5 * s, ly1), IM_COL32(40, 36, 32, 255), 2 * s);
        const float t0 = (float)scroll / rows.size(), t1 = (float)(scroll + visible) / rows.size();
        dl->AddRectFilled(ImVec2(x, ly0 + h * t0), ImVec2(x + 5 * s, ly0 + h * t1), IM_COL32(180, 170, 160, 255), 2 * s);
    }

    // Center & Right columns: Screen A 3-Column PLAY Hub vs standard 2-Column Details
    if (play_3col) {
        game_frontend::Snapshot snap;
        if (m.network()) snap = m.network()->snapshot();
        using SK = game_frontend::SessionKind;
        const float cx0 = ca.x + pad, cx1 = cb.x - pad;
        float cy = ca.y + pad;

        // Center Column: Track/Car Disc Preview + Rules/Career Summary Card
        const bool is_career = (m.session_kind() == SK::Career);
        const std::string track_name = !m.pic_track().empty() ? m.pic_track() : m.offline_track();
        const frontend::Row *sel_row = (sel >= 0 && sel < (int)rows.size()) ? &rows[sel] : nullptr;
        const bool show_car_preview = !is_career && sel_row &&
            (sel_row->id == "seat0_car" || sel_row->id == "seat0_paint" || sel_row->id == "open_car_browser" ||
             sel_row->id == "opt:car" || sel_row->id == "opt:paint") && m.pic_car() != 0;
        const float pic_h = 190 * s;
        const ImVec2 p0(cx0, cy), p1(cx1, cy + pic_h);
        dl->AddRectFilled(p0, p1, IM_COL32(18, 16, 14, 240));
        if (show_car_preview) {
            const auto car_pic = ui_preview::car(m.pic_car());
            if (car_pic.ok()) dl->AddImage((ImTextureID)car_pic.tex, p0, p1, ImVec2(car_pic.u0, car_pic.v0), ImVec2(car_pic.u1, car_pic.v1));
        } else if (is_career && snap.career.active_car_num) {
            const auto car_pic = ui_preview::car(snap.career.active_car_num);
            if (car_pic.ok()) dl->AddImage((ImTextureID)car_pic.tex, p0, p1, ImVec2(car_pic.u0, car_pic.v0), ImVec2(car_pic.u1, car_pic.v1));
        } else if (!track_name.empty()) {
            const auto tr_pic = ui_preview::track(track_name);
            if (tr_pic.ok()) dl->AddImage((ImTextureID)tr_pic.tex, p0, p1, ImVec2(tr_pic.u0, tr_pic.v0), ImVec2(tr_pic.u1, tr_pic.v1));
        }
        // Hazard header strip + dark bottom scrim
        if (ui_art::sprite(Spr::Hazard).ok()) {
            image(d, Spr::Hazard, p0, ImVec2(p1.x, p0.y + 18 * s), IM_COL32(255, 255, 255, 190));
        }
        dl->AddRectFilledMultiColor(ImVec2(p0.x, p1.y - 54 * s), p1, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
                                    IM_COL32(0, 0, 0, 220), IM_COL32(0, 0, 0, 220));
        dl->AddRect(p0, p1, IM_COL32(120, 106, 92, 255), 0, 0, 2 * s);
        const std::string hero_title = show_car_preview
            ? upper(m.session_kind() == SK::Online && sel_row && !sel_row->value.empty() ? sel_row->value : m.split_seats()[0].car_model)
            : is_career ? "CAREER FREE ROAM"
            : (!snap.custom_map_title.empty() && m.session_kind() == SK::Online) ? upper(snap.custom_map_title)
            : upper(track_name.empty() ? "ANOKA" : track_name);
        text(d, fit(d, 26 * s, cx1 - cx0 - 16 * s, hero_title.c_str()), p0.x + 10 * s, p1.y - 34 * s, kOrange, hero_title.c_str());
        if (m.session_kind() == SK::Online && snap.map_downloading) {
            const float bw = (p1.x - p0.x) - 20 * s;
            const float frac = std::clamp(snap.map_download_pct / 100.0f, 0.0f, 1.0f);
            dl->AddRectFilled(ImVec2(p0.x + 10 * s, p1.y - 9 * s), ImVec2(p0.x + 10 * s + bw, p1.y - 4 * s), IM_COL32(36, 32, 28, 230), 2 * s);
            dl->AddRectFilled(ImVec2(p0.x + 10 * s, p1.y - 9 * s), ImVec2(p0.x + 10 * s + bw * frac, p1.y - 4 * s), kOrange, 2 * s);
        }
        cy = p1.y + 14 * s;

        // Rules Summary Card or Career Snapshot Card
        const ImVec2 card0(cx0, cy), card1(cx1, cb.y - pad);
        dl->AddRectFilled(card0, card1, IM_COL32(14, 12, 11, 200), 4 * s);
        dl->AddRect(card0, card1, IM_COL32(80, 70, 62, 255), 4 * s, 0, 1.5f * s);
        float sy = card0.y + 12 * s;
        if (is_career) {
            text(d, 22 * s, card0.x + 12 * s, sy, kOrange, "CAREER SNAPSHOT");
            sy += 30 * s;
            auto ckv = [&](const char *k2, const std::string &v2, ImU32 col) {
                text(d, 20 * s, card0.x + 12 * s, sy, kGrey, k2);
                text(d, 20 * s, card0.x + 150 * s, sy, col, v2.c_str());
                sy += 26 * s;
            };
            ckv("RANK", snap.career.loaded ? ("#" + std::to_string(snap.career.rank)) : "READY TO LOAD", kWhite);
            ckv("CASH", snap.career.loaded ? ("$" + std::to_string(snap.career.money)) : "-", kGreen);
            ckv("GARAGE", snap.career.loaded ? (std::to_string(snap.career.garage_count) + " / " + std::to_string(snap.career.garage_cap) + " CARS") : "-", kWhite);
            ckv("EVES WON", snap.career.loaded ? (std::to_string(snap.career.events_done) + " / " + std::to_string(snap.career.events_total)) : "-", kOrange);
        } else {
            text(d, 22 * s, card0.x + 12 * s, sy, kOrange, "RULES SUMMARY");
            sy += 28 * s;
            const auto &draft = (m.session_kind() == SK::Online && !snap.mutator_draft.name.empty())
                              ? snap.mutator_draft : m.mutator_draft();
            text(d, 20 * s, card0.x + 12 * s, sy, kWhite, upper(draft.name.empty() ? "STANDARD (VANILLA)" : draft.name).c_str());
            sy += 26 * s;
            text(d, 18 * s, card0.x + 12 * s, sy, draft.uses_weapons ? kGreen : kRed,
                 draft.uses_weapons ? "WEAPONS: ALLOWED" : "WEAPONS: OFF (MODE ENFORCED)");
            sy += 24 * s;
            char phys[96];
            snprintf(phys, sizeof phys, "GRAV %.2fX  |  GRIP %.2fX  |  SPD %.2fX",
                     draft.gravity_scale, draft.grip_scale, draft.top_speed_scale);
            text(d, 17 * s, card0.x + 12 * s, sy, kGrey, phys);
            sy += 24 * s;
            for (const auto &line : draft.summary_lines) {
                if (sy + 20 * s > card1.y - 10 * s) break;
                sy += paragraph(d, 17 * s, card0.x + 12 * s, sy, (card1.x - card0.x) - 24 * s, IM_COL32(210, 205, 196, 255), line) + 4 * s;
            }
        }

        // Right Column: 16-Slot Roster / 4-Seat Couch Strip / Career Destinations
        const float rx0 = ra.x + pad, rx1 = rb.x - pad;
        float ry = ra.y + pad;
        if (is_career) {
            text(d, 22 * s, rx0, ry, kOrange, "QUICK DESTINATIONS");
            ry += 32 * s;
            static const char *kDest[8] = {
                "01  YOUR TRAILER (HOME)", "02  TJ'S DINER (MALT SHOP)", "03  CHOOSE NEXT EVE",
                "04  SWEENY'S SCRAPYARD (DEALER)", "05  GARAGE & UPGRADES",
                "06  TIME CHALLENGE GATE 1", "07  TIME CHALLENGE GATE 2", "08  TROY'S PLACE"
            };
            for (int i = 0; i < 8; ++i) {
                const bool active = (i == m.career_warp_index());
                if (active) dl->AddRectFilled(ImVec2(rx0 - 4 * s, ry - 2 * s), ImVec2(rx1 + 4 * s, ry + 26 * s), kOrangeDim, 3 * s);
                text(d, fit(d, 19 * s, rx1 - rx0, kDest[i]), rx0, ry + 2 * s, active ? kOrange : kWhite, kDest[i]);
                ry += 32 * s;
            }
        } else if (m.session_kind() == SK::SplitScreen) {
            char hdr[64];
            snprintf(hdr, sizeof hdr, "4-SEAT COUCH STRIP (%d / 6 CHASSIS)", m.distinct_chassis_count());
            text(d, fit(d, 20 * s, rx1 - rx0, hdr), rx0, ry, kOrange, hdr);
            ry += 32 * s;
            const auto &seats = m.split_seats();
            for (int i = 0; i < 4; ++i) {
                const bool act = (i < m.split_players());
                const ImVec2 sa(rx0, ry), sb(rx1, ry + 78 * s);
                dl->AddRectFilled(sa, sb, act ? IM_COL32(28, 24, 20, 220) : IM_COL32(16, 14, 13, 140), 4 * s);
                dl->AddRect(sa, sb, act ? kOrange : IM_COL32(70, 64, 58, 180), 4 * s, 0, 1.5f * s);
                char ptitle[64];
                const std::string dev = m.host().device[i].empty() ? "CONNECT PAD" : upper(m.host().device[i]);
                snprintf(ptitle, sizeof ptitle, "SEAT %d  •  %s", i + 1, dev.c_str());
                text(d, fit(d, 19 * s, rx1 - rx0 - 16 * s, ptitle), sa.x + 10 * s, sa.y + 8 * s, act ? kOrange : kGrey, ptitle);
                const std::string car_line = act ? (upper(seats[i].car_model) + "  (PAINT " + std::to_string(seats[i].paint + 1) + ")") : "INACTIVE SEAT";
                text(d, fit(d, 20 * s, rx1 - rx0 - 16 * s, car_line.c_str()), sa.x + 10 * s, sa.y + 38 * s, act ? kWhite : kDisabled, car_line.c_str());
                ry += 88 * s;
            }
        } else {
            // Fixed 16-slot Roster (all 16 slots visible without paging at 720p/1080p)
            const bool is_online = (m.session_kind() == SK::Online);
            const int cap = is_online ? std::clamp(snap.lobby_capacity, 2, 16) : 16;
            const int humans = is_online ? std::max(1, (int)snap.players.size()) : 1;
            const int ai_n = is_online
                ? (snap.lobby_ai_count >= 16 ? std::max(0, cap - humans) : std::min(std::max(0, cap - humans), std::max(0, snap.lobby_ai_count)))
                : (m.offline_ai_bots() < 0 ? (16 - humans) : std::min(16 - humans, m.offline_ai_bots()));
            char rhdr[64];
            snprintf(rhdr, sizeof rhdr, "ROSTER    %d / %d SLOTS", std::min(cap, humans + ai_n), cap);
            text(d, 21 * s, rx0, ry, kOrange, rhdr);
            ry += 28 * s;
            const float slot_h = std::max(18.0f * s, (rb.y - pad - ry) / 16.0f);
            for (int i = 0; i < 16; ++i) {
                std::string tag, name;
                ImU32 col = kDisabled;
                if (i < (int)snap.roster_16.size() && snap.roster_16[i].occupied) {
                    tag = snap.roster_16[i].is_host ? "[HOST]"
                        : !snap.roster_16[i].map_ready ? "[SYNC]"
                        : snap.roster_16[i].ready ? "[RDY]" : "[WAIT]";
                    name = upper(snap.roster_16[i].name);
                    col = !snap.roster_16[i].map_ready ? kOrange
                        : (snap.roster_16[i].ready || snap.roster_16[i].is_host ? kGreen : kWhite);
                } else if (m.session_kind() == SK::Solo && i == 0) {
                    tag = "[P1]";
                    name = upper(m.cfg().nickname.empty() ? "PLAYER 1" : m.cfg().nickname) + "  " + upper(m.split_seats()[0].car_model);
                    col = kOrange;
                } else if (i < humans + ai_n) {
                    tag = "[AI]";
                    name = "BOT DRIVER #" + std::to_string(i + 1);
                    col = kGrey;
                } else {
                    tag = "[OPEN]";
                    name = "-";
                    col = kDisabled;
                }
                char line[96];
                snprintf(line, sizeof line, "%02d %-6s %s", i + 1, tag.c_str(), name.c_str());
                text(d, fit(d, 16.5f * s, rx1 - rx0, line), rx0, ry + i * slot_h, col, line);
            }
        }
    } else {
        const float x0 = ra.x + pad, x1 = rb.x - pad;
        float y = ra.y + pad;
        const frontend::Row *r = (sel >= 0 && sel < (int)rows.size()) ? &rows[sel] : nullptr;
        if (err) {
            text(d, 28 * s, x0, y, kRed, "WHAT HAPPENED");
            y += 40 * s;
            y += paragraph(d, 23 * s, x0, y, x1 - x0, kWhite, m.error_message()) + 20 * s;
            if (m.browsing()) {
                text(d, 20 * s, x0, y, kGrey, "LOOKING IN:");
                y += 26 * s;
                y += paragraph(d, 20 * s, x0, y, x1 - x0, kWhite, m.browse_dir()) + 16 * s;
            }
            if (r && !r->help.empty()) paragraph(d, 21 * s, x0, y, x1 - x0, kGrey, r->help);
        } else if (m.tab() == frontend::Tab::Mods && r && r->id.compare(0, 3, "mod") == 0) {
            const frontend::ModEntry &mod = m.mods()[std::stoi(r->id.substr(3))];
            text(d, fit(d, 32 * s, x1 - x0, upper(mod.name).c_str()), x0, y, kOrange, upper(mod.name).c_str());
            y += 44 * s;
            auto kv = [&](const char *k2, const std::string &v, ImU32 col) {
                text(d, 21 * s, x0, y, kGrey, k2);
                text(d, 21 * s, x0 + 130 * s, y, col, upper(v.empty() ? "-" : v).c_str());
                y += 30 * s;
            };
            kv("VERSION", mod.version, kWhite);
            kv("AUTHOR", mod.author, kWhite);
            kv("CHANGES", mod.kind, kWhite);
            kv("STATE", mod.builtin ? "always on (built in)" : mod.enabled ? "enabled (loads with the mod loader)" : "disabled",
               mod.enabled ? kGreen : kGrey);
            if (!mod.builtin) {
                paragraph(d, 19 * s, x0, y, x1 - x0, kGrey,
                          "Listed from the mods folder. Loading mods is a later step: toggles and order are kept for it.");
                y += 50 * s;
            }
            if (!mod.conflict.empty()) {
                text(d, 21 * s, x0, y, kRed, "PROBLEM");
                y += 28 * s;
                y += paragraph(d, 21 * s, x0, y, x1 - x0, kRed, mod.conflict) + 8 * s;
            }
            y += 8 * s;
            if (m.details_open() || true) paragraph(d, 21 * s, x0, y, x1 - x0, IM_COL32(210, 205, 196, 255), mod.description);
        } else if (is_rules) {
            // Right-side Live Rules Summary & Conflict Inspector
            game_frontend::Snapshot rsnap;
            if (m.network()) rsnap = m.network()->snapshot();
            using P = game_frontend::Snapshot::Phase;
            const bool guest_ro = (rsnap.phase != P::Offline && rsnap.phase != P::Disconnected && rsnap.phase != P::Error && !rsnap.is_host);
            const auto &draft = (guest_ro && !rsnap.mutator_draft.name.empty()) ? rsnap.mutator_draft : m.mutator_draft();
            text(d, 26 * s, x0, y, kOrange, guest_ro ? "HOST RULES INSPECTOR (READ-ONLY)" : "LIVE RULES & CONFLICT INSPECTOR");
            y += 34 * s;
            text(d, 22 * s, x0, y, kWhite, upper(draft.name.empty() ? "STANDARD (VANILLA)" : draft.name).c_str());
            y += 28 * s;
            if (!draft.uses_weapons) {
                const ImVec2 b0(x0, y), b1(x1, y + 30 * s);
                dl->AddRectFilled(b0, b1, IM_COL32(130, 32, 24, 210), 4 * s);
                dl->AddRect(b0, b1, kRed, 4 * s, 0, 1.5f * s);
                text(d, 18 * s, b0.x + 10 * s, b0.y + 6 * s, kWhite, "[NO WEAPONS MODE]  WEAPONS LOCKED OFF BY MODE RULES");
                y += 38 * s;
            } else {
                text(d, 18 * s, x0, y, kGreen, "[WEAPONS ALLOWED]  CHICKENS & FEATS ACTIVE");
                y += 26 * s;
            }
            auto kvp = [&](const char *k2, const std::string &v2, ImU32 col) {
                text(d, 18 * s, x0, y, kGrey, k2);
                text(d, 18 * s, x0 + 175 * s, y, col, v2.c_str());
                y += 24 * s;
            };
            char b[48];
            snprintf(b, sizeof b, "%.2fX", draft.gravity_scale); kvp("GRAVITY", b, draft.gravity_scale != 1.0f ? kOrange : kWhite);
            snprintf(b, sizeof b, "%.2fX", draft.mass_scale); kvp("MASS", b, draft.mass_scale != 1.0f ? kOrange : kWhite);
            snprintf(b, sizeof b, "%.2fX", draft.grip_scale); kvp("TIRE GRIP", b, draft.grip_scale != 1.0f ? kOrange : kWhite);
            snprintf(b, sizeof b, "%.2fX", draft.torque_scale); kvp("TORQUE", b, draft.torque_scale != 1.0f ? kOrange : kWhite);
            snprintf(b, sizeof b, "%.2fX", draft.top_speed_scale); kvp("TOP SPEED", b, draft.top_speed_scale != 1.0f ? kOrange : kWhite);
            snprintf(b, sizeof b, "%.2fX", draft.damage_taken_scale); kvp("DAMAGE TAKEN", b, draft.damage_taken_scale != 1.0f ? kOrange : kWhite);
            if (r && !r->help.empty()) {
                y += 10 * s;
                dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), IM_COL32(232, 134, 12, 90), 1.5f * s);
                y += 10 * s;
                paragraph(d, 19 * s, x0, y, x1 - x0, IM_COL32(214, 208, 200, 255), r->help);
            }
        } else if (r) {
            if (m.tab() == frontend::Tab::Play) {
                for (const auto &row : rows) if (row.id == "gcode") {
                    const std::string line = row.label + "   " + row.value;
                    text(d, fit(d,22*s,x1-x0,line.c_str()), x0,y,kOrange,line.c_str());
                    y += 30*s;
                }
                for (const auto &row : rows) if (row.id == "error")
                    y += paragraph(d,20*s,x0,y,x1-x0,kRed,row.value)+10*s;
                std::vector<const frontend::Row*> players;
                for (const auto &row : rows) if(row.id == "player") players.push_back(&row);
                const int columns=players.size()>6 ? 2 : 1;
                const float cell=(x1-x0)/columns, line_h=(columns==2 ? 23 : 28)*s;
                for(size_t i=0;i<players.size();++i) {
                    const auto &row=*players[i];
                    const std::string line=row.label+"   "+row.value;
                    const float x=x0+(i%columns)*cell;
                    text(d,fit(d,(columns==2 ? 18 : 22)*s,cell-8*s,line.c_str()),
                         x,y+(i/columns)*line_h,row.good?kGreen:kWhite,line.c_str());
                }
                y += ((players.size()+columns-1)/columns)*line_h+8*s;
            }
            dl->PushClipRect(ImVec2(x0,y),ImVec2(x1,rb.y-pad),true);
            text(d, fit(d, 30 * s, x1 - x0, r->label.c_str()), x0, y, kOrange, r->label.c_str());
            y += 42 * s;
            if (!r->note.empty()) {
                const std::string n = "APPLIES: " + r->note;
                const ImU32 col = r->note == "LIVE" ? kGreen : IM_COL32(240, 190, 90, 255);
                text(d, 20 * s, x0, y, col, n.c_str());
                y += 32 * s;
            }
            y += paragraph(d, 22 * s, x0, y, x1 - x0, IM_COL32(214, 208, 200, 255), r->help) + 18 * s;
            dl->PopClipRect();
        }
        if (!m.status().empty()) {
            const float py = rb.y - pad - 52 * s;
            dl->AddLine(ImVec2(x0, py - 8 * s), ImVec2(x1, py - 8 * s), IM_COL32(232, 134, 12, 90), 1.5f * s);
            paragraph(d, 19 * s, x0, py, x1 - x0, IM_COL32(240, 190, 90, 255), m.status());
        }
    }
    } // end !is_browser

    if (!c.busy.empty()) {
        dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), IM_COL32(0, 0, 0, 120));
        const float px = 40 * s;
        text(d, px, (W - text_size(d, px, c.busy.c_str()).x) * 0.5f, H * 0.45f, kOrange, c.busy.c_str());
    }

    // bottom prompt bar
    {
        const ImVec2 a(bx + 70 * k, H - 74 * s), b(bx + 1210 * k, H - 20 * s);
        dl->AddRectFilled(a, b, IM_COL32(14, 12, 11, 240), 10 * s);
        dl->AddRect(a, b, IM_COL32(110, 98, 86, 255), 10 * s, 0, 2.5f * s);
        dl->AddRect(ImVec2(a.x + 4 * s, a.y + 4 * s), ImVec2(b.x - 4 * s, b.y - 4 * s), IM_COL32(60, 52, 44, 255), 8 * s, 0, 1.5f * s);
        struct P { G g; const char *label; };
        std::vector<P> ps = {{G::Navigate, "NAVIGATE"}, {G::Accept, "SELECT / TOGGLE"}, {G::Back, err || online_modal ? "BACK" : "BACK / CLOSE"}};
        if (m.rebinding() >= 0) ps = {{G::Back, "CANCEL (ESC)"}};
        else if (m.editing_text()) ps = {{G::Accept, "CONFIRM"}, {G::Back, "CANCEL"}};
        else if (!err && !online_modal) {
            if (m.tab() == frontend::Tab::Play && !m.quick_rail_open()) {
                bool guest_ro = false;
                if (m.network()) {
                    const auto psnap = m.network()->snapshot();
                    using Ph = game_frontend::Snapshot::Phase;
                    guest_ro = (psnap.phase != Ph::Offline && psnap.phase != Ph::Disconnected && psnap.phase != Ph::Error && !psnap.is_host);
                }
                ps.push_back({G::Details, guest_ro ? "VIEW RULES" : "CUSTOMIZE RULES"});
            }
            if (m.tab() == frontend::Tab::Mods) {
                ps.push_back({G::Details, "MOD DETAILS"});
                ps.push_back({G::Move, "LOAD ORDER"});
            }
            if (m.tab() != frontend::Tab::Play) ps.push_back({G::Reset, "RESET DEFAULTS"});
        }
        const float gh = 32 * s, px = 24 * s;
        float total = 0;
        for (const P &p : ps) total += gh * 1.6f + text_size(d, px, p.label).x + 30 * s;
        float x = (a.x + b.x - total) * 0.5f;
        const float y = (a.y + b.y - gh) * 0.5f;
        for (const P &p : ps) {
            x += glyph(d, c.glyphs, p.g, x, y, gh) + 8 * s;
            text(d, px, x, y + (gh - px) * 0.5f + 1 * s, kWhite, p.label);
            x += text_size(d, px, p.label).x + 30 * s;
        }
    }
}

void draw_fps(ImDrawList *dl, const Ctx &c, const char *t) {
    if (!t || !t[0]) return;
    D d{dl, c.font ? c.font : ImGui::GetFont(), c.h / 720.0f};
    const float px = 22 * d.s;
    ImVec2 sz = text_size(d, px, t);
    const float pad = 8 * d.s;
    ImVec2 p(c.w - sz.x - pad * 3, pad);
    dl->AddRectFilled(ImVec2(p.x - pad, p.y - pad * 0.5f), ImVec2(p.x + sz.x + pad, p.y + sz.y + pad * 0.5f), IM_COL32(0, 0, 0, 160), pad);
    dl->AddText(d.font, px, p, IM_COL32(255, 255, 255, 235), t);
}

}  // namespace frontend_draw
