// SPDX-License-Identifier: GPL-2.0-or-later
// The system UI the PS4 draws over a game (save list, text entry, trophy and screenshot notices) and
// the shader-cache notes, drawn with Dear ImGui by the presenter after the game frame.
#include "deadnation_overlay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>
#include "deadnation_graphics.h"
#include "deadnation_page.h"
#include "deadnation_settings.h"
#include "imgui.h"
#include "imgui_internal.h" // RegisterUserTexture (save pictures)
#include "imgui_impl_vulkan.h"
#include "../deadnation_gpu.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

// The overlay's fonts, embedded (third_party/fonts): DejaVu Sans (Bitstream Vera license) for the
// system dialogs, Roboto Condensed Light and Bold (SIL OFL 1.1) for the Display Settings and exit
// pages, a condensed grotesque like the game's own menu type.
DEADNATION_EMBED(deadnation_font_ttf, DEADNATION_FONT_PATH)
DEADNATION_EMBED(deadnation_font_light_ttf, DEADNATION_FONT_LIGHT_PATH)
DEADNATION_EMBED(deadnation_font_bold_ttf, DEADNATION_FONT_BOLD_PATH)
extern "C" const unsigned char deadnation_font_ttf[];
extern "C" const unsigned char deadnation_font_ttf_end[];
extern "C" const unsigned char deadnation_font_light_ttf[];
extern "C" const unsigned char deadnation_font_light_ttf_end[];
extern "C" const unsigned char deadnation_font_bold_ttf[];
extern "C" const unsigned char deadnation_font_bold_ttf_end[];

