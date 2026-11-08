// First launch uses the same verified, transactional disc importer as macOS.
#include "windows_disc.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace fs = std::filesystem;

void windows_utf8_arguments(int &argc, char **&argv) {
    int wide_argc = 0;
    wchar_t **wide_argv = CommandLineToArgvW(GetCommandLineW(), &wide_argc);
    if (!wide_argv) return;
    static std::vector<std::string> strings;
    static std::vector<char *> pointers;
    strings.clear(); pointers.clear();
    strings.reserve(wide_argc);
    for (int i = 0; i < wide_argc; ++i) strings.push_back(fs::path(wide_argv[i]).u8string());
    LocalFree(wide_argv);
    for (auto &s : strings) pointers.push_back(s.data());
    pointers.push_back(nullptr);
    argc = wide_argc; argv = pointers.data();
}

bool windows_prepare_disc(std::string &root, const std::string &exe_dir, bool headless, const DiscId &id) {
    const char *narrow_env = std::getenv("EOD_DATA");
    if (root.empty() || (narrow_env && root == narrow_env)) {
        if (const wchar_t *wide_env = _wgetenv(L"EOD_DATA"))
            if (*wide_env) root = fs::path(wide_env).u8string();
    }
    wchar_t local[MAX_PATH];
    if (SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local) != S_OK) return false;
    const auto install = fs::path(local) / L"TDEODRecomp" / L"disc";
    std::error_code ec;
    const auto recovery = disc_recover_install(install.u8string());
    if (!recovery.ok()) std::fprintf(stderr, "%s\n", recovery.message.c_str());
    if (!root.empty() && disc_check_folder(root, id).ok()) return true;
    // Explicit sources (including a folder holding a single ISO) must be tried,
    // rather than silently ignored in favor of an older installed copy.
    const bool explicit_source = !root.empty();
    if (!explicit_source) {
        for (const auto &dir : {root, install.u8string(), exe_dir, std::string(".")}) {
            if (!dir.empty() && disc_check_folder(dir, id).ok()) { root = dir; return true; }
        }
    }
    std::string image = explicit_source ? root : "";
    for (;;) {
        if (image.empty()) {
            if (headless) { std::fprintf(stderr, "Choose a game ISO or pass --data <image>.\n"); return false; }
            wchar_t path[32768] = {};
            OPENFILENAMEW chooser{};
            chooser.lStructSize = sizeof chooser;
            chooser.lpstrTitle = L"Choose your Eve of Destruction disc image";
            chooser.lpstrFilter = L"Game disc image (*.iso;*.cue;*.bin)\0*.iso;*.cue;*.bin\0All files\0*.*\0";
            chooser.lpstrFile = path; chooser.nMaxFile = 32768;
            chooser.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
            if (!GetOpenFileNameW(&chooser)) {
                if (CommDlgExtendedError()) MessageBoxW(nullptr, L"The disc picker could not open. Try launching the game again.", L"Eve of Destruction", MB_OK | MB_ICONERROR);
                return false;
            }
            image = fs::path(path).u8string();
        }
        HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        IProgressDialog *progress = nullptr;
        if (!headless && SUCCEEDED(CoCreateInstance(CLSID_ProgressDialog,nullptr,CLSCTX_INPROC_SERVER,IID_IProgressDialog, reinterpret_cast<void **>(&progress)))) {
            progress->SetTitle(L"Installing Eve of Destruction");
            progress->SetLine(1,L"Copying game files from your disc image…",FALSE,nullptr);
            progress->StartProgressDialog(nullptr,nullptr,PROGDLG_AUTOTIME,nullptr);
        }
        const auto result = disc_import(image, install.u8string(), id, [&](uint64_t done,uint64_t total,const std::string &) {
            if (!progress) return true;
            progress->SetProgress64(done,total);
            return !progress->HasUserCancelled();
        });
        if (progress) { progress->StopProgressDialog(); progress->Release(); }
        if (SUCCEEDED(init)) CoUninitialize();
        if (result.ok()) { root = install.u8string(); return true; }
        std::fprintf(stderr,"Disc install: %s\n",result.message.c_str());
        if (headless || result.status == DiscStatus::Cancelled) return false;
        auto msg = fs::u8path(result.message + "\n\nChoose another disc image?").wstring();
        if (MessageBoxW(nullptr,msg.c_str(),L"Eve of Destruction",MB_RETRYCANCEL|MB_ICONWARNING) != IDRETRY) return false;
        image.clear();
    }
}
