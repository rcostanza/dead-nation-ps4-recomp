// SPDX-License-Identifier: GPL-2.0-or-later
// deadnation-setup.exe: sets up the release folder it sits in (tools/package.py) from the player's
// Dead Nation dump, so the game starts from deadnation.exe or a desktop shortcut.
//
// With no arguments it opens a window: the game folder (the dump), the update folder, the save folder, whether to skip the intro logos (settings.ini skip_intro), Install and Exit; after an
// install it offers a desktop shortcut. Run it again to change a folder, or after unpacking a newer
// release over the folder.
// With arguments it installs from the command line:
//
//     deadnation-setup.exe --game <dump or eboot.bin> [--update <folder>|--no-update]
//                           [--save <folder>] [--copy-saves] [--skip-intro|--play-intro]
//                           [--shortcut <folder>|desktop]

#include "setup.h"

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr wchar_t kTitle[] = L"Dead Nation setup";

// The old save folder to copy from when the save folder changes and only the old one has saves.
std::optional<fs::path> saves_to_move(const fs::path& root, const fs::path& save) {
    auto old = setup::remembered_paths(root);
    if (!old || setup::same_path(old->save, save) || !setup::has_saves(old->save) || setup::has_saves(save))
        return std::nullopt;
    return old->save;
}

// --- command line -------------------------------------------------------------------------------

// Writes to the standard handles a parent gave us, or else to the console of the shell we were started from.
class Console {
public:
    Console() {
        out_ = GetStdHandle(STD_OUTPUT_HANDLE);
        err_ = GetStdHandle(STD_ERROR_HANDLE);
        if ((!out_ || !err_) && AttachConsole(ATTACH_PARENT_PROCESS)) {
            HANDLE con = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_WRITE, nullptr,
                                     OPEN_EXISTING, 0, nullptr);
            if (!out_)
                out_ = con;
            if (!err_)
                err_ = con;
        }
    }
    void out(const std::wstring& s) const { write(out_, s + L"\n"); }
    void err(const std::wstring& s) const { write(err_, s + L"\n"); }

private:
    static void write(HANDLE h, const std::wstring& s) {
        if (!h || h == INVALID_HANDLE_VALUE)
            return;
        DWORD mode, n;
        if (GetConsoleMode(h, &mode)) {
            WriteConsoleW(h, s.data(), static_cast<DWORD>(s.size()), &n, nullptr);
            return;
        }
        int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
        std::string u(len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), u.data(), len, nullptr, nullptr);
        WriteFile(h, u.data(), static_cast<DWORD>(u.size()), &n, nullptr);
    }
    HANDLE out_ = nullptr;
    HANDLE err_ = nullptr;
};

constexpr wchar_t kUsage[] =
    L"usage: deadnation-setup.exe --game <dump or eboot.bin> [--update <folder>|--no-update]\n"
    L"                             [--save <folder>] [--copy-saves] [--skip-intro|--play-intro]\n"
    L"                             [--shortcut <folder>|desktop]\n"
    L"  --game        the decrypted game dump (eboot.bin, sce_module, sce_sys, stream.dat); only ever read\n"
    L"  --update      the decrypted update (patch, e.g. CUSA00236-patch), served over the dump; its\n"
    L"                version is the one played (default: the one paths.ini names); --no-update plays\n"
    L"                the dump's own version\n"
    L"  --save        where your saves go (default: the one paths.ini names, else save\\ here)\n"
    L"  --copy-saves  when --save changes, copy the saves from the old save folder\n"
    L"  --skip-intro, --play-intro\n"
    L"                skip the start-up logos and notices, or show them again\n"
    L"                (settings.ini skip_intro; default: leave it as it is)\n"
    L"  --shortcut    also write the game's shortcut into this folder, or onto the Desktop\n"
    L"Sets up the folder this exe is in. With no arguments, opens the setup window.";

