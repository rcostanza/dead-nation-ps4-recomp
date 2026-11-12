// SPDX-License-Identifier: GPL-2.0-or-later
// settings.exe (settings on Linux): the game's options outside the game. It shows the
// in-game Display Settings page (deadnation_page.h, the game's menu look) with the same rows, plus
// the options only settings.ini had (speakers, skip intro, Back on the title screen), and writes
// each change at once into the settings.ini the game reads next: the save folder's when it has one,
// else the one beside the exe (setup/setup.h). Keyboard, mouse and gamepad.
//
//   settings [--config <settings.ini>]
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include "deadnation_graphics.h"
#include "deadnation_page.h"
#include "deadnation_settings.h"
#include "../setup/setup.h"

DEADNATION_EMBED(deadnation_font_light_ttf, DEADNATION_FONT_LIGHT_PATH)
DEADNATION_EMBED(deadnation_font_bold_ttf, DEADNATION_FONT_BOLD_PATH)
extern "C" const unsigned char deadnation_font_light_ttf[];
extern "C" const unsigned char deadnation_font_light_ttf_end[];
extern "C" const unsigned char deadnation_font_bold_ttf[];
extern "C" const unsigned char deadnation_font_bold_ttf_end[];

namespace fs = std::filesystem;
namespace G = DeadNationGraphics;
namespace S = DeadNationSettings;

