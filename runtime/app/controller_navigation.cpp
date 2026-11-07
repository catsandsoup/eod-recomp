#include "controller_navigation.h"
#include <algorithm>
namespace controller_ui {
void Navigation::set_owner(uint32_t device){
    if(owner_==device)return;
    owner_=device;previous_=0;x_=y_=0;repeat_=0;armed_=false;
}
Event Navigation::poll(uint64_t now,uint32_t device,const HostPadState&pad,bool focused){
    Event result;
    if(!owner_||device!=owner_)return result;
    if(!pad.connected||!focused){previous_=0;x_=y_=0;repeat_=0;armed_=false;return result;}
    int x=((pad.buttons&PAD_RIGHT)||pad.lx>176)-int((pad.buttons&PAD_LEFT)||pad.lx<80);
    int y=((pad.buttons&PAD_DOWN)||pad.ly>176)-int((pad.buttons&PAD_UP)||pad.ly<80);
    if(!armed_){
        if(!pad.buttons&&!x&&!y)armed_=true;
        previous_=pad.buttons;return result;
    }
    const uint16_t pressed=pad.buttons&~previous_;previous_=pad.buttons;
    result.accept=(pressed&PAD_CROSS)!=0;result.back=(pressed&(PAD_CIRCLE|PAD_TRIANGLE))!=0;
    result.auxiliary=(pressed&PAD_SQUARE)!=0;result.left_shoulder=(pressed&PAD_L1)!=0;
    result.right_shoulder=(pressed&PAD_R1)!=0;result.start=(pressed&PAD_START)!=0;
    if(x!=x_||y!=y_){result.horizontal=x;result.vertical=y;repeat_=now+350;}
    else if((x||y)&&now>=repeat_){result.horizontal=x;result.vertical=y;repeat_=now+100;}
    if(!x&&!y)repeat_=0;
    x_=x;y_=y;return result;
}
size_t move_selection(size_t current,int direction,const std::vector<bool>&enabled){
    if(enabled.empty())return 0;
    current=std::min(current,enabled.size()-1);
    if(!direction)return current;
    size_t next=current;
    for(size_t n=0;n<enabled.size();++n){
        next=direction>0?(next+1)%enabled.size():(next+enabled.size()-1)%enabled.size();
        if(enabled[next])return next;
    }
    return current;
}
TextEntry::TextEntry(std::string initial,size_t capacity):initial_(std::move(initial)),value_(initial_),capacity_(capacity){
    if(value_.size()>capacity_)value_.resize(capacity_);
}
void TextEntry::append(char value){
    if(value>=' '&&value<='~'&&value_.size()<capacity_)value_+=value;
}
void TextEntry::erase(){if(!value_.empty())value_.pop_back();}
const std::string&TextEntry::characters(){
    static const std::string chars="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.-_:[]/@ ";return chars;
}
void TextEntry::move(int x,int y){
    const int count=int(characters().size()),next=int(selected_)+x+10*y;
    selected_=size_t((next%count+count)%count);
}
char TextEntry::selected_character()const{return characters()[selected_];}
}