int run_cli(const std::vector<std::wstring>& args) {
    Console con;
    std::wstring game, save, shortcut, update;
    bool copy_saves = false, no_update = false;
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
        } else if (a == L"--no-update") {
            no_update = true;
        } else {
            con.err(a == L"--help" || a == L"-h" ? kUsage : std::wstring(kUsage) + L"\n\nunexpected: " + a);
            return 2;
        }
    }
    if (game.empty()) {
        con.err(std::wstring(kUsage) + L"\n\n--game is required");
        return 2;
    }
    try {
        setup::Options opt;
        opt.root = setup::exe_folder();
        opt.game = game;
        auto old = setup::remembered_paths(opt.root);
        opt.save = !save.empty() ? fs::path(save) : old ? old->save : setup::default_save();
        opt.update = no_update ? fs::path() : !update.empty() ? fs::path(update) : old ? old->update : fs::path();
        if (copy_saves)
            opt.copy_saves_from = saves_to_move(opt.root, opt.save);
        opt.skip_intro = skip_intro;
        if (std::wstring warning = setup::version_warning(opt.game, opt.update); !warning.empty())
            con.err(L"Warning:\n" + warning + L"\n");
        fs::path exe = setup::install(opt, [&](const std::wstring& line) { con.out(line); });
        if (!shortcut.empty()) {
            fs::path lnk = (shortcut == L"desktop" ? setup::desktop_folder() : fs::path(shortcut)) / setup::kShortcutName;
            setup::create_shortcut(exe, lnk);
            con.out(L"shortcut " + lnk.wstring());
        }
        con.out(L"\nDone. Run " + exe.wstring());
        return 0;
    } catch (const setup::Error& e) {
        con.err(e.message);
        return 1;
    }
}

// --- window -------------------------------------------------------------------------------------
// Drawn as the game's own menus, like settings.exe and the in-game Display Settings page
// (gpu/shim/deadnation_page.h, its Srgb palette): near black with a dark red glow down the left, the
// title in heavy white capitals at the top left, the folders as dark grey bars in a brushed silver
// frame with the focused one on the blood red bar, white Roboto Condensed (embedded, setup.rc) with a
// dark shadow, and the buttons on the dark strip at the bottom right. Install stays disabled until the
// game folder holds a Dead Nation dump, the update folder (optional) holds an update when one is
// named, and a save folder is named.

enum : int {
    kGameEdit = 101,
    kGameBrowse,
    kUpdateEdit,
    kUpdateBrowse,
    kSaveEdit,
    kSaveBrowse,
    kPlay,
    kSkipIntro,
};
enum : UINT {
    kMsgLog = WM_APP + 1, // lParam: std::wstring* line
    kMsgDone,             // wParam: 1 ok; lParam: std::wstring* exe path or error
};
enum : int { kFontLight = 101, kFontBold = 102 }; // setup.rc

namespace color {
constexpr COLORREF BackRed = RGB(0x46, 0x00, 0x00), BackEdge = RGB(0x00, 0x00, 0x00), Panel = RGB(0x0d, 0x0d, 0x0d);
constexpr COLORREF FrameLight = RGB(0xdc, 0xdc, 0xdc), FrameDark = RGB(0x8a, 0x8a, 0x8a);
constexpr COLORREF RowTop = RGB(0x3c, 0x3c, 0x3c), RowLow = RGB(0x18, 0x18, 0x18), RowEdge = RGB(0x5a, 0x5a, 0x5a);
constexpr COLORREF FocusTop = RGB(0x9a, 0x00, 0x00), FocusMid = RGB(0xd8, 0x10, 0x10), FocusLow = RGB(0x6e, 0x00, 0x00);
constexpr COLORREF FocusEdge = RGB(0xff, 0x96, 0x96);
constexpr COLORREF Text = RGB(0xff, 0xff, 0xff), Dim = RGB(0xd6, 0xd6, 0xd6), Off = RGB(0x4f, 0x4f, 0x4f);
constexpr COLORREF Accent = RGB(0xe8, 0x38, 0x38), Help = RGB(0xc4, 0xc4, 0xc4), Strip = RGB(0x08, 0x08, 0x08);
constexpr COLORREF Shadow = RGB(0x00, 0x00, 0x00), Field = RGB(0x0a, 0x0a, 0x0a);
constexpr COLORREF Bad = RGB(0xff, 0x6e, 0x6e), BadOnFocus = RGB(0xff, 0xc8, 0xc8);
} // namespace color

// One folder row: its label, the field and Browse, and what the check says about the folder.
struct Folder {
    const wchar_t* label;
    int y;
    HWND edit = nullptr, browse = nullptr;
    std::wstring note;
    bool ok = false;
};

