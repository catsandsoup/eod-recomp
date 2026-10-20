// Explicit disc validation must finish without starting SDL or showing UI.
#import <Cocoa/Cocoa.h>
#include "../runtime/app/macos_app.h"
#include "../runtime/core/config.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unistd.h>
namespace fs = std::filesystem;
int main() {
    setenv("EOD_HEADLESS", "1", 1);
    setenv("EOD_MUTE", "1", 1);
    char pattern[] = "/private/tmp/eod-startup-test.XXXXXX";
    char *temporary = mkdtemp(pattern);
    if (!temporary) return 1;
    fs::path scratch(temporary);
    std::string support = (scratch / "support").string();
    setenv("EOD_APP_SUPPORT", support.c_str(), 1);
    std::string config = (scratch / "eod.ini").string();
    const uint8_t elf[] = {'t','e','s','t'};
    DiscId id{sizeof(elf), disc_crc32(elf,sizeof(elf))};
    int failures = 0;
    auto check = [&](bool condition, const char *description) {
        if (!condition) { fprintf(stderr, "FAIL: %s\n", description); ++failures; }
    };
    std::string data = (scratch / "missing").string();
    check(!mac_app_prepare(data,config,true,id), "missing explicit folder rejected");
    fs::create_directories(data);
    check(!mac_app_prepare(data,config,true,id), "missing executable rejected");
    std::ofstream(data + "/SLUS_209.10",std::ios::binary).write((const char *)elf,sizeof(elf));
    check(!mac_app_prepare(data,config,true,id), "missing DATA rejected");
    fs::create_directory(data + "/DATA");
    DiscId wrong = id; wrong.elf_crc32 ^= 1;
    check(!mac_app_prepare(data,config,true,wrong), "wrong game version rejected");
    check(mac_app_prepare(data,config,true,id), "valid explicit disc accepted");
    data.clear();
    std::string missing = (scratch / "absent-env").string();
    setenv("EOD_DATA",missing.c_str(),1);
    check(!mac_app_prepare(data,config,true,id), "invalid EOD_DATA rejected");
    check(data == missing, "explicit environment path retained without fallback");
    check(NSApp == nil, "headless validation never creates NSApplication");
    unsetenv("EOD_DATA");
    fs::create_directories(fs::path(support) / "mods/phantom/DATA/CAR");
    fs::create_directories(fs::path(support) / "mods/phantom/DATA/RES");
    setenv("EOD_MOD_DIR", "explicit-test-overlay", 1);
    check(mac_app_select_mods(true), "headless startup bypasses selector");
    check(mac_app_select_mods(false), "explicit developer overlay bypasses visible selector");
    check(std::string(getenv("EOD_MOD_DIR")) == "explicit-test-overlay", "explicit overlay preserved");
    check(!mac_app_set_mod_profile("../disc"), "unknown profile rejected");
    check(!mac_app_set_mod_profile("phantom"), "incomplete profile rejected");
    check(std::string(getenv("EOD_MOD_DIR")) == "explicit-test-overlay", "failed selection preserves current overlay");
    for (const char *file : {"CAR/PHANTOM.CAR", "RES/COMMON.RES", "RES/CT.RES", "RES/UICAR.RES"}) {
        std::ofstream(fs::path(support) / "mods/phantom/DATA" / file).put('x');
    }
    check(mac_app_set_mod_profile("phantom"), "complete installed profile selected");
    check(std::string(getenv("EOD_MOD_DIR")) == support + "/mods/phantom", "profile resolves inside application support");
    check(mac_app_set_mod_profile("native"), "native profile selected");
    check(getenv("EOD_MOD_DIR") == nullptr, "native mode removes overlay");
    check(NSApp == nil, "profile selection tests never create NSApplication");
    fs::remove_all(scratch);
    if (!failures) puts("macos_startup_test: all tests passed without SDL initialization");
    return failures ? 1 : 0;
}
