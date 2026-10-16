// Verify block enumeration against independent per-pixel address enumeration.
#include "../../runtime/gs/vulkan/gs_page_coverage.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <set>
using namespace gs;
static void check(uint32_t bp, uint32_t bw, uint32_t psm, int x, int y, int w, int h) {
    const SwizzleTab *tab = swizzle_table(psm,bw);
    auto actual = hw::page_coverage(bp,bw,psm,x,y,w,h);
    std::array<uint32_t,kPages> expected{};
    int unit = psm==PSMCT16 || psm==PSMCT16S || psm==PSMZ16 || psm==PSMZ16S ? 16 : psm==PSMT8 ? 8 : psm==PSMT4 ? 4 : 32;
    std::map<uint32_t,std::set<uint32_t>> written_units;
    const bool whole = psm!=PSMCT24 && psm!=PSMZ24 && psm!=PSMT8H && psm!=PSMT4HL && psm!=PSMT4HH;
    for (int py=y;py<y+h;py++) for(int px=x;px<x+w;px++) {
        uint32_t byte=uint32_t(uint64_t(tab->addr(bp,px,py))*unit/8)%kVramSize;
        expected[byte/8192] |= 1u<<((byte/256)%32);
        if(whole) written_units[byte/256].insert(tab->addr(bp,px,py) % (kVramSize*8/unit));
    }
    for(uint32_t page=0;page<kPages;page++) for(unsigned block=0;block<32;block++)
        if(actual.complete[page] & (1u<<block))
            if(!whole || written_units[page*32+block].size()!=unsigned(256*8/unit))std::exit(8);
    std::array<bool,kPages> seen{};
    for(unsigned i=0;i<actual.count;i++) {
        if(seen[actual.pages[i]] || !actual.touched[actual.pages[i]])std::exit(6);
        seen[actual.pages[i]]=true;
    }
    for(unsigned page=0;page<kPages;page++)if(seen[page] != bool(actual.touched[page]))std::exit(7);
    if(actual.touched!=expected) {std::fprintf(stderr,"coverage mismatch bp%x bw%u psm%x rect%d,%d,%d,%d\n",bp,bw,psm,x,y,w,h);std::exit(1);}
}
int main() {
    std::mt19937 rng(0x50414745);
    for(uint32_t psm:{PSMCT32,PSMCT24,PSMCT16,PSMCT16S,PSMT8,PSMT4,PSMT8H,PSMT4HL,PSMT4HH,PSMZ32,PSMZ24,PSMZ16,PSMZ16S})
        for(int i=0;i<40;i++) check(rng()%0x4000,1+rng()%16,psm,rng()%2048,rng()%2048,1+rng()%128,1+rng()%64);
    check(0x2fc4,1,PSMCT32,0,0,16,16);
    auto palette=hw::page_coverage(0x2fc4,1,PSMCT32,0,0,16,16);
    unsigned pages=0;for(auto mask:palette.touched)pages+=mask!=0;
    if(pages!=1 || palette.touched[(0x2fc4>>5)+1])return 2;
    auto full=hw::page_coverage(0,1,PSMCT32,0,0,64,32);
    if(full.touched[0]!=0xffffffffu || full.complete[0]!=0xffffffffu)return 3;
    auto partial=hw::page_coverage(0,1,PSMCT32,0,0,1,1);
    if(partial.touched[0]!=1 || partial.complete[0])return 4;
    uint32_t first,count;
    page_range(0x2fc4,1,PSMCT32,0,0,16,16,first,count);
    if(count!=2)return 5; // The old conservative API remains unchanged for other callers.
    std::puts("GS exact page coverage matches 520 per-pixel cases; unaligned CLUT uses one page");
}
