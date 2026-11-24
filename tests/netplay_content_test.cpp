#include "netplay_content.h"
#include <cstdio>
#include <fstream>
#include <cstdlib>
namespace fs=std::filesystem;
static void check(bool ok,const char *why) { if(!ok) { std::fprintf(stderr,"FAIL: %s\n",why); std::exit(1); } }
static void put(const fs::path &p,const std::string &s) { fs::create_directories(p.parent_path()); std::ofstream(p,std::ios::binary)<<s; }
int main() {
    const auto root=fs::temp_directory_path()/fs::u8path("eod-content-Montréal-日本語-test");
    fs::remove_all(root);
    put(root/"a/DATA/CARS/VEH.RES","same");
    put(root/"b/data/cars/veh.res","same");
    auto a=netplay::content_fingerprint((root/"a").u8string(),"");
    auto b=netplay::content_fingerprint((root/"b").u8string(),"");
    check(a.error.empty() && b.error.empty() && a.value==b.value,"location/case/Unicode independent match");
    put(root/"b/data/cars/veh.res","diff");
    check(netplay::content_fingerprint((root/"b").u8string(),"").value!=a.value,"same-size changed physics archive rejected");
    put(root/"overlay/DATA/CARS/VEH.RES","same");
    check(netplay::content_fingerprint((root/"b").u8string(),(root/"overlay").u8string()).value==a.value,"effective overlay wins");
    put(root/"b/data/VIDEO/MOVIE.PSS","local movie");
    put(root/"b/data/LOCAL.BNK","local audio");
    put(root/"b/data/music/track.mp3","my playlist");
    check(netplay::content_fingerprint((root/"b").u8string(),(root/"overlay").u8string()).value==a.value,"local audio/movie choices excluded");
    put(root/"overlay/battle-alternates/TRACKS/ANOKA/c_battle.va0","battle layout");
    check(netplay::content_fingerprint((root/"b").u8string(),(root/"overlay").u8string()).value!=a.value,"Battle alternate layout included");
    fs::remove_all(root/"overlay/battle-alternates");
    put(root/"overlay/DATA/TRACKS/NEW.RES","newtrack");
    check(netplay::content_fingerprint((root/"a").u8string(),(root/"overlay").u8string()).value!=a.value,"added track changes compatibility");
    check(!netplay::content_fingerprint((root/"absent").u8string(),"").error.empty(),"missing content cannot be admitted");
#ifndef _WIN32
    fs::create_symlink(root/"a/DATA/CARS/VEH.RES",root/"a/DATA/CARS/LINK.RES");
    check(!netplay::content_fingerprint((root/"a").u8string(),"").error.empty(),"ambiguous linked content rejected");
#endif
    put(root/"conflict/DATA/CARS","file instead of folder");
    check(!netplay::content_fingerprint((root/"b").u8string(),(root/"conflict").u8string()).error.empty(),"overlay type collisions rejected");
#ifndef _WIN32
    // Only a case-sensitive filesystem can construct this ambiguous tree.
    put(root/"ambiguous/DATA/Cars/A.RES","a");
    put(root/"ambiguous/DATA/CARS/B.RES","b");
    size_t directories=0;
    for(const auto &e:fs::directory_iterator(root/"ambiguous/DATA")) if(e.is_directory()) ++directories;
    if(directories==2) check(!netplay::content_fingerprint((root/"ambiguous").u8string(),"").error.empty(),"case-folded directories rejected");
    fs::create_directories(root/"linked");
    fs::create_directory_symlink(root/"b/data",root/"linked/DATA");
    check(!netplay::content_fingerprint((root/"linked").u8string(),"").error.empty(),"linked DATA root rejected");
#endif
    const std::string digest(64, 'a');
    auto profile = root/"profile";
    put(profile/"PROFILE.TXT", "profile private-folder-name\ngame " + digest + "\npackage track-kit 2.1.0 manifest " + digest + "\n");
    const auto custom = netplay::describe_overlay(profile);
    check(custom.source == netplay::ContentSource::Custom && custom.packages == std::vector<std::string>{"track-kit@2.1.0"}, "custom recovery shares only declared package IDs and versions");
    put(profile/"PROFILE.TXT", "profile /Users/private\ngame " + digest + "\npackage /Users/private 2.1.0 manifest " + digest + "\n");
    check(netplay::describe_overlay(profile).packages.empty(), "local path cannot become a network package label");
    put(profile/"PROFILE.TXT", "profile name\ngame " + digest + "\npackage car 1.0.0 manifest " + digest + "\npackage car 2.0.0 manifest " + digest + "\n");
    check(netplay::describe_overlay(profile).packages.empty(), "ambiguous package metadata falls back to custom");
    fs::remove(profile/"PROFILE.TXT");
    put(profile/"INTEGRATED.MANIFEST", "obsolete marker");
    check(netplay::describe_overlay(profile).source == netplay::ContentSource::Custom, "a stale marker alone cannot identify integrated content");
    check(a.profile.source == netplay::ContentSource::Disc, "stock disc requires no mod overlay");
    fs::remove_all(root); std::puts("netplay_content_test ok");
}