namespace {

// The rows: the in-game page's first, in its order, then the start-up options.
enum Row : int {
    RowAspect, RowMode, RowResolution, RowMonitor, RowVsync, RowFps, RowRender, RowSharpen,
    RowAudio, RowSkipIntro, RowTitleBack, RowCount
};

// settings.ini's values, parsed as the game parses them (gpu/shim/deadnation_settings.cpp Set).
struct Values {
    int display_mode = G::Borderless;
    int aspect = S::Aspect16x9;
    int monitor = G::PrimaryMonitor;
    std::pair<int, int> window{1920, 1080};
    bool vsync = true, tearing = false;
    int frame_rate = 60;
    int render = S::Render1080p;
    bool sharpening = true;
    int audio = S::AudioChannelsAuto;
    bool skip_intro = true, title_back_exits = true;
};

struct Display {
    int index; // in SDL_GetDisplays(), what settings.ini's monitor names
    SDL_DisplayID id;
    std::string name;
};

struct App {
    fs::path ini;
    Values v;
    std::vector<Display> displays;
    std::vector<std::pair<int, int>> resolutions[S::AspectRatioCount];
    std::vector<std::pair<int, int>> modes;
    int desktop_w = 0, desktop_h = 0;
    float refresh = 0.0f;
    int focus = RowAspect;
    std::string status; // the last save's error, else empty
    bool quit = false;
    DeadNationPage::Fonts fonts{};
    std::vector<DeadNationPage::RowArea> areas;
    std::vector<ImRect> hint_rects;
};

bool Truthy(std::string value) {
    for (char& c : value) c = char(std::tolower(static_cast<unsigned char>(c)));
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

// The settings.ini the game reads at its next start (src/launcher.c): the save folder's once it
// has one, else the one beside the exe, which the game copies to the save folder at its first start.
// When neither exists yet the latter is made from prepare/settings.example.ini (as the setup does).
fs::path FindSettings(const fs::path& root) {
    std::error_code ec;
    if (const auto paths = setup::read_paths(root)) {
        const fs::path saved = paths->save / "settings.ini";
        if (fs::is_regular_file(saved, ec)) return saved;
    }
    return root / "settings.ini";
}

void EnsureFile(const fs::path& ini, const fs::path& root) {
    std::error_code ec;
    if (fs::is_regular_file(ini, ec)) return;
    if (!fs::copy_file(root / "prepare" / "settings.example.ini", ini, ec)) {
        if (FILE* f = std::fopen(setup::narrow(setup::text(ini)).c_str(), "ab")) std::fclose(f);
    }
}

std::string Get(const fs::path& ini, const wchar_t* key) {
    try {
        const auto value = setup::read_setting(ini, key);
        return value ? setup::narrow(*value) : std::string();
    } catch (const setup::Error&) {
        return {};
    }
}

Values Load(const fs::path& ini) {
    Values v;
    if (std::string s = Get(ini, L"display_mode"); !s.empty()) v.display_mode = G::ParseDisplayMode(s);
    for (int r = 0; r < S::AspectRatioCount; ++r) {
        if (Get(ini, L"aspect_ratio") == S::AspectRatioKeys[r]) v.aspect = r;
    }
    v.monitor = G::ParseMonitor(Get(ini, L"monitor"));
    std::string size = Get(ini, L"output_resolution");
    if (size.empty()) size = Get(ini, L"window"); // its name before DeadNation
    v.window = G::ParseWindowSize(size);
    if (std::string s = Get(ini, L"vsync"); !s.empty()) v.vsync = Truthy(s);
    if (std::string s = Get(ini, L"tearing"); !s.empty()) v.tearing = Truthy(s);
    const int rate = std::atoi(Get(ini, L"frame_rate").c_str());
    v.frame_rate = G::IndexOf(G::FrameRates, rate, -1) >= 0 ? rate : 60;
    std::string render = Get(ini, L"render_resolution");
    for (char& c : render) c = char(std::tolower(static_cast<unsigned char>(c)));
    v.render = render == "720p" ? S::Render720p : render == "1440p" ? S::Render1440p : render == "4k" || render == "2160p" ? S::Render4k : S::Render1080p;
    if (std::string s = Get(ini, L"sharpening"); !s.empty()) v.sharpening = Truthy(s);
    v.audio = S::ParseAudioChannels(Get(ini, L"audio_channels"));
    if (std::string s = Get(ini, L"skip_intro"); !s.empty()) v.skip_intro = Truthy(s);
    if (std::string s = Get(ini, L"title_back_exits"); !s.empty()) v.title_back_exits = Truthy(s);
    return v;
}

// The displays SDL lists with a size, as the game lists them (gpu/shim/window.cpp MonitorDisplay).
std::vector<Display> ListDisplays() {
    std::vector<Display> out;
    int count = 0;
    if (SDL_DisplayID* list = SDL_GetDisplays(&count)) {
        for (int i = 0; i < count; ++i) {
            SDL_Rect bounds{};
            if (SDL_GetDisplayBounds(list[i], &bounds) && bounds.w > 0 && bounds.h > 0) {
                const char* name = SDL_GetDisplayName(list[i]);
                out.push_back({i, list[i], name && name[0] ? name : "Display " + std::to_string(i + 1)});
            }
        }
        SDL_free(list);
    }
    return out;
}

// The display the game window opens on: the chosen one, else the primary.
SDL_DisplayID ChosenDisplay(const App& app) {
    for (const Display& d : app.displays) {
        if (d.index == app.v.monitor) return d.id;
    }
    return SDL_GetPrimaryDisplay();
}

// The chosen display's fullscreen modes, desktop, refresh rate and window sizes (as the game lists
// them, gpu/shim/window.cpp); a window size of another shape than the aspect ratio moves to the
// nearest in height of that shape's.
void ListModes(App& app) {
    const SDL_DisplayID display = ChosenDisplay(app);
    app.modes.clear();
    int count = 0;
    if (SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(display, &count)) {
        for (int i = 0; i < count; ++i) {
            if (modes[i]->w >= 640 && modes[i]->h >= 360) app.modes.emplace_back(modes[i]->w, modes[i]->h);
        }
        SDL_free(modes);
    }
    std::sort(app.modes.begin(), app.modes.end(), std::greater<>());
    app.modes.erase(std::unique(app.modes.begin(), app.modes.end()), app.modes.end());
    const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(display);
    app.desktop_w = desktop ? desktop->w : 0;
    app.desktop_h = desktop ? desktop->h : 0;
    app.refresh = desktop ? desktop->refresh_rate : 0.0f;
    S::ListResolutions(app.resolutions, app.modes, app.desktop_w, app.desktop_h, app.v.window);
    if (S::AspectRatioOf(app.v.window.first, app.v.window.second) != app.v.aspect)
        app.v.window = S::NearestHeight(app.resolutions[app.v.aspect], app.v.window.second);
}

// Writes `keys` of the row just changed into the settings.ini.
void Save(App& app, int row) {
    const Values& v = app.v;
    std::vector<std::pair<const wchar_t*, std::string>> keys;
    const std::string size = std::to_string(v.window.first) + "x" + std::to_string(v.window.second);
    switch (row) {
    case RowAspect: keys = {{L"aspect_ratio", std::string(S::AspectRatioKeys[v.aspect])}, {L"output_resolution", size}}; break;
    case RowMode: keys = {{L"display_mode", std::string(G::DisplayModeKeys[v.display_mode])}}; break;
    case RowResolution: keys = {{L"output_resolution", size}}; break;
    case RowMonitor:
        keys = {{L"monitor", v.monitor < 0 ? std::string() : std::to_string(v.monitor)}, {L"output_resolution", size}};
        break;
    case RowVsync: keys = {{L"vsync", v.vsync ? "1" : "0"}, {L"tearing", v.tearing ? "1" : "0"}}; break;
    case RowFps: keys = {{L"frame_rate", std::to_string(v.frame_rate)}}; break;
    case RowRender: keys = {{L"render_resolution", std::string(S::RenderResolutionKeys[v.render])}}; break;
    case RowSharpen: keys = {{L"sharpening", v.sharpening ? "1" : "0"}}; break;
    case RowAudio: keys = {{L"audio_channels", std::string(S::AudioChannelKeys[v.audio])}}; break;
    case RowSkipIntro: keys = {{L"skip_intro", v.skip_intro ? "1" : "0"}}; break;
    case RowTitleBack: keys = {{L"title_back_exits", v.title_back_exits ? "1" : "0"}}; break;
    default: return;
    }
    try {
        for (const auto& [key, value] : keys) setup::write_setting(app.ini, key, setup::widen(value));
        app.status.clear();
    } catch (const setup::Error& e) {
        app.status = setup::narrow(e.message);
        for (char& c : app.status) {
            if (c == '\n') c = ' ';
        }
    }
}

template <typename T, std::size_t N>
int Step(const std::array<T, N>& values, int current, int delta) {
    const int i = G::IndexOf(values, current, 0);
    return values[(i + delta + int(N)) % int(N)];
}

// The row's value one step forward (delta 1) or back (-1), saved at once.
void Change(App& app, int row, int delta) {
    Values& v = app.v;
    switch (row) {
    case RowMode: v.display_mode = (v.display_mode + delta + G::DisplayModeCount) % G::DisplayModeCount; break;
    case RowAspect: // the window moves to the new shape's size nearest in height
        v.aspect = (v.aspect + delta + S::AspectRatioCount) % S::AspectRatioCount;
        v.window = S::NearestHeight(app.resolutions[v.aspect], v.window.second);
        break;
    case RowResolution: { // windowed only, listed largest first: right steps up to a larger size
        const auto& sizes = app.resolutions[v.aspect];
        if (sizes.empty() || v.display_mode != G::Windowed) return;
        int at = 0;
        while (at + 1 < int(sizes.size()) && sizes[at] > v.window) ++at;
        v.window = sizes[std::clamp(at - delta, 0, int(sizes.size()) - 1)];
        break;
    }
    case RowMonitor: { // the primary, then each listed display
        const int count = int(app.displays.size()) + 1;
        int at = 0;
        for (int i = 0; i < int(app.displays.size()); ++i) {
            if (app.displays[i].index == v.monitor) at = i + 1;
        }
        at = (at + delta + count) % count;
        v.monitor = at == 0 ? G::PrimaryMonitor : app.displays[at - 1].index;
        ListModes(app);
        break;
    }
    case RowVsync: { // On, Off, Off with tearing
        const int next = ((v.vsync ? 0 : v.tearing ? 2 : 1) + delta + 3) % 3;
        v.vsync = next == 0;
        v.tearing = next == 2;
        break;
    }
    case RowFps: v.frame_rate = Step(G::FrameRates, v.frame_rate, delta); break;
    case RowRender: v.render = (v.render + delta + S::RenderResolutionCount) % S::RenderResolutionCount; break;
    case RowSharpen: v.sharpening = !v.sharpening; break;
    case RowAudio: v.audio = (v.audio + delta + S::AudioChannelsCount) % S::AudioChannelsCount; break;
    case RowSkipIntro: v.skip_intro = !v.skip_intro; break;
    case RowTitleBack: v.title_back_exits = !v.title_back_exits; break;
    default: return;
    }
    Save(app, row);
}

// What a row shows; the texts follow the in-game page's (gpu/shim/deadnation_overlay.cpp DisplayRowAt).
DeadNationPage::Row RowAt(const App& app, int row) {
    const Values& v = app.v;
    switch (row) {
    case RowMode: {
        static const char* const names[] = {"Windowed", "Borderless windowed", "Fullscreen"};
        std::string help = "Windowed, a borderless window covering the screen, or exclusive fullscreen.";
        if (v.display_mode != G::Windowed) {
            const auto [w, h] = S::PickFullscreen(v.display_mode == G::Fullscreen, v.aspect, {app.desktop_w, app.desktop_h},
                                                  app.modes).picture;
            help += " Fullscreen picks its size from the screen and the aspect ratio, here " + std::to_string(w) + " x " +
                    std::to_string(h) + ", with black bars when the screen is of another shape.";
        }
        return {"Window mode", names[std::clamp(v.display_mode, 0, 2)], help};
    }
    case RowAspect: {
        std::string text(S::AspectRatioKeys[v.aspect]);
        if (v.aspect == S::Aspect16x9) text += "  (PS4)";
        return {"Aspect ratio", text,
                "The picture's shape. 21:9 and 32:9 show more of the level to the sides, for ultrawide screens; "
                "the menus and HUD stay 16:9 in the middle. 4:3 and 16:10 show more above and below. "
                "A window takes sizes of this shape; fullscreen picks one from the screen."};
    }
    case RowResolution: {
        const bool windowed = v.display_mode == G::Windowed;
        return {"Windowed resolution",
                std::to_string(v.window.first) + " x " + std::to_string(v.window.second) + "  " +
                    S::AspectLabel(v.window.first, v.window.second),
                "The window's size, among those of the aspect ratio that fit the screen. Only for the Windowed mode.",
                windowed, windowed};
    }
    case RowMonitor: {
        std::string name = "Primary display";
        for (const Display& d : app.displays) {
            if (d.index == v.monitor) name = std::to_string(d.index + 1) + ": " + d.name;
        }
        return {"Monitor", name, "The display the game window opens on."};
    }
    case RowVsync:
        return {"VSync", v.vsync ? "On" : v.tearing ? "Off, tearing allowed" : "Off",
                "On waits for the monitor: no tearing, slightly more input delay. Off shows the newest frame "
                "at each refresh, still without tearing. Off, tearing allowed shows each frame at once: the "
                "least delay, and the only way past twice the refresh rate."};
    case RowFps: {
        std::string value = std::to_string(v.frame_rate) + " FPS";
        if (v.frame_rate == 30) value += "  (PS4)";
        int runs = v.frame_rate;
        if (app.refresh > 0.0f && !(!v.vsync && v.tearing)) {
            runs = G::FrameRates[0];
            for (const int r : G::FrameRates) {
                if (r <= v.frame_rate && r <= app.refresh + 0.5f) runs = std::max(runs, r);
            }
        }
        if (runs != v.frame_rate)
            value += "  (display is " + std::to_string(int(app.refresh + 0.5f)) + " Hz: runs at " + std::to_string(runs) + ")";
        return {"Max FPS", value,
                "The game's frame rate, at normal game speed: 30 is the PS4's. It is held to what the display "
                "refreshes at, and the PC must keep up with it: when it cannot, the game runs slower instead "
                "of skipping frames."};
    }
    case RowRender: {
        static const char* const names[] = {"720p (1280 x 720)", "1080p  (PS4)", "1440p (2560 x 1440)", "4K (3840 x 2160)"};
        return {"Render resolution", names[std::clamp(v.render, 0, S::RenderResolutionCount - 1)],
                "The size the game renders at, filtered to the window. 1080p is the PS4's; 720p is lighter "
                "for weak GPUs and softer. 1440p and 4K are sharper and cost much more GPU (4K about three times 1080p). "
                "Best matched to the window: 1440p for a 1440p screen."};
    }
    case RowSharpen:
        return {"Sharpening", v.sharpening ? "On" : "Off",
                "AMD FSR 1 RCAS sharpening of the picture at the window's size: crisper edges and detail, "
                "without halos. Costs well under a millisecond a frame. In the game F9 switches it to compare."};
    case RowAudio: {
        static const char* const names[] = {"Automatic", "Stereo", "5.1", "7.1"};
        return {"Speakers", names[std::clamp(v.audio, 0, S::AudioChannelsCount - 1)],
                "The speakers the game's 7.1 sound is mixed down to. Automatic follows the playback device."};
    }
    case RowSkipIntro:
        return {"Skip intro", v.skip_intro ? "On" : "Off",
                "On starts at the main menu, skipping the logos, the autosave notice, the brightness screen, "
                "the intro movie and the title screen."};
    case RowTitleBack:
        return {"Back on title screen", v.title_back_exits ? "Asks to exit" : "Does nothing",
                "Whether Back (Circle, B, Esc) on the title screen asks to close the game. F4 and Select+Start always do."};
    default: return {};
    }
}

void Move(App& app, int delta) { // greyed-out rows are passed over
    for (int i = 0; i < RowCount; ++i) {
        app.focus = (app.focus + delta + RowCount) % RowCount;
        if (RowAt(app, app.focus).enabled) break;
    }
}

void HandleEvent(App& app, const SDL_Event& e) {
    int move = 0, change = 0;
    switch (e.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED: app.quit = true; return;
    case SDL_EVENT_KEY_DOWN: {
        const SDL_Keycode key = e.key.key;
        move = key == SDLK_UP || key == SDLK_W ? -1 : key == SDLK_DOWN || key == SDLK_S || key == SDLK_TAB ? 1 : 0;
        change = key == SDLK_LEFT || key == SDLK_A ? -1
                 : key == SDLK_RIGHT || key == SDLK_D || key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE
                     ? 1
                     : 0;
        if (!e.key.repeat && (key == SDLK_ESCAPE || key == SDLK_BACKSPACE)) app.quit = true;
        break;
    }
    case SDL_EVENT_GAMEPAD_ADDED: SDL_OpenGamepad(e.gdevice.which); return;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
        const Uint8 b = e.gbutton.button;
        move = b == SDL_GAMEPAD_BUTTON_DPAD_UP ? -1 : b == SDL_GAMEPAD_BUTTON_DPAD_DOWN ? 1 : 0;
        change = b == SDL_GAMEPAD_BUTTON_DPAD_LEFT                                     ? -1
                 : b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT || b == SDL_GAMEPAD_BUTTON_SOUTH ? 1
                                                                                        : 0;
        if (b == SDL_GAMEPAD_BUTTON_EAST || b == SDL_GAMEPAD_BUTTON_START || b == SDL_GAMEPAD_BUTTON_BACK) app.quit = true;
        break;
    }
    case SDL_EVENT_MOUSE_MOTION: { // hovering a row focuses it, as the game's menus
        const ImVec2 at(e.motion.x, e.motion.y);
        for (int row = 0; row < int(app.areas.size()); ++row) {
            if (app.areas[row].bar.Contains(at) && RowAt(app, row).enabled) app.focus = row;
        }
        return;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN: { // left click steps forward (back on the "<"), right click back
        const ImVec2 at(e.button.x, e.button.y);
        for (int row = 0; row < int(app.areas.size()); ++row) {
            const DeadNationPage::RowArea& area = app.areas[row];
            if (!area.bar.Contains(at) || !RowAt(app, row).enabled) continue;
            app.focus = row;
            const bool back = e.button.button == SDL_BUTTON_RIGHT || (at.x >= area.back_x0 && at.x < area.back_x1);
            Change(app, row, back ? -1 : 1);
            return;
        }
        // The hints: Move, Change, Back.
        for (int i = 0; i < int(app.hint_rects.size()); ++i) {
            if (!app.hint_rects[i].Contains(at)) continue;
            if (i == 0) Move(app, 1);
            if (i == 1) Change(app, app.focus, 1);
            if (i == 2) app.quit = true;
        }
        return;
    }
    default: return;
    }
    if (move) Move(app, move);
    if (change) Change(app, app.focus, change);
}

void Draw(App& app) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport->Size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##settings", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = viewport->Pos, size = viewport->Size;
    const auto& ink = DeadNationPage::Srgb;
    std::vector<DeadNationPage::Row> rows;
    for (int row = 0; row < RowCount; ++row) rows.push_back(RowAt(app, row));
    using DeadNationPage::Glyph;
    const DeadNationPage::Hint hints[3] = {{Glyph::UpDown, "Move"}, {Glyph::Cross, "Change"}, {Glyph::Circle, "Back"}};
    DeadNationPage::SettingsPage(dl, ink, app.fonts, o, size, "SETTINGS", rows, app.focus, hints, 3,
                                 "Keyboard: arrows, Enter, Esc  |  Mouse: click, right-click steps back",
                                 &app.areas, &app.hint_rects);
    // Where the settings go, under the hint strip; a failed save in red instead.
    const float px = size.y / 1080.0f, note = 24.0f * px;
    const std::string where = app.status.empty()
                                  ? "Saved at once to " + setup::narrow(setup::text(app.ini)) + ". The game reads it when it starts."
                                  : app.status;
    DeadNationPage::Text(dl, ink, app.fonts.bold, note,
                         ImVec2(o.x + 110.0f * px, DeadNationPage::HintStripTop(o, size, px) + 66.0f * px), where.c_str(),
                         app.status.empty() ? ink.help : ink.accent);
    ImGui::End();
    ImGui::PopStyleVar();
}

} // namespace

