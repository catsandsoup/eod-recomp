// The game and setup share one settings file across versioned installation folders.
#pragma once
#include "../core/config.h"
#include <windows.h>
#include <shlobj.h>
#include <filesystem>
#include <cstdio>
#include <cstdlib>

inline std::filesystem::path windows_support_folder() {
    if (const auto *override_dir = _wgetenv(L"EOD_APP_SUPPORT"); override_dir && *override_dir)
        return override_dir;
    wchar_t local[MAX_PATH];
    if (SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local) == S_OK)
        return std::filesystem::path(local) / L"TDEODRecomp";
    return {};
}

inline std::string windows_settings_path(const std::string &exe_dir) {
    namespace fs = std::filesystem;
    if (const auto *override_file = _wgetenv(L"EOD_CONFIG_PATH"); override_file && *override_file)
        return fs::path(override_file).u8string();
    const auto legacy = fs::u8path(exe_dir) / L"eod.ini";
    const auto support = windows_support_folder();
    if (support.empty()) return legacy.u8string();
    const auto target = support / L"eod.ini";
    std::error_code ec;
    fs::create_directories(support, ec);
    if (!ec && !fs::exists(target, ec) && fs::is_regular_file(legacy, ec)) {
        // Resolve portable installs' relative paths before changing their base.
        // Never replace a player's shared settings with a later release's copy.
        const auto temp = support / (L"eod.ini.migrate-" + std::to_wstring(GetCurrentProcessId()));
        if (fs::copy_file(legacy, temp, fs::copy_options::overwrite_existing, ec)) {
            Config migrated;
            bool ready = config_load(temp.u8string(), migrated);
            bool relative = false;
            for (std::string *value : {&migrated.data_dir, &migrated.memcard_dir}) {
                const auto old = fs::u8path(*value);
                if (!old.empty() && old.is_relative()) {
                    *value = fs::absolute(legacy.parent_path() / old, ec).lexically_normal().u8string();
                    if (ec) ready = false;
                    relative = true;
                }
            }
            if (ready && (!relative || config_save(temp.u8string(), migrated)))
                MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH);
            fs::remove(temp, ec);
        }
        if (!fs::is_regular_file(target, ec)) {
            std::fprintf(stderr, "Could not migrate settings to the user profile. Using the existing settings for this launch.\n");
            return legacy.u8string();
        }
    }
    return target.u8string();
}
