// CPU-only oracle: enumerate pixels through the original GS address functions,
// independently of the planner's precomputed address tables/block traversal.
#include "../../runtime/gs/vulkan/gpu_upload_blocks.h"
#include "../../runtime/gs/gs_mem.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
using namespace gs;
static unsigned cases = 0;
static void require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "case %u: %s\n", cases, message); std::exit(1); }
}
static bool half_format(uint32_t psm) {
    return psm == PSMCT16 || psm == PSMCT16S || psm == PSMZ16 || psm == PSMZ16S;
}
static uint32_t storage_byte(uint32_t bp, uint32_t bw, uint32_t psm, int x, int y) {
    uint32_t address = 0;
    switch (psm) {
    case PSMCT32: case PSMCT24: address = addr32(x,y,bp,bw); break;
    case PSMZ32: case PSMZ24: address = addr32z(x,y,bp,bw); break;
    case PSMCT16: address = addr16(x,y,bp,bw); break;
    case PSMCT16S: address = addr16s(x,y,bp,bw); break;
    case PSMZ16: address = addr16z(x,y,bp,bw); break;
    case PSMZ16S: address = addr16sz(x,y,bp,bw); break;
    default: require(false,"unsupported oracle format");
    }
    return (address * (half_format(psm) ? 2u : 4u)) % kVramSize;
}
static void check(uint32_t bp, uint32_t bw, uint32_t psm, int width, int height,
                  uint32_t page, uint32_t mask) {
    ++cases;
    const unsigned unit = half_format(psm) ? 2 : 4;
    std::array<unsigned,32> bytes{};
    for (int y=0; y<height; ++y) for (int x=0; x<width; ++x) {
        const auto a = storage_byte(bp,bw,psm,x,y);
        if (a/8192 == page) bytes[(a/256)%32] += unit;
    }
    bool expected = mask != 0;
    for (unsigned b=0; b<32; ++b) if (mask & (1u<<b)) expected &= bytes[b] == 256;
    std::vector<hw::UploadBlockRect> result{{-1,-2,-3,-4}};
    const bool actual = hw::gpu_upload_blocks(bp,bw,psm,width,height,page,mask,result);
    require(actual == expected,"planner success disagrees with per-pixel storage oracle");
    if (!actual) {
        require(result.size()==1 && result[0].x==-1 && result[0].y==-2 &&
                result[0].w==-3 && result[0].h==-4,"failure mutated output");
        return;
    }
    require(!result.empty() && result.size()<=32,"invalid rectangle count");
    std::vector<uint8_t> seen(kVramSize);
    for (const auto& r:result) {
        require(r.x>=0 && r.y>=0 && r.w>0 && r.h>0 && r.x+r.w<=width && r.y+r.h<=height,
                "rectangle exceeds logical bounds");
        for(int y=r.y;y<r.y+r.h;++y) for(int x=r.x;x<r.x+r.w;++x) {
            const auto a=storage_byte(bp,bw,psm,x,y);
            require(a/8192==page && (mask & (1u<<((a/256)%32))),"rectangle writes unselected storage");
            for(unsigned n=0;n<unit;++n) require(++seen[a+n]==1,"overlapping storage");
        }
    }
    for(unsigned b=0;b<32;++b) for(unsigned n=0;n<256;++n)
        require(seen[page*8192+b*256+n] == unsigned(bool(mask & (1u<<b))),"missing selected storage byte");
}
static void invalid(uint32_t bp,uint32_t bw,uint32_t psm,int w,int h,uint32_t page,uint32_t mask) {
    ++cases;
    std::vector<hw::UploadBlockRect> result{{7,8,9,10}};
    require(!hw::gpu_upload_blocks(bp,bw,psm,w,h,page,mask,result),"invalid input accepted");
    require(result.size()==1 && result[0].x==7 && result[0].y==8 && result[0].w==9 && result[0].h==10,
            "invalid input mutated output");
}
int main() {
    std::mt19937 rng(0x424c4f43);
    for(uint32_t psm:{PSMCT32,PSMCT24,PSMCT16,PSMCT16S,PSMZ32,PSMZ24,PSMZ16,PSMZ16S}) {
        const int ph=half_format(psm)?64:32;
        for(uint32_t bp:{0u,32u,0x3fe0u}) {
            for(unsigned b=0;b<32;++b) check(bp,1,psm,64,ph,bp/32,1u<<b);
            for(uint32_t mask:{0xffffffffu,0x55555555u,0x80000001u,0u})
                check(bp,2,psm,128,ph*2,(bp/32+3)%kPages,mask);
            for(unsigned i=0;i<20;++i) {
                uint32_t bw=1+rng()%4, page=(bp/32+rng()%(bw*3))%kPages;
                int w=1+rng()%(bw*64),h=1+rng()%(ph*3);
                check(bp,bw,psm,w,h,page,rng());
                // Single physical blocks also exercise successful partial-page owners.
                check(bp,bw,psm,w,h,page,1u<<(rng()%32));
            }
        }
        check(0x3fe0,32,psm,2048,ph*16,510,0xffffffffu); // exactly 512 unique pages
        check(0x3fe0,1,psm,64,2048,(510+2048/ph)%kPages,0xffffffffu); // last logical row
        check(0x4000,1,psm,64,ph,0,0xffffffffu); // wrapped BP
        check(0,1,psm,64,ph,1,1); // no owner mapping
        check(0,1,psm,63,ph,0,0xffffffffu); // incomplete right edge
        check(0,1,psm,64,ph-1,0,0xffffffffu); // incomplete bottom edge
        invalid(0,32,psm,2048,ph*16+1,0,1); // repeated physical pages
        invalid(1,1,psm,64,ph,0,1);
    }
    for(uint32_t psm:{PSMT8,PSMT4,PSMT8H,PSMT4HL,PSMT4HH}) invalid(0,1,psm,64,32,0,1);
    invalid(0,1,0x3f,64,32,0,1);
    invalid(0,0,PSMCT32,64,32,0,1);
    invalid(0,33,PSMCT32,64,32,0,1);
    invalid(0,1,PSMCT32,65,32,0,1);
    invalid(0,32,PSMCT32,2049,32,0,1);
    invalid(0,1,PSMCT32,64,2049,0,1);
    invalid(0,1,PSMCT32,0,32,0,1);
    invalid(0,1,PSMCT32,64,0,0,1);
    invalid(0,1,PSMCT32,-1,32,0,1);
    invalid(0,1,PSMCT32,64,-1,0,1);
    invalid(0,1,PSMCT32,64,32,512,1);
    std::printf("GPU upload block planner: %u CPU storage-oracle cases passed\n",cases);
}