struct Ui {
    HWND wnd = nullptr, play = nullptr, install = nullptr, exit = nullptr, skip_intro = nullptr;
    Folder game{L"Game folder", 86}, update{L"Update folder (optional)", 164}, save{L"Save folder", 242};
    std::vector<HWND> inputs; // disabled while installing (Play Game too, and until the folder is set up)
    HFONT body = nullptr, label = nullptr, title = nullptr, hint = nullptr;
    HBRUSH field = nullptr;
    HWND focus = nullptr; // the focused control, whose row is drawn on the red bar
    UINT dpi = 96;
    bool busy = false, skip = true;
    std::wstring status;
    bool status_bad = false;
    fs::path root;
};
Ui ui;

// Layout in 96-dpi units; the client area is kWidth x kHeight.
constexpr int kWidth = 720, kHeight = 492;
constexpr int kPanelLeft = 28, kPanelRight = 692, kPanelTop = 72, kPanelBottom = 370;
constexpr int kRowLeft = 40, kRowRight = 680, kRowHeight = 72, kFieldLeft = 58, kBrowseLeft = 564, kBrowseWidth = 104;
constexpr int kSkipTop = 320, kSkipHeight = 40;
constexpr int kStripTop = 432, kStripHeight = 46, kButtonWidth = 116;

int px(int v) {
    return MulDiv(v, static_cast<int>(ui.dpi), 96);
}

RECT rect(int x, int y, int w, int h) {
    return {px(x), px(y), px(x + w), px(y + h)};
}

std::wstring text_of(HWND h) {
    std::wstring s(static_cast<size_t>(GetWindowTextLengthW(h)), L'\0');
    GetWindowTextW(h, s.data(), static_cast<int>(s.size()) + 1);
    size_t a = s.find_first_not_of(L" \t\""), b = s.find_last_not_of(L" \t\"");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}

// --- drawing ---

// `r` filled from `a` to `b`, top to bottom (GRADIENT_FILL_RECT_V) or left to right (_H).
void gradient(HDC dc, RECT r, COLORREF a, COLORREF b, ULONG mode = GRADIENT_FILL_RECT_V) {
    TRIVERTEX v[2] = {
        {r.left, r.top, static_cast<COLOR16>(GetRValue(a) << 8), static_cast<COLOR16>(GetGValue(a) << 8),
         static_cast<COLOR16>(GetBValue(a) << 8), 0},
        {r.right, r.bottom, static_cast<COLOR16>(GetRValue(b) << 8), static_cast<COLOR16>(GetGValue(b) << 8),
         static_cast<COLOR16>(GetBValue(b) << 8), 0},
    };
    GRADIENT_RECT g = {0, 1};
    GradientFill(dc, v, 2, &g, 1, mode);
}

