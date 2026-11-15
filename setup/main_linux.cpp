// SPDX-License-Identifier: GPL-2.0-or-later
// deadnation-setup (Linux): sets up the release folder it sits in (tools/package.py) from the player's
// Dead Nation dump, so the game starts from deadnation or its menu entry.
//
// With no arguments it asks for the game folder (the dump), the 1.03 update folder, the save folder
// and whether to skip the intro logos (settings.ini skip_intro), installs, and offers
// a menu entry and to start the game: in the terminal it was started from, or with the desktop's
// dialogs (SDL: the folder picker and message boxes) when it has none, as from a file manager.
// deadnation runs it when paths.ini is missing or names a folder that is gone; run it again to
// change a folder.
// With --patch (deadnation, when game/ isn't prepared for this release and these eboot.bin files) it
// prepares game/ again from paths.ini's folders, with a small progress window when there is no
// terminal: 0 when done.
// With other arguments it installs from the command line:
//
//     deadnation-setup --game <dump or eboot.bin> --update <folder> [--save <folder>] [--copy-saves]
//                       [--skip-intro|--play-intro]
//                       [--shortcut <folder>|desktop|menu] [--prepare]

#include "setup.h"

#include <SDL3/SDL.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr char kTitle[] = "Dead Nation setup";

// The old save folder to copy from when the save folder changes and only the old one has saves.
std::optional<fs::path> saves_to_move(const fs::path& root, const fs::path& save) {
    auto old = setup::remembered_paths(root);
    if (!old || setup::same_path(old->save, save) || !setup::has_saves(old->save) || setup::has_saves(save))
        return std::nullopt;
    return old->save;
}

void out(const std::wstring& s) {
    std::cout << setup::narrow(s) << std::endl;
}

void err(const std::wstring& s) {
    std::cerr << setup::narrow(s) << std::endl;
}

// --- command line -------------------------------------------------------------------------------

constexpr wchar_t kUsage[] =
    L"usage: deadnation-setup --game <dump or eboot.bin> --update <folder> [--save <folder>]\n"
    L"                         [--copy-saves] [--skip-intro|--play-intro]\n"
    L"                         [--shortcut <folder>|desktop|menu] [--prepare]\n"
    L"  --game        the decrypted game dump (eboot.bin, sce_module, sce_sys, stream.dat); only ever read\n"
    L"  --save        where your saves go (default: the one paths.ini names, else save/ here)\n"
    L"  --copy-saves  when --save changes, copy the saves from the old save folder\n"
    L"  --update      the decrypted 1.03 update (patch, e.g. CUSA00236-patch), served over the dump\n"
    L"                (default: the one paths.ini names)\n"
    L"  --skip-intro, --play-intro\n"
    L"                skip the start-up logos and notices, or show them again\n"
    L"                (settings.ini skip_intro; default: leave it as it is)\n"
    L"  --shortcut    also write the game's desktop entry into this folder, onto the Desktop or into\n"
    L"                the application menu\n"
    L"  --prepare     for launchers (RecompLauncher): also print \"progress <0..1> <step>\" lines; the\n"
    L"                update is only --update's folder, if it exists (it may be update/ here)\n"
    L"  --patch       prepare the game again from paths.ini's folders (deadnation runs this)\n"
    L"Sets up the folder this program is in. With no arguments, asks for the folders.";

fs::path shortcut_folder(const std::wstring& where) {
    if (where == L"desktop")
        return setup::desktop_folder();
    if (where == L"menu")
        return setup::applications_folder();
    return setup::path_of(where);
}

