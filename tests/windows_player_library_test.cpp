// Exercise the real startup path and wide-character filesystem on a private user profile.
#include "../runtime/app/windows_disc.h"
#include "../runtime/app/windows_settings.h"
#include <fstream>

namespace fs = std::filesystem;

int main() {
    const auto scratch = fs::temp_directory_path() /
        (L"eod-library-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    const auto first = scratch / L"release-1";
    const auto second = scratch / L"release-2";
    const auto library = scratch / fs::u8path(u8"Bibliothèque; 車");
    const auto support = scratch / L"profile";
    _wputenv_s(L"EOD_APP_SUPPORT", support.c_str());
    _wputenv_s(L"EOD_CONFIG_PATH", L"");
    fs::create_directories(first);
    fs::create_directories(second);
    fs::create_directories(library / "DATA");
    const std::string elf = "owned-game-fixture";
    std::ofstream(library / L"SLUS_209.10", std::ios::binary) << elf;
    const DiscId id{static_cast<uint32_t>(elf.size()), disc_crc32(reinterpret_cast<const uint8_t *>(elf.data()), elf.size())};
    std::ofstream(first / L"eod.ini") << "[Audio]\nVolume=36\n[System]\nDataPath=../" << library.filename().u8string() << "\n";
    std::ofstream(second / L"eod.ini") << "[System]\nDataPath=distributor-default\n";
    int failed = 0;
    auto check = [&](bool valid, const char *label) {
        std::printf("%s %s\n", valid ? "PASS" : "FAIL", label);
        if (!valid) ++failed;
    };
    std::string path = windows_settings_path(first.u8string());
    Config migrated;
    check(fs::u8path(path) == support / "eod.ini" && config_load(path, migrated), "migrated to per-user settings");
    std::string root = migrated.data_dir;
    check(fs::u8path(root) == library && migrated.volume == 36, "relative legacy library resolved; Unicode, semicolon and volume retained");
    check(windows_prepare_disc(root, first.u8string(), true, id), "first release accepts the extracted library");
    fs::rename(first, scratch / L"removed-old-release");
    path = windows_settings_path(second.u8string());
    Config upgraded;
    check(config_load(path, upgraded) && upgraded.data_dir == root, "different release folder retains the player's settings");
    check(windows_prepare_disc(root, second.u8string(), true, id) && fs::u8path(root) == library, "updated game opens the same library without a source argument");

    const auto moved = scratch / L"external-disc-now-elsewhere";
    fs::rename(library, moved);
    check(!windows_prepare_disc(root, second.u8string(), true, id), "unavailable library rejected without changing the preference");
    check(config_load(path, upgraded) && fs::u8path(upgraded.data_dir) == library, "failed source preserves saved preference");
    root = (moved / L"SLUS_209.10").u8string();
    check(windows_prepare_disc(root, second.u8string(), true, id) && fs::u8path(root) == moved, "selecting an extracted disc file uses its containing folder directly");

    const auto explicit_ini = scratch / L"isolated.ini";
    _wputenv_s(L"EOD_CONFIG_PATH", explicit_ini.c_str());
    check(fs::u8path(windows_settings_path(second.u8string())) == explicit_ini, "explicit settings path isolates alternate profiles");
    fs::remove_all(scratch);
    return failed ? 1 : 0;
}
