// SPDX-License-Identifier: GPL-2.0-or-later
// The look of Dead Nation's own menus (Main Menu, Options), drawn with Dear ImGui: shared by the
// in-game overlay's pages (deadnation_overlay.cpp) and settings.exe (settings/main.cpp), so both
// show the same Display Settings page. A near-black backdrop with a dark red glow down the left,
// the page title in heavy white capitals at the top left, the list as dark grey bars in a brushed
// silver frame, the focused item on a blood red bar, white condensed type with a dark shadow, and
// button hints at the bottom right as "MOVE (glyph)". Roboto Condensed (Light, Bold) stands in for
// the game's own condensed type.
#pragma once

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <string>
#include <string_view>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h" // ImRect

// Embeds a font file (third_party/fonts) as symbol[] .. symbol_end[]; used once per program.
#ifdef _WIN32
#define DEADNATION_EMBED(symbol, path)                                                             \
    asm(".section .rdata,\"dr\"\n"                                                                 \
        ".balign 16\n"                                                                             \
        ".global " #symbol "\n" #symbol ":\n"                                                      \
        ".incbin \"" path "\"\n"                                                                   \
        ".global " #symbol "_end\n" #symbol "_end:\n"                                              \
        ".text\n");
#else
#define DEADNATION_EMBED(symbol, path)                                                             \
    asm(".section .rodata\n"                                                                       \
        ".balign 16\n"                                                                             \
        ".hidden " #symbol "\n.global " #symbol "\n" #symbol ":\n"                                 \
        ".incbin \"" path "\"\n"                                                                   \
        ".hidden " #symbol "_end\n.global " #symbol "_end\n" #symbol "_end:\n"                     \
        ".previous\n");
#endif

