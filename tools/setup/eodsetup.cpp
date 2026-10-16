// eodsetup.cpp - settings program for the PC port (writes eod.ini next to
// eod.exe and can start the game).  Plain Win32 controls, no dependencies
// beyond what Windows 7 ships.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dxgi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "../../runtime/core/config.h"
#include "../../runtime/core/eodsave.h"
#include "../../runtime/core/ps2mc.h"

namespace {

HINSTANCE g_inst;
HWND g_main;
HFONT g_font, g_bold;
std::string g_dir;  // folder of eodsetup.exe (and eod.exe / eod.ini)
Config g_cfg;

enum : int {
    ID_RENDERER = 100, ID_ADAPTER, ID_FULLSCREEN, ID_WINSIZE, ID_ASPECT, ID_SCALE, ID_VSYNC, ID_SMOOTH, ID_SKIP,
    ID_AUDIO, ID_VOLUME, ID_VOLUME_LABEL, ID_VIBRATION, ID_DURABILITY, ID_CONTROLS, ID_MEMCARD, ID_MEMCARD_BROWSE, ID_DATA,
    ID_DATA_BROWSE, ID_IMPORT_PCSX2, ID_EDIT_SAVE, ID_PLAY, ID_SAVE, ID_DEFAULTS, ID_EXIT,
    ID_KEY_EDIT = 300, ID_KEY_BIND = 400, ID_KEYS_OK = 500, ID_KEYS_CANCEL, ID_KEYS_DEFAULTS,
    ID_SE_MONEY = 600, ID_SE_REPA, ID_SE_REPB, ID_SE_REPP, ID_SE_REPTOTAL, ID_SE_REPMAX, ID_SE_LIST, ID_SE_VALUE,
    ID_SE_VALUE_LABEL, ID_SE_SET, ID_SE_SAVE, ID_SE_CANCEL, ID_SE_SLOT  // ID_SE_SLOT + 0..3
};

struct Size {
    int w, h;
};
const Size kSizes[] = {{640, 480},   {800, 600},   {960, 720},   {1024, 768}, {1280, 960}, {1440, 1080},
                       {1600, 1200}, {1280, 720},  {1366, 768},  {1600, 900}, {1920, 1080}, {2560, 1440}};
std::vector<Size> g_sizes;  // what the size combo lists

HWND ctl(const char *cls, const char *text, DWORD style, int x, int y, int w, int h, HWND parent, int id,
         DWORD ex = 0) {
    HWND c = CreateWindowExA(ex, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent, (HMENU)(INT_PTR)id,
                             g_inst, nullptr);
    SendMessageA(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}
HWND label(const char *text, int x, int y, int w, HWND parent, int h = 16) {
    return ctl("STATIC", text, 0, x, y + 3, w, h, parent, -1);
}
HWND group(const char *text, int x, int y, int w, int h, HWND parent) {
    HWND g = ctl("BUTTON", text, BS_GROUPBOX, x, y, w, h, parent, -1);
    SendMessageA(g, WM_SETFONT, (WPARAM)g_bold, TRUE);
    return g;
}
HWND combo(int x, int y, int w, HWND parent, int id) {
    return ctl("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, x, y, w, 300, parent, id);
}
HWND check(const char *text, int x, int y, int w, HWND parent, int id) {
    return ctl("BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, x, y, w, 20, parent, id);
}
void add(HWND cb, const char *s) { SendMessageA(cb, CB_ADDSTRING, 0, (LPARAM)s); }
void sel(HWND cb, int i) { SendMessageA(cb, CB_SETCURSEL, (WPARAM)i, 0); }
int cur(int id) { return (int)SendDlgItemMessageA(g_main, id, CB_GETCURSEL, 0, 0); }
bool checked(int id) { return IsDlgButtonChecked(g_main, id) == BST_CHECKED; }
void set_check(int id, bool v) { CheckDlgButton(g_main, id, v ? BST_CHECKED : BST_UNCHECKED); }
std::string text_of(HWND parent, int id) {
    char buf[1024] = {};
    GetDlgItemTextA(parent, id, buf, sizeof buf);
    return buf;
}

// ------------------------------------------------------------ controls
std::string g_edit_keys[KEY_COUNT];
int g_binding = -1;  // row waiting for a key press

LRESULT CALLBACK keys_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id >= ID_KEY_BIND && id < ID_KEY_BIND + KEY_COUNT) {
            g_binding = id - ID_KEY_BIND;
            SetDlgItemTextA(h, ID_KEY_EDIT + g_binding, "press a key... (Esc: clear)");
            SetFocus(h);
            return 0;
        }
        if (id == ID_KEYS_DEFAULTS) {
            Config d;
            for (int i = 0; i < KEY_COUNT; i++) SetDlgItemTextA(h, ID_KEY_EDIT + i, d.keys[i].c_str());
            return 0;
        }
        if (id == ID_KEYS_OK) {
            for (int i = 0; i < KEY_COUNT; i++) g_cfg.keys[i] = text_of(h, ID_KEY_EDIT + i);
            DestroyWindow(h);
            return 0;
        }
        if (id == ID_KEYS_CANCEL) {
            DestroyWindow(h);
            return 0;
        }
        break;
    }
    case WM_KEYDOWN:
        if (g_binding >= 0) {
            std::string name = wp == VK_ESCAPE ? "" : key_name((int)wp);
            SetDlgItemTextA(h, ID_KEY_EDIT + g_binding, name.c_str());
            g_binding = -1;
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        g_binding = -1;
        EnableWindow(g_main, TRUE);
        SetForegroundWindow(g_main);
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

// Key presses go to the controls window while a binding is pending.
bool keys_filter(MSG &m) {
    if (g_binding < 0 || (m.message != WM_KEYDOWN && m.message != WM_SYSKEYDOWN)) return false;
    HWND top = GetAncestor(m.hwnd, GA_ROOT);
    SendMessageA(top, WM_KEYDOWN, m.wParam, m.lParam);
    return true;
}

void open_controls() {
    const int rowh = 26, w = 440;
    int h = 60 + KEY_COUNT * rowh + 50;
    RECT r = {0, 0, w, h};
    AdjustWindowRect(&r, WS_CAPTION | WS_SYSMENU, FALSE);
    RECT pr;
    GetWindowRect(g_main, &pr);
    HWND d = CreateWindowExA(WS_EX_DLGMODALFRAME, "EodSetupKeys", "Keyboard controls", WS_CAPTION | WS_SYSMENU | WS_POPUP,
                             pr.left + 40, pr.top + 30, r.right - r.left, r.bottom - r.top, g_main, nullptr, g_inst,
                             nullptr);
    label("Keys for each PS2 button (several keys: separate with commas).", 12, 8, w - 24, d);
    label("Xbox-compatible controllers work automatically.", 12, 26, w - 24, d);
    for (int i = 0; i < KEY_COUNT; i++) {
        int y = 52 + i * rowh;
        label(kPadKeyLabels[i], 12, y, 130, d);
        ctl("EDIT", g_cfg.keys[i].c_str(), ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 145, y, 200, 22, d,
            ID_KEY_EDIT + i);
        ctl("BUTTON", "Set...", BS_PUSHBUTTON | WS_TABSTOP, 352, y, 76, 22, d, ID_KEY_BIND + i);
    }
    int by = 52 + KEY_COUNT * rowh + 12;
    ctl("BUTTON", "Defaults", BS_PUSHBUTTON | WS_TABSTOP, 12, by, 90, 26, d, ID_KEYS_DEFAULTS);
    ctl("BUTTON", "OK", BS_DEFPUSHBUTTON | WS_TABSTOP, w - 196, by, 88, 26, d, ID_KEYS_OK);
    ctl("BUTTON", "Cancel", BS_PUSHBUTTON | WS_TABSTOP, w - 100, by, 88, 26, d, ID_KEYS_CANCEL);
    EnableWindow(g_main, FALSE);
    ShowWindow(d, SW_SHOW);
}

// ------------------------------------------------------------ main window
void list_adapters(HWND cb) {
    bool any = false;
    IDXGIFactory *f = nullptr;
    if (SUCCEEDED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void **)&f))) {
        IDXGIAdapter *a = nullptr;
        for (UINT i = 0; f->EnumAdapters(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC d;
            if (SUCCEEDED(a->GetDesc(&d))) {
                char name[256];
                WideCharToMultiByte(CP_ACP, 0, d.Description, -1, name, sizeof name, nullptr, nullptr);
                add(cb, name);
                any = true;
            }
            a->Release();
        }
        f->Release();
    }
    if (!any) add(cb, "Default adapter");
}

void load_into_ui() {
    sel(GetDlgItem(g_main, ID_RENDERER), g_cfg.renderer == "software" ? 1 : g_cfg.renderer == "warp" ? 2 : 0);
    HWND ad = GetDlgItem(g_main, ID_ADAPTER);
    int n = (int)SendMessageA(ad, CB_GETCOUNT, 0, 0);
    sel(ad, g_cfg.adapter < n ? g_cfg.adapter : 0);
    set_check(ID_FULLSCREEN, g_cfg.fullscreen);
    HWND ws = GetDlgItem(g_main, ID_WINSIZE);
    SendMessageA(ws, CB_RESETCONTENT, 0, 0);
    g_sizes.assign(std::begin(kSizes), std::end(kSizes));
    int found = -1;
    for (size_t i = 0; i < g_sizes.size(); i++)
        if (g_sizes[i].w == g_cfg.width && g_sizes[i].h == g_cfg.height) found = (int)i;
    if (found < 0) {
        g_sizes.push_back({g_cfg.width, g_cfg.height});
        found = (int)g_sizes.size() - 1;
    }
    for (const Size &s : g_sizes) {
        char b[64];
        snprintf(b, sizeof b, "%d x %d", s.w, s.h);
        add(ws, b);
    }
    sel(ws, found);
    sel(GetDlgItem(g_main, ID_ASPECT), g_cfg.aspect == "4:3" ? 1 : g_cfg.aspect == "16:9" ? 2 : g_cfg.aspect == "stretch" ? 3 : 0);
    sel(GetDlgItem(g_main, ID_SCALE), g_cfg.render_scale - 1);
    set_check(ID_VSYNC, g_cfg.vsync);
    set_check(ID_SMOOTH, g_cfg.smooth_display);
    set_check(ID_SKIP, g_cfg.frame_skip);
    set_check(ID_AUDIO, g_cfg.audio);
    SendDlgItemMessageA(g_main, ID_VOLUME, TBM_SETPOS, TRUE, g_cfg.volume);
    char v[32];
    snprintf(v, sizeof v, "%d%%", g_cfg.volume);
    SetDlgItemTextA(g_main, ID_VOLUME_LABEL, v);
    set_check(ID_VIBRATION, g_cfg.vibration);
    set_check(ID_DURABILITY, g_cfg.car_durability);
    SetDlgItemTextA(g_main, ID_MEMCARD, g_cfg.memcard_dir.c_str());
    SetDlgItemTextA(g_main, ID_DATA, g_cfg.data_dir.c_str());
}

void read_from_ui() {
    g_cfg.renderer = cur(ID_RENDERER) == 1 ? "software" : cur(ID_RENDERER) == 2 ? "warp" : "d3d11";
    g_cfg.adapter = cur(ID_ADAPTER) < 0 ? 0 : cur(ID_ADAPTER);
    g_cfg.fullscreen = checked(ID_FULLSCREEN);
    int s = cur(ID_WINSIZE);
    if (s >= 0 && s < (int)g_sizes.size()) {
        g_cfg.width = g_sizes[(size_t)s].w;
        g_cfg.height = g_sizes[(size_t)s].h;
    }
    static const char *const aspects[] = {"auto", "4:3", "16:9", "stretch"};
    g_cfg.aspect = aspects[std::max(0, std::min(3, (int)cur(ID_ASPECT)))];
    g_cfg.render_scale = cur(ID_SCALE) + 1;
    g_cfg.vsync = checked(ID_VSYNC);
    g_cfg.smooth_display = checked(ID_SMOOTH);
    g_cfg.frame_skip = checked(ID_SKIP);
    g_cfg.audio = checked(ID_AUDIO);
    g_cfg.volume = (int)SendDlgItemMessageA(g_main, ID_VOLUME, TBM_GETPOS, 0, 0);
    g_cfg.vibration = checked(ID_VIBRATION);
    g_cfg.car_durability = checked(ID_DURABILITY);
    g_cfg.memcard_dir = text_of(g_main, ID_MEMCARD);
    g_cfg.data_dir = text_of(g_main, ID_DATA);
}

void update_enabled() {
    bool d3d = cur(ID_RENDERER) != 1;
    EnableWindow(GetDlgItem(g_main, ID_ADAPTER), d3d && cur(ID_RENDERER) != 2);
    EnableWindow(GetDlgItem(g_main, ID_SCALE), d3d);
    EnableWindow(GetDlgItem(g_main, ID_VSYNC), d3d);
    EnableWindow(GetDlgItem(g_main, ID_SMOOTH), d3d);
    EnableWindow(GetDlgItem(g_main, ID_WINSIZE), !checked(ID_FULLSCREEN));
    EnableWindow(GetDlgItem(g_main, ID_VOLUME), checked(ID_AUDIO));
}

bool save() {
    read_from_ui();
    std::string path = g_dir + "\\eod.ini";
    if (!config_save(path, g_cfg)) {
        MessageBoxA(g_main, ("Could not write " + path).c_str(), "Eve of Destruction setup", MB_OK | MB_ICONERROR);
        return false;
    }
    return true;
}

void browse(int edit_id, const char *title) {
    BROWSEINFOA bi = {};
    bi.hwndOwner = g_main;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST l = SHBrowseForFolderA(&bi);
    if (!l) return;
    char path[MAX_PATH];
    if (SHGetPathFromIDListA(l, path)) SetDlgItemTextA(g_main, edit_id, path);
    CoTaskMemFree(l);
}

// The folder holding the memory card in port 0 (as the game finds it).
std::string memcard_port0() {
    std::string root = text_of(g_main, ID_MEMCARD);
    if (root.empty()) {
        std::string data = text_of(g_main, ID_DATA);
        root = (data.empty() ? g_dir : data) + "\\memcard";
    }
    CreateDirectoryA(root.c_str(), nullptr);
    return root + "\\port0";
}

// Deletes a save folder (its files, then the folder).
void remove_save_dir(const std::string &dir) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileA((dir + "\\" + fd.cFileName).c_str());
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir.c_str());
}