namespace DeadNationOverlay {

namespace {

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
bool initialized = false;
ImFont* page_light = nullptr; // Roboto Condensed Light: page titles, list items, values
ImFont* page_bold = nullptr;  // Roboto Condensed Bold: labels, help, button hints
float base_scale = 1.0f;

namespace Ink {
constexpr ImVec4 Rgb(int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); }
constexpr ImVec4 Bg = Rgb(0x10, 0x12, 0x16), Panel = Rgb(0x18, 0x1b, 0x21), Card = Rgb(0x1f, 0x23, 0x2b);
constexpr ImVec4 Line = Rgb(0x2e, 0x34, 0x3f), Text = Rgb(0xe8, 0xea, 0xee), Muted = Rgb(0x8c, 0x93, 0xa0);
constexpr ImVec4 Accent = Rgb(0x3d, 0x7c, 0xd9), AccentDeep = Rgb(0x1c, 0x33, 0x55);
} // namespace Ink

std::atomic<bool> loading{false};
std::atomic<u32> loading_done{0}, loading_total{0};
std::atomic<u32> background_done{0}, background_total{0};
std::atomic<u32> prefetch_done{0}, prefetch_total{0};
std::atomic<s64> prefetch_until{0}; // "ready" stays this long after the last program
// Shader translations: in progress, and started since the note last appeared.
std::atomic<u32> compile_pending{0}, compile_count{0}, compile_waits{0};
std::atomic<s64> compile_until{0};
// A short message in the lower right corner (trophy, screenshot), shown until notice_until.
std::mutex notice_mutex;
std::string notice_text;
std::atomic<s64> notice_until{0}; // steady_clock milliseconds
// The game's text dialog (SetTextEntry), guarded by imgui_mutex.
bool text_entry_active = false;
std::string text_entry_prompt, text_entry_text;
// The game's save list (sceSaveDataDialog): a choice made with the arrows/D-pad.
std::atomic<bool> choice_active{false};
std::string choice_title;
std::string choice_preview_label;
std::vector<std::string> choice_items;
int choice_index = 0;
std::atomic<int> choice_result{-1}; // -1 open, -2 cancelled, else the chosen index
// Save pictures: decoded by BeginChoice, made textures on the present thread (null = none).
// Closed lists' textures wait in choice_retired until the backend has destroyed them.
std::vector<DeadNationPng::Image> choice_pictures;
std::vector<ImTextureData*> choice_textures, choice_retired;
std::atomic<bool> choice_retiring{false};
// The start-up picture (SetSplash): decoded at start, made a texture on the present thread (both
// guarded by imgui_mutex), retired through choice_retired once faded out.
enum SplashState : int { SplashNone, SplashShown, SplashFading, SplashDone };
constexpr s64 kSplashFadeMs = 400, kSplashMaxMs = 60000;
DeadNationPng::Image splash_picture;
ImTextureData* splash_texture = nullptr;
std::atomic<int> splash_state{SplashNone}, splash_lit_frames{0};
std::atomic<s64> splash_since{0}, splash_fade_from{0};
std::chrono::steady_clock::time_point last_present{};
// The Display Settings panel (F10, or Select held for 2 s: src/runtime_pad.c). The row and
// dirty flag are guarded by imgui_mutex; the values live in DeadNationSettings.
std::atomic<bool> display_open{false};

int display_row = 0;
bool display_dirty = false;
// An aspect ratio and window size being chosen on the panel: the window takes them once the rows
// have rested for a second (or the panel closes), not at each step. -1 and {0, 0} when none.
int pending_aspect = -1;
std::pair<int, int> pending_resolution{0, 0};
std::chrono::steady_clock::time_point pending_since{};
constexpr auto kResolutionSettle = std::chrono::seconds(1);
// The exit prompt (F4, Select+Start, or Back on the title screen: src/runtime_pad.c); its focus (0 No, 1 Yes) is
// guarded by imgui_mutex.
std::atomic<bool> exit_open{false};
int exit_focus = 0;
// The cheat menu (L3+R3, or X+M on the keyboard): a row per DeadNationSettings::Cheat, toggled
// On/Off. The game waits at its flip while it is open (src/runtime_game.c), so it is frozen; the
// focused row is guarded by imgui_mutex, the values live in DeadNationSettings::Values::cheats.
std::atomic<bool> cheats_open{false};
int cheat_row = 0;
std::atomic<s64> cheats_close_at{0}; // scripted runs: when the menu closes by itself (0: never)
enum DisplayRow : int { RowAspect, RowMode, RowResolution, RowMonitor, RowVsync, RowFps, RowRender, RowSharpen, RowCount };

// The game's menu colours (deadnation_page.h), as the overlay's sRGB image takes them.
constexpr const DeadNationPage::Palette& PageInk = DeadNationPage::Linear;

s64 NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// The game's text dialog: what is typed, and how to finish (keyboard or controller).
void TextEntryBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.42f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(viewport->WorkSize.x * 0.32f, 0.0f),
                                        ImVec2(viewport->WorkSize.x * 0.8f, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::Begin("##text_entry", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextColored(Ink::Accent, "%s", text_entry_prompt.c_str());
    ImGui::Separator();
    ImGui::SetWindowFontScale(1.4f);
    ImGui::Text("%s_", text_entry_text.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    ImGui::TextDisabled("Keyboard: type, Backspace to erase, Enter = OK, Esc = cancel");
    ImGui::TextDisabled("Controller: Cross (A) = OK, Circle (B) = cancel");
    ImGui::End();
}

// One save (or one answer) of the game's save list as a card: its picture on the left when the list
// has pictures, then its lines: the first is the name, the rest in grey. `draw` false only
// measures. Returns the card's height.
float SaveCard(ImVec2 at, float width, const std::string& text, ImTextureData* picture, bool pictures, bool selected,
               bool draw) {
    const float px = base_scale;
    const float pad = 12.0f * px, gap = 16.0f * px;
    const ImVec2 picture_size(192.0f * px, 108.0f * px);
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize(), name_size = size * 1.15f;
    const float text_x = pad + (pictures ? picture_size.x + gap : 0.0f);
    const float wrap = std::max(width - text_x - pad, 40.0f * px);
    std::vector<std::string_view> lines;
    for (size_t start = 0; start <= text.size();) {
        const size_t end = std::min(text.find('\n', start), text.size());
        lines.emplace_back(text.data() + start, end - start);
        start = end + 1;
    }
    const auto line_gap = [&](size_t i) { return i == 0 && lines.size() > 1 ? 4.0f * px : 1.0f * px; };
    float text_h = 0.0f;
    for (size_t i = 0; i < lines.size(); ++i) {
        const char* begin = lines[i].data();
        text_h += font->CalcTextSizeA(i == 0 ? name_size : size, FLT_MAX, wrap, begin, begin + lines[i].size()).y +
                  line_gap(i);
    }
    const float height = std::max(pictures ? picture_size.y : 0.0f, text_h) + pad * 2.0f;
    if (!draw) {
        return height;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 max(at.x + width, at.y + height);
    const float rounding = 6.0f * px;
    dl->AddRectFilled(at, max, ImGui::GetColorU32(selected ? Ink::AccentDeep : Ink::Card), rounding);
    dl->AddRect(at, max, ImGui::GetColorU32(selected ? Ink::Accent : Ink::Line), rounding, 0,
                (selected ? 2.0f : 1.0f) * px);
    if (pictures) {
        const ImVec2 p0(at.x + pad, at.y + (height - picture_size.y) * 0.5f);
        const ImVec2 p1(p0.x + picture_size.x, p0.y + picture_size.y);
        if (picture) {
            dl->AddImage(picture->GetTexRef(), p0, p1);
        } else { // the new save: no picture yet
            dl->AddRectFilled(p0, p1, ImGui::GetColorU32(Ink::Panel), 4.0f * px);
        }
        dl->AddRect(p0, p1, ImGui::GetColorU32(Ink::Line), 4.0f * px, 0, 1.0f * px);
    }
    float y = at.y + (height - text_h) * 0.5f;
    for (size_t i = 0; i < lines.size(); ++i) {
        const float s = i == 0 ? name_size : size;
        const char* begin = lines[i].data();
        const char* end = begin + lines[i].size();
        dl->AddText(font, s, ImVec2(at.x + text_x, y), ImGui::GetColorU32(i == 0 ? Ink::Text : Ink::Muted), begin,
                    end, wrap);
        y += font->CalcTextSizeA(s, FLT_MAX, wrap, begin, end).y + line_gap(i);
    }
    return height;
}

// The game's save list (sceSaveDataDialog): the action as the heading, then a card per save.
// Asked to overwrite, the new save is shown first (its picture and the details of the one it
// replaces), then the answers.
void ChoiceBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float px = base_scale;
    const float width = std::min(760.0f * px, viewport->WorkSize.x * 0.92f);
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.97f);
    ImGui::Begin("##choice", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    const bool confirm = !choice_preview_label.empty();
    const size_t details = confirm ? choice_title.find("\n\n") : std::string::npos;
    ImGui::PushTextWrapPos(0.0f);
    ImGui::SetWindowFontScale(1.5f);
    ImGui::TextColored(Ink::Text, "%s", choice_title.substr(0, details).c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopTextWrapPos();
    ImGui::Separator();
    const float inner = ImGui::GetContentRegionAvail().x;
    const float card_gap = 8.0f * px;
    if (confirm) {
        std::string text = choice_preview_label;
        if (details != std::string::npos) text += "\n" + choice_title.substr(details + 2);
        ImTextureData* preview = choice_textures.size() > choice_items.size() ? choice_textures.back() : nullptr;
        const float h = SaveCard(ImGui::GetCursorScreenPos(), inner, text, preview, true, false, true);
        ImGui::Dummy(ImVec2(inner, h + card_gap));
    }
    const int count = int(choice_items.size());
    const size_t listed = std::min(choice_textures.size(), choice_items.size());
    const bool pictures = !confirm && std::any_of(choice_textures.begin(), choice_textures.begin() + listed,
                                                  [](ImTextureData* tex) { return tex != nullptr; });
    const auto picture_of = [&](int i) { return pictures && i < int(listed) ? choice_textures[i] : nullptr; };
    float total = 0.0f;
    for (int i = 0; i < count; ++i) {
        total += SaveCard(ImVec2(), inner, choice_items[i], picture_of(i), pictures, false, false) +
                 (i + 1 < count ? card_gap : 0.0f);
    }
    // A long list scrolls in its own area, between the heading and the keys.
    const float most = viewport->WorkSize.y * 0.86f - ImGui::GetCursorPosY() - ImGui::GetFrameHeightWithSpacing() * 2.0f;
    ImGui::BeginChild("##saves", ImVec2(inner, std::min(total, std::max(most, 120.0f * px))), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
    for (int i = 0; i < count; ++i) {
        const bool selected = i == choice_index;
        const float h = SaveCard(ImGui::GetCursorScreenPos(), inner, choice_items[i], picture_of(i), pictures,
                                 selected, true);
        ImGui::Dummy(ImVec2(inner, h));
        if (selected) {
            ImGui::SetScrollHereY(0.5f);
        }
        if (i + 1 < count) {
            ImGui::Dummy(ImVec2(inner, card_gap));
        }
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::Spacing();
    ImGui::TextDisabled("Up/Down or D-pad: choose   Enter or Cross (A): OK   Esc or Circle (B): cancel");
    ImGui::End();
}

template <typename T, std::size_t N>
int StepValue(const std::array<T, N>& values, int current, int delta) {
    const int i = DeadNationGraphics::IndexOf(values, current, 0);
    return values[(i + delta + int(N)) % int(N)];
}

// imgui_mutex held: the resolution the panel shows, the one being chosen or the window's.
std::pair<int, int> ShownResolution() {
    const auto& s = DeadNationSettings::Get();
    return pending_resolution.first ? pending_resolution
                                    : std::pair<int, int>{s.window_width.load(), s.window_height.load()};
}

// imgui_mutex held: the aspect ratio the panel shows, the one being chosen or the setting.
int ShownAspect() {
    return pending_aspect >= 0 ? pending_aspect : DeadNationSettings::Get().aspect_ratio.load();
}

// imgui_mutex held: hands the chosen aspect ratio and window size to the window (which follows
// aspect_ratio, window_width and window_height) once they have rested for kResolutionSettle, or at
// once with `now`.
void SettleResolution(bool now) {
    if ((pending_aspect < 0 && !pending_resolution.first) ||
        (!now && std::chrono::steady_clock::now() - pending_since < kResolutionSettle)) {
        return;
    }
    auto& s = DeadNationSettings::Get();
    if (pending_resolution.first) {
        s.window_width = pending_resolution.first;
        s.window_height = pending_resolution.second;
    }
    if (pending_aspect >= 0) s.aspect_ratio = pending_aspect;
    pending_aspect = -1;
    pending_resolution = {0, 0};
}

// imgui_mutex held: the size the fullscreen modes show for the panel's aspect ratio.
std::pair<int, int> ShownFullscreen() {
    const auto& s = DeadNationSettings::Get();
    if (pending_aspect < 0 && s.fullscreen_width > 0) return {s.fullscreen_width.load(), s.fullscreen_height.load()};
    return DeadNationSettings::PickFullscreen(s.display_mode == DeadNationGraphics::Fullscreen, ShownAspect(),
                                           {s.desktop_width, s.desktop_height}, s.modes)
        .picture;
}

// imgui_mutex held: the focused row's value one step forward (delta 1) or back (-1).
void ChangeDisplayRow(int row, int delta) {
    namespace G = DeadNationGraphics;
    auto& s = DeadNationSettings::Get();
    switch (row) {
    case RowMode: s.display_mode = (s.display_mode + delta + G::DisplayModeCount) % G::DisplayModeCount; break;
    case RowAspect: { // the window moves to the new shape's size nearest in height
        const int next = (ShownAspect() + delta + DeadNationSettings::AspectRatioCount) % DeadNationSettings::AspectRatioCount;
        pending_aspect = next;
        pending_resolution = DeadNationSettings::NearestHeight(s.resolutions[next], ShownResolution().second);
        pending_since = std::chrono::steady_clock::now();
        break;
    }
    case RowResolution: { // windowed only, listed largest first: right steps up to a larger size
        const auto& sizes = s.resolutions[ShownAspect()];
        if (sizes.empty() || s.display_mode != G::Windowed) return;
        const std::pair<int, int> now = ShownResolution();
        int at = 0;
        while (at + 1 < int(sizes.size()) && sizes[at] > now) ++at;
        at = std::clamp(at - delta, 0, int(sizes.size()) - 1);
        pending_resolution = sizes[at];
        pending_since = std::chrono::steady_clock::now();
        break;
    }
    case RowMonitor: { // the primary, then each listed display
        const int count = int(s.monitors.size()) + 1;
        int at = 0;
        for (int i = 0; i < int(s.monitors.size()); ++i) {
            if (s.monitors[i] == s.monitor) at = i + 1;
        }
        at = (at + delta + count) % count;
        s.monitor = at == 0 ? G::PrimaryMonitor : s.monitors[at - 1];
        break;
    }
    case RowVsync: { // On, Off, Off with tearing
        const int at = (s.vsync ? 0 : s.tearing ? 2 : 1) + delta;
        const int next = (at + 3) % 3;
        s.vsync = next == 0;
        s.tearing = next == 2;
        break;
    }
    case RowFps: s.frame_rate = StepValue(G::FrameRates, s.frame_rate.load(), delta); break;
    case RowRender:
        s.render_resolution = (s.render_resolution + delta + DeadNationSettings::RenderResolutionCount) %
                              DeadNationSettings::RenderResolutionCount;
        break;
    case RowSharpen: s.sharpening = !s.sharpening; break;
    default: return;
    }
    display_dirty = true;
}

// imgui_mutex held. Saving happens once, on close.
void CloseDisplaySettings() {
    SettleResolution(true);
    display_open = false;
    if (display_dirty) {
        display_dirty = false;
        DeadNationSettings::SaveDisplay();
    }
}

struct DisplayRowText {
    const char* label;
    std::string value;
    std::string help;
    bool changes = true; ///< Left/Right change it (arrows around the value)
    bool enabled = true; ///< applies in the current window mode; else greyed out and skipped
};

DisplayRowText DisplayRowAt(int row) {
    auto& s = DeadNationSettings::Get();
    switch (row) {
    case RowMode: {
        static const char* const names[] = {"Windowed", "Borderless windowed", "Fullscreen"};
        std::string help = "Windowed, a borderless window covering the screen, or exclusive fullscreen.";
        if (s.display_mode != DeadNationGraphics::Windowed) {
            const auto [fw, fh] = ShownFullscreen();
            help += " Fullscreen picks its size from the screen and the aspect ratio, now " + std::to_string(fw) +
                    " x " + std::to_string(fh) + ", with black bars when the screen is of another shape.";
        }
        return {"Window mode", names[std::clamp(s.display_mode.load(), 0, 2)], help};
    }
    case RowAspect: {
        const int ratio = ShownAspect();
        std::string text(DeadNationSettings::AspectRatioKeys[ratio]);
        if (ratio == DeadNationSettings::Aspect16x9) text += "  (PS4)";
        if (ratio != s.startup_aspect_ratio) text += "  (after restart)";
        return {"Aspect ratio", text,
                "The picture's shape. 21:9 and 32:9 show more of the level to the sides, for ultrawide screens; "
                "the menus and HUD stay 16:9 in the middle. 4:3 and 16:10 show more above and below. "
                "A window takes sizes of this shape; fullscreen picks one from the screen. "
                "The game takes a new shape after a restart."};
    }
    case RowResolution: {
        const auto [w, h] = ShownResolution();
        const bool windowed = s.display_mode == DeadNationGraphics::Windowed;
        return {"Windowed resolution", std::to_string(w) + " x " + std::to_string(h) + "  " + DeadNationSettings::AspectLabel(w, h),
                "The window's size, among those of the aspect ratio that fit the screen.", windowed, windowed};
    }
    case RowMonitor: {
        std::string name = "Primary display";
        for (size_t i = 0; i < s.monitors.size() && i < s.monitor_names.size(); ++i) {
            if (s.monitors[i] == s.monitor) name = std::to_string(s.monitors[i] + 1) + ": " + s.monitor_names[i];
        }
        return {"Monitor", name, "The display the game window is on."};
    }
    case RowVsync:
        return {"VSync", s.vsync ? "On" : s.tearing ? "Off, tearing allowed" : "Off",
                "On waits for the monitor: no tearing, slightly more input delay. Off shows the newest frame "
                "at each refresh, still without tearing. Off, tearing allowed shows each frame at once: the "
                "least delay, and the only way past twice the refresh rate."};
    case RowFps: {
        const int chosen = s.frame_rate.load(), runs = DeadNationSettings::EffectiveFrameRate();
        std::string value = std::to_string(chosen) + " FPS";
        if (const char* env = std::getenv("DEADNATION_VBLANK_HZ"); env && env[0]) {
            value += "  (DEADNATION_VBLANK_HZ overrides)";
        } else if (runs != chosen) {
            value += "  (display is " + std::to_string(int(DeadNationSettings::DisplayRefresh() + 0.5)) +
                     " Hz: runs at " + std::to_string(runs) + ")";
        }
        return {"Max FPS", value,
                "The game's frame rate, at normal game speed: 30 is the PS4's. "
                "Applies at once. It is held "
                "to what the display refreshes at, and the PC must keep up with it: when it cannot, the "
                "game runs slower instead of skipping frames."};
    }
    case RowRender: {
        const int value = s.render_resolution;
        static const char* const names[] = {"720p (1280 x 720)", "1080p", "1440p (2560 x 1440)", "4K (3840 x 2160)"};
        std::string text = names[std::clamp(value, 0, DeadNationSettings::RenderResolutionCount - 1)];
        if (value != s.startup_render_resolution) text += "  (after restart)";
        return {"Render resolution", text,
                "The size the game renders at, filtered to the window. 1080p is the PS4's; 720p is lighter "
                "for weak GPUs and softer. 1440p and 4K are sharper and cost much more GPU (4K about three times 1080p). "
                "Best matched to the window: 1440p for a 1440p screen. "
                "Applies after a restart."};
    }
    case RowSharpen:
        return {"Sharpening", s.sharpening ? "On" : "Off",
                "AMD FSR 1 RCAS sharpening of the picture at the window's size: crisper edges and detail, "
                "without halos. Costs well under a millisecond a frame. Applies at once; F9 switches it over the game."};
    default: return {"", "", ""};
    }
}

struct CheatText {
    const char* label;
    const char* help;
};
// Indexed by DeadNationSettings::Cheat.
constexpr CheatText kCheats[DeadNationSettings::CheatCount] = {
    {"Invulnerability", "Nothing hurts you: zombies, explosions and fire take no health. Both players in co-op."},
    {"Unlock all weapons",
     "Every weapon the game mode has is yours, loaded, without buying it. Weapons unlocked this way stay "
     "unlocked in the game in progress (and its save) when this is turned off."},
    {"Unlock all items",
     "Every item (flares, grenades, mines, dynamite, auto-turret...) the game mode has is yours, stocked. "
     "Like weapons, they stay unlocked in the game in progress."},
    {"Infinite money", "Your cash is kept at 9,999,999: buy anything in the shops."},
    {"Infinite ammo", "Spare ammo never runs out: every weapon's reserve stays full. Clips still need reloading."},
    {"Infinite items", "Thrown and placed items are never used up: their count stays full."},
    {"Infinite clip (no reload)", "The weapon's clip stays full, so it never needs reloading."},
    {"Max weapon upgrades",
     "Every upgrade of every weapon and item at its highest level: clip, power, rate of fire, range... "
     "They stay upgraded in the game in progress when this is turned off."},
    {"Unlock all armors",
     "Unlocks every armor piece (arms, torso and legs), in the campaign and in Arcade, to pick on the armor "
     "screens. They are saved to your profile and stay unlocked when this is turned off."},
    {"Dash recovers immediately", "Dash (rush) is ready again at once: no wait for its bar to refill."},
};
// The menu's rows, as Cheat values: dash next to invulnerability (new cheats take the next bit,
// so the settings keep theirs).
constexpr int kCheatOrder[DeadNationSettings::CheatCount] = {
    DeadNationSettings::CheatInvulnerable, DeadNationSettings::CheatDash,  DeadNationSettings::CheatAllWeapons,
    DeadNationSettings::CheatAllItems,     DeadNationSettings::CheatMoney, DeadNationSettings::CheatAmmo,
    DeadNationSettings::CheatItems,        DeadNationSettings::CheatNoReload,
    DeadNationSettings::CheatWeaponUpgrades, DeadNationSettings::CheatArmor,
};

// imgui_mutex held. Saved when the menu closes.
void CloseCheats() {
    cheats_open = false;
    DeadNationSettings::SaveDisplayIfChanged();
    std::puts("Overlay: cheat menu closed; the game resumes");
}

// The pages draw with deadnation_page.h, the game's menu look (shared with settings.exe).
DeadNationPage::Fonts PageFonts() { return {page_light, page_bold}; }

// The Display Settings page: deadnation_page.h's settings list of DisplayRowAt's rows.
void DisplaySettingsBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport->Size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##display_settings", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    std::vector<DeadNationPage::Row> rows;
    for (int row = 0; row < RowCount; ++row) {
        const DisplayRowText text = DisplayRowAt(row);
        rows.push_back({text.label, text.value, text.help, text.changes, text.enabled});
    }
    using DeadNationPage::Glyph;
    const DeadNationPage::Hint hints[3] = {{Glyph::UpDown, "Move"}, {Glyph::Cross, "Change"}, {Glyph::Circle, "Back"}};
    DeadNationPage::SettingsPage(ImGui::GetWindowDrawList(), PageInk, PageFonts(), viewport->Pos, viewport->Size,
                                 "DISPLAY SETTINGS", rows, display_row, hints, 3,
                                 "Keyboard: arrows, Enter, Esc  |  Hold Select or F10 to close");
    ImGui::End();
    ImGui::PopStyleVar();
}

// The cheat menu, as the Display Settings page: a list of On/Off rows.
void CheatsBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport->Size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##cheats", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    const unsigned on = DeadNationSettings::Get().cheats;
    std::vector<DeadNationPage::Row> rows;
    for (const int c : kCheatOrder) {
        rows.push_back({kCheats[c].label, on >> c & 1 ? "On" : "Off", kCheats[c].help});
    }
    using DeadNationPage::Glyph;
    const DeadNationPage::Hint hints[4] = {
        {Glyph::UpDown, "Move"}, {Glyph::Cross, "Change"}, {Glyph::Triangle, "All off"}, {Glyph::Circle, "Back"}};
    DeadNationPage::SettingsPage(ImGui::GetWindowDrawList(), PageInk, PageFonts(), viewport->Pos, viewport->Size,
                                 "CHEATS", rows, cheat_row, hints, 4,
                                 "Keyboard: arrows, Enter, H all off, Esc  |  L3+R3 or X+M to close  |  The game is paused");
    ImGui::End();
    ImGui::PopStyleVar();
}

// The exit prompt, as the game's own confirmations: the question as the page title over a small
// framed list of No and Yes (the focused one on the red bar), and the button hints.
void ExitPromptBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float px = viewport->Size.y / 1080.0f;
    ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport->Size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##exit_prompt", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = viewport->Pos, size = viewport->Size;
    DeadNationPage::Backdrop(dl, PageInk, o, size);

    const float row_h = 68.0f * px, gap = 8.0f * px, pad = 18.0f * px;
    const float panel_w = std::min(size.x - 80.0f * px, 620.0f * px), panel_h = pad * 2.0f + row_h * 2.0f + gap;
    const float x0 = o.x + (size.x - panel_w) * 0.5f, x1 = x0 + panel_w;
    const float top = o.y + (size.y - panel_h) * 0.5f;
    DeadNationPage::Title(dl, PageInk, PageFonts(), ImVec2(x0 + 4.0f * px, top - 100.0f * px), px, "EXIT DEAD NATION?");
    DeadNationPage::Panel(dl, PageInk, ImVec2(x0, top), ImVec2(x1, top + panel_h), px);

    const char* const answers[2] = {"No", "Yes"};
    const float text_size = 42.0f * px;
    float y = top + pad;
    for (int i = 0; i < 2; ++i) {
        const bool focus = i == exit_focus;
        const ImVec2 a(x0 + pad, y), b(x1 - pad, y + row_h);
        if (focus) DeadNationPage::FocusBar(dl, PageInk, a, b, px);
        else DeadNationPage::RowBar(dl, PageInk, a, b, px);
        DeadNationPage::Text(dl, PageInk, page_bold, text_size, ImVec2(a.x + 24.0f * px, y + (row_h - text_size) * 0.5f), answers[i],
                             PageInk.text);
        y += row_h + gap;
    }

    using DeadNationPage::Glyph;
    const DeadNationPage::Hint hints[3] = {{Glyph::UpDown, "Move"}, {Glyph::Cross, "Select"}, {Glyph::Circle, "Back"}};
    DeadNationPage::Hints(dl, PageInk, PageFonts(), o, size, px, hints, 3, "Keyboard: arrows, Enter, Esc");
    ImGui::End();
    ImGui::PopStyleVar();
}

// imgui_mutex held. The exit prompt takes every key and button while it is open: up/down (or
// left/right) moves, confirm answers the focused choice, cancel or Start answers No.
bool HandleExitEvent(const SDL_Event& event) {
    int move = 0, answer = -1;
    if (event.type == SDL_EVENT_KEY_DOWN) {
        const SDL_Keycode key = event.key.key;
        move = key == SDLK_UP || key == SDLK_W || key == SDLK_LEFT || key == SDLK_A     ? -1
               : key == SDLK_DOWN || key == SDLK_S || key == SDLK_RIGHT || key == SDLK_D ? 1
                                                                                          : 0;
        if (!event.key.repeat) {
            if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE) answer = exit_focus;
            if (key == SDLK_ESCAPE || key == SDLK_BACKSPACE || key == SDLK_F4) answer = 0;
        }
    } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        const u8 button = event.gbutton.button;
        const u8 confirm_btn = SDL_GAMEPAD_BUTTON_SOUTH;
        const u8 cancel_btn = SDL_GAMEPAD_BUTTON_EAST;
        move = button == SDL_GAMEPAD_BUTTON_DPAD_UP || button == SDL_GAMEPAD_BUTTON_DPAD_LEFT     ? -1
               : button == SDL_GAMEPAD_BUTTON_DPAD_DOWN || button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT ? 1
                                                                                                    : 0;
        if (button == confirm_btn) answer = exit_focus;
        if (button == cancel_btn || button == SDL_GAMEPAD_BUTTON_START) answer = 0;
    } else {
        return event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP;
    }
    if (move) {
        exit_focus = std::clamp(exit_focus + move, 0, 1);
    }
    if (answer >= 0) {
        exit_open = false;
        if (answer == 1) {
            std::puts("Overlay: exit confirmed; closing the game");
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT; // the window thread ends the process as when the window is closed
            SDL_PushEvent(&quit);
        }
    }
    return true;
}

// The panel takes every key and button while it is open (the game sees neutral input).
bool HandleDisplayEvent(const SDL_Event& event) {
    int move = 0, change = 0;
    bool close = false;
    if (event.type == SDL_EVENT_KEY_DOWN) {
        const SDL_Keycode key = event.key.key;
        move = key == SDLK_UP || key == SDLK_W ? -1 : key == SDLK_DOWN || key == SDLK_S ? 1 : 0;
        change = key == SDLK_LEFT || key == SDLK_A ? -1
                 : key == SDLK_RIGHT || key == SDLK_D || key == SDLK_RETURN || key == SDLK_KP_ENTER ||
                         key == SDLK_SPACE
                     ? 1
                     : 0;
        close = !event.key.repeat && (key == SDLK_ESCAPE || key == SDLK_BACKSPACE || key == SDLK_F10);
    } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        const u8 button = event.gbutton.button;
        const u8 confirm_btn = SDL_GAMEPAD_BUTTON_SOUTH;
        const u8 cancel_btn = SDL_GAMEPAD_BUTTON_EAST;
        move = button == SDL_GAMEPAD_BUTTON_DPAD_UP ? -1 : button == SDL_GAMEPAD_BUTTON_DPAD_DOWN ? 1 : 0;
        change = button == SDL_GAMEPAD_BUTTON_DPAD_LEFT ? -1
                 : button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT || button == confirm_btn ? 1
                                                                                      : 0;
        close = button == cancel_btn || button == SDL_GAMEPAD_BUTTON_START || button == SDL_GAMEPAD_BUTTON_BACK;
    } else {
        return event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP;
    }
    if (move) { // greyed-out rows are passed over
        for (int i = 0; i < RowCount; ++i) {
            display_row = (display_row + move + RowCount) % RowCount;
            if (DisplayRowAt(display_row).enabled) break;
        }
    }
    if (change) {
        ChangeDisplayRow(display_row, change);
    }
    if (close) {
        CloseDisplaySettings();
    }
    return true;
}