int run_cli(const std::vector<std::wstring>& args) {
    std::wstring game, save, shortcut, update;
    bool copy_saves = false, prepare = false;
    std::optional<bool> skip_intro;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::wstring& a = args[i];
        std::wstring* value = a == L"--game"       ? &game
                              : a == L"--save"     ? &save
                              : a == L"--shortcut" ? &shortcut
                              : a == L"--update"   ? &update
                                                   : nullptr;
        if (value && i + 1 < args.size()) {
            *value = args[++i];
        } else if (a == L"--copy-saves") {
            copy_saves = true;
        } else if (a == L"--skip-intro" || a == L"--play-intro") {
            skip_intro = a == L"--skip-intro";
        } else if (a == L"--prepare") {
            prepare = true;
        } else {
            err(a == L"--help" || a == L"-h" ? kUsage : std::wstring(kUsage) + L"\n\nunexpected: " + a);
            return 2;
        }
    }
    if (game.empty()) {
        err(std::wstring(kUsage) + L"\n\n--game is required");
        return 2;
    }
    try {
        setup::Options opt;
        opt.root = setup::exe_folder();
        opt.game = setup::path_of(game);
        auto old = setup::remembered_paths(opt.root);
        opt.save = !save.empty() ? setup::path_of(save) : old ? old->save : setup::default_save();
        opt.update = !update.empty() ? setup::path_of(update) : old ? old->update : fs::path();
        // --prepare: the launcher names the update each time (the folder it put the player's in).
        if (prepare) {
            opt.update = setup::update_in(setup::path_of(update));
            opt.update_in_release = true;
        }
        if (copy_saves)
            opt.copy_saves_from = saves_to_move(opt.root, opt.save);
        opt.skip_intro = skip_intro;
        fs::path exe = setup::install(opt, [&](const std::wstring& line) {
            out(line);
            if (std::wstring step = setup::progress_line(line); prepare && !step.empty())
                out(step);
        });
        if (!shortcut.empty()) {
            fs::path entry = shortcut_folder(shortcut) / setup::kShortcutName;
            setup::create_shortcut(exe, entry);
            out(L"shortcut " + setup::text(entry));
        }
        out(L"\nDone. Run " + setup::text(exe));
        return 0;
    } catch (const setup::Error& e) {
        err(e.message);
        return 1;
    }
}

// --- asking -------------------------------------------------------------------------------------

// How the no-argument setup asks its questions.
class Asker {
public:
    virtual ~Asker() = default;
    // A folder (or the dump's eboot.bin), starting from `current`; nullopt when the player cancels.
    virtual std::optional<fs::path> folder(const std::wstring& title, const fs::path& current) = 0;
    // The index of the answer picked (the first is the default); -1 when the player cancels.
    virtual int choose(const std::wstring& question, const std::vector<std::wstring>& answers) = 0;
    virtual void tell(const std::wstring& message, bool error) = 0;
    // A line of the install's output.
    virtual void log(const std::wstring& line) = 0;
};

// Questions in the terminal the setup was started from.
class TerminalAsker : public Asker {
public:
    std::optional<fs::path> folder(const std::wstring& title, const fs::path& current) override {
        for (;;) {
            out(title + (current.empty() ? L"" : L"\n  (Enter keeps " + setup::text(current) + L")"));
            std::cout << "> " << std::flush;
            std::string line;
            if (!std::getline(std::cin, line))
                return std::nullopt;
            std::wstring value = setup::widen(line);
            size_t a = value.find_first_not_of(L" \t'\""), b = value.find_last_not_of(L" \t'\"");
            value = a == std::wstring::npos ? L"" : value.substr(a, b - a + 1);
            if (value.empty() && !current.empty())
                return current;
            if (value.empty())
                continue;
            if (value.rfind(L"~/", 0) == 0)
                if (const char* home = std::getenv("HOME"))
                    value = setup::widen(home) + value.substr(1);
            return setup::path_of(value);
        }
    }
    int choose(const std::wstring& question, const std::vector<std::wstring>& answers) override {
        for (;;) {
            out(question);
            for (size_t i = 0; i < answers.size(); ++i)
                out(L"  " + std::to_wstring(i + 1) + L") " + answers[i] + (i ? L"" : L" (Enter)"));
            std::cout << "> " << std::flush;
            std::string line;
            if (!std::getline(std::cin, line))
                return -1;
            if (line.empty())
                return 0;
            char* end = nullptr;
            long n = std::strtol(line.c_str(), &end, 10);
            if (end != line.c_str() && n >= 1 && n <= static_cast<long>(answers.size()))
                return static_cast<int>(n - 1);
        }
    }
    void tell(const std::wstring& message, bool error) override { (error ? err : out)(L"\n" + message + L"\n"); }
    void log(const std::wstring& line) override { out(line); }
};

