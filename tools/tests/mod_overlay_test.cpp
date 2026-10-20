// Exercise the real FILEIO resolver and stream TOC against disposable files.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <unistd.h>
namespace fs = std::filesystem;
#ifdef TEST_STREAM_OVERLAY
#include "../../runtime/iop/stream.cpp"
#else
#include "../../runtime/iop/fileio.cpp"
#endif
std::string g_game_root;
bool g_log_enabled[LOG_CAT_COUNT] = {};
void log_write(LogCat, const char *, ...) {}
bool platform_list_dir(const char *path, std::vector<DirEntryInfo> &out) {
    if (!fs::is_directory(path)) return false;
    for (const auto &entry : fs::directory_iterator(path)) {
        DirEntryInfo info{};
        snprintf(info.name, sizeof info.name, "%s", entry.path().filename().string().c_str());
        info.is_dir = entry.is_directory();
        info.size = info.is_dir ? 0 : entry.file_size();
        out.push_back(info);
    }
    return true;
}
bool platform_find_file(const char *root, const char *rel, char *out, size_t size) {
    fs::path p(root);
    for (auto part : fs::path(rel)) {
        if (!fs::is_directory(p)) return false;
        bool found = false;
        for (const auto &entry : fs::directory_iterator(p)) {
            auto a = entry.path().filename().string(), b = part.string();
            for (auto &c : a) c = (char)toupper((unsigned char)c);
            for (auto &c : b) c = (char)toupper((unsigned char)c);
            if (a == b) { p = entry.path(); found = true; break; }
        }
        if (!found) return false;
    }
    snprintf(out, size, "%s", p.string().c_str());
    return true;
}
static void require(bool yes, const char *message) {
    if (!yes) { std::cerr << message << '\n'; std::exit(1); }
}
static void write(const fs::path &p, const char *s) {
    fs::create_directories(p.parent_path()); std::ofstream(p) << s;
}
int main() {
    char tmp[] = "/tmp/eod-overlay-XXXXXX";
    require(mkdtemp(tmp) != nullptr, "temporary directory");
    const fs::path base(tmp), old = fs::current_path();
    g_game_root = (base / "disc").string();
    write(base / "disc/DATA/CAR/BASE.CAR", "retail");
    write(base / "selected/data/car/BASE.CAR", "replacement larger than retail");
    write(base / "selected/data/car/CUSTOM.CAR", "new custom car");
    write(base / "selected/data/SPEECH.BNK", "real selected bank payload");
    write(base / "mods/unrelated/DATA/CAR/BASE.CAR", "wrong mod");
    fs::current_path(base);
    unsetenv("EOD_MOD_DIR");
#ifdef TEST_STREAM_OVERLAY
    build_disc();
    auto find = [](const std::string &name) -> const DiscFile * {
        for (const auto &file : g_files) if (fs::path(file.host_path).filename() == name) return &file;
        return nullptr;
    };
    require(find("BASE.CAR") && find("BASE.CAR")->size == 6, "no mod selected must retain retail");
    g_files.clear(); g_toc.clear(); g_built = false; g_next_lsn = 0x100;
    setenv("EOD_MOD_DIR", (base / "selected").c_str(), 1);
    build_disc();
    require(find("CUSTOM.CAR") != nullptr, "new car must enter disc TOC");
    require(find("BASE.CAR")->size == 30, "selected replacement size must determine sectors");
    char bytes[14]{};
    disc_read(find("CUSTOM.CAR")->lsn, (uint8_t *)bytes, 14);
    require(std::string(bytes, 14) == "new custom car", "LSN reads must reach selected new car");
    require(find("SPEECH.BNK") != nullptr, "selected bank replaces missing retail bank");
    disc_read(find("SPEECH.BNK")->lsn, (uint8_t *)bytes, 4);
    require(std::string(bytes, 4) == "real", "host bank must replace synthetic fallback bytes");
#else
    std::string host;
    require(resolve("cdrom0:\\DATA\\CAR\\BASE.CAR;1", host, false), "retail resolve");
    require(host == (base / "disc/DATA/CAR/BASE.CAR").string(), "no implicit mod loading");
    setenv("EOD_MOD_DIR", (base / "selected").c_str(), 1);
    require(resolve("DATA/CAR/BASE.CAR", host, false), "mod resolve");
    require(host == (base / "selected/data/car/BASE.CAR").string(), "selected mod wins");
    require(resolve("DATA/CAR/BASE.CAR", host, true), "write resolve");
    require(host == (base / "disc/DATA/CAR/BASE.CAR").string(), "writes must not modify mod package");
    require(resolve("DATA/CAR/CUSTOM.CAR", host, false), "new file resolves via FILEIO");
    for (uint32_t flags : {2u, 3u, 3u | 0x400u, 3u | 0x100u}) {
        const int fd = file_open("DATA/CAR/BASE.CAR", flags);
        require(fd >= 3, "writable retail open");
        fputs("changed", file_get(fd));
        fclose(file_get(fd)); g_files[fd - 3] = nullptr;
        require(fs::file_size(base / "selected/data/car/BASE.CAR") == 30,
                "actual writable opens must leave selected overlay untouched");
    }
#endif
    fs::current_path(old); fs::remove_all(base);
    std::cout << "overlay contract passed\n";
}
