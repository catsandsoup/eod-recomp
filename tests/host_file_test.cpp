// A real filesystem check of the runtime's UTF-8 path convention.
#include "../runtime/core/host_file.h"
#include <chrono>
#include <cstring>
#include <filesystem>
#ifdef EOD_TEST_WIN_PLATFORM
#include "../runtime/platform/platform.h"
#include "../runtime/core/config.h"
// The platform translation unit also contains window/pad code. These inert
// dependencies let this standalone test link; no window or game is started.
bool overlay_wndproc(void *, unsigned, uintptr_t, intptr_t) { return false; }
bool overlay_captures_input() { return false; }
uint64_t kernel_vblank_count() { return 0; }
#endif
namespace fs = std::filesystem;
int main() {
    const auto root = fs::temp_directory_path() / fs::u8path("EOD Montréal 日本語-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { fs::path p; ~Cleanup() { std::error_code ec; fs::remove_all(p, ec); } } cleanup{root};
    fs::create_directories(root);
    const auto name = (root / fs::u8path("disc 日本語.bin")).u8string();
    FILE *file = host_fopen(name.c_str(), "wb");
    if (!file) return 1;
    const char bytes[] = "owner disc data";
    if (fwrite(bytes, 1, sizeof bytes, file) != sizeof bytes || fclose(file)) return 2;
    file = host_fopen(name.c_str(), "rb");
    if (!file) return 3;
    char read[sizeof bytes] = {};
    const auto n = fread(read, 1, sizeof read, file); fclose(file);
    if (n != sizeof bytes || memcmp(read, bytes, sizeof bytes)) return 4;
    if (host_fopen((name + ".missing").c_str(), "rb")) return 5;
#ifdef EOD_TEST_WIN_PLATFORM
    char found[1024];
    if (!platform_find_file(root.u8string().c_str(), "disc 日本語.bin", found, sizeof found)) return 6;
    file = host_fopen(found, "rb");
    if (!file) return 7;
    fclose(file);
    char tiny[2];
    if (platform_find_file(root.u8string().c_str(), "disc 日本語.bin", tiny, sizeof tiny)) return 8;
    if (!platform_mkdir((root / fs::u8path("cards 日本語")).u8string().c_str())) return 9;
    std::vector<DirEntryInfo> entries;
    if (!platform_list_dir(root.u8string().c_str(), entries)) return 10;
    bool saw_file = false, saw_dir = false;
    for (const auto &e : entries) {
        if (!strcmp(e.name, "disc 日本語.bin")) saw_file = !e.is_dir && e.size == sizeof bytes;
        if (!strcmp(e.name, "cards 日本語")) saw_dir = e.is_dir;
    }
    if (!saw_file || !saw_dir) return 11;
    Config written, loaded;
    written.data_dir = root.u8string();
    written.memcard_dir = (root / fs::u8path("cards 日本語")).u8string();
    const auto config = (root / fs::u8path("réglages 日本語.ini")).u8string();
    if (!config_save(config, written) || !config_load(config, loaded)) return 12;
    if (loaded.data_dir != written.data_dir || loaded.memcard_dir != written.memcard_dir) return 13;
#endif
    puts("host_file_test: UTF-8 filesystem IO passed");
    return 0;
}