// imgui_mutex held. The cheat menu takes every key and button while it is open: up/down moves,
// left/right or confirm switches the focused cheat, Triangle (H) turns them all off, cancel, Start
// or a stick click closes it.
bool HandleCheatEvent(const SDL_Event& event) {
    int move = 0;
    bool toggle = false, close = false, all_off = false;
    if (event.type == SDL_EVENT_KEY_DOWN) {
        const SDL_Keycode key = event.key.key;
        move = key == SDLK_UP || key == SDLK_W ? -1 : key == SDLK_DOWN || key == SDLK_S ? 1 : 0;
        toggle = !event.key.repeat && (key == SDLK_LEFT || key == SDLK_A || key == SDLK_RIGHT || key == SDLK_D ||
                                       key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE);
        close = !event.key.repeat &&
                (key == SDLK_ESCAPE || key == SDLK_BACKSPACE || key == SDLK_X || key == SDLK_M);
        all_off = !event.key.repeat && key == SDLK_H;
    } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        const u8 button = event.gbutton.button;
        move = button == SDL_GAMEPAD_BUTTON_DPAD_UP ? -1 : button == SDL_GAMEPAD_BUTTON_DPAD_DOWN ? 1 : 0;
        toggle = button == SDL_GAMEPAD_BUTTON_DPAD_LEFT || button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT ||
                 button == SDL_GAMEPAD_BUTTON_SOUTH;
        close = button == SDL_GAMEPAD_BUTTON_EAST || button == SDL_GAMEPAD_BUTTON_START ||
                button == SDL_GAMEPAD_BUTTON_LEFT_STICK || button == SDL_GAMEPAD_BUTTON_RIGHT_STICK;
        all_off = button == SDL_GAMEPAD_BUTTON_NORTH;
    } else {
        return event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP;
    }
    if (move) {
        cheat_row = (cheat_row + move + DeadNationSettings::CheatCount) % DeadNationSettings::CheatCount;
    }
    if (toggle) {
        const int cheat = kCheatOrder[cheat_row];
        auto& cheats = DeadNationSettings::Get().cheats;
        const unsigned now = cheats.fetch_xor(1u << cheat) ^ (1u << cheat);
        std::printf("Overlay: cheat %s %s\n", kCheats[cheat].label, now >> cheat & 1 ? "on" : "off");
    }
    if (all_off) {
        DeadNationSettings::Get().cheats = 0;
        std::puts("Overlay: every cheat off");
    }
    if (close) {
        CloseCheats();
    }
    return true;
}

