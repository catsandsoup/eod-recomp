// Integrated-content activation: filesystem/environment only. No NSApplication, SDL or GPU.
// Covers default activation, corrupt/missing content refusal, the explicit EOD_MOD_DIR
// developer override, the EOD_NATIVE_ONLY diagnostic and unchanged headless behaviour.
#include "app/macos_app.h"
#include "core/config.h"
#include <CommonCrypto/CommonDigest.h>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
namespace fs=std::filesystem;
static std::string sha256(const std::string&bytes){
    unsigned char d[CC_SHA256_DIGEST_LENGTH];CC_SHA256(bytes.data(),(CC_LONG)bytes.size(),d);
    char out[65];for(int i=0;i<32;i++)snprintf(out+i*2,3,"%02x",d[i]);return out;
}
int main(int argc,char**argv){
    if(argc!=2)return 1;
    fs::create_directories(argv[1]);
    const fs::path root=fs::path(argv[1])/("integrated-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if(!fs::create_directory(root))return 12;
    setenv("EOD_APP_SUPPORT",root.c_str(),1);
    const fs::path content=root/"mods/integrated";
    auto require=[](bool ok,int code){if(!ok){std::cerr<<"integrated content failure "<<code<<'\n';std::exit(code);}};
    const char*files[]={"DATA/RES/COMMON.RES","DATA/RES/CT.RES","DATA/RES/UICAR.RES","DATA/RES/RACE.RES",
        "DATA/CAR/PHANTOM.CAR","DATA/CAR/CUPCAR.CAR"};
    auto manifest=[&](const char*skip){
        std::ofstream out(content/"INTEGRATED.MANIFEST");
        for(auto f:files){if(skip&&std::string(f)==skip)continue;
            std::ifstream in(content/f,std::ios::binary);std::stringstream b;b<<in.rdbuf();
            out<<sha256(b.str())<<' '<<b.str().size()<<' '<<f<<'\n';}
    };
    unsetenv("EOD_MOD_DIR");unsetenv("EOD_NATIVE_ONLY");
    std::string why;
    require(!mac_app_activate_integrated(&why)&&!why.empty()&&!getenv("EOD_MOD_DIR"),2); // nothing installed
    for(auto f:files){auto p=content/f;fs::create_directories(p.parent_path());std::ofstream(p)<<"fixture "<<f;}
    require(!mac_app_activate_integrated(&why)&&why.find("MANIFEST")!=std::string::npos&&!getenv("EOD_MOD_DIR"),3);
    manifest("DATA/RES/RACE.RES");
    require(!mac_app_activate_integrated(&why)&&why.find("RACE.RES")!=std::string::npos&&!getenv("EOD_MOD_DIR"),4); // not listed
    manifest(nullptr);
    require(mac_app_validate_integrated(content.string()).empty(),5);
    require(mac_app_activate_integrated(&why)&&fs::path(getenv("EOD_MOD_DIR"))==content,6);
    unsetenv("EOD_MOD_DIR");
    {std::fstream f(content/"DATA/RES/CT.RES",std::ios::in|std::ios::out|std::ios::binary);f.seekp(3);f.put('Z');} // same size, one byte flipped
    require(!mac_app_activate_integrated(&why)&&why.find("damaged")!=std::string::npos&&!getenv("EOD_MOD_DIR"),7);
    std::ofstream(content/"DATA/RES/CT.RES")<<"fixture DATA/RES/CT.RES";
    require(mac_app_activate_integrated(&why),8);
    unsetenv("EOD_MOD_DIR");
    fs::remove(content/"DATA/CAR/CUPCAR.CAR");
    require(!mac_app_activate_integrated(&why)&&why.find("CUPCAR.CAR")!=std::string::npos&&!getenv("EOD_MOD_DIR"),9);
    std::ofstream(content/"DATA/CAR/CUPCAR.CAR")<<"fixture DATA/CAR/CUPCAR.CAR";
    // Startup default: no UI, integrated activated.
    g_config.unlock_all=false; // as if the owner's eod.ini said UnlockAll=0
    require(mac_app_select_mods(false)&&fs::path(getenv("EOD_MOD_DIR"))==content&&g_config.unlock_all,10);
    unsetenv("EOD_MOD_DIR");g_config.unlock_all=false;setenv("EOD_UNLOCK_ALL","0",1); // explicit override wins
    require(mac_app_select_mods(false)&&!g_config.unlock_all,14);
    unsetenv("EOD_UNLOCK_ALL");unsetenv("EOD_MOD_DIR");
    // Explicit developer override is left exactly alone, even with broken built-in content.
    setenv("EOD_MOD_DIR","sentinel",1);
    fs::remove(content/"INTEGRATED.MANIFEST");
    g_config.unlock_all=false;
    require(mac_app_select_mods(false)&&std::string(getenv("EOD_MOD_DIR"))=="sentinel"&&!g_config.unlock_all,11);
    // Headless is unchanged: never activates or shows anything.
    unsetenv("EOD_MOD_DIR");
    require(mac_app_select_mods(true)&&!getenv("EOD_MOD_DIR"),12);
    // Native-only diagnostic: retail game, no overlay, even with broken content.
    setenv("EOD_NATIVE_ONLY","1",1);
    require(mac_app_select_mods(false)&&!getenv("EOD_MOD_DIR"),13);
    unsetenv("EOD_NATIVE_ONLY");
    fs::remove_all(root);
    std::cout<<"Integrated content activation, corruption refusal and overrides passed; no game launched\n";
}
