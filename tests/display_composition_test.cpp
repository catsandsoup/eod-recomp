#include "../runtime/platform/display_rect.h"
#include "../runtime/platform/screenshot_size.h"
#include "../runtime/core/config.h"
#include "../runtime/platform/platform.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#ifdef EOD_DISPLAY_BACKEND_TEST
extern "C" { uint8_t *g_ram = nullptr; }
uint64_t kernel_vblank_count() { return 0; }
uint64_t platform_time_us() { return 0; }
bool mac_app_settings() { return false; }
#endif
static int failures;
static void rect(int aspect,int width,int height,DisplayRect expected) {
    auto got=display_rect(aspect,width,height);
    if(got.x!=expected.x || got.y!=expected.y || got.width!=expected.width || got.height!=expected.height) {
        fprintf(stderr,"rect aspect%d %dx%d -> %d,%d %dx%d\n",aspect,width,height,got.x,got.y,got.width,got.height);++failures;
    }
}
int main() {
    rect(3,3840,2400,{0,0,3840,2400});
    rect(1,3840,2400,{0,120,3840,2160});
    rect(3,3840,2160,{192,0,3456,2160});
    rect(0,3840,2160,{480,0,2880,2160});
    rect(3,3024,1964,{0,37,3024,1890}); // MBP: avoid stretching into the extra top/bottom area
    rect(1,3024,1964,{0,131,3024,1701});
    rect(3,2880,1800,{0,0,2880,1800});
    rect(2,3840,2400,{0,0,3840,2400});
    rect(3,0,2400,{0,0,0,0});
    auto size=screenshot_size(640,448,6,"3840","2400");
    if(!size.valid || !size.composed || size.width!=3840 || size.height!=2400) ++failures;
    auto native=screenshot_size(640,448,6,nullptr,nullptr);
    if(!native.valid || native.composed || native.width!=3840 || native.height!=2688) ++failures;
    if(screenshot_size(640,448,6,"3840",nullptr).valid || screenshot_size(640,448,6,"90000","2400").valid ||
       screenshot_size(640,448,6,"3840x","2400").valid || screenshot_size(640,448,6,"0","2400").valid) ++failures;
    if(display_aspect("4:3")!=0 || display_aspect("16:9")!=1 || display_aspect("stretch")!=2 || display_aspect("16:10")!=3) ++failures;
#ifdef EOD_DISPLAY_BACKEND_TEST
    PlatformConfig startup;startup.aspect=3;
    platform_configure_display(startup); // no SDL_Init, window, audio or controller discovery
    g_config.aspect="4:3"; // a later Settings save must not change this launch's camera/composition
    int x,y,w,h;platform_display_rect(3840,2400,&x,&y,&w,&h);
    if(x || y || w!=3840 || h!=2400) ++failures;
#endif
    std::string file=(std::filesystem::temp_directory_path()/"eod-aspect-config.XXXXXX").string();
    int fd=mkstemp(file.data());if(fd<0)return 2;close(fd);
    Config saved,loaded;
    if(saved.aspect!="4:3") ++failures;
    saved.aspect="16:10";saved.render_scale=6;saved.width=1920;saved.height=1200;
    if(!config_save(file,saved) || !config_load(file,loaded) || loaded.aspect!="16:10" || loaded.render_scale!=6) ++failures;
    std::filesystem::remove(file);
    if(failures) { fprintf(stderr,"display composition: %d failure(s)\n",failures);return 1; }
    puts("4K/MBP aspect rectangles, output capture dimensions, and 16:10 config round-trip passed (CPU only)");
}