// imgui_mutex held: opens the cheat menu (the game freezes at its next flip).
void OpenCheatsLocked() {
    if (cheats_open) return;
    cheat_row = 0;
    cheats_open = true;
    std::puts("Overlay: cheat menu opened; the game is paused");
}

void LoadingScreen() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x * 0.4f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("##loading", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    const u32 done = loading_done, total = loading_total;
    ImGui::SetWindowFontScale(1.6f);
    ImGui::TextUnformatted(total ? "Loading shader cache..." : "Preparing graphics...");
    ImGui::SetWindowFontScale(1.0f);
    if (total) {
        char count[64];
        std::snprintf(count, sizeof(count), "%u / %u (%u%%)", done, total, u32(u64(done) * 100 / total));
        ImGui::ProgressBar(float(done) / float(total), ImVec2(-1.0f, 0.0f), count);
    }
    ImGui::End();
}

void BackgroundLoading() {
    const u32 done = background_done, total = std::max(background_total.load(), 1u);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + pad, viewport->WorkPos.y + viewport->WorkSize.y - pad),
                            ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.35f);
    ImGui::Begin("##background_loading", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextDisabled("Loading shader cache %u%%", u32(u64(done) * 100 / total));
    ImGui::End();
}

bool NoticeActive() {
    return notice_until.load(std::memory_order_relaxed) > NowMs();
}

