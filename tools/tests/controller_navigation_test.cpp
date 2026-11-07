#include "app/controller_navigation.h"
#include <cassert>
#include <iostream>
using namespace controller_ui;
int main(){
    Navigation nav;HostPadState p;p.connected=true;
    nav.set_owner(41);
    p.buttons=PAD_CROSS;assert(!nav.poll(0,41,p,true).accept); // held at connection cannot confirm
    p.buttons=0;nav.poll(1,41,p,true);
    p.buttons=PAD_CROSS;assert(nav.poll(2,41,p,true).accept);
    assert(!nav.poll(3,41,p,true).accept); // edge, not repeat
    p.buttons=0;nav.poll(4,41,p,true);
    p.buttons=PAD_CROSS;assert(!nav.poll(5,42,p,true).accept); // P2 does not own launcher
    nav.poll(6,41,p,false);assert(!nav.poll(7,41,p,true).accept); // focus regain must release
    p.buttons=0;nav.poll(8,41,p,true);p.buttons=PAD_CIRCLE;assert(nav.poll(9,41,p,true).back);
    p.buttons=0;nav.poll(10,41,p,true);p.buttons=PAD_DOWN;
    assert(nav.poll(20,41,p,true).vertical==1);
    assert(nav.poll(300,41,p,true).vertical==0);
    assert(nav.poll(420,41,p,true).vertical==1);
    assert(nav.poll(450,41,p,true).vertical==0);
    p.connected=false;nav.poll(500,41,p,true);p.connected=true;
    assert(nav.poll(501,41,p,true).vertical==0);p.buttons=0;nav.poll(502,41,p,true);
    p.ly=40;assert(nav.poll(503,41,p,true).vertical==-1);p.ly=128;nav.poll(504,41,p,true);
    std::vector<bool> enabled={true,false,true};assert(move_selection(0,1,enabled)==2);
    assert(move_selection(2,1,enabled)==0);assert(move_selection(0,-1,enabled)==2);
    TextEntry text("host",8);text.append('a');assert(text.value()=="hosta");text.erase();assert(text.value()=="host");
    text.append('\n');assert(text.value()=="host");
    for(int i=0;i<20;++i)text.append('b');assert(text.value().size()==8);
    text.cancel();assert(text.value()=="host");
    text.move(1,0);assert(text.selected_character()=='B');text.confirm();assert(text.value()=="hostB");
    std::cout<<"Controller ownership, focus/reconnect, repeat, disabled rows and text entry pass\n";
}
