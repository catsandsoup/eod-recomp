// layered_vfs_test.cpp - Tests for live hot-mountable 4-layer C++ VFS.
#include "vfs/layered_vfs.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using eod::vfs::Layer;
using eod::vfs::LayeredVfs;

namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

void write_file(const fs::path &path, const std::string &content) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << content;
}

std::string read_file(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
    const fs::path base =
        fs::temp_directory_path() / ("eod-layered-vfs-test-" + std::to_string(std::rand()));
    fs::remove_all(base);
    fs::create_directories(base);

    const fs::path retail_dir = base / "retail";
    const fs::path profile_dir = base / "profile";
    const fs::path server_dir = base / "server";
    const fs::path pie_dir = base / "pie";
    const fs::path pie_v2_dir = base / "pie_v2";

    write_file(retail_dir / "DATA/CAR/CUP24.CAR", "retail-car");
    write_file(retail_dir / "DATA/ONLY_RETAIL.DAT", "retail-only");
    write_file(profile_dir / "data/car/cup24.car", "profile-car");
    write_file(server_dir / "Data/Car/Cup24.CAR", "server-car");
    write_file(pie_dir / "DATA/CAR/CUP24.CAR", "pie-car");
    write_file(pie_v2_dir / "DATA/CAR/CUP24.CAR", "pie-car-v2");
    write_file(base / "secret_outside.txt", "outside-secret");

    LayeredVfs vfs;

    // 1. Mount RetailBase (layer 0) and verify base resolution + overlay-only skips retail.
    require(vfs.mount(Layer::RetailBase, retail_dir.string(), "retail"), "mount RetailBase");
    require(vfs.is_mounted(Layer::RetailBase), "RetailBase mounted");
    require(vfs.layer_root(Layer::RetailBase) == retail_dir.lexically_normal().string(),
            "RetailBase root matches");
    require(!vfs.mount(Layer::ActiveProfile, (base / "does-not-exist").string()),
            "mounting non-existent dir must fail");

    std::string host;
    require(vfs.resolve("DATA/CAR/CUP24.CAR", host), "resolve from RetailBase");
    require(read_file(host) == "retail-car", "RetailBase payload matches");
    require(!vfs.resolve_overlay_only("DATA/CAR/CUP24.CAR", host),
            "resolve_overlay_only must skip RetailBase");

    // 2. Mount all 4 layers and verify strict priority: EphemeralPie > ServerSession > ActiveProfile > RetailBase.
    require(vfs.mount(Layer::ActiveProfile, profile_dir.string(), "profile-nascar"),
            "mount ActiveProfile");
    require(vfs.resolve("DATA/CAR/CUP24.CAR", host), "resolve from ActiveProfile");
    require(read_file(host) == "profile-car", "ActiveProfile overrides RetailBase");
    require(vfs.resolve_overlay_only("data/car/CUP24.car", host),
            "resolve_overlay_only resolves ActiveProfile case-insensitively");
    require(read_file(host) == "profile-car", "overlay-only returns ActiveProfile");

    require(vfs.mount(Layer::ServerSession, server_dir.string(), "srv-42"),
            "mount ServerSession");
    require(vfs.resolve("DATA/CAR/CUP24.CAR", host), "resolve from ServerSession");
    require(read_file(host) == "server-car", "ServerSession overrides ActiveProfile");

    require(vfs.mount(Layer::EphemeralPie, pie_dir.string(), "pie-1"),
            "mount EphemeralPie");
    require(vfs.resolve("data\\car\\cup24.car", host),
            "case-insensitive backslash resolve hits EphemeralPie");
    require(read_file(host) == "pie-car", "EphemeralPie overrides ServerSession");

    // Retail-only file still falls through cleanly.
    require(vfs.resolve("data/only_retail.dat", host), "fallthrough to RetailBase");
    require(read_file(host) == "retail-only", "fallthrough content matches");
    require(!vfs.resolve_overlay_only("data/only_retail.dat", host),
            "overlay-only must not return RetailBase-only file");

    // 3. Live hot-swap on EphemeralPie takes effect immediately.
    require(vfs.mount(Layer::EphemeralPie, pie_v2_dir.string(), "pie-2"),
            "hot-remount EphemeralPie");
    require(vfs.resolve("DATA/CAR/CUP24.CAR", host), "resolve after hot-remount");
    require(read_file(host) == "pie-car-v2", "hot-remount immediately serves v2");

    // 4. Live unmount restores lower layers immediately in exact priority order.
    vfs.unmount(Layer::EphemeralPie);
    require(!vfs.is_mounted(Layer::EphemeralPie), "EphemeralPie unmounted");
    require(vfs.layer_root(Layer::EphemeralPie).empty(), "unmounted layer root empty");
    require(vfs.resolve("DATA/CAR/CUP24.CAR", host), "resolve after PIE unmount");
    require(read_file(host) == "server-car", "unmounting PIE restores ServerSession");

    vfs.unmount(Layer::ServerSession);
    require(vfs.resolve("DATA/CAR/CUP24.CAR", host), "resolve after ServerSession unmount");
    require(read_file(host) == "profile-car", "unmounting ServerSession restores ActiveProfile");

    vfs.unmount(Layer::ActiveProfile);
    require(vfs.resolve("DATA/CAR/CUP24.CAR", host), "resolve after ActiveProfile unmount");
    require(read_file(host) == "retail-car", "unmounting ActiveProfile restores RetailBase");

    // 5. Security: path traversal and absolute paths strictly rejected.
    require(!vfs.resolve("../secret_outside.txt", host), "reject leading ..");
    require(!vfs.resolve("DATA/../../secret_outside.txt", host), "reject nested ..");
    require(!vfs.resolve("DATA\\..\\..\\secret_outside.txt", host), "reject backslash ..");
    require(!vfs.resolve("/etc/passwd", host), "reject leading /");
    require(!vfs.resolve("C:\\Windows\\win.ini", host), "reject drive prefix");
    require(!vfs.resolve("", host), "reject empty path");

    // 6. Singleton accessibility check.
    auto &gvfs = eod::vfs::global_vfs();
    require(gvfs.mount(Layer::ActiveProfile, profile_dir.string(), "global-test"),
            "global_vfs mount");
    require(gvfs.resolve_overlay_only("DATA/CAR/CUP24.CAR", host), "global_vfs overlay resolve");
    require(read_file(host) == "profile-car", "global_vfs payload matches");
    gvfs.unmount(Layer::ActiveProfile);
    require(!gvfs.is_mounted(Layer::ActiveProfile), "global_vfs unmount");

    fs::remove_all(base);
    std::cout << "layered_vfs_test passed (all 4-layer priority, hot-mount/unmount, case-insensitive, traversal checks OK)\n";
    return 0;
}
