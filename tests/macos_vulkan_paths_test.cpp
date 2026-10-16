#include "../runtime/app/macos_vulkan.h"
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <cstdio>
#include <unistd.h>
namespace fs = std::filesystem;
int main() {
    char pattern[]="/private/tmp/eod-vulkan-paths.XXXXXX";
    char *temporary=mkdtemp(pattern); if (!temporary) return 1;
    fs::path app=fs::path(temporary)/"Copied App With Spaces.app";
    fs::path contents=app/"Contents";
    fs::create_directories(contents/"MacOS");
    fs::create_directories(contents/"Frameworks");
    fs::create_directories(contents/"Resources/vulkan/icd.d");
    std::ofstream(contents/"MacOS/eod").put('x');
    std::ofstream(contents/"Frameworks/libvulkan.1.4.363.dylib").put('x');
    fs::path manifest=contents/"Resources/vulkan/icd.d/MoltenVK_icd.json";
    std::ofstream(manifest).put('x');
    setenv("VK_DRIVER_FILES","/external/invalid.json",1);
    setenv("VK_ICD_FILENAMES","/external/invalid.json",1);
    setenv("SDL_VULKAN_LIBRARY","/external/loader.dylib",1);
    mac_app_init_vulkan_at((contents/"MacOS/eod").string());
    bool ok=std::string(getenv("VK_DRIVER_FILES"))==manifest.string() &&
        std::string(getenv("VK_ICD_FILENAMES"))==manifest.string() &&
        std::string(getenv("SDL_VULKAN_LIBRARY"))==(contents/"Frameworks/libvulkan.1.4.363.dylib").string();
    setenv("VK_DRIVER_FILES","plain-build-override",1);
    mac_app_init_vulkan_at("/private/tmp/eod");
    ok=ok && std::string(getenv("VK_DRIVER_FILES"))=="plain-build-override";
    fs::remove_all(temporary);
    if (!ok) { fputs("Vulkan app path discovery failed\n",stderr); return 1; }
    puts("Vulkan discovery uses relocated bundle paths; plain builds preserve overrides");
}
