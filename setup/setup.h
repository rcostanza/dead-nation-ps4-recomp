// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// The install itself, in the release folder the setup exe sits in (tools/package.py): prepares the game
// image from the player's dump with prepare/run.py, and writes paths.ini (the dump, the save folder, an
// optional update folder, read by src/launcher.c). The dump is only ever read. Windows (main.cpp) and Linux (main_linux.cpp)
// share it.

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace setup {

// Why an install stopped; the text is meant for the user.
struct Error {
    std::wstring message;
};

struct Options {
    std::filesystem::path root; // the release folder: the game exe, prepare/, and what the setup adds
    std::filesystem::path game; // the dump folder or its eboot.bin
    std::filesystem::path save; // saves and the launch log (run.py's --user)
    std::optional<std::filesystem::path> copy_saves_from; // the old save folder, whose saves go to `save`
    std::optional<bool> skip_intro; // settings.ini skip_intro to write; unset keeps what is there
    std::filesystem::path update; // the decrypted 1.03 update (patch) served over the dump; required
    bool update_in_release = false; // --prepare: the update may be root/update (RecompLauncher puts it there)
};

// The folders a release folder's paths.ini names.
struct Paths {
    std::filesystem::path game, save;
    std::filesystem::path update; // empty when paths.ini names none
};

// Save data in a save folder (what the runtime keeps there apart from the log).
constexpr const wchar_t* kSaveFolders[] = {L"savedata"};

using Log = std::function<void(const std::wstring&)>;

#ifdef _WIN32
constexpr wchar_t kGameExe[] = L"deadnation.exe";
constexpr wchar_t kShortcutName[] = L"DeadNation.lnk";
#else
constexpr wchar_t kGameExe[] = L"deadnation";
// A desktop entry (freedesktop.org), in the application menu or on the Desktop.
constexpr wchar_t kShortcutName[] = L"deadnation.desktop";
#endif

// A path as text for the user and paths.ini, and text back as a path. Linux paths are UTF-8 bytes,
// which these convert without depending on the locale.
std::wstring text(const std::filesystem::path& p);
std::filesystem::path path_of(const std::wstring& s);
// UTF-8 to and from the wide text this setup uses.
std::wstring widen(std::string_view s);
std::string narrow(std::wstring_view s);

// The folder this exe is in.
std::filesystem::path exe_folder();
// The dump folder for a path to it or to its eboot.bin (absolute, normalized).
std::filesystem::path game_folder(const std::filesystem::path& path);
// The files an DeadNation dump needs that `game` (a path game_folder() accepts) lacks; empty for a dump.
std::vector<std::wstring> missing_game_files(const std::filesystem::path& game);
// True when `update` is a decrypted update folder (eboot.bin and sce_sys/param.sfo).
bool is_update_folder(const std::filesystem::path& update);
// Why this dump and update (empty for none) aren't exactly the ones the release runs (the CUSA00236
// 1.00 dump with its 1.03 update, compared by eboot.bin's sha256); empty when they are. install()
// refuses them. Meant for the user.
std::wstring version_error(const std::filesystem::path& game, const std::filesystem::path& update);
// What `root`/paths.ini names, if it names both folders.
std::optional<Paths> read_paths(const std::filesystem::path& root);
// read_paths(root): the folders to start from. Nothing is kept outside the release folder.
std::optional<Paths> remembered_paths(const std::filesystem::path& root);
// save/ in the release folder: a release is never zipped with one, so unpacking a newer release over
// the folder keeps the saves.
std::filesystem::path default_save();
// True when `root` is set up for its release: paths.ini names the folders and game/ was prepared
// for this release.txt and these eboot.bin files (src/prepared_stamp.h; what src/launcher.c checks
// before it starts the game).
bool ready(const std::filesystem::path& root);
// True when `save` holds game saves (a savedata folder with something in it).
bool has_saves(const std::filesystem::path& save);
// settings.ini skip_intro as the game reads it: from the save folder's file when there is one,
// else the release folder's; false when unset.
bool skip_intro(const std::filesystem::path& root, const std::filesystem::path& save);
bool same_path(const std::filesystem::path& a, const std::filesystem::path& b);
// `key`'s value in a settings.ini (lower case key): the last line that sets it wins, as in the game;
// unset when no line sets it or the file is missing. Throws Error when it cannot be read.
std::optional<std::wstring> read_setting(const std::filesystem::path& ini, const std::wstring& key);
// Sets `key` in a settings.ini: its first line is replaced and any later ones dropped, or the line is
// added at the end; the other lines (comments too) and their line endings stay. Throws Error.
void write_setting(const std::filesystem::path& ini, const std::wstring& key, const std::wstring& value);

// --prepare's update: `named` when it is an update folder, else the update folder directly inside it
// (where a launcher may have put the one the player picked); empty when there is none.
std::filesystem::path update_in(const std::filesystem::path& named);
// The "progress <0..1> <step>" line a launcher's setup screen shows for a line install() logs
// (--prepare, RecompLauncher); empty for a line that starts no new step.
std::wstring progress_line(const std::wstring& line);

// Sets up opt.root, reporting each step through `log`. Throws Error. Returns the game exe's path.
std::filesystem::path install(const Options& opt, const Log& log);

// Writes a shortcut at `lnk` that starts `target` in its own folder. Throws Error.
void create_shortcut(const std::filesystem::path& target, const std::filesystem::path& lnk);
// The user's Desktop folder. Throws Error.
std::filesystem::path desktop_folder();
#ifndef _WIN32
// Where desktop entries go to show up in the application menu (and Steam's "Add a Non-Steam Game").
std::filesystem::path applications_folder();
#endif

} // namespace setup
