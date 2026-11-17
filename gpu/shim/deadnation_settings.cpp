// SPDX-License-Identifier: GPL-2.0-or-later
#include "deadnation_settings.h"
#include "deadnation_graphics.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <utility>

double DeadNationWindowRefreshRate(); // deadnation_gpu.cpp: the window's display, 0 when unknown

namespace DeadNationSettings {

namespace {

const char* Path() {
    const char* env = std::getenv("DEADNATION_CONFIG");
    return env && env[0] ? env : "settings.ini";
}

// The file read: Path() when it exists, else DEADNATION_CONFIG_DEFAULT (the launcher's: the one the
// setup wrote next to the exe, while Path() is the save folder's). Writes always go to Path().
const char* ReadPath() {
    if (FILE* file = std::fopen(Path(), "r")) {
        std::fclose(file);
        return Path();
    }
    const char* fallback = std::getenv("DEADNATION_CONFIG_DEFAULT");
    return fallback && fallback[0] ? fallback : Path();
}

bool Truthy(std::string_view value) {
    std::string lower;
    for (const char c : value) lower += char(std::tolower(static_cast<unsigned char>(c)));
    return lower == "1" || lower == "true" || lower == "yes" || lower == "on";
}

void Set(Values& v, const std::string& key, const std::string& value) {
    const int i = std::atoi(value.c_str());
    if (key == "display_mode") {
        v.display_mode = DeadNationGraphics::ParseDisplayMode(value);
    } else if (key == "vsync") {
        v.vsync = Truthy(value);
    } else if (key == "tearing") {
        v.tearing = Truthy(value);
    } else if (key == "monitor") {
        v.monitor = DeadNationGraphics::ParseMonitor(value);
    } else if (key == "output_resolution" || key == "window") { // window: its name before DeadNation
        const auto [w, h] = DeadNationGraphics::ParseWindowSize(value);
        v.window_width = w;
        v.window_height = h;
    } else if (key == "aspect_ratio") {
        int ratio = Aspect16x9;
        for (int r = 0; r < AspectRatioCount; ++r) {
            if (value == AspectRatioKeys[r]) ratio = r;
        }
        if (!value.empty() && value != AspectRatioKeys[ratio]) {
            std::printf("Settings: aspect_ratio=%s is not 4:3, 16:10, 16:9, 21:9 or 32:9; using 16:9\n", value.c_str());
        }
        v.startup_aspect_ratio = v.aspect_ratio = ratio;
    } else if (key == "skip_intro") {
        v.skip_intro = Truthy(value);
    } else if (key == "title_back_exits") {
        v.title_back_exits = Truthy(value);
    } else if (key == "sharpening") {
        v.sharpening = Truthy(value);
    } else if (key == "frame_rate") {
        const bool listed = DeadNationGraphics::IndexOf(DeadNationGraphics::FrameRates, i, -1) >= 0;
        v.frame_rate = listed ? i : 60;
        if (!value.empty() && !listed) {
            std::printf("Settings: frame_rate=%s is not 30, 60, 90, 120, 144, 180, 240 or 360; using 60\n", value.c_str());
        }
    } else if (key == "debug_speed") {
        v.debug_speed = std::clamp(i, 1, 4);
        if (!value.empty() && (i < 1 || i > 4)) {
            std::printf("Settings: debug_speed=%s is not 1 to 4; using %d\n", value.c_str(), v.debug_speed.load());
        }
    } else if (key == "audio_channels") {
        v.audio_channels = ParseAudioChannels(value);
    } else if (key == "render_resolution") {
        std::string lower;
        for (const char c : value) lower += char(std::tolower(static_cast<unsigned char>(c)));
        v.startup_render_resolution = v.render_resolution =
            lower == "720p" ? Render720p : lower == "1440p" ? Render1440p : lower == "4k" || lower == "2160p" ? Render4k : Render1080p;
        if (!lower.empty() && lower != "720p" && lower != "1080p" && lower != "1440p" && lower != "4k" && lower != "2160p") {
            std::printf("Settings: render_resolution=%s is not 720p, 1080p, 1440p or 4k; using 1080p\n", value.c_str());
        }
    } else if (const auto cheat = std::find(std::begin(CheatKeys), std::end(CheatKeys), key);
               cheat != std::end(CheatKeys)) {
        const unsigned bit = 1u << (cheat - std::begin(CheatKeys));
        if (Truthy(value)) v.cheats |= bit;
        else v.cheats &= ~bit;
    } else {
        std::printf("Settings: unknown key %s\n", key.c_str());
    }
}

std::vector<std::pair<std::string, std::string>> DisplayKeys() {
    const auto& v = Get();
    const int monitor = v.monitor;
    std::vector<std::pair<std::string, std::string>> keys{
        {"display_mode", std::string(DeadNationGraphics::DisplayModeKeys[std::clamp(v.display_mode.load(), 0, 2)])},
        {"monitor", monitor < 0 ? std::string() : std::to_string(monitor)},
        {"aspect_ratio", std::string(AspectRatioKeys[std::clamp(v.aspect_ratio.load(), 0, AspectRatioCount - 1)])},
        {"output_resolution", std::to_string(v.window_width.load()) + "x" + std::to_string(v.window_height.load())},
        {"vsync", v.vsync ? "1" : "0"},
        {"tearing", v.tearing ? "1" : "0"},
        {"frame_rate", std::to_string(v.frame_rate.load())},
        {"render_resolution", std::string(RenderResolutionKeys[std::clamp(v.render_resolution.load(), 0, RenderResolutionCount - 1)])},
        {"sharpening", v.sharpening ? "1" : "0"},
    };
    const unsigned cheats = v.cheats;
    for (int c = 0; c < CheatCount; ++c) keys.emplace_back(CheatKeys[c], cheats >> c & 1 ? "1" : "0");
    return keys;
}

std::mutex save_mutex;
// The display keys as Path() holds them: loaded at start, updated by each save.
std::vector<std::pair<std::string, std::string>> saved_keys;

bool Exists(const char* path) {
    FILE* file = std::fopen(path, "r");
    if (file) std::fclose(file);
    return file != nullptr;
}

// save_mutex held. Writes the display keys into Path() in place, from ReadPath()'s lines.
void WriteDisplay() {
    auto keys = DisplayKeys();
    saved_keys = keys;
    std::vector<std::string> lines;
    if (FILE* file = std::fopen(ReadPath(), "r")) {
        char line[1024];
        while (std::fgets(line, sizeof(line), file)) {
            std::string text{line};
            text.erase(text.find_last_not_of("\r\n") + 1);
            lines.push_back(text);
        }
        std::fclose(file);
    }
    for (std::string& line : lines) {
        const auto eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        // An old window= line becomes output_resolution= (the same setting, renamed).
        const std::string name = line.substr(0, eq) == "window" ? "output_resolution" : line.substr(0, eq);
        const auto key = std::find_if(keys.begin(), keys.end(), [&](const auto& k) { return name == k.first; });
        if (key == keys.end()) continue;
        line = key->first + "=" + key->second;
        keys.erase(key);
    }
    for (const auto& [key, value] : keys) lines.push_back(key + "=" + value);
    FILE* file = std::fopen(Path(), "wb");
    if (!file) {
        std::printf("Settings: cannot write %s\n", Path());
        return;
    }
    for (const std::string& line : lines) std::fprintf(file, "%s\n", line.c_str());
    std::fclose(file);
    std::printf("Settings: saved display settings to %s\n", Path());
}

// Gives the save folder its own settings.ini at start: the default file copied as it is (the
// environment overrides stay out of it), or the display keys when there is none.
void CopyToSaveFolder() {
    const char* env = std::getenv("DEADNATION_CONFIG"); /* the launcher's or run.py's save folder */
    if (!env || !env[0] || Exists(Path())) return;
    const char* source = ReadPath();
    if (std::string_view(source) == Path()) {
        WriteDisplay();
        return;
    }
    FILE* in = std::fopen(source, "rb");
    if (!in) return;
    std::string data;
    char buffer[4096];
    for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), in)) > 0;) data.append(buffer, n);
    std::fclose(in);
    FILE* out = std::fopen(Path(), "wb");
    if (!out) {
        std::printf("Settings: cannot write %s\n", Path());
        return;
    }
    const bool ok = std::fwrite(data.data(), 1, data.size(), out) == data.size();
    if (std::fclose(out) != 0 || !ok) {
        std::printf("Settings: cannot write %s\n", Path());
        std::remove(Path());
        return;
    }
    std::printf("Settings: copied %s to %s\n", source, Path());
}

} // namespace