// The desktop's dialogs, through SDL.
class DialogAsker : public Asker {
public:
    std::optional<fs::path> folder(const std::wstring& title, const fs::path& current) override {
        struct Pick {
            std::atomic<bool> done{false};
            std::optional<fs::path> path;
            std::string error;
        } pick;
        std::string title_text = setup::narrow(title), location = current.native();
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, title_text.c_str());
        std::error_code ec;
        fs::path start = current.empty() ? fs::path() : setup::game_folder(current);
        if (!start.empty() && fs::is_directory(start, ec))
            SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, start.c_str());
        // The callback may run on another thread; `done` is set last.
        auto picked = [](void* data, const char* const* files, int) {
            auto* p = static_cast<Pick*>(data);
            if (!files)
                p->error = SDL_GetError();
            else if (files[0])
                p->path = fs::path(files[0]);
            p->done = true;
        };
        SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFOLDER, picked, &pick, props);
        while (!pick.done) {
            SDL_Event e;
            SDL_WaitEventTimeout(&e, 50);
        }
        SDL_DestroyProperties(props);
        if (!pick.error.empty())
            tell(L"No folder picker is available (" + setup::widen(pick.error) +
                     L").\n\nRun deadnation-setup in a terminal, or with --game <folder>.",
                 true);
        return pick.path;
    }
    int choose(const std::wstring& question, const std::vector<std::wstring>& answers) override {
        std::string message = setup::narrow(question);
        std::vector<std::string> labels;
        for (const std::wstring& a : answers)
            labels.push_back(setup::narrow(a));
        std::vector<SDL_MessageBoxButtonData> buttons;
        for (size_t i = 0; i < labels.size(); ++i) {
            SDL_MessageBoxButtonFlags flags = i == 0                    ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT
                                              : i + 1 == labels.size() ? SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT
                                                                       : 0;
            buttons.push_back({flags, static_cast<int>(i), labels[i].c_str()});
        }
        SDL_MessageBoxData box = {SDL_MESSAGEBOX_INFORMATION, nullptr, kTitle, message.c_str(),
                                  static_cast<int>(buttons.size()), buttons.data(), nullptr};
        int picked = -1;
        return SDL_ShowMessageBox(&box, &picked) ? picked : -1;
    }
    void tell(const std::wstring& message, bool error) override {
        (error ? err : out)(message);
        std::wstring text = message;
        if (error && !lines_.empty()) {
            text += L"\n\nThe last lines of the setup's output:";
            for (const std::wstring& line : lines_)
                text += L"\n" + line;
        }
        SDL_ShowSimpleMessageBox(error ? SDL_MESSAGEBOX_ERROR : SDL_MESSAGEBOX_INFORMATION, kTitle,
                                 setup::narrow(text).c_str(), nullptr);
    }
    void log(const std::wstring& line) override {
        out(line);
        lines_.push_back(line);
        if (lines_.size() > 12)
            lines_.pop_front();
    }

private:
    std::deque<std::wstring> lines_;
};

// Replaces this process with the game, started in its folder as its menu entry would.
void play(Asker& ask, const fs::path& root) {
    fs::path exe = root / setup::kGameExe;
    SDL_Quit();
    if (!chdir(root.c_str()))
        execl(exe.c_str(), exe.c_str(), static_cast<char*>(nullptr));
    ask.tell(L"Cannot start " + setup::text(exe), true);
}