// Copies the game's saves from a PCSX2 memory card image into port 0's folder.
void import_pcsx2() {
    const char *title = "Import PCSX2 save";
    char file[MAX_PATH] = "";
    OPENFILENAMEA of = {};
    of.lStructSize = sizeof of;
    of.hwndOwner = g_main;
    of.lpstrFilter = "PS2 memory cards (*.ps2, *.bin, *.mcd)\0*.ps2;*.bin;*.mcd\0All files\0*.*\0";
    of.lpstrFile = file;
    of.nMaxFile = MAX_PATH;
    of.lpstrTitle = "PCSX2 memory card (in PCSX2's memcards folder)";
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&of)) return;
    std::vector<Ps2McSave> saves;
    std::string err;
    if (!ps2mc_read_saves(file, "BASLUS-20910", saves, err)) {
        MessageBoxA(g_main, ("Could not read the memory card: " + err + ".").c_str(), title, MB_OK | MB_ICONERROR);
        return;
    }
    if (saves.empty()) {
        MessageBoxA(g_main, "There is no Test Drive: Eve of Destruction save on this memory card.", title,
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::string dir = memcard_port0();
    CreateDirectoryA(dir.c_str(), nullptr);
    // the current save (also under the name older versions of the port used) is kept as <name>.bak
    std::vector<std::string> existing;
    for (const Ps2McSave &s : saves) {
        for (const std::string &name : {s.dir, std::string("US-20910DRIVEN")}) {
            DWORD a = GetFileAttributesA((dir + "\\" + name).c_str());
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) existing.push_back(name);
        }
    }
    if (!existing.empty()) {
        std::string msg = "Replace the save in " + dir + " with the one from the PCSX2 memory card?\n\nThe current save is kept as " +
                          existing[0] + ".bak.";
        if (MessageBoxA(g_main, msg.c_str(), title, MB_YESNO | MB_ICONQUESTION) != IDYES) return;
        for (const std::string &name : existing) {
            std::string from = dir + "\\" + name, to = from + ".bak";
            remove_save_dir(to);
            if (!MoveFileA(from.c_str(), to.c_str())) {
                MessageBoxA(g_main, ("Could not rename " + from + ".").c_str(), title, MB_OK | MB_ICONERROR);
                return;
            }
        }
    }
    int n = ps2mc_write_saves(dir, saves, err);
    if (n < (int)saves.size()) {
        MessageBoxA(g_main, ("Could not copy the save: " + err + ".").c_str(), title, MB_OK | MB_ICONERROR);
        return;
    }
    std::string msg = "Imported " + saves[0].dir + " into " + dir + ".";
    MessageBoxA(g_main, msg.c_str(), title, MB_OK | MB_ICONINFORMATION);
}