Values& Get() {
    static Values values;
    return values;
}

void Load() {
    auto& v = Get();
    const char* path = ReadPath();
    if (FILE* file = std::fopen(path, "r")) {
        char line[1024];
        while (std::fgets(line, sizeof(line), file)) {
            std::string text{line};
            text.erase(text.find_last_not_of(" \t\r\n") + 1);
            const auto eq = text.find('=');
            if (text.empty() || text[0] == '#' || eq == std::string::npos) {
                continue;
            }
            Set(v, text.substr(0, eq), text.substr(eq + 1));
        }
        std::fclose(file);
        std::printf("Settings: %s\n", path);
        if (std::string_view(path) != Path()) std::printf("Settings: changes are saved to %s\n", Path());
    }
    const std::pair<const char*, const char*> env_keys[] = {
        {"DEADNATION_DISPLAY_MODE", "display_mode"}, {"DEADNATION_VSYNC", "vsync"},
        {"DEADNATION_TEARING", "tearing"},
        {"DEADNATION_MONITOR", "monitor"},           {"DEADNATION_WINDOW", "output_resolution"},
        {"DEADNATION_OUTPUT_RESOLUTION", "output_resolution"},
        {"DEADNATION_ASPECT_RATIO", "aspect_ratio"},
        {"DEADNATION_AUDIO_CHANNELS", "audio_channels"},
        {"DEADNATION_SKIP_INTRO", "skip_intro"},     {"DEADNATION_FRAME_RATE", "frame_rate"},
        {"DEADNATION_TITLE_BACK_EXITS", "title_back_exits"},
        {"DEADNATION_RENDER_RESOLUTION", "render_resolution"},

        {"DEADNATION_SHARPENING", "sharpening"},
        {"DEADNATION_DEBUG_SPEED", "debug_speed"},
    };
    for (const auto& [env, key] : env_keys) {
        if (const char* value = std::getenv(env); value && value[0]) {
            Set(v, key, value);
        }
    }
    v.startup_monitor = v.monitor;
    std::scoped_lock lock{save_mutex};
    saved_keys = DisplayKeys();
    CopyToSaveFolder();
}

