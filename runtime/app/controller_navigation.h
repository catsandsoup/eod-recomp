#pragma once
#include "../platform/platform.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace controller_ui {
struct Event {
    int horizontal=0,vertical=0;
    bool accept=false,back=false,auxiliary=false,left_shoulder=false,right_shoulder=false,start=false;
};
class Navigation {
public:
    void set_owner(uint32_t device);
    Event poll(uint64_t now_ms,uint32_t device,const HostPadState&pad,bool focused);
    bool armed()const{return armed_;}
private:
    uint32_t owner_=0;
    uint16_t previous_=0;
    int x_=0,y_=0;
    uint64_t repeat_=0;
    bool armed_=false;
};
size_t move_selection(size_t current,int direction,const std::vector<bool>&enabled);
class TextEntry {
public:
    TextEntry(std::string initial,size_t capacity);
    const std::string&value()const{return value_;}
    void append(char value);
    void erase();
    void cancel(){value_=initial_;}
    void move(int x,int y);
    char selected_character()const;
    void confirm(){append(selected_character());}
    size_t selected()const{return selected_;}
    static const std::string&characters();
private:
    std::string initial_,value_;
    size_t capacity_,selected_=0;
};
}