int main(int argc, char** argv) {
    const fs::path root = setup::exe_folder();
    fs::path ini;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            ini = setup::path_of(setup::widen(argv[++i]));
        } else {
            std::fprintf(stderr, "usage: %s [--config <settings.ini>]\n", argv[0]);
            return 2;
        }
    }
    if (ini.empty()) ini = FindSettings(root);
    EnsureFile(ini, root);

    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Dead Nation Settings", SDL_GetError(), nullptr);
        return 1;
    }
    App app;
    app.ini = ini;
    app.v = Load(ini);
    app.displays = ListDisplays();
    ListModes(app);

    // A 16:9 window like the game's page, 2/3 of 1080p at 100% scaling.
    const float scale = std::max(1.0f, SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay()));
    SDL_Window* window = SDL_CreateWindow("Dead Nation Settings", int(1280 * scale), int(720 * scale),
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Dead Nation Settings", SDL_GetError(), nullptr);
        return 1;
    }
    SDL_SetWindowMinimumSize(window, int(854 * scale), int(480 * scale));
    SDL_SetWindowAspectRatio(window, 16.0f / 9.0f, 16.0f / 9.0f);
    SDL_SetRenderVSync(renderer, 1);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // nothing written but settings.ini
    io.LogFilename = nullptr;
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;
    app.fonts.light = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(deadnation_font_light_ttf),
                                                     int(deadnation_font_light_ttf_end - deadnation_font_light_ttf), 18.0f, &config);
    app.fonts.bold = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(deadnation_font_bold_ttf),
                                                    int(deadnation_font_bold_ttf_end - deadnation_font_bold_ttf), 18.0f, &config);
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    // Redrawn on input (and a few times a second, for a display plugged in or a gamepad).
    while (!app.quit) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 250)) {
            do {
                ImGui_ImplSDL3_ProcessEvent(&e);
                if (e.type == SDL_EVENT_DISPLAY_ADDED || e.type == SDL_EVENT_DISPLAY_REMOVED) {
                    app.displays = ListDisplays();
                    ListModes(app);
                }
                HandleEvent(app, e);
            } while (SDL_PollEvent(&e));
        }
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        Draw(app);
        ImGui::Render();
        SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
