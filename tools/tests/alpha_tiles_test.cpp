#include "gs/vulkan/alpha_tiles.h"
#include <cstdio>
#include <cstdlib>
#include <random>
static void check(bool ok,const char *why) { if(!ok){std::fprintf(stderr,"FAIL %s\n",why);std::exit(1);} }
static bool overlaps(alpha_order::Bounds a,alpha_order::Bounds b) {
    return a.x0<a.x1 && a.y0<a.y1 && b.x0<b.x1 && b.y0<b.y1 && a.x0<b.x1 && b.x0<a.x1 && a.y0<b.y1 && b.y0<a.y1;
}
struct Pixel { int color,alpha,z; bool operator==(const Pixel &p) const {return color==p.color && alpha==p.alpha && z==p.z;} };
struct Primitive { alpha_order::Bounds coverage;int color,alpha,z; };
static void draw(std::vector<Pixel> &image,const Primitive &p,int phase,int afail,bool strict) {
    bool pass=p.alpha>=96;
    if((phase==1 && !pass) || (phase==2 && pass)) return;
    for(int y=p.coverage.y0;y<p.coverage.y1;y++) for(int x=p.coverage.x0;x<p.coverage.x1;x++) {
        auto &dst=image[y*128+x];
        if(strict ? p.z<=dst.z : p.z<dst.z) continue;
        if(pass || afail==1 || afail==3) {
            // Destination-alpha blend makes primitive ordering observable too.
            dst.color=std::clamp(((p.color-dst.color)*dst.alpha>>7)+dst.color,0,255);
            if(pass || afail==1) dst.alpha=p.alpha;
        }
        if(pass || afail==2) dst.z=p.z;
    }
}
static void differential() {
    std::mt19937 random(0xD15D01);
    for(int trial=0;trial<256;trial++) {
        std::vector<Pixel> expected(128*96),actual;
        for(auto &p:expected)p={int(random()%256),int(random()%256),int(random()%8)};
        actual=expected;
        std::vector<Primitive> primitives;
        for(int n=0;n<128;n++) {
            int x=int(random()%160)-16,y=int(random()%128)-16;
            primitives.push_back({{std::clamp(x,0,128),std::clamp(y,0,96),std::clamp(x+int(random()%24),0,128),std::clamp(y+int(random()%24),0,96)},int(random()%256),int(random()%256),int(random()%8)});
        }
        int afail=trial%4;bool strict=trial%2;
        for(auto p:primitives)draw(expected,p,0,afail,strict);
        alpha_order::Tiles tiles(128,96);size_t first=0;
        auto flush=[&](size_t end){for(int phase=1;phase<=2;phase++)for(size_t n=first;n<end;n++)draw(actual,primitives[n],phase,afail,strict);};
        for(size_t n=0;n<primitives.size();n++)if(!tiles.append(primitives[n].coverage)){
            flush(n);first=n;tiles.clear();tiles.append(primitives[n].coverage);
        }
        flush(primitives.size());
        check(expected==actual,"grouped PASS/FAIL matches native color/alpha/Z with all AFAIL modes and strict/GE depth");
    }
}
int main() {
    differential();
    using namespace alpha_order;
    Bounds clip{0,0,3840,2400};
    auto a=bounds(-100,-100,10,10,6,clip);
    check(a.x0==0 && a.y0==0 && a.x1==64 && a.y1==64,"negative coordinates clip conservatively");
    auto empty=bounds(-100,-100,-50,-50,6,clip);
    check(empty.x0==empty.x1 && empty.y0==empty.y1,"fully clipped geometry has no coverage");
    Tiles tiles(3840,2400);
    check(tiles.append({0,0,16,16}),"first bounds");
    check(tiles.append({16,0,32,16}),"disjoint tile boundary");
    check(!tiles.append({15,15,17,17}),"crossing coverage rejected");
    tiles.clear();
    check(tiles.append(bounds(0,0,10,10,6,clip)),"triangle bounds");
    check(!tiles.append(bounds(0,0,10,10,6,clip)),"crossing triangles/shared edges conservatively split");
    tiles.clear();
    check(tiles.append(bounds(0,0,10,10,6,clip)),"first point/line bounds");
    check(!tiles.append(bounds(10,0,10,10,6,clip)),"line diamond edge expansion rejects adjacency");
    std::mt19937 random(0xA17E5);
    for(int round=0;round<1000;round++) {
        Tiles mask(3840,2400);std::vector<Bounds> group;
        for(int n=0;n<128;n++) {
            int x=int(random()%5000)-500,y=int(random()%3200)-400;
            Bounds b{std::clamp(x,0,3840),std::clamp(y,0,2400),std::clamp(x+int(random()%80),0,3840),std::clamp(y+int(random()%80),0,2400)};
            if(mask.append(b)) {for(auto p:group) check(!overlaps(p,b),"accepted group has provably disjoint physical pixels");group.push_back(b);}
            else {mask.clear();group.clear();check(mask.append(b),"reset accepts rejected bounds");group.push_back(b);}
        }
    }
    puts("128000 conservative bounds and 256 image color/alpha/Z differentials passed; all AFAIL modes and strict/GE depth");
}
