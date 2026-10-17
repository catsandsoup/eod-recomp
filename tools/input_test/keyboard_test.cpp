#include "platform/keyboard_input.h"
#include "core/config.h"
#include <cstdio>
#include <unistd.h>
using namespace keyboard_input;
static int checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)
int main() {
    CHECK(context(0,0,0,0) == Context::Menu);
    CHECK(context(0,0,1,0) == Context::Driving);
    CHECK(context(0,0,1,123) == Context::Menu); // pause overrides race loop
    CHECK(context(123,0,1,456) == Context::Title);
    CHECK(context(0,123,0,456) == Context::Title);
    std::vector<Binding> bindings = {{0x0d,PAD_START},{0x0d,PAD_CROSS},{0x20,PAD_CIRCLE},
                                    {'X',PAD_CROSS},{'Q',PAD_L1},{0x25,PAD_LEFT},{0x27,PAD_RIGHT}};
    auto single = [&](int key, Context c) { Mapper m; Keys k{}; k[key]=true; return m.read(k,c,true,bindings); };
    for (int key : {int('W'),0x26}) CHECK(single(key,Context::Menu).buttons == PAD_UP);
    for (int key : {int('S'),0x28}) CHECK(single(key,Context::Menu).buttons == PAD_DOWN);
    for (int key : {int('W'),0x26}) CHECK(single(key,Context::Driving).buttons == PAD_CROSS);
    for (int key : {int('S'),0x28,0x20}) CHECK(single(key,Context::Driving).buttons == PAD_SQUARE);
    CHECK(single(0x0d,Context::Menu).buttons == PAD_CROSS);
    CHECK(single(0x0d,Context::Title).buttons == PAD_START);
    CHECK(single(0x0d,Context::Driving).buttons == 0);
    CHECK(single(0x1b,Context::Menu).buttons == PAD_CIRCLE);
    CHECK(single(0x1b,Context::Driving).buttons == PAD_START);
    CHECK(single(0x20,Context::Menu).buttons == 0);
    CHECK(single('A',Context::Driving).lx == 0);
    CHECK(single('D',Context::Driving).lx == 255);
    CHECK(single('Q',Context::Driving).buttons == PAD_L1);
    CHECK(single('X',Context::Menu).buttons == PAD_CROSS);
    Mapper m; Keys k{};
    k['A']=k[0x27]=true;
    auto out=m.read(k,Context::Driving,true,bindings);
    CHECK(out.lx==128 && !(out.buttons&(PAD_LEFT|PAD_RIGHT)));
    k={};k['W']=k['S']=true;
    CHECK(m.read(k,Context::Menu,true,bindings).buttons==0);
    k={};k['W']=true;
    CHECK(m.read(k,Context::Menu,true,bindings).buttons==PAD_UP);
    CHECK(m.read(k,Context::Driving,true,bindings).buttons==PAD_CROSS); // held gas survives entering the driving context
    CHECK(m.read(k,Context::Driving,true,bindings).buttons==PAD_CROSS);
    k={};m.read(k,Context::Driving,true,bindings);k['W']=true;
    CHECK(m.read(k,Context::Driving,true,bindings).buttons==PAD_CROSS);
    k={};k[0x1b]=true;
    CHECK(m.read(k,Context::Driving,true,bindings).buttons==PAD_START);
    CHECK(m.read(k,Context::Menu,true,bindings).buttons==0); // holding pause cannot back out immediately
    k={};m.read(k,Context::Menu,true,bindings);k[0x1b]=true;
    CHECK(m.read(k,Context::Menu,true,bindings).buttons==PAD_CIRCLE);
    k={};m.read(k,Context::Title,true,bindings);k[0x0d]=true;
    CHECK(m.read(k,Context::Title,true,bindings).buttons==PAD_START);
    CHECK(m.read(k,Context::Menu,true,bindings).buttons==0); // title accept cannot accept next screen
    k={};m.read(k,Context::Menu,true,bindings);k[0x0d]=true;
    CHECK(m.read(k,Context::Menu,true,bindings).buttons==PAD_CROSS);
    k={};m.read(k,Context::Menu,true,bindings);k['Q']=true;
    CHECK(m.read(k,Context::Menu,true,bindings).buttons==PAD_L1);
    CHECK(m.read(k,Context::Driving,true,bindings).buttons==PAD_L1); // unchanged custom action survives context change
    k={}; k['W']=true;
    CHECK(m.read(k,Context::Driving,true,bindings).buttons==PAD_CROSS);
    CHECK(m.read(k,Context::Menu,true,bindings).buttons==PAD_UP);
    CHECK(m.read(k,Context::Driving,true,bindings).buttons==PAD_CROSS);
    k={}; CHECK(m.read(k,Context::Driving,true,bindings).buttons==0); // focus loss releases
    k['W']=true; CHECK(m.read(k,Context::Driving,true,bindings).buttons==PAD_CROSS);
    CHECK(m.read(k,Context::Driving,true,bindings,false).buttons==0);
    CHECK(m.read(k,Context::Driving,true,bindings,true).buttons==PAD_CROSS);
    Mapper original;k={};k[0x20]=true;
    CHECK(original.read(k,Context::Menu,false,bindings).buttons==PAD_CIRCLE);
    k={};k[0x25]=k[0x27]=true;
    out=original.read(k,Context::Menu,false,bindings);
    CHECK(out.lx==128 && out.buttons==0);
    Config cfg;
    CHECK(cfg.keyboard_port == -1);
    CHECK(cfg.keyboard_layout=="racing");
    char path[]="/tmp/eod-keyboard-test-XXXXXX"; int fd=mkstemp(path); CHECK(fd>=0); if(fd<0)return 1; close(fd);
    cfg.keyboard_layout="original"; cfg.keys[KEY_CROSS]="F4";
    CHECK(config_save(path,cfg)); Config loaded; CHECK(config_load(path,loaded));
    CHECK(loaded.keyboard_layout=="original" && loaded.keys[KEY_CROSS]=="F4");
    FILE *f=fopen(path,"w");fprintf(f,"[Input]\nKeyboardLayout=RACING\n");fclose(f);
    CHECK(config_load(path,loaded) && loaded.keyboard_layout=="racing");
    f=fopen(path,"w");fprintf(f,"[Input]\nKeyboardLayout=invalid\n");fclose(f);
    loaded.keyboard_layout="original";CHECK(config_load(path,loaded) && loaded.keyboard_layout=="original");
    f=fopen(path,"w");fprintf(f,"[Input]\nKeyboardPort=-1\n");fclose(f);
    CHECK(config_load(path,loaded) && loaded.keyboard_port==-1);
    for(int port=0;port<=2;++port) {
        loaded.keyboard_port=port; CHECK(config_save(path,loaded));
        Config restored; CHECK(config_load(path,restored) && restored.keyboard_port==port);
    }
    unlink(path);
    printf("%d keyboard checks passed, %d failed\n",checks-failures,failures); return failures?1:0;
}