int run_asking(Asker& ask) {
    fs::path root = setup::exe_folder();
    std::optional<setup::Paths> paths;
    try {
        paths = setup::remembered_paths(root);
    } catch (const setup::Error&) {
    }
    if (setup::ready(root)) {
        int answer = ask.choose(L"The game is set up.\n\nGame folder: " + setup::text(paths->game) +
                                    L"\nSave folder: " + setup::text(paths->save) +
                                    (paths->update.empty() ? L"" : L"\nUpdate folder: " + setup::text(paths->update)),
                                {L"Play Game", L"Set up again", L"Exit"});
        if (answer == 0) {
            play(ask, root);
            return 1;
        }
        if (answer != 1)
            return 0;
    }

    std::optional<fs::path> game =
        ask.folder(L"Select the game folder (your game dump, the one with eboot.bin)", paths ? paths->game : fs::path());
    if (!game)
        return 1;
    // The 1.03 update (patch) served over the dump: required.
    fs::path update = paths ? paths->update : fs::path();
    int patched = update.empty() ? 1
                                 : ask.choose(L"Use the update in this folder?\n\n" + setup::text(update),
                                              {L"Yes", L"Choose another folder", L"Cancel"});
    if (patched < 0 || patched == 2)
        return 1;
    if (patched == 1) {
        std::optional<fs::path> picked =
            ask.folder(L"Select the game's 1.03 update folder (e.g. CUSA00236-patch, the one with eboot.bin)", update);
        if (!picked)
            return 1;
        update = *picked;
    }
    fs::path save = paths ? paths->save : setup::default_save();
    int keep = ask.choose(L"Keep your saves in this folder?\n\n" + setup::text(save),
                          {L"Yes", L"Choose another folder", L"Cancel"});
    if (keep == 1) {
        std::optional<fs::path> picked = ask.folder(L"Select the save folder", save);
        if (!picked)
            return 1;
        save = *picked;
    } else if (keep != 0) {
        return 1;
    }
    // The current choice (settings.ini skip_intro) is the default answer; Install writes the new one.
    const bool skipping = setup::skip_intro(root, save);
    std::vector<std::wstring> answers = {L"Skip it", L"Play it"};
    if (!skipping)
        std::swap(answers[0], answers[1]);
    int skip = ask.choose(L"Skip the intro movies (set skip_intro=0 in settings.ini to bring them back)?", answers);
    if (skip < 0)
        return 1;

    setup::Options opt;
    opt.root = root;
    opt.game = *game;
    opt.save = save;
    opt.skip_intro = (skip == 0) == skipping;
    opt.update = update; // install() checks the versions
    try {
        if (auto old = saves_to_move(opt.root, opt.save)) {
            int answer = ask.choose(L"Copy your saves to the new save folder?\n\nFrom: " + setup::text(*old) +
                                        L"\nTo: " + setup::text(save),
                                    {L"Yes", L"No", L"Cancel"});
            if (answer < 0 || answer == 2)
                return 1;
            if (answer == 0)
                opt.copy_saves_from = old;
        }
        fs::path exe = setup::install(opt, [&](const std::wstring& line) { ask.log(line); });
        if (ask.choose(L"Done!\n\nAdd the game to your application menu?", {L"Yes", L"No"}) == 0) {
            fs::path entry = setup::applications_folder() / setup::kShortcutName;
            setup::create_shortcut(exe, entry);
            ask.log(L"Menu entry " + setup::text(entry));
        }
    } catch (const setup::Error& e) {
        ask.tell(e.message, true);
        return 1;
    }
    if (ask.choose(L"Start the game now?", {L"Play Game", L"Exit"}) == 0) {
        play(ask, root);
        return 1;
    }
    return 0;
}

// --- --patch ------------------------------------------------------------------------------------
// deadnation found game/ not prepared for this release or these eboot.bin files: prepare it again
// from paths.ini's folders. Without a terminal, a small window shows the step and a red bar (SDL's
// renderer and its built-in text), and a message box says why when it fails: when the files aren't
// the supported ones (install() checks them first), the game does not start.

struct Progress {
    std::mutex lock;
    double done = 0;
    std::string step = "Checking your game files";
    std::wstring error;
    bool ok = false;
    std::atomic<bool> finished{false};
};