// ------------------------------------------------------------ save editor
EodSave g_save;
std::string g_save_path;
HWND g_save_wnd;

void save_editor_total(HWND h) {
    int32_t a = (int32_t)strtol(text_of(h, ID_SE_REPA).c_str(), nullptr, 10);
    int32_t b = (int32_t)strtol(text_of(h, ID_SE_REPB).c_str(), nullptr, 10);
    int32_t p = (int32_t)strtol(text_of(h, ID_SE_REPP).c_str(), nullptr, 10);
    char t[64];
    snprintf(t, sizeof t, "= %d reputation points", EodSave::reputation_total(a, b, p));
    SetDlgItemTextA(h, ID_SE_REPTOTAL, t);
}

int save_editor_selected(HWND h) {
    return (int)SendDlgItemMessageA(h, ID_SE_LIST, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
}

bool parse_int(const std::string &s, int32_t &v) {
    char *end;
    long r = strtol(s.c_str(), &end, 10);
    if (s.empty() || *end) return false;
    v = (int32_t)r;
    return true;
}

LRESULT CALLBACK save_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    const char *title = "Save editor";
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if ((id == ID_SE_REPA || id == ID_SE_REPB || id == ID_SE_REPP) && HIWORD(wp) == EN_CHANGE) {
            save_editor_total(h);
            return 0;
        }
        if (id == ID_SE_REPMAX) {  // the game's cap: the points that make the total 999999
            int32_t a = 0, b = 0;
            parse_int(text_of(h, ID_SE_REPA), a);
            parse_int(text_of(h, ID_SE_REPB), b);
            int64_t p = 999999 - (int64_t)a * 100 - (int64_t)b * 50;
            SetDlgItemTextA(h, ID_SE_REPP, std::to_string(p < 0 ? 0 : p).c_str());
            return 0;
        }
        if (id == ID_SE_SET) {
            int i = save_editor_selected(h);
            if (i < 0) return 0;
            std::string err;
            if (!g_save.set_option((size_t)i, text_of(h, ID_SE_VALUE), err)) {
                MessageBoxA(h, (g_save.options()[(size_t)i].name + ": " + err + ".").c_str(), title, MB_OK | MB_ICONERROR);
                return 0;
            }
            LVITEMA it = {};
            it.iSubItem = 2;
            it.pszText = (char *)g_save.options()[(size_t)i].value.c_str();
            SendDlgItemMessageA(h, ID_SE_LIST, LVM_SETITEMTEXTA, (WPARAM)i, (LPARAM)&it);
            SetDlgItemTextA(h, ID_SE_VALUE, g_save.options()[(size_t)i].value.c_str());
            return 0;
        }
        if (id == ID_SE_SAVE) {
            int32_t money = 0, a = 0, b = 0, p = 0;
            if (g_save.has_career() && !parse_int(text_of(h, ID_SE_MONEY), money)) {
                MessageBoxA(h, "Money: enter a whole number.", title, MB_OK | MB_ICONERROR);
                return 0;
            }
            if (g_save.has_reputation() && (!parse_int(text_of(h, ID_SE_REPA), a) || !parse_int(text_of(h, ID_SE_REPB), b) ||
                                            !parse_int(text_of(h, ID_SE_REPP), p) || a < 0 || b < 0 || p < 0)) {
                MessageBoxA(h, "Reputation: enter whole numbers (0 or more).", title, MB_OK | MB_ICONERROR);
                return 0;
            }
            std::vector<std::pair<int, int>> looks = eodsave_parse_looks(g_cfg.car_looks);
            if (g_save.has_career()) {
                std::vector<int> shown;
                for (int k = 0; k < g_save.car_count() && k < 4; k++) {
                    int i = (int)SendDlgItemMessageA(h, ID_SE_SLOT + k, CB_GETCURSEL, 0, 0);
                    shown.push_back(i < 0 ? eodsave_slot_shown(g_save, k, looks)
                                          : (int)SendDlgItemMessageA(h, ID_SE_SLOT + k, CB_GETITEMDATA, (WPARAM)i, 0));
                }
                std::string err;
                if (!shown.empty() && !eodsave_set_slots(g_save, shown, looks, err)) {
                    MessageBoxA(h, ("Garage: " + err + ".").c_str(), title, MB_OK | MB_ICONERROR);
                    return 0;
                }
                g_save.set_money(money);
            }
            if (g_save.has_reputation()) g_save.set_reputation(a, b, p);
            std::string err, bak = g_save_path + ".bak";
            CopyFileA(g_save_path.c_str(), bak.c_str(), FALSE);  // the previous save
            if (!g_save.save(g_save_path, err)) {
                MessageBoxA(h, ("Could not save: " + err + ".").c_str(), title, MB_OK | MB_ICONERROR);
                return 0;
            }
            std::string looks_text = eodsave_format_looks(looks);
            if (looks_text != g_cfg.car_looks) {  // the cars shown as other vehicles (eod.ini)
                g_cfg.car_looks = looks_text;
                if (!config_save(g_dir + "\\eod.ini", g_cfg))
                    MessageBoxA(h, ("Could not write " + g_dir + "\\eod.ini.").c_str(), title, MB_OK | MB_ICONERROR);
            }
            MessageBoxA(h, ("Saved. The previous save is kept as " + bak + ".").c_str(), title, MB_OK | MB_ICONINFORMATION);
            DestroyWindow(h);
            return 0;
        }
        if (id == ID_SE_CANCEL) {
            DestroyWindow(h);
            return 0;
        }
        break;
    }
    case WM_NOTIFY: {
        NMHDR *n = (NMHDR *)lp;
        if (n->idFrom == ID_SE_LIST && n->code == LVN_ITEMCHANGED) {
            NMLISTVIEW *lv = (NMLISTVIEW *)lp;
            if ((lv->uNewState & LVIS_SELECTED) && lv->iItem >= 0 && (size_t)lv->iItem < g_save.options().size()) {
                const EodSaveOption &o = g_save.options()[(size_t)lv->iItem];
                SetDlgItemTextA(h, ID_SE_VALUE, o.value.c_str());
                static const char *const kinds[] = {"text or number", "yes or no", "whole number", "text", "number"};
                std::string hint = o.name + " (" + kinds[o.type >= 0 && o.type <= 4 ? o.type : 0] + "):";
                SetDlgItemTextA(h, ID_SE_VALUE_LABEL, hint.c_str());
            }
        }
        break;
    }
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        g_save_wnd = nullptr;
        EnableWindow(g_main, TRUE);
        SetForegroundWindow(g_main);
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

