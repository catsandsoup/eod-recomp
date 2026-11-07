#include "app/game_frontend.h"
#include "app/controller_panel.h"
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
namespace fs=std::filesystem;
int main(int argc,char**argv){
    if(argc!=2)return 1;
    const fs::path root=argv[1];fs::create_directories(root/"disc/DATA");
    std::ofstream(root/"disc/SLUS_209.10",std::ios::binary)<<"fixture";
    setenv("EOD_HEADLESS","1",1);setenv("EOD_MUTE","1",1);setenv("EOD_MOD_DIR","",1);
    setenv("EOD_APP_SUPPORT",(root/"support").c_str(),1);
    const std::string elf="fixture";DiscId id{unsigned(elf.size()),disc_crc32(reinterpret_cast<const uint8_t*>(elf.data()),elf.size())};
    std::string data=(root/"disc").string();PlatformConfig display;
    assert(game_frontend::prepare(data,(root/"eod.ini").string(),true,id,display));
    assert(!fs::exists(root/"eod.ini"));assert(!game_frontend::captures_input());
    game_frontend::show_menu();game_frontend::tick();assert(!controller_ui::panel_visible());
    data=(root/"missing").string();assert(!game_frontend::prepare(data,(root/"eod.ini").string(),true,id,display));
    assert(!game_frontend::paused());
    std::cout<<"Frontend headless paths preserve explicit disc errors and never open a chooser/UI\n";
}