// Lower-right notes stack upward from `bottom`: the notice, then the shader notes.
template <typename Body>
void CornerNote(const char* id, float& bottom, float alpha, Body&& body) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad, bottom), ImGuiCond_Always,
                            ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(alpha);
    ImGui::Begin(id, nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    body();
    bottom -= ImGui::GetWindowHeight() + pad / 2;
    ImGui::End();
}

void Notice(float& bottom) {
    std::string text;
    {
        std::scoped_lock lock{notice_mutex};
        text = notice_text;
    }
    CornerNote("##notice", bottom, 0.6f, [&] { ImGui::TextUnformatted(text.c_str()); });
}

// DEADNATION_SHADER_NOTES=1: shader cache and compile notes in the corner.
const bool shader_notes = [] {
    const char* env = std::getenv("DEADNATION_SHADER_NOTES");
    return env && *env != '0';
}();

bool PrefetchActive() {
    const u32 total = prefetch_total;
    return shader_notes && total && (prefetch_done < total || prefetch_until > NowMs());
}

void ShaderPrefetch(float& bottom) {
    const u32 done = prefetch_done, total = std::max(prefetch_total.load(), 1u);
    CornerNote("##shader_prefetch", bottom, 0.35f, [&] {
        if (done >= total) {
            ImGui::TextDisabled("Shader cache ready (%u)", total);
            return;
        }
        ImGui::TextDisabled("Reading shader cache %u%% (%u/%u)", u32(u64(done) * 100 / total), done, total);
        ImGui::ProgressBar(float(done) / float(total), ImVec2(ImGui::GetContentRegionAvail().x, 4.0f * base_scale),
                           "");
    });
}