void fill(HDC dc, RECT r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

void frame(HDC dc, RECT r, COLORREF c, int width = 1) {
    HPEN pen = CreatePen(PS_INSIDEFRAME, width, c);
    HGDIOBJ old_pen = SelectObject(dc, pen), old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(pen);
}

// Text with the dark shadow the game's menus give their type.
void text(HDC dc, const std::wstring& s, RECT r, HFONT font, COLORREF c, UINT format, bool shadow = true) {
    HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    if (shadow) {
        const int d = std::max(1, px(1));
        RECT sr = {r.left + d, r.top + d, r.right + d, r.bottom + d};
        SetTextColor(dc, color::Shadow);
        DrawTextW(dc, s.c_str(), static_cast<int>(s.size()), &sr, format | DT_NOPREFIX);
    }
    SetTextColor(dc, c);
    DrawTextW(dc, s.c_str(), static_cast<int>(s.size()), &r, format | DT_NOPREFIX);
    SelectObject(dc, old);
}

int text_width(HDC dc, const std::wstring& s, HFONT font) {
    HGDIOBJ old = SelectObject(dc, font);
    SIZE size = {};
    GetTextExtentPoint32W(dc, s.c_str(), static_cast<int>(s.size()), &size);
    SelectObject(dc, old);
    return size.cx;
}

// An item's bar: dark grey, lighter at its top, with a thin light edge above.
void row_bar(HDC dc, RECT r) {
    gradient(dc, r, color::RowTop, color::RowLow);
    fill(dc, {r.left, r.top, r.right, r.top + std::max(1, px(1))}, color::RowEdge);
}

// The focused item's bar: blood red, brightest a little above its middle, with a light red edge on top.
void focus_bar(HDC dc, RECT r) {
    const LONG mid = r.top + (r.bottom - r.top) * 2 / 5;
    gradient(dc, {r.left, r.top, r.right, mid}, color::FocusTop, color::FocusMid);
    gradient(dc, {r.left, mid, r.right, r.bottom}, color::FocusMid, color::FocusLow);
    fill(dc, {r.left, r.top, r.right, r.top + std::max(1, px(1))}, color::FocusEdge);
}

// A panel as the game frames its lists: a brushed silver frame, light at the top and darker below,
// around a near black inside.
void panel(HDC dc, RECT r) {
    const int border = px(4);
    gradient(dc, r, color::FrameLight, color::FrameDark);
    RECT inside = {r.left + border, r.top + border, r.right - border, r.bottom - border};
    fill(dc, inside, color::Panel);
    frame(dc, inside, color::Shadow, std::max(1, px(1)));
}

// An arrow ("<" for dir -1, ">" for 1) as one stroke, centred on (x, y).
void chevron(HDC dc, int x, int y, int size, int dir, COLORREF c) {
    const int w = size * 28 / 100 * dir, h = size / 2;
    HPEN pen = CreatePen(PS_SOLID, std::max(1, px(2)), c);
    HGDIOBJ old = SelectObject(dc, pen);
    POINT pts[3] = {{x - w, y - h}, {x + w, y}, {x - w, y + h}};
    Polyline(dc, pts, 3);
    SelectObject(dc, old);
    DeleteObject(pen);
}

bool has_focus(const Folder& f) {
    return ui.focus && (ui.focus == f.edit || ui.focus == f.browse);
}

// The box the edit sits in: a near black field on the row.
RECT field_box(const Folder& f) {
    return rect(kFieldLeft, f.y + 36, kBrowseLeft - kFieldLeft - 10, 28);
}

void paint_folder(HDC dc, const Folder& f) {
    const bool on = has_focus(f);
    RECT bar = rect(kRowLeft, f.y, kRowRight - kRowLeft, kRowHeight);
    on ? focus_bar(dc, bar) : row_bar(dc, bar);
    text(dc, f.label, rect(kFieldLeft, f.y + 5, 300, 28), ui.label, color::Text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    text(dc, f.note, rect(kFieldLeft + 300, f.y + 5, kRowRight - 16 - kFieldLeft - 300, 28), ui.body,
         f.ok ? (on ? color::Text : color::Dim) : (on ? color::BadOnFocus : color::Bad),
         DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT box = field_box(f);
    fill(dc, box, color::Field);
    frame(dc, box, on ? color::FocusEdge : color::RowEdge);
}

void paint(HDC dc) {
    RECT client;
    GetClientRect(ui.wnd, &client);
    gradient(dc, client, color::BackRed, color::BackEdge, GRADIENT_FILL_RECT_H);
    text(dc, L"DEAD NATION SETUP", rect(kPanelLeft + 2, 2, kPanelRight - kPanelLeft, kPanelTop - 8), ui.title, color::Text,
         DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
    panel(dc, rect(kPanelLeft, kPanelTop, kPanelRight - kPanelLeft, kPanelBottom - kPanelTop));
    for (const Folder* f : {&ui.game, &ui.update, &ui.save})
        paint_folder(dc, *f);
    text(dc, ui.status, rect(kRowLeft, kPanelBottom + 10, kRowRight - kRowLeft, kStripTop - kPanelBottom - 14), ui.body,
         ui.status_bad ? color::Accent : color::Help, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
    // The hint strip: the keyboard's keys at its left, the buttons (child windows) at its right.
    fill(dc, rect(kPanelLeft, kStripTop, kWidth - kPanelLeft, kStripHeight), color::Strip);
    text(dc, L"TAB  MOVE    ENTER  INSTALL    ESC  EXIT", rect(kPanelLeft + 14, kStripTop, 280, kStripHeight), ui.hint,
         color::Help, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

// The Skip intro row: a setting as the game's settings pages show one, its name on the left and its
// value on the right, on the red bar with arrows around the value when focused.
void draw_skip_intro(HDC dc, RECT r, bool focus, bool disabled) {
    focus ? focus_bar(dc, r) : row_bar(dc, r);
    const COLORREF ink = disabled ? color::Off : color::Text;
    text(dc, L"Skip intro", {r.left + px(kFieldLeft - kRowLeft), r.top, r.right, r.bottom}, ui.label, ink,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE, !disabled);
    const std::wstring value = ui.skip ? L"On" : L"Off";
    const int value_right = r.right - px(40), value_left = value_right - text_width(dc, value, ui.label);
    text(dc, value, {value_left, r.top, value_right, r.bottom}, ui.label, disabled ? color::Off : focus ? color::Text : color::Dim,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE, !disabled);
    text(dc, L"Start at the main menu: no logos, notices or intro movie", {r.left + px(130), r.top, value_left - px(40), r.bottom},
         ui.body, disabled ? color::Off : focus ? color::Text : color::Help,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, !disabled);
    if (focus) {
        const int mid = (r.top + r.bottom) / 2, size = (r.bottom - r.top) * 34 / 100;
        chevron(dc, value_left - px(16), mid, size, -1, color::Text);
        chevron(dc, r.right - px(20), mid, size, 1, color::Text);
    }
}

// Owner-drawn buttons: grey bars, the blood red bar for the one action that is ready, darker with
// grey type when disabled, and a light red frame on the focused one.
void draw_button(const DRAWITEMSTRUCT& d) {
    HDC dc = d.hDC;
    RECT r = d.rcItem;
    bool disabled = d.itemState & ODS_DISABLED, pressed = d.itemState & ODS_SELECTED, focus = d.itemState & ODS_FOCUS;
    if (d.hwndItem == ui.skip_intro) {
        draw_skip_intro(dc, r, focus, disabled);
        return;
    }
    wchar_t label[128] = {};
    GetWindowTextW(d.hwndItem, label, 128);
    bool browse = d.CtlID == kGameBrowse || d.CtlID == kUpdateBrowse || d.CtlID == kSaveBrowse;
    bool primary = !browse && d.hwndItem == (setup::ready(ui.root) && !IsWindowEnabled(ui.install) ? ui.play : ui.install);
    if (disabled)
        fill(dc, r, color::RowLow);
    else if (pressed)
        fill(dc, r, primary ? color::FocusLow : color::RowLow);
    else
        primary ? focus_bar(dc, r) : row_bar(dc, r);
    if (focus)
        frame(dc, r, color::FocusEdge, std::max(1, px(2)));
    else if (browse || disabled)
        frame(dc, r, color::RowEdge);
    text(dc, label, r, ui.label, disabled ? color::Off : color::Text, DT_CENTER | DT_VCENTER | DT_SINGLELINE, !disabled);
}

// --- state ---

void set_status(std::wstring s, bool bad = false) {
    ui.status = std::move(s);
    ui.status_bad = bad;
    RECT r = rect(0, kPanelBottom + 4, kWidth, kStripTop - kPanelBottom - 6);
    InvalidateRect(ui.wnd, &r, FALSE);
}

// Checks the three folders, shows the result in each row and enables Install when all are usable.
void validate() {
    std::error_code ec;
    std::wstring game = text_of(ui.game.edit), save = text_of(ui.save.edit), update = text_of(ui.update.edit);
    ui.game.ok = false;
    if (game.empty()) {
        ui.game.note = L"Required: your decrypted dump";
    } else if (!fs::is_directory(setup::game_folder(game), ec)) {
        ui.game.note = L"Folder not found";
    } else if (auto missing = setup::missing_game_files(game); !missing.empty()) {
        ui.game.note = L"Not a Dead Nation dump: no " + missing.front();
    } else {
        ui.game.ok = true;
        ui.game.note = L"Dead Nation dump found";
    }
    ui.update.ok = update.empty() || setup::is_update_folder(fs::path(update));
    ui.update.note = update.empty() ? L"Optional: the dump's own version" : ui.update.ok ? L"Update found" : L"Not an update folder";
    ui.save.ok = !save.empty();
    ui.save.note = !ui.save.ok                         ? L"Required"
                   : fs::is_directory(fs::path(save), ec) ? L"Folder found"
                                                        : L"Created when you install";
    EnableWindow(ui.install, !ui.busy && ui.game.ok && ui.update.ok && ui.save.ok);
    InvalidateRect(ui.wnd, nullptr, FALSE);
    InvalidateRect(ui.install, nullptr, FALSE);
    InvalidateRect(ui.play, nullptr, FALSE);
}

void set_busy(bool busy) {
    ui.busy = busy;
    for (HWND h : ui.inputs)
        EnableWindow(h, !busy);
    EnableWindow(ui.play, !busy && setup::ready(ui.root));
    validate();
}

// Starts the game as a double-click would (its own folder, no console), then closes the setup.
void play() {
    fs::path exe = ui.root / L"deadnation.exe";
    auto started = reinterpret_cast<INT_PTR>(
        ShellExecuteW(ui.wnd, L"open", exe.c_str(), nullptr, ui.root.c_str(), SW_SHOWNORMAL));
    if (started <= 32) {
        MessageBoxW(ui.wnd, (L"Cannot start " + exe.wstring()).c_str(), kTitle, MB_OK | MB_ICONERROR);
        return;
    }
    DestroyWindow(ui.wnd);
}

// The shell's folder picker, starting in the folder the field names when it exists.
void browse(HWND edit, const wchar_t* title) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetTitle(title);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    std::wstring current = text_of(edit);
    if (!current.empty()) {
        std::error_code ec;
        fs::path start = setup::game_folder(current);
        IShellItem* item = nullptr;
        if (fs::is_directory(start, ec) &&
            SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
            dlg->SetFolder(item);
            item->Release();
        }
    }
    IShellItem* result = nullptr;
    if (SUCCEEDED(dlg->Show(ui.wnd)) && SUCCEEDED(dlg->GetResult(&result))) {
        PWSTR path = nullptr;
        if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            SetWindowTextW(edit, path); // EN_CHANGE validates it
            CoTaskMemFree(path);
        }
        result->Release();
    }
    dlg->Release();
}

void start_install() {
    if (ui.busy || !IsWindowEnabled(ui.install))
        return;
    std::wstring game = text_of(ui.game.edit), save = text_of(ui.save.edit), update = text_of(ui.update.edit);
    setup::Options opt;
    opt.root = ui.root;
    opt.game = game;
    opt.save = save;
    opt.update = update;
    opt.skip_intro = ui.skip;
    // Not in validate(): hashing eboot.bin on every keystroke would be slow.
    if (std::wstring warning = setup::version_warning(opt.game, opt.update); !warning.empty() &&
        MessageBoxW(ui.wnd, (warning + L"\n\nInstall anyway?").c_str(), kTitle, MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK)
        return;
    try {
        if (auto old = saves_to_move(opt.root, opt.save)) {
            std::wstring ask = L"Copy your saves to the new save folder?\n\nFrom: " + old->wstring() + L"\nTo: " + save;
            int answer = MessageBoxW(ui.wnd, ask.c_str(), kTitle, MB_YESNOCANCEL | MB_ICONQUESTION);
            if (answer == IDCANCEL)
                return;
            if (answer == IDYES)
                opt.copy_saves_from = old;
        }
    } catch (const setup::Error& e) {
        MessageBoxW(ui.wnd, e.message.c_str(), kTitle, MB_OK | MB_ICONERROR);
        return;
    }
    set_status(L"Installing...");
    set_busy(true);
    std::thread([opt, wnd = ui.wnd] {
        auto log = [wnd](const std::wstring& line) {
            PostMessageW(wnd, kMsgLog, 0, reinterpret_cast<LPARAM>(new std::wstring(line)));
        };
        try {
            fs::path exe = setup::install(opt, log);
            PostMessageW(wnd, kMsgDone, 1, reinterpret_cast<LPARAM>(new std::wstring(exe.wstring())));
        } catch (const setup::Error& e) {
            PostMessageW(wnd, kMsgDone, 0, reinterpret_cast<LPARAM>(new std::wstring(e.message)));
        }
    }).detach();
}

void finish_install(bool ok, const std::wstring& value) {
    set_busy(false);
    if (!ok) {
        set_status(L"Install failed. Check the folders and press Install again.", true);
        MessageBoxW(ui.wnd, value.c_str(), kTitle, MB_OK | MB_ICONERROR);
        return;
    }
    set_status(L"Done. Press Play Game to start.");
    if (MessageBoxW(ui.wnd, L"Done!\n\nAdd a shortcut to the game on your desktop?", kTitle,
                    MB_YESNO | MB_ICONQUESTION) == IDYES) {
        try {
            setup::create_shortcut(value, setup::desktop_folder() / setup::kShortcutName);
            set_status(L"Done, with a desktop shortcut. Press Play Game to start.");
        } catch (const setup::Error& e) {
            MessageBoxW(ui.wnd, e.message.c_str(), kTitle, MB_OK | MB_ICONERROR);
        }
    }
    SetFocus(ui.play); // Enter now starts the game
}

// --- controls ---

HWND add(const wchar_t* cls, const wchar_t* label, DWORD style, RECT r, int id) {
    HWND c = CreateWindowExW(0, cls, label, WS_CHILD | WS_VISIBLE | style, r.left, r.top, r.right - r.left,
                             r.bottom - r.top, ui.wnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(ui.body), FALSE);
    return c;
}

void add_folder(Folder& f, int edit_id, int browse_id) {
    RECT box = field_box(f);
    // The edit inside its drawn box, its text centred on the box's height.
    int h = px(20);
    RECT r = {box.left + px(7), box.top + (box.bottom - box.top - h) / 2, box.right - px(7), 0};
    r.bottom = r.top + h;
    f.edit = add(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, r, edit_id);
    f.browse = add(WC_BUTTONW, L"BROWSE", WS_TABSTOP | BS_OWNERDRAW, rect(kBrowseLeft, f.y + 35, kBrowseWidth, 30),
                   browse_id);
}

void create_controls() {
    // Tab order: the three folders top to bottom, the intro box, then the buttons.
    add_folder(ui.game, kGameEdit, kGameBrowse);
    add_folder(ui.update, kUpdateEdit, kUpdateBrowse);
    add_folder(ui.save, kSaveEdit, kSaveBrowse);
    ui.skip_intro = add(WC_BUTTONW, L"Skip intro", WS_TABSTOP | BS_OWNERDRAW,
                        rect(kRowLeft, kSkipTop, kRowRight - kRowLeft, kSkipHeight), kSkipIntro);
    // The buttons on the hint strip, right-aligned under the panel's right edge (slot 0 the rightmost).
    auto button = [](const wchar_t* label, int slot, int id) {
        return add(WC_BUTTONW, label, WS_TABSTOP | BS_OWNERDRAW,
                   rect(kPanelRight - (slot + 1) * kButtonWidth + 8, kStripTop + 7, kButtonWidth - 8, kStripHeight - 14), id);
    };
    ui.play = button(L"PLAY GAME", 2, kPlay);
    ui.install = button(L"INSTALL", 1, IDOK);
    ui.exit = button(L"EXIT", 0, IDCANCEL);
    ui.inputs = {ui.game.edit, ui.game.browse, ui.update.edit, ui.update.browse,
                 ui.save.edit, ui.save.browse,
                 ui.skip_intro, ui.exit};

    // Start from what the last install chose (paths.ini here), else empty game and update
    // folders and save\ here.
    std::optional<setup::Paths> paths;
    try {
        paths = setup::remembered_paths(ui.root);
    } catch (const setup::Error&) {
    }
    fs::path save = paths ? paths->save : setup::default_save();
    if (paths) {
        SetWindowTextW(ui.game.edit, paths->game.c_str());
        SetWindowTextW(ui.update.edit, paths->update.c_str());
    }
    SetWindowTextW(ui.save.edit, save.c_str());
    // What settings.ini says now; Install writes the box's state, so unticking it brings the logos back.
    ui.skip = setup::skip_intro(ui.root, save);
    set_status(setup::ready(ui.root) ? L"The game is set up. Press Play Game, or change the folders and press Install."
               : paths              ? L"This version isn't set up yet. Press Install."
                                    : L"Choose your game dump (and its update if you have it), then press Install.");
    set_busy(false);
}

LRESULT CALLBACK wnd_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        if (HIWORD(wp) == EN_CHANGE) {
            validate();
            return 0;
        }
        switch (LOWORD(wp)) {
        case IDOK:
            start_install();
            return 0;
        case kPlay:
            play();
            return 0;
        case IDCANCEL:
            if (!ui.busy)
                DestroyWindow(wnd);
            return 0;
        case kSkipIntro:
            ui.skip = !ui.skip;
            InvalidateRect(ui.skip_intro, nullptr, FALSE);
            return 0;
        case kGameBrowse:
            browse(ui.game.edit, L"Select the game folder (the one with eboot.bin)");
            return 0;
        case kUpdateBrowse:
            browse(ui.update.edit, L"Select the update folder (e.g. CUSA00236-patch)");
            return 0;
        case kSaveBrowse:
            browse(ui.save.edit, L"Select the save folder");
            return 0;
        }
        break;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: { // a disabled edit asks with WM_CTLCOLORSTATIC
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, IsWindowEnabled(reinterpret_cast<HWND>(lp)) ? color::Text : color::Off);
        SetBkColor(dc, color::Field);
        return reinterpret_cast<LRESULT>(ui.field);
    }
    case WM_DRAWITEM:
        draw_button(*reinterpret_cast<const DRAWITEMSTRUCT*>(lp));
        return TRUE;
    case WM_ERASEBKGND:
        return 1; // WM_PAINT draws everything
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(wnd, &ps);
        RECT client;
        GetClientRect(wnd, &client);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, client.right, client.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        paint(mem);
        BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(wnd, &ps);
        return 0;
    }
    case kMsgLog: {
        std::unique_ptr<std::wstring> line(reinterpret_cast<std::wstring*>(lp));
        set_status(*line);
        return 0;
    }
    case kMsgDone: {
        std::unique_ptr<std::wstring> value(reinterpret_cast<std::wstring*>(lp));
        finish_install(wp == 1, *value);
        return 0;
    }
    case WM_CLOSE:
        if (!ui.busy)
            DestroyWindow(wnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

// Roboto Condensed Light and Bold (setup.rc), for this process only: the type of settings.exe and
// the in-game pages. Without them GDI falls back to a system face.
void load_fonts(HINSTANCE inst) {
    for (int id : {kFontLight, kFontBold}) {
        HRSRC res = FindResourceW(inst, MAKEINTRESOURCEW(id), RT_RCDATA);
        HGLOBAL data = res ? LoadResource(inst, res) : nullptr;
        DWORD count = 0;
        if (data)
            AddFontMemResourceEx(LockResource(data), SizeofResource(inst, res), nullptr, &count);
    }
}

HFONT make_font(int height, int weight) {
    LOGFONTW lf = {};
    lf.lfHeight = -px(height);
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Roboto Condensed");
    return CreateFontIndirectW(&lf);
}

int run_window(HINSTANCE inst) {
    ui.root = setup::exe_folder();
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    ui.dpi = GetDpiForSystem();
    load_fonts(inst);
    ui.body = make_font(16, FW_BOLD);
    ui.label = make_font(20, FW_BOLD);
    ui.title = make_font(46, FW_BOLD);
    ui.hint = make_font(16, FW_LIGHT);
    ui.field = CreateSolidBrush(color::Field);

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"deadnation_setup";
    RegisterClassExW(&wc);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT r = {0, 0, px(kWidth), px(kHeight)};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, ui.dpi);
    ui.wnd = CreateWindowExW(0, wc.lpszClassName, kTitle, style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                             r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    // A dark title bar in the backdrop's red (Windows 11; older versions ignore it).
    BOOL dark = TRUE;
    DwmSetWindowAttribute(ui.wnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    COLORREF caption = color::BackRed;
    DwmSetWindowAttribute(ui.wnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
    create_controls();
    ShowWindow(ui.wnd, SW_SHOW);
    SetFocus(ui.game.edit);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(ui.wnd, &msg)) { // Tab between controls, Enter = Install, Esc = Exit
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (HWND focus = GetFocus(); focus != ui.focus) { // the focused folder's row moves to the red bar
            ui.focus = focus;
            InvalidateRect(ui.wnd, nullptr, FALSE);
        }
    }
    DeleteObject(ui.body);
    DeleteObject(ui.label);
    DeleteObject(ui.title);
    DeleteObject(ui.hint);
    DeleteObject(ui.field);
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args(argv + 1, argv + argc);
    LocalFree(argv);
    if (!args.empty())
        return run_cli(args);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int rc = run_window(inst);
    CoUninitialize();
    return rc;
}
