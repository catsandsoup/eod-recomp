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
        if (style == Glyphs::Keyboard) return keycap("\xE2\x86\x91\xE2\x86\x93");  // arrows
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

    // panels' geometry
    const float top = 150 * s, bottom = H - 96 * s;
    const ImVec2 la(bx + 70 * k, top), lb(bx + 668 * k, bottom);
    const ImVec2 ra(bx + 690 * k, top), rb(bx + 1210 * k, bottom);
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
        if (err) {
            const std::string t = upper(m.error_title());
            float px = fit(d, 30 * s, b.x - a.x - 30 * s, t.c_str());
            text(d, px, (a.x + b.x - text_size(d, px, t.c_str()).x) * 0.5f, a.y + 7 * s, kRed, t.c_str());
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

    panel(d, la, lb);
    panel(d, ra, rb);

    // left: rows
    const auto &rows = m.rows();
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
            text(d, vpx, (vx0 + vx1 - tw) * 0.5f, y + 11 * s + (24 * s - vpx) * 0.5f, vc, r.value.c_str());
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

    // right: details of the selected row (mods: the mod's details; online: the lobby)
    {
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
        } else if (r) {
            text(d, fit(d, 30 * s, x1 - x0, r->label.c_str()), x0, y, kOrange, r->label.c_str());
            y += 42 * s;
            if (!r->note.empty()) {
                const std::string n = "APPLIES: " + r->note;
                const ImU32 col = r->note == "LIVE" ? kGreen : IM_COL32(240, 190, 90, 255);
                text(d, 20 * s, x0, y, col, n.c_str());
                y += 32 * s;
            }
            y += paragraph(d, 22 * s, x0, y, x1 - x0, IM_COL32(214, 208, 200, 255), r->help) + 18 * s;
            if (m.tab() == frontend::Tab::Online) {
                text(d, 22 * s, x0, y, kOrange, "NETWORK PLAY");
                y += 30 * s;
                paragraph(d, 20 * s, x0, y, x1 - x0, kGrey,
                          "Host or join a two-player session. Both machines then start the agreed Race or Battle "
                          "directly. The transport is not built yet: this build always shows NOT CONNECTED.");
            }
        }
        if (!m.status().empty()) {
            const float py = rb.y - pad - 52 * s;
            dl->AddLine(ImVec2(x0, py - 8 * s), ImVec2(x1, py - 8 * s), IM_COL32(232, 134, 12, 90), 1.5f * s);
            paragraph(d, 19 * s, x0, py, x1 - x0, IM_COL32(240, 190, 90, 255), m.status());
        }
    }

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
        std::vector<P> ps = {{G::Navigate, "NAVIGATE"}, {G::Accept, "SELECT / TOGGLE"}, {G::Back, err ? "BACK" : "BACK / CLOSE"}};
        if (m.rebinding() >= 0) ps = {{G::Back, "CANCEL (ESC)"}};
        else if (m.editing_text()) ps = {{G::Accept, "CONFIRM"}, {G::Back, "CANCEL"}};
        else if (!err) {
            if (m.tab() == frontend::Tab::Mods) {
                ps.push_back({G::Details, "MOD DETAILS"});
                ps.push_back({G::Move, "LOAD ORDER"});
            }
            ps.push_back({G::Reset, "RESET DEFAULTS"});
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