namespace DeadNationPage {

constexpr ImU32 Rgb(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }

// The colours of the game's menus. The overlay draws to an sRGB image (Linear); settings.exe to a
// plain window (Srgb): the same colours as seen.
struct Palette {
    ImU32 back_red, back_edge;     // backdrop: dark red at the left, black at the right
    ImU32 panel;                   // inside a frame
    ImU32 frame_light, frame_dark; // the brushed silver frame, top and bottom
    ImU32 row_top, row_low, row_edge;
    ImU32 focus_top, focus_mid, focus_low, focus_edge;
    ImU32 text, text_dim, text_off; // text_off: a setting that does not apply in the current mode
    ImU32 accent, help, strip, shadow;
};
inline constexpr Palette Linear = {
    Rgb(0x10, 0x00, 0x00, 0xf4), Rgb(0x00, 0x00, 0x00, 0xfa), Rgb(0x01, 0x01, 0x01, 0xf0),
    Rgb(0xb4, 0xb4, 0xb4),       Rgb(0x40, 0x40, 0x40),
    Rgb(0x0e, 0x0e, 0x0e, 0xf0), Rgb(0x02, 0x02, 0x02, 0xf0), Rgb(0x1c, 0x1c, 0x1c),
    Rgb(0x50, 0x00, 0x00),       Rgb(0xb0, 0x02, 0x02),       Rgb(0x28, 0x00, 0x00), Rgb(0xff, 0x30, 0x30),
    Rgb(0xff, 0xff, 0xff),       Rgb(0xa8, 0xa8, 0xa8),       Rgb(0x14, 0x14, 0x14),
    Rgb(0xd0, 0x0a, 0x0a),       Rgb(0x8a, 0x8a, 0x8a),       Rgb(0x00, 0x00, 0x00, 0xc0), Rgb(0x00, 0x00, 0x00, 0xd0),
};
inline constexpr Palette Srgb = {
    Rgb(0x46, 0x00, 0x00),       Rgb(0x00, 0x00, 0x00),       Rgb(0x0d, 0x0d, 0x0d),
    Rgb(0xdc, 0xdc, 0xdc),       Rgb(0x8a, 0x8a, 0x8a),
    Rgb(0x3c, 0x3c, 0x3c),       Rgb(0x18, 0x18, 0x18),       Rgb(0x5a, 0x5a, 0x5a),
    Rgb(0x9a, 0x00, 0x00),       Rgb(0xd8, 0x10, 0x10),       Rgb(0x6e, 0x00, 0x00), Rgb(0xff, 0x96, 0x96),
    Rgb(0xff, 0xff, 0xff),       Rgb(0xd6, 0xd6, 0xd6),       Rgb(0x4f, 0x4f, 0x4f),
    Rgb(0xe8, 0x38, 0x38),       Rgb(0xc4, 0xc4, 0xc4),       Rgb(0x00, 0x00, 0x00, 0xc0), Rgb(0x00, 0x00, 0x00, 0xd0),
};

// Light for titles and hints, Bold for labels, values and help (as the game's menus).
struct Fonts {
    ImFont* light;
    ImFont* bold;
};

// Text with the dark shadow the game's menus give white type.
inline void Text(ImDrawList* dl, const Palette& ink, ImFont* font, float size, ImVec2 at, const char* text, ImU32 color,
                 bool shadow = true) {
    if (shadow) {
        const float d = std::max(1.0f, size * 0.05f);
        dl->AddText(font, size, ImVec2(at.x + d, at.y + d), ink.shadow, text);
    }
    dl->AddText(font, size, at, color, text);
}

inline std::string Upper(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = char(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

// An arrow ("<" for dir -1, ">" for 1) as one stroke, centred on `at`.
inline void Chevron(ImDrawList* dl, ImVec2 at, float size, int dir, ImU32 color, float thickness) {
    const float w = size * 0.28f * float(dir), h = size * 0.5f;
    const ImVec2 points[3] = {ImVec2(at.x - w, at.y - h), ImVec2(at.x + w, at.y), ImVec2(at.x - w, at.y + h)};
    dl->AddPolyline(points, 3, color, ImDrawFlags_None, thickness);
}

// The screen behind a page: near black with a dark red glow down the left.
inline void Backdrop(ImDrawList* dl, const Palette& ink, ImVec2 o, ImVec2 size) {
    dl->AddRectFilledMultiColor(o, ImVec2(o.x + size.x, o.y + size.y), ink.back_red, ink.back_edge, ink.back_edge,
                                ink.back_red);
}

// The page title: heavy white capitals, left-aligned over the panel below it.
inline void Title(ImDrawList* dl, const Palette& ink, const Fonts& fonts, ImVec2 at, float px, const char* title) {
    Text(dl, ink, fonts.bold, 84.0f * px, at, title, ink.text);
}

// A panel as the game frames its lists: a brushed silver frame, light at the top and darker below,
// around a near black inside.
inline void Panel(ImDrawList* dl, const Palette& ink, ImVec2 a, ImVec2 b, float px) {
    const float frame = std::max(2.0f, 8.0f * px), line = std::max(1.0f, 1.5f * px);
    dl->AddRectFilledMultiColor(a, b, ink.frame_light, ink.frame_light, ink.frame_dark, ink.frame_dark);
    const ImVec2 ia(a.x + frame, a.y + frame), ib(b.x - frame, b.y - frame);
    dl->AddRectFilled(ia, ib, ink.panel);
    dl->AddRect(ImVec2(ia.x - line, ia.y - line), ImVec2(ib.x + line, ib.y + line), ink.shadow, 0.0f, 0, line);
}

// An item's bar: dark grey, lighter at its top, with a thin light edge above.
inline void RowBar(ImDrawList* dl, const Palette& ink, ImVec2 a, ImVec2 b, float px) {
    dl->AddRectFilledMultiColor(a, b, ink.row_top, ink.row_top, ink.row_low, ink.row_low);
    dl->AddLine(a, ImVec2(b.x, a.y), ink.row_edge, std::max(1.0f, 1.5f * px));
}

// The focused item's bar: blood red, brightest a little above its middle, with a light red edge on top.
inline void FocusBar(ImDrawList* dl, const Palette& ink, ImVec2 a, ImVec2 b, float px) {
    const float mid = a.y + (b.y - a.y) * 0.4f;
    dl->AddRectFilledMultiColor(a, ImVec2(b.x, mid), ink.focus_top, ink.focus_top, ink.focus_mid, ink.focus_mid);
    dl->AddRectFilledMultiColor(ImVec2(a.x, mid), b, ink.focus_mid, ink.focus_mid, ink.focus_low, ink.focus_low);
    dl->AddLine(a, ImVec2(b.x, a.y), ink.focus_edge, std::max(1.0f, 1.5f * px));
}

// A pad glyph as the game's button hints draw them: a white ring with the button's mark inside.
enum class Glyph { Cross, Circle, UpDown, Triangle };
inline void PadGlyph(ImDrawList* dl, const Palette& ink, ImVec2 c, float r, Glyph glyph, float px) {
    const float line = std::max(1.0f, 2.2f * px);
    dl->AddCircle(c, r, ink.text, 32, line);
    const float m = r * 0.45f;
    switch (glyph) {
    case Glyph::Cross:
        dl->AddLine(ImVec2(c.x - m, c.y - m), ImVec2(c.x + m, c.y + m), ink.text, line);
        dl->AddLine(ImVec2(c.x - m, c.y + m), ImVec2(c.x + m, c.y - m), ink.text, line);
        break;
    case Glyph::Circle: dl->AddCircle(c, r * 0.5f, ink.text, 24, line); break;
    case Glyph::Triangle:
        dl->AddTriangle(ImVec2(c.x, c.y - m * 1.1f), ImVec2(c.x - m * 1.05f, c.y + m * 0.75f),
                        ImVec2(c.x + m * 1.05f, c.y + m * 0.75f), ink.text, line);
        break;
    case Glyph::UpDown:
        dl->AddTriangleFilled(ImVec2(c.x, c.y - m * 1.2f), ImVec2(c.x - m * 0.8f, c.y - m * 0.15f),
                              ImVec2(c.x + m * 0.8f, c.y - m * 0.15f), ink.text);
        dl->AddTriangleFilled(ImVec2(c.x, c.y + m * 1.2f), ImVec2(c.x + m * 0.8f, c.y + m * 0.15f),
                              ImVec2(c.x - m * 0.8f, c.y + m * 0.15f), ink.text);
        break;
    }
}

// Where the hint strip sits on a page of `size` (its top, in the page's coordinates from o.y).
inline float HintStripTop(ImVec2 o, ImVec2 size, float px) { return o.y + size.y - 110.0f * px; }

// The button hints on a dark strip along the bottom, right-aligned as the game's ("MOVE (glyph)
// SELECT (glyph)"), and `keys` (the keyboard's keys for the same), dimmer, at the strip's left.
// `rects`, when given, receives each hint's area (for a mouse).
struct Hint {
    Glyph glyph;
    const char* label;
};
inline void Hints(ImDrawList* dl, const Palette& ink, const Fonts& fonts, ImVec2 o, ImVec2 size, float px,
                  const Hint* hints, int count, const char* keys, std::vector<ImRect>* rects = nullptr) {
    const float text = 36.0f * px, r = 16.0f * px, strip_h = 54.0f * px, space = 36.0f * px, after = 8.0f * px;
    const float strip_y = HintStripTop(o, size, px), y = strip_y + (strip_h - text) * 0.5f;
    dl->AddRectFilled(ImVec2(o.x + 80.0f * px, strip_y), ImVec2(o.x + size.x - 40.0f * px, strip_y + strip_h), ink.strip);
    std::vector<std::string> labels;
    float width = 0.0f;
    for (int i = 0; i < count; ++i) {
        labels.push_back(Upper(hints[i].label));
        width += fonts.light->CalcTextSizeA(text, FLT_MAX, 0.0f, labels[i].c_str()).x + after + r * 2.0f +
                 (i ? space : 0.0f);
    }
    float x = o.x + size.x - 100.0f * px - width;
    if (rects) rects->clear();
    for (int i = 0; i < count; ++i) {
        if (i) x += space;
        const float x0 = x;
        Text(dl, ink, fonts.light, text, ImVec2(x, y), labels[i].c_str(), ink.text);
        x += fonts.light->CalcTextSizeA(text, FLT_MAX, 0.0f, labels[i].c_str()).x + after;
        PadGlyph(dl, ink, ImVec2(x + r, y + text * 0.52f), r, hints[i].glyph, px);
        x += r * 2.0f;
        if (rects) rects->push_back(ImRect(ImVec2(x0, strip_y), ImVec2(x, strip_y + strip_h)));
    }
    if (keys && keys[0])
        Text(dl, ink, fonts.bold, text * 0.7f, ImVec2(o.x + 110.0f * px, y + text * 0.16f), keys, ink.help);
}

// One setting of a settings page.
struct Row {
    std::string label;
    std::string value;
    std::string help;
    bool changes = true; ///< Left/Right change it (arrows around the value)
    bool enabled = true; ///< applies in the current window mode; else greyed out and skipped
};

// Where a settings page put a row (for a mouse): its bar, and the span of x around its "<" arrow.
struct RowArea {
    ImRect bar;
    float back_x0 = 0.0f, back_x1 = 0.0f;
};

// A settings page: the backdrop and title, a framed list of `rows` (name on the left, value on the
// right, the focused one on the red bar with arrows around its value), a framed panel with the
// focused setting's help, and the button hints. Rows shrink from 64 lines (of 1080) to fit above
// the hints. `areas`, when given, receives each row's area.
inline void SettingsPage(ImDrawList* dl, const Palette& ink, const Fonts& fonts, ImVec2 o, ImVec2 size,
                         const char* title, const std::vector<Row>& rows, int focus, const Hint* hints, int hint_count,
                         const char* keys, std::vector<RowArea>* areas = nullptr,
                         std::vector<ImRect>* hint_rects = nullptr) {
    const float px = size.y / 1080.0f;
    Backdrop(dl, ink, o, size);
    const int count = int(rows.size());
    const float gap = 8.0f * px, pad = 18.0f * px, top_min = 190.0f * px;
    const float room = HintStripTop(o, size, px) - o.y - 30.0f * px - top_min;
    const float row_h = std::min(64.0f * px, (room - pad * 2.0f - gap * float(count - 1)) / float(std::max(count, 1)));
    const float list_w = std::min(size.x * 0.56f, 1080.0f * px), info_w = std::min(size.x * 0.28f, 540.0f * px);
    const float space = 40.0f * px, total_w = list_w + space + info_w;
    const float x0 = o.x + (size.x - total_w) * 0.5f, x1 = x0 + list_w;
    const float panel_h = pad * 2.0f + float(count) * row_h + float(count - 1) * gap;
    const float top = o.y + std::max(top_min, (size.y - panel_h) * 0.5f);
    Title(dl, ink, fonts, ImVec2(x0 + 4.0f * px, top - 100.0f * px), px, title);
    Panel(dl, ink, ImVec2(x0, top), ImVec2(x1, top + panel_h), px);

    const float scale = row_h / (64.0f * px);
    const float label = 38.0f * px * scale, value = 32.0f * px * scale;
    const float bx0 = x0 + pad, bx1 = x1 - pad;
    float y = top + pad;
    const Row* focused = nullptr;
    if (areas) areas->clear();
    for (int row = 0; row < count; ++row) {
        const Row& text = rows[row];
        const bool on = row == focus, off = !text.enabled;
        const float mid = y + row_h * 0.5f;
        if (on) FocusBar(dl, ink, ImVec2(bx0, y), ImVec2(bx1, y + row_h), px);
        else RowBar(dl, ink, ImVec2(bx0, y), ImVec2(bx1, y + row_h), px);
        const ImU32 color = on ? ink.text : off ? ink.text_off : ink.text;
        Text(dl, ink, fonts.bold, label, ImVec2(bx0 + 24.0f * px, mid - label * 0.56f), text.label.c_str(), color, !off);
        const float vw = fonts.bold->CalcTextSizeA(value, FLT_MAX, 0.0f, text.value.c_str()).x;
        const float value_x = bx1 - 56.0f * px - vw;
        Text(dl, ink, fonts.bold, value, ImVec2(value_x, mid - value * 0.56f), text.value.c_str(),
             on ? ink.text : off ? ink.text_off : ink.text_dim, !off);
        if (on && text.changes) {
            const float chevron = row_h * 0.34f, stroke = 3.0f * px;
            Chevron(dl, ImVec2(value_x - 24.0f * px, mid), chevron, -1, ink.text, stroke);
            Chevron(dl, ImVec2(bx1 - 30.0f * px, mid), chevron, 1, ink.text, stroke);
        }
        if (on) focused = &text;
        if (areas) areas->push_back({ImRect(ImVec2(bx0, y), ImVec2(bx1, y + row_h)), value_x - 48.0f * px, value_x - 4.0f * px});
        y += row_h + gap;
    }

    // The info panel: the focused setting's name, a red heading, then its help.
    const float ix0 = x1 + space, ix1 = ix0 + info_w, ipad = 32.0f * px;
    Panel(dl, ink, ImVec2(ix0, top), ImVec2(ix1, top + panel_h), px);
    if (focused) {
        const float head = 42.0f * px, sub = 26.0f * px, body = 27.0f * px;
        float iy = top + ipad;
        Text(dl, ink, fonts.bold, head, ImVec2(ix0 + ipad, iy), focused->label.c_str(), ink.text);
        iy += head + 12.0f * px;
        Text(dl, ink, fonts.bold, sub, ImVec2(ix0 + ipad, iy), "SETTING INFO", ink.accent);
        iy += sub + 16.0f * px;
        const float d = std::max(1.0f, body * 0.05f), wrap = info_w - ipad * 2.0f;
        dl->AddText(fonts.bold, body, ImVec2(ix0 + ipad + d, iy + d), ink.shadow, focused->help.c_str(), nullptr, wrap);
        dl->AddText(fonts.bold, body, ImVec2(ix0 + ipad, iy), ink.text_dim, focused->help.c_str(), nullptr, wrap);
    }
    Hints(dl, ink, fonts, o, size, px, hints, hint_count, keys, hint_rects);
}

} // namespace DeadNationPage