bool CompileNoteActive() {
    return shader_notes && (compile_pending || compile_until > NowMs());
}

void ShaderCompiles(float& bottom) {
    const u32 pending = compile_pending, count = compile_count, waits = compile_waits;
    CornerNote("##shader_compiles", bottom, 0.35f, [&] {
        if (pending) {
            ImGui::TextDisabled("Compiling shaders (%u left)", pending);
        } else {
            ImGui::TextDisabled("Compiled %u new shader%s", count, count == 1 ? "" : "s");
        }
        if (waits) {
            ImGui::TextDisabled("Waited for %u build%s", waits, waits == 1 ? "" : "s");
        }
    });
}

void ConfigureContext() {
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window positions are not kept
    io.BackendPlatformName = "deadnation";
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 10.0f;
    style.ChildRounding = 8.0f;
    style.FrameRounding = 5.0f;
    style.WindowPadding = ImVec2(20.0f, 16.0f);
    style.ItemSpacing = ImVec2(10.0f, 7.0f);
    style.WindowBorderSize = 1.0f;
    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = Ink::Text;
    c[ImGuiCol_TextDisabled] = Ink::Muted;
    c[ImGuiCol_WindowBg] = ImVec4(Ink::Bg.x, Ink::Bg.y, Ink::Bg.z, 0.97f);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Border] = Ink::Line;
    c[ImGuiCol_Separator] = Ink::Line;
    c[ImGuiCol_FrameBg] = Ink::Card;
    c[ImGuiCol_PlotHistogram] = Ink::Accent;
    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(deadnation_font_ttf),
                                   int(deadnation_font_ttf_end - deadnation_font_ttf), 18.0f, &font_config);
    // The pages' type: Light for titles and values, Bold for labels and hints (as the game's menus).
    page_light = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(deadnation_font_light_ttf),
                                                int(deadnation_font_light_ttf_end - deadnation_font_light_ttf),
                                                18.0f, &font_config);
    page_bold = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(deadnation_font_bold_ttf),
                                               int(deadnation_font_bold_ttf_end - deadnation_font_bold_ttf),
                                               18.0f, &font_config);
}

// imgui_mutex held. The backend frees a texture some frames after its last use.
void RetireChoiceTextures() {
    for (ImTextureData* tex : choice_textures) {
        if (tex) {
            tex->WantDestroyNextFrame = true;
            choice_retired.push_back(tex);
        }
    }
    choice_textures.clear();
    choice_pictures.clear();
    choice_retiring = !choice_retired.empty();
}

// imgui_mutex held, before NewFrame: new pictures become textures, destroyed ones are freed.
void UpdateChoiceTextures() {
    if (!choice_pictures.empty()) {
        choice_textures.assign(choice_pictures.size(), nullptr);
        for (size_t i = 0; i < choice_pictures.size(); ++i) {
            const DeadNationPng::Image& picture = choice_pictures[i];
            if (picture.rgba.empty()) {
                continue;
            }
            ImTextureData* tex = IM_NEW(ImTextureData)();
            tex->Create(ImTextureFormat_RGBA32, picture.width, picture.height);
            std::memcpy(tex->GetPixels(), picture.rgba.data(), picture.rgba.size());
            ImGui::RegisterUserTexture(tex);
            choice_textures[i] = tex;
        }
        choice_pictures.clear();
    }
    std::erase_if(choice_retired, [](ImTextureData* tex) {
        if (tex->Status != ImTextureStatus_Destroyed) {
            return false;
        }
        ImGui::UnregisterUserTexture(tex);
        IM_DELETE(tex);
        return true;
    });
    choice_retiring = !choice_retired.empty();
}

void StartSplashFade(const char* why) {
    int shown = SplashShown;
    splash_fade_from = NowMs();
    if (splash_state.compare_exchange_strong(shown, SplashFading)) {
        std::printf("Splash: %s after %.1f s\n", why, double(splash_fade_from - splash_since) / 1000.0);
    }
}

// imgui_mutex held, before NewFrame and UpdateChoiceTextures: the picture becomes a texture, and
// one faded out is retired.
void UpdateSplashTexture() {
    if (!splash_picture.rgba.empty()) {
        splash_texture = IM_NEW(ImTextureData)();
        splash_texture->Create(ImTextureFormat_RGBA32, splash_picture.width, splash_picture.height);
        std::memcpy(splash_texture->GetPixels(), splash_picture.rgba.data(), splash_picture.rgba.size());
        ImGui::RegisterUserTexture(splash_texture);
        splash_picture = {};
    }
    const int state = splash_state;
    if (state == SplashShown && NowMs() - splash_since > kSplashMaxMs) {
        StartSplashFade("no game picture yet, hidden");
    }
    if (state == SplashFading && NowMs() - splash_fade_from >= kSplashFadeMs) {
        splash_state = SplashDone;
        if (splash_texture) {
            splash_texture->WantDestroyNextFrame = true;
            choice_retired.push_back(splash_texture);
            choice_retiring = true;
            splash_texture = nullptr;
        }
    }
}

