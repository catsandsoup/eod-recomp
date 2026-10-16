#include "macos_vulkan.h"
#include <mach-o/dyld.h>
#include <filesystem>
#include <cstdlib>
#include <vector>
namespace fs = std::filesystem;
void mac_app_init_vulkan_at(const std::string &executable) {
    std::error_code ec;
    fs::path binary = fs::weakly_canonical(executable, ec);
    if (ec || binary.parent_path().filename() != "MacOS") return;
    fs::path contents = binary.parent_path().parent_path();
    if (contents.filename() != "Contents") return;
    fs::path manifest = contents / "Resources/vulkan/icd.d/MoltenVK_icd.json";
    fs::path loader = contents / "Frameworks/libvulkan.1.dylib";
    if (!fs::is_regular_file(manifest, ec)) return;
    if (!fs::is_regular_file(loader, ec)) {
        // Homebrew resolves the loader symlink to its full versioned basename
        // when packaging, e.g. libvulkan.1.4.363.dylib.
        loader.clear(); ec.clear();
        for (const auto &entry : fs::directory_iterator(contents / "Frameworks", ec)) {
            const auto name = entry.path().filename().string();
            if (name.rfind("libvulkan.", 0) == 0 && entry.path().extension() == ".dylib" && entry.is_regular_file(ec)) {
                loader = entry.path(); break;
            }
        }
        if (loader.empty()) return;
    }
    // The manifest's driver path is relative to the manifest, so copied apps
    // work regardless of the install location, cwd or Homebrew installation.
    setenv("VK_DRIVER_FILES", manifest.c_str(), 1);
    setenv("VK_ICD_FILENAMES", manifest.c_str(), 1); // older Vulkan loaders
    setenv("SDL_VULKAN_LIBRARY", loader.c_str(), 1);
}
void mac_app_init_vulkan() {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> path(size);
    if (_NSGetExecutablePath(path.data(), &size) == 0)
        mac_app_init_vulkan_at(path.data());
}
