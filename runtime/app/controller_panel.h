#pragma once
#include <functional>
#include <string>
#include <vector>
namespace controller_ui {
struct Row {
    std::string label,value;
    bool enabled=true;
    std::function<void()> activate;
    std::function<void(int)> adjust;
};
struct Page {
    std::string title,message;
    std::vector<Row> rows;
    int columns=1;
    std::function<void()> back,left_shoulder,right_shoulder;
    std::function<void()> tick;
};
// Main thread only. Initial launcher/recovery may run before the EE starts;
// in-game online panels are modeless and poll through platform_pump.
int run_panel(Page page);
void open_panel(Page page);
void replace_page(Page page,bool preserve_selection=false);
void close_panel(int result=0);
void panel_tick();
bool panel_visible();
void text_entry(std::string title,std::string initial,size_t capacity,
                std::function<void(std::string)> accepted,std::function<void()>cancelled);
}