// The start-up picture fitted to the window (black bars like the game's frame), under the rest.
void Splash() {
    const int state = splash_state;
    if (!splash_texture || (state != SplashShown && state != SplashFading)) {
        return;
    }
    float alpha = 1.0f;
    if (state == SplashFading) {
        alpha = std::clamp(1.0f - float(NowMs() - splash_fade_from) / float(kSplashFadeMs), 0.0f, 1.0f);
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float w = float(splash_texture->Width), h = float(splash_texture->Height);
    const float scale = std::min(viewport->Size.x / w, viewport->Size.y / h);
    const ImVec2 size(w * scale, h * scale);
    const ImVec2 p0(viewport->Pos.x + (viewport->Size.x - size.x) * 0.5f,
                    viewport->Pos.y + (viewport->Size.y - size.y) * 0.5f);
    ImGui::GetBackgroundDrawList()->AddImage(splash_texture->GetTexRef(), p0, ImVec2(p0.x + size.x, p0.y + size.y),
                                             ImVec2(0, 0), ImVec2(1, 1),
                                             IM_COL32(255, 255, 255, int(alpha * 255.0f + 0.5f)));
}

// The save list takes the keys and buttons that move and confirm; the rest stays with the game
// (held neutral by CapturesInput).
bool HandleChoiceEvent(const SDL_Event& event) {
    int move = 0, finish = 0; // finish: 1 accept, 2 cancel
    if (event.type == SDL_EVENT_KEY_DOWN) {
        const SDL_Keycode key = event.key.key;
        move = key == SDLK_UP || key == SDLK_W ? -1 : key == SDLK_DOWN || key == SDLK_S ? 1 : 0;
        finish = key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE ? 1
                 : key == SDLK_ESCAPE || key == SDLK_BACKSPACE                   ? 2
                                                                                 : 0;
    } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        const u8 button = event.gbutton.button;
        move = button == SDL_GAMEPAD_BUTTON_DPAD_UP ? -1 : button == SDL_GAMEPAD_BUTTON_DPAD_DOWN ? 1 : 0;
        const u8 confirm_btn = SDL_GAMEPAD_BUTTON_SOUTH;
        const u8 cancel_btn = SDL_GAMEPAD_BUTTON_EAST;
        finish = button == confirm_btn ? 1 : button == cancel_btn ? 2 : 0;
    } else {
        return event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP;
    }
    const int count = int(choice_items.size());
    if (move && count) {
        choice_index = (choice_index + move + count) % count;
    }
    if (finish) {
        choice_result = finish == 1 && count ? choice_index : -2;
        choice_active = false;
        RetireChoiceTextures();
    }
    return true;
}

void DrawWindows() {
    SettleResolution(false);
    Splash();
    if (loading) {
        LoadingScreen();
    }
    float corner = ImGui::GetMainViewport()->WorkPos.y + ImGui::GetMainViewport()->WorkSize.y - 12.0f * base_scale;
    if (NoticeActive() && !display_open && !exit_open && !cheats_open) { // their button hints use the bottom of the screen
        Notice(corner);
    }
    if (PrefetchActive() && !loading) {
        ShaderPrefetch(corner);
    }
    if (CompileNoteActive() && !loading) {
        ShaderCompiles(corner);
    }
    if (background_total && !loading) {
        BackgroundLoading();
    }
    if (text_entry_active) {
        TextEntryBox();
    }
    if (choice_active) {
        ChoiceBox();
    }
    if (display_open && !choice_active) {
        DisplaySettingsBox();
    } else if (exit_open && !choice_active) {
        ExitPromptBox();
    } else if (cheats_open && !choice_active) {
        CheatsBox();
        if (const s64 at = cheats_close_at; at && NowMs() >= at) {
            cheats_close_at = 0;
            CloseCheats();
        }
    }
}

} // namespace

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ConfigureContext();

    const vk::Instance vk_instance = instance.GetInstance();
    ImGui_ImplVulkan_LoadFunctions(
        instance.ApiVersion(),
        [](const char* name, void* user) {
            return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(*static_cast<const vk::Instance*>(user),
                                                                       name);
        },
        const_cast<vk::Instance*>(&vk_instance));

    const VkFormat color_format = static_cast<VkFormat>(format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = vk_instance;
    info.PhysicalDevice = instance.GetPhysicalDevice();
    info.Device = instance.GetDevice();
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = instance.GetGraphicsQueue();
    // The font atlas plus the save list pictures (up to 33, and a closed list's until freed).
    info.DescriptorPoolSize = 80;
    info.MinImageCount = std::max(image_count, 2u);
    info.ImageCount = std::max(image_count, 2u);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        std::printf("Overlay: ImGui Vulkan backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    initialized = true;
}

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false;
    }
    if (choice_active) {
        return HandleChoiceEvent(event);
    }
    if (display_open) {
        return HandleDisplayEvent(event);
    }
    if (exit_open) {
        return HandleExitEvent(event);
    }
    if (cheats_open) {
        return HandleCheatEvent(event);
    }
    // L3+R3 (either stick clicked while the other is held) opens the cheat menu; on the keyboard,
    // X and M (the keys of L3 and R3, src/runtime_pad.c).
    if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && (event.gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_STICK ||
                                                        event.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_STICK)) {
        SDL_Gamepad* pad = SDL_GetGamepadFromID(event.gbutton.which);
        const auto other = event.gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_STICK ? SDL_GAMEPAD_BUTTON_RIGHT_STICK
                                                                                 : SDL_GAMEPAD_BUTTON_LEFT_STICK;
        if (pad && SDL_GetGamepadButton(pad, other)) {
            OpenCheatsLocked();
            return true;
        }
    }
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && !text_entry_active &&
        (event.key.scancode == SDL_SCANCODE_X || event.key.scancode == SDL_SCANCODE_M)) {
        const bool* keys = SDL_GetKeyboardState(nullptr);
        const SDL_Scancode other = event.key.scancode == SDL_SCANCODE_X ? SDL_SCANCODE_M : SDL_SCANCODE_X;
        if (keys && keys[other]) {
            OpenCheatsLocked();
            return true;
        }
    }
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F10 && !event.key.repeat && !text_entry_active) {
        display_row = 0;
        display_open = true;
        return true;
    }
    // F9 turns sharpening on or off over the game itself, to compare (saved at once). Left to the
    // pad recorder when DEADNATION_PAD_RECORD is set (src/runtime_pad.c), which starts on F9.
    static const bool pad_record = [] {
        const char* record = std::getenv("DEADNATION_PAD_RECORD");
        return record && record[0];
    }();
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F9 && !event.key.repeat && !text_entry_active &&
        !pad_record) {
        auto& s = DeadNationSettings::Get();
        s.sharpening = !s.sharpening;
        ShowNotice(s.sharpening ? "Sharpening: On" : "Sharpening: Off", 1500);
        DeadNationSettings::SaveDisplay();
        return true;
    }
    // F4 asks to close the game anywhere; Alt+F4 still closes the window at once.
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F4 && !event.key.repeat && !text_entry_active &&
        !(event.key.mod & SDL_KMOD_ALT)) {
        exit_focus = 0;
        exit_open = true;
        return true;
    }
    // Select+Start (either pressed while the other is held) asks the same, as F4 does. The pad
    // thread keeps Start from the game while Select is down (src/runtime_pad.c, select_hold).
    if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
        (event.gbutton.button == SDL_GAMEPAD_BUTTON_START || event.gbutton.button == SDL_GAMEPAD_BUTTON_BACK)) {
        SDL_Gamepad* pad = SDL_GetGamepadFromID(event.gbutton.which);
        const auto other = event.gbutton.button == SDL_GAMEPAD_BUTTON_START ? SDL_GAMEPAD_BUTTON_BACK
                                                                             : SDL_GAMEPAD_BUTTON_START;
        if (pad && SDL_GetGamepadButton(pad, other)) {
            std::puts("Overlay: Select+Start; asking whether to close the game");
            exit_focus = 0;
            exit_open = true;
            return true;
        }
    }
    return false;
}