void SaveDisplay() {
    std::scoped_lock lock{save_mutex};
    WriteDisplay();
}

void SaveDisplayIfChanged() {
    std::scoped_lock lock{save_mutex};
    if (DisplayKeys() != saved_keys) WriteDisplay();
}

double DisplayRefresh() {
    static std::atomic<double> refresh{0.0};
    static std::atomic<long long> next_check{0};
    const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    if (now >= next_check.load(std::memory_order_relaxed)) {
        next_check.store(now + 500, std::memory_order_relaxed);
        refresh.store(DeadNationWindowRefreshRate(), std::memory_order_relaxed);
    }
    return refresh.load(std::memory_order_relaxed);
}

int EffectiveFrameRate() {
    const auto& v = Get();
    const int rate = v.frame_rate.load(std::memory_order_relaxed);
    const double refresh = DisplayRefresh();
    if ((!v.vsync && v.tearing) || refresh <= 0.0) return rate;
    int best = DeadNationGraphics::FrameRates[0];
    for (const int r : DeadNationGraphics::FrameRates) {
        if (r <= rate && r <= refresh + 0.5) best = std::max(best, r);
    }
    return best;
}

} // namespace DeadNationSettings

extern "C" int deadnation_skip_intro(void) {
    return DeadNationSettings::Get().skip_intro ? 1 : 0;
}

extern "C" int deadnation_title_back_exits(void) {
    return DeadNationSettings::Get().title_back_exits.load(std::memory_order_relaxed) ? 1 : 0;
}

extern "C" int deadnation_frame_rate(void) {
    return DeadNationSettings::EffectiveFrameRate();
}

extern "C" unsigned deadnation_cheats(void) {
    return DeadNationSettings::Get().cheats.load(std::memory_order_relaxed);
}

extern "C" int deadnation_debug_speed(void) {
    return DeadNationSettings::Get().debug_speed.load(std::memory_order_relaxed);
}

extern "C" void deadnation_set_frame_rate(int rate) {
    DeadNationSettings::Get().frame_rate = rate;
}

// DeadNation has no HDR output: it never asks the video out for BT.2020 PQ
// (sceVideoOutGetDeviceCapabilityInfo) and always configures SDR, and its param.sfo does not flag
// HDR. The presenter therefore always uses an SDR surface.
extern "C" int deadnation_hdr(void) {
    return 0;
}

extern "C" int deadnation_width_4k(void) {
    return DeadNationSettings::Get().startup_width_4k.load();
}

extern "C" void deadnation_set_width_4k(int width) {
    DeadNationSettings::Get().startup_width_4k = width;
}

// The lines of settings.ini render_resolution: 720, 1080, 1440 or 2160.
static int RenderRow() {
    const int r = DeadNationSettings::Get().startup_render_resolution;
    return DeadNationSettings::RenderHeights[std::clamp(r, 0, DeadNationSettings::RenderResolutionCount - 1)];
}

extern "C" int deadnation_render_4k(void) {
    return RenderRow() > 1080;
}

// Narrower than 16:9 (4:3, 16:10) the picture is the 16:9 width of the row and taller: 1920x1440
// at 4:3 and 1080p, 1920x1200 at 16:10, a multiple of 8. 16:9 and wider keep the row's lines.
extern "C" int deadnation_render_height(void) {
    const int row = RenderRow(), w4k = deadnation_width_4k();
    if (w4k >= 3840) return row;
    return ((int(row * 3840.0 / w4k + 0.5)) + 7) & ~7;
}

// The width of the output's shape (deadnation_width_4k at 2160 lines) at the row's lines, a
// multiple of 8: 1280, 1920, 2560 or 3840 at 16:9 and narrower, 3416 at 21:9 and 1440p.
extern "C" int deadnation_render_width(void) {
    const int row = RenderRow(), w4k = std::max(deadnation_width_4k(), 3840);
    return ((int(row * (w4k / 2160.0) + 0.5)) + 7) & ~7;
}