void draw_patch(SDL_Renderer* r, Progress& p) {
    double done;
    std::string step;
    {
        std::lock_guard<std::mutex> hold(p.lock);
        done = p.done;
        step = p.step;
    }
    SDL_SetRenderDrawColor(r, 0x20, 0x00, 0x00, 0xff);
    SDL_RenderClear(r);
    SDL_SetRenderScale(r, 2.0f, 2.0f);
    SDL_SetRenderDrawColor(r, 0xff, 0xff, 0xff, 0xff);
    SDL_RenderDebugText(r, 12, 14, "PREPARING DEAD NATION");
    SDL_SetRenderScale(r, 1.0f, 1.0f);
    SDL_SetRenderDrawColor(r, 0xd6, 0xd6, 0xd6, 0xff);
    SDL_RenderDebugText(r, 24, 78, step.c_str());
    SDL_FRect bar = {24, 100, 472, 22};
    SDL_SetRenderDrawColor(r, 0x3c, 0x3c, 0x3c, 0xff);
    SDL_RenderFillRect(r, &bar);
    SDL_FRect filled = {bar.x, bar.y, bar.w * static_cast<float>(std::clamp(done, 0.0, 1.0)), bar.h};
    SDL_SetRenderDrawColor(r, 0xd8, 0x10, 0x10, 0xff);
    SDL_RenderFillRect(r, &filled);
    SDL_SetRenderDrawColor(r, 0x5a, 0x5a, 0x5a, 0xff);
    SDL_RenderRect(r, &bar);
    SDL_RenderPresent(r);
}

int run_patch() {
    fs::path root = setup::exe_folder();
    std::optional<setup::Paths> paths = setup::remembered_paths(root);
    bool window = !isatty(STDOUT_FILENO) && !isatty(STDERR_FILENO);
    if (window) {
        SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
        window = SDL_Init(SDL_INIT_VIDEO);
    }
    auto fail = [&](const std::wstring& message) {
        err(message);
        if (window) {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, kTitle, setup::narrow(message).c_str(), nullptr);
            SDL_Quit();
        }
        return 1;
    };
    if (!paths)
        return fail(L"The game is not set up yet. Run deadnation-setup.");
    setup::Options opt;
    opt.root = root;
    opt.game = paths->game;
    opt.save = paths->save;
    opt.update = paths->update;
    opt.update_in_release = true; // RecompLauncher's update/ folder, if it put the update there

    Progress p;
    auto work = [&] {
        try {
            setup::install(opt, [&](const std::wstring& line) {
                out(line);
                std::wstring step = setup::progress_line(line);
                if (step.empty())
                    return;
                size_t space = step.find(L' ', 9);
                std::lock_guard<std::mutex> hold(p.lock);
                p.done = std::wcstod(step.c_str() + 9, nullptr);
                p.step = space == std::wstring::npos ? std::string() : setup::narrow(step.substr(space + 1));
            });
            p.ok = true;
        } catch (const setup::Error& e) {
            p.error = e.message;
        }
        p.finished = true;
    };
    SDL_Window* w = nullptr;
    SDL_Renderer* r = nullptr;
    if (window && SDL_CreateWindowAndRenderer(kTitle, 520, 150, 0, &w, &r)) {
        std::thread worker(work);
        while (!p.finished) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
            } // closing waits for the install to end
            draw_patch(r, p);
            SDL_Delay(33);
        }
        worker.join();
        SDL_DestroyRenderer(r);
        SDL_DestroyWindow(w);
    } else {
        work();
    }
    if (!p.ok)
        return fail(p.error);
    if (window)
        SDL_Quit();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::wstring> args;
    for (int i = 1; i < argc; ++i)
        args.push_back(setup::widen(argv[i]));
    if (args.size() == 1 && args[0] == L"--patch")
        return run_patch();
    if (!args.empty())
        return run_cli(args);
    if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) {
        TerminalAsker ask;
        out(L"Dead Nation setup (Ctrl+C or Ctrl+D cancels)\n");
        return run_asking(ask);
    }
    // SIGTERM and SIGINT end the setup as usual (SDL would turn them into a quit event nobody reads).
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        err(std::wstring(kUsage) + L"\n\nNo terminal and no display (" + setup::widen(SDL_GetError()) + L").");
        return 2;
    }
    DialogAsker ask;
    int rc = run_asking(ask);
    SDL_Quit();
    return rc;
}