void OpenDisplaySettings() {
    std::scoped_lock lock{imgui_mutex};
    if (!display_open) {
        display_row = 0;
        display_open = true;
    }
}

bool DisplaySettingsOpen() {
    return display_open;
}

void OpenCheats() {
    std::scoped_lock lock{imgui_mutex};
    OpenCheatsLocked();
}

bool CheatsOpen() {
    return cheats_open;
}

void OpenExitPrompt() {
    std::scoped_lock lock{imgui_mutex};
    if (!exit_open) {
        exit_focus = 0;
        exit_open = true;
    }
}

void ShowNotice(const std::string& text, int milliseconds) {
    {
        std::scoped_lock lock{notice_mutex};
        notice_text = text;
    }
    notice_until.store(NowMs() + milliseconds, std::memory_order_relaxed);
}

void SetBackgroundLoading(u32 done, u32 total) {
    background_done = done;
    background_total = total;
}

void SetShaderPrefetch(u32 done, u32 total) {
    if (total && done >= total) {
        prefetch_until = NowMs() + 2000;
    }
    prefetch_done = done;
    prefetch_total = total;
}

void NoteShaderCompiles(u32 pending, u32 started, u32 waits) {
    const s64 now = NowMs();
    if (!compile_pending && compile_until <= now) {
        compile_count = 0; // a new burst
        compile_waits = 0;
    }
    compile_count += started;
    compile_waits += waits;
    compile_pending = pending;
    compile_until = now + 2000;
}

void SetLoading(bool active, u32 done, u32 total) {
    loading_done = done;
    loading_total = total;
    loading = active;
}

void SetSplash(DeadNationPng::Image picture) {
    std::scoped_lock lock{imgui_mutex};
    if (picture.rgba.empty() || splash_state != SplashNone) {
        return;
    }
    splash_picture = std::move(picture);
    splash_since = NowMs();
    splash_state = SplashShown;
}

bool SplashWaiting() {
    return splash_state == SplashShown;
}

// Two picture frames in a row: a single stray one (a buffer not yet cleared) keeps the splash.
void NoteSplashFrame(bool picture) {
    if (!picture) {
        splash_lit_frames = 0;
    } else if (++splash_lit_frames >= 2) {
        StartSplashFade("game picture");
    }
}

bool Visible() {
    const int splash = splash_state;
    return initialized && (splash == SplashShown || splash == SplashFading || loading ||background_total || PrefetchActive() || CompileNoteActive() ||
                           text_entry_active || choice_active || choice_retiring || NoticeActive() || display_open ||
                           exit_open || cheats_open);
}

bool CapturesInput() {
    return text_entry_active || choice_active || display_open || exit_open || cheats_open;
}

void BeginChoice(const std::string& title, const std::vector<std::string>& items,
                 std::vector<DeadNationPng::Image> pictures, int focus, const std::string& preview_label,
                 DeadNationPng::Image preview) {
    std::scoped_lock lock{imgui_mutex};
    RetireChoiceTextures();
    choice_pictures = std::move(pictures);
    choice_pictures.resize(items.size());
    choice_preview_label = preview_label;
    if (!preview_label.empty()) choice_pictures.push_back(std::move(preview));
    choice_title = title;
    choice_items = items;
    choice_index = focus >= 0 && focus < int(items.size()) ? focus : 0;
    choice_result = -1;
    choice_active = true;
}

int PollChoice() {
    return choice_result;
}

void SetTextEntry(bool active, const std::string& prompt, const std::string& text) {
    std::scoped_lock lock{imgui_mutex};
    text_entry_active = active;
    text_entry_prompt = prompt;
    text_entry_text = text;
}

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent) {
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last_present).count();
    last_present = now;
    if (!Visible()) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    io.DeltaTime = ms > 0.0f && ms < 1000.0f ? ms / 1000.0f : 1.0f / 60.0f;
    // UI scale follows the display height (1080p = 1).
    const float scale = std::max(float(extent.height) / 1080.0f, 0.75f);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }

    UpdateSplashTexture();
    UpdateChoiceTextures();
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    DrawWindows();
    ImGui::Render();

    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    {
        // Font atlas uploads submit to the graphics queue themselves.
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
}

} // namespace DeadNationOverlay

// A message from the runtime to the player (src/runtime_pad.c: where the Display Settings are).
// Scripted runs (DEADNATION_PAD_FILE "settings[=row]", src/runtime_pad.c): the Display Settings
// panel opened as F10 opens it, with `row` focused.
extern "C" void deadnation_gpu_open_display_settings(int row) {
    DeadNationOverlay::OpenDisplaySettings();
    std::scoped_lock lock{DeadNationOverlay::imgui_mutex};
    DeadNationOverlay::display_row = std::clamp(row, 0, int(DeadNationOverlay::RowCount) - 1);
}

extern "C" void deadnation_gpu_open_cheats(int seconds) {
    DeadNationOverlay::OpenCheats();
    if (seconds > 0) DeadNationOverlay::cheats_close_at = DeadNationOverlay::NowMs() + seconds * 1000LL;
}

extern "C" int deadnation_cheat_menu_open(void) {
    return DeadNationOverlay::CheatsOpen() ? 1 : 0;
}

extern "C" void deadnation_gpu_notice(const char* text, int milliseconds) {
    if (text && text[0]) {
        DeadNationOverlay::ShowNotice(text, milliseconds);
    }
}

// Called by the runtime (src/runtime_services.c) when the game unlocks a trophy: its name as a
// notice in the corner.
extern "C" void deadnation_gpu_toast_trophy(int id, const char* title, const char* detail, const unsigned char* png,
                                            uint64_t png_size) {
    (void)id;
    (void)detail;
    (void)png;
    (void)png_size;
    if (title && title[0]) {
        DeadNationOverlay::ShowNotice(std::string("Trophy: ") + title, 5000);
    }
}
