#include "app/controller_panel.h"
#include <cassert>
#include <cstdlib>
#include <iostream>
int main(){
    setenv("EOD_HEADLESS","1",1);setenv("EOD_MUTE","1",1);
    controller_ui::Page page;page.title="Headless UI guard";
    assert(controller_ui::run_panel(page)==-1);
    controller_ui::open_panel(page);assert(!controller_ui::panel_visible());
    controller_ui::panel_tick();
    bool accepted=false;controller_ui::text_entry("Address","localhost",255,[&](std::string){accepted=true;},[]{});
    assert(!accepted&&!controller_ui::panel_visible());
    std::cout<<"Controller panel does not create UI in headless tests\n";
}