// Opens the save in port 0's folder in the editor window.
void open_save_editor() {
    const char *title = "Save editor";
    g_save_path = memcard_port0() + "\\BASLUS-20910DRIVEN\\BASLUS-20910DRIVEN";
    std::string err;
    if (GetFileAttributesA(g_save_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxA(g_main, ("There is no save yet (" + g_save_path + ").\n\nSave in the game first, or import a PCSX2 save.").c_str(),
                    title, MB_OK | MB_ICONINFORMATION);
        return;
    }
    g_save = EodSave();
    if (!g_save.load(g_save_path, err)) {
        MessageBoxA(g_main, ("Could not read the save: " + err + ".").c_str(), title, MB_OK | MB_ICONERROR);
        return;
    }
    const int w = 560, hgt = 664;
    RECT r = {0, 0, w, hgt};
    AdjustWindowRect(&r, WS_CAPTION | WS_SYSMENU, FALSE);
    RECT pr, wa;
    GetWindowRect(g_main, &pr);
    int x = pr.left + 20, top = pr.top + 20;
    if (SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0)) {  // keep it on screen (768-pixel tall laptops)
        x = std::max((int)wa.left, std::min(x, (int)wa.right - (int)(r.right - r.left)));
        top = std::max((int)wa.top, std::min(top, (int)wa.bottom - (int)(r.bottom - r.top)));
    }
    HWND d = CreateWindowExA(WS_EX_DLGMODALFRAME, "EodSetupSave", "Save editor", WS_CAPTION | WS_SYSMENU | WS_POPUP, x,
                             top, r.right - r.left, r.bottom - r.top, g_main, nullptr, g_inst, nullptr);
    g_save_wnd = d;
    std::string who = "Player: " + g_save.player_name() + (g_save.checksum_ok() ? "" : "   (the save's checksum was wrong: saving fixes it)");
    label(who.c_str(), 12, 8, w - 24, d);
    label(g_save_path.c_str(), 12, 26, w - 24, d);

    int y = 50;
    group("Career", 12, y, w - 24, 112, d);
    label("Money:", 28, y + 24, 90, d);
    ctl("EDIT", std::to_string(g_save.money()).c_str(), ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 130, y + 22, 110, 22, d,
        ID_SE_MONEY);
    int32_t a, b, p;
    g_save.reputation(a, b, p);
    label("Reputation:", 28, y + 54, 90, d);
    ctl("EDIT", std::to_string(a).c_str(), ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 130, y + 52, 60, 22, d, ID_SE_REPA);
    label("x 100  +", 194, y + 54, 50, d);
    ctl("EDIT", std::to_string(b).c_str(), ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 248, y + 52, 60, 22, d, ID_SE_REPB);
    label("x 50  +", 312, y + 54, 44, d);
    ctl("EDIT", std::to_string(p).c_str(), ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 358, y + 52, 80, 22, d, ID_SE_REPP);
    ctl("BUTTON", "Max", BS_PUSHBUTTON | WS_TABSTOP, 446, y + 51, 90, 24, d, ID_SE_REPMAX);
    ctl("STATIC", "", 0, 130, y + 84, 300, 16, d, ID_SE_REPTOTAL);
    save_editor_total(d);
    if (!g_save.has_career()) EnableWindow(GetDlgItem(d, ID_SE_MONEY), FALSE);

    y += 124;
    group("Garage", 12, y, w - 24, 176, d);
    std::vector<std::pair<int, int>> looks = eodsave_parse_looks(g_cfg.car_looks);
    for (int k = 0; k < 4; k++) {
        label(("Slot " + std::to_string(k + 1) + ":").c_str(), 28, y + 26 + k * 30, 60, d);
        HWND cb = combo(96, y + 22 + k * 30, w - 132, d, ID_SE_SLOT + k);
        bool owned = g_save.has_career() && k < g_save.car_count();
        if (!owned) {
            add(cb, g_save.has_career() ? "(no car: buy one in the game first)" : "(no career)");
            sel(cb, 0);
            EnableWindow(cb, FALSE);
            continue;
        }
        int shown = eodsave_slot_shown(g_save, k, looks), pick = -1;
        for (const EodVehicle &v : eodsave_vehicles()) {
            int i = (int)SendMessageA(cb, CB_ADDSTRING, 0, (LPARAM)eodsave_vehicle_label(v.id).c_str());
            SendMessageA(cb, CB_SETITEMDATA, (WPARAM)i, (LPARAM)v.id);
            if (v.id == shown) pick = i;
        }
        sel(cb, pick);
    }
    label("Career cars are written into the save. Action paint jobs and special vehicles", 28, y + 144, w - 56, d);
    label("are shown by this port over the slot's career car (on a PS2 it stays as it is).", 28, y + 158, w - 56, d);

    y += 188;
    group("Game settings", 12, y, w - 24, 262, d);
    HWND lv = ctl(WC_LISTVIEWA, "", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, 24, y + 22,
                  w - 48, 178, d, ID_SE_LIST);
    SendMessageA(lv, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    const char *cols[3] = {"Section", "Setting", "Value"};
    const int widths[3] = {120, 200, 160};
    for (int c = 0; c < 3; c++) {
        LVCOLUMNA col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = (char *)cols[c];
        col.cx = widths[c];
        SendMessageA(lv, LVM_INSERTCOLUMNA, (WPARAM)c, (LPARAM)&col);
    }
    for (size_t i = 0; i < g_save.options().size(); i++) {
        const EodSaveOption &o = g_save.options()[i];
        LVITEMA it = {};
        it.mask = LVIF_TEXT;
        it.iItem = (int)i;
        it.pszText = (char *)eodsave_group_name(o.group);
        SendMessageA(lv, LVM_INSERTITEMA, 0, (LPARAM)&it);
        it.iSubItem = 1;
        it.pszText = (char *)o.name.c_str();
        SendMessageA(lv, LVM_SETITEMTEXTA, (WPARAM)i, (LPARAM)&it);
        it.iSubItem = 2;
        it.pszText = (char *)o.value.c_str();
        SendMessageA(lv, LVM_SETITEMTEXTA, (WPARAM)i, (LPARAM)&it);
    }
    ctl("STATIC", "Select a setting to change it.", 0, 24, y + 210, w - 48, 16, d, ID_SE_VALUE_LABEL);
    ctl("EDIT", "", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 24, y + 230, 300, 22, d, ID_SE_VALUE);
    ctl("BUTTON", "Set", BS_PUSHBUTTON | WS_TABSTOP, 332, y + 229, 80, 24, d, ID_SE_SET);

    int by = hgt - 40;
    ctl("BUTTON", "Save", BS_DEFPUSHBUTTON | WS_TABSTOP, w - 196, by, 88, 28, d, ID_SE_SAVE);
    ctl("BUTTON", "Cancel", BS_PUSHBUTTON | WS_TABSTOP, w - 100, by, 88, 28, d, ID_SE_CANCEL);
    EnableWindow(g_main, FALSE);
    ShowWindow(d, SW_SHOW);
}

void play() {
    if (!save()) return;
    std::string exe = g_dir + "\\eod.exe";
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    std::string cmd = "\"" + exe + "\"";
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    if (!CreateProcessA(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, 0, nullptr, g_dir.c_str(), &si, &pi)) {
        MessageBoxA(g_main, ("Could not start " + exe).c_str(), "Eve of Destruction setup", MB_OK | MB_ICONERROR);
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    PostMessageA(g_main, WM_CLOSE, 0, 0);
}

void build_ui(HWND w) {
    const int W = 520;
    HWND t = ctl("STATIC", "Test Drive: Eve of Destruction", 0, 16, 12, W - 32, 26, w, -1);
    LOGFONTA lf = {};
    lf.lfHeight = -20;
    lf.lfWeight = FW_BOLD;
    lf.lfQuality = CLEARTYPE_QUALITY;
    strcpy(lf.lfFaceName, "Segoe UI");
    SendMessageA(t, WM_SETFONT, (WPARAM)CreateFontIndirectA(&lf), TRUE);
    label("PC port settings", 18, 38, W - 32, w);

    int y = 66;
    group("Graphics", 12, y, W - 24, 214, w);
    label("Renderer:", 28, y + 24, 120, w);
    HWND r = combo(160, y + 22, 330, w, ID_RENDERER);
    add(r, "Direct3D 11 (graphics card, recommended)");
    add(r, "Software (CPU only, most compatible)");
    add(r, "Direct3D 11 WARP (in software, for testing)");
    label("Graphics adapter:", 28, y + 54, 120, w);
    list_adapters(combo(160, y + 52, 330, w, ID_ADAPTER));
    check("Fullscreen", 28, y + 84, 120, w, ID_FULLSCREEN);
    label("Window size:", 160, y + 84, 90, w);
    combo(255, y + 82, 235, w, ID_WINSIZE);
    label("Aspect ratio:", 28, y + 116, 120, w);
    HWND a = combo(160, y + 114, 140, w, ID_ASPECT);
    add(a, "Auto (game option)");
    add(a, "4:3 (original)");
    add(a, "16:9 widescreen");
    add(a, "Stretch to window");
    label("Resolution:", 312, y + 116, 70, w);
    HWND s = combo(385, y + 114, 105, w, ID_SCALE);
    add(s, "1x (640x448)");
    add(s, "2x (1280x896)");
    add(s, "3x (1920x1344)");
    add(s, "4x (2560x1792)");
    add(s, "5x (3200x2240)");
    add(s, "6x (3840x2688)");
    check("Vertical sync", 28, y + 148, 140, w, ID_VSYNC);
    check("Smooth scaling", 175, y + 148, 140, w, ID_SMOOTH);
    check("Frame skipping", 330, y + 148, 150, w, ID_SKIP);
    label("Frame skipping keeps the game at full speed on slow computers.", 28, y + 178, W - 60, w);

    y += 224;
    group("Sound, controls and gameplay", 12, y, W - 24, 122, w);
    check("Sound", 28, y + 24, 80, w, ID_AUDIO);
    label("Volume:", 112, y + 24, 50, w);
    HWND v = ctl(TRACKBAR_CLASSA, "", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 165, y + 22, 180, 26, w, ID_VOLUME);
    SendMessageA(v, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    ctl("STATIC", "100%", 0, 350, y + 27, 50, 16, w, ID_VOLUME_LABEL);
    check("Controller vibration", 28, y + 56, 170, w, ID_VIBRATION);
    ctl("BUTTON", "Keyboard controls...", BS_PUSHBUTTON | WS_TABSTOP, 330, y + 54, 160, 26, w, ID_CONTROLS);
    check("Car durability as rated in the menu (each car's damage_scale, unused by the game)", 28, y + 88, W - 60, w,
          ID_DURABILITY);

    y += 132;
    group("Folders (leave empty for the defaults)", 12, y, W - 24, 120, w);
    label("Memory card:", 28, y + 24, 90, w);
    ctl("EDIT", "", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 125, y + 22, 280, 22, w, ID_MEMCARD);
    ctl("BUTTON", "Browse...", BS_PUSHBUTTON | WS_TABSTOP, 412, y + 21, 78, 24, w, ID_MEMCARD_BROWSE);
    label("Game data:", 28, y + 54, 90, w);
    ctl("EDIT", "", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 125, y + 52, 280, 22, w, ID_DATA);
    ctl("BUTTON", "Browse...", BS_PUSHBUTTON | WS_TABSTOP, 412, y + 51, 78, 24, w, ID_DATA_BROWSE);
    ctl("BUTTON", "Import PCSX2 save...", BS_PUSHBUTTON | WS_TABSTOP, 28, y + 83, 150, 26, w, ID_IMPORT_PCSX2);
    ctl("BUTTON", "Edit save...", BS_PUSHBUTTON | WS_TABSTOP, 186, y + 83, 110, 26, w, ID_EDIT_SAVE);
    label("(PCSX2 cards: .ps2 files)", 306, y + 88, W - 330, w);

    y += 132;
    ctl("BUTTON", "Defaults", BS_PUSHBUTTON | WS_TABSTOP, 12, y, 90, 30, w, ID_DEFAULTS);
    ctl("BUTTON", "Play", BS_DEFPUSHBUTTON | WS_TABSTOP, W - 300, y, 90, 30, w, ID_PLAY);
    ctl("BUTTON", "Save", BS_PUSHBUTTON | WS_TABSTOP, W - 204, y, 90, 30, w, ID_SAVE);
    ctl("BUTTON", "Exit", BS_PUSHBUTTON | WS_TABSTOP, W - 108, y, 90, 30, w, ID_EXIT);
}

LRESULT CALLBACK main_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_RENDERER:
        case ID_FULLSCREEN:
        case ID_AUDIO:
            update_enabled();
            return 0;
        case ID_CONTROLS:
            read_from_ui();
            open_controls();
            return 0;
        case ID_MEMCARD_BROWSE: browse(ID_MEMCARD, "Folder for the memory card saves"); return 0;
        case ID_DATA_BROWSE: browse(ID_DATA, "Folder with the game disc files (SLUS_209.10 and DATA)"); return 0;
        case ID_IMPORT_PCSX2: import_pcsx2(); return 0;
        case ID_EDIT_SAVE: open_save_editor(); return 0;
        case ID_DEFAULTS:
            g_cfg = Config();
            load_into_ui();
            update_enabled();
            return 0;
        case ID_SAVE:
            if (save()) MessageBoxA(h, "Settings saved.", "Eve of Destruction setup", MB_OK | MB_ICONINFORMATION);
            return 0;
        case ID_PLAY: play(); return 0;
        case ID_EXIT: PostMessageA(h, WM_CLOSE, 0, 0); return 0;
        }
        break;
    case WM_HSCROLL:
        if ((HWND)lp == GetDlgItem(h, ID_VOLUME)) {
            char v[32];
            snprintf(v, sizeof v, "%d%%", (int)SendDlgItemMessageA(h, ID_VOLUME, TBM_GETPOS, 0, 0));
            SetDlgItemTextA(h, ID_VOLUME_LABEL, v);
        }
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

}  // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int show) {
    g_inst = inst;
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_WIN95_CLASSES | ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&icc);
    CoInitialize(nullptr);

    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    g_dir = path;
    g_dir = g_dir.substr(0, g_dir.find_last_of("\\/"));
    config_load(g_dir + "\\eod.ini", g_cfg);

    NONCLIENTMETRICSA ncm = {};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    g_font = CreateFontIndirectA(&ncm.lfMessageFont);
    LOGFONTA b = ncm.lfMessageFont;
    b.lfWeight = FW_BOLD;
    g_bold = CreateFontIndirectA(&b);

    WNDCLASSA wc = {};
    wc.lpfnWndProc = main_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(1));
    if (!wc.hIcon) wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = "EodSetup";
    RegisterClassA(&wc);
    wc.lpfnWndProc = keys_proc;
    wc.lpszClassName = "EodSetupKeys";
    RegisterClassA(&wc);
    wc.lpfnWndProc = save_proc;
    wc.lpszClassName = "EodSetupSave";
    RegisterClassA(&wc);

    RECT r = {0, 0, 520, 610};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    g_main = CreateWindowA("EodSetup", "Eve of Destruction - Setup", style, (GetSystemMetrics(SM_CXSCREEN) - ww) / 2,
                           (GetSystemMetrics(SM_CYSCREEN) - wh) / 2, ww, wh, nullptr, nullptr, inst, nullptr);
    build_ui(g_main);
    load_into_ui();
    update_enabled();
    ShowWindow(g_main, show);

    MSG m;
    while (GetMessageA(&m, nullptr, 0, 0) > 0) {
        if (keys_filter(m)) continue;
        HWND top = GetAncestor(m.hwnd, GA_ROOT);
        if (top && IsDialogMessageA(top, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    CoUninitialize();
    return 0;
}
