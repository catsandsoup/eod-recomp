// Public-register GS transfer fixtures. No game code/assets or GPU required.
#include "../../runtime/gs/gs_mem.h"
#include "../../runtime/gs/gs_replay_types.h"
#include "../../runtime/gs/gs_state.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
using Pair=std::pair<uint64_t,uint64_t>;
static uint64_t rgba(unsigned r,unsigned g,unsigned b,unsigned a=200){return r|uint64_t(g)<<8|uint64_t(b)<<16|uint64_t(a)<<24;}
static uint64_t xyz(unsigned x,unsigned y){return x|uint64_t(y)<<16|1ull<<32;}
static void command(FILE *f,const std::vector<Pair> &regs){
    std::vector<Pair> packet{{regs.size()|1ull<<15|1ull<<60,14}};packet.insert(packet.end(),regs.begin(),regs.end());
    uint32_t h[4]={1,3,0,uint32_t(packet.size()*16)};fwrite(h,1,16,f);fwrite(packet.data(),16,packet.size(),f);
}
static void image(FILE *f,const std::vector<uint8_t> &bytes,unsigned declared_qwords=0){
    unsigned n=(bytes.size()+15)/16;
    Pair tag{(declared_qwords?declared_qwords:n)|1ull<<15|2ull<<58,0};
    uint32_t h[4]={1,3,0,(n+1)*16};fwrite(h,1,16,f);fwrite(&tag,1,16,f);
    std::vector<uint8_t> padded(n*16);memcpy(padded.data(),bytes.data(),bytes.size());fwrite(padded.data(),1,padded.size(),f);
}
int main(int argc,char **argv){
    if(argc!=3)return 2;std::string name=argv[1];
    gs::State s{};gs::PathState paths[4]{};gs::Transfer transfer{};
    s.prmodecont=1;s.q=1;s.colclamp=1;s.ctx[0].frame=1ull<<16;s.ctx[0].zbuf=1ull<<32;
    s.ctx[0].scissor=63ull<<16|63ull<<48;s.pmode=1;s.dispfb[0]=1ull<<9;s.display[0]=63ull<<32|63ull<<44;
    bool neighbor=name=="neighbor", multi=name=="multi_partial";
    unsigned bw=multi?2:1, draw_w=multi?128:64, draw_h=multi?64:32;
    s.ctx[0].frame=uint64_t(bw)<<16;s.dispfb[0]=uint64_t(bw)<<9;
    s.ctx[0].scissor=uint64_t(draw_w-1)<<16|63ull<<48;
    if(neighbor)s.dispfb[0]|=1;
    if(name=="localcopy")s.dispfb[0]|=3;
    gs::mem_init();
    for(int y=0;y<64;y++)for(int x=0;x<64;x++)gs::write_pixel(gs::PSMCT32,x,y,0,1,rgba(32,64,96,128));
    FILE *f=fopen(argv[2],"wb");if(!f)return 2;
    fwrite("EODGSD01",1,8,f);uint32_t sizes[]={sizeof s,sizeof paths,sizeof transfer};fwrite(sizes,1,12,f);
    fwrite(&s,1,sizeof s,f);fwrite(paths,1,sizeof paths,f);fwrite(&transfer,1,sizeof transfer,f);fwrite(gs::vram,1,gs::kVramSize,f);
    std::vector<Pair> draw;
    auto reg=[&](uint64_t id,uint64_t value){draw.emplace_back(value,id);};
    if(neighbor)reg(0x4c,1|1ull<<16);
    reg(0,6);reg(1,rgba(37,83,126));reg(5,xyz(0,0));reg(5,xyz(draw_w*16,draw_h*16));
    // Most cases force prior rendering to execute via a second offscreen draw.
    // "pending" instead proves a CPU write flushes the queued first primitive.
    if(name!="pending" && name!="localcopy"){
        // Use a sprite: a GS point is one physical GPU pixel in the current
        // upscale path, so a 6x->1x readback can miss it independently of CPU
        // transfer preservation. This separate target only forces the flush.
        reg(0x4c,8|uint64_t(bw)<<16);reg(0,6);reg(1,rgba(9,19,29));
        reg(5,xyz(16*16,16*16));reg(5,xyz(24*16,24*16));
    }
    command(f,draw);draw.clear();
    if(name=="localcopy") {
        reg(0x50,1ull<<16|96ull<<32|1ull<<48);reg(0x51,0);reg(0x52,64ull|32ull<<32);reg(0x53,2);
        command(f,draw);uint32_t end[4]={3,1,0,0};fwrite(end,1,16,f);fclose(f);free(gs::vram);return 0;
    }
    uint32_t psm=gs::PSMCT32;
    if(name=="ct24")psm=gs::PSMCT24;
    else if(name=="high8")psm=gs::PSMT8H;
    else if(name=="high4lo")psm=gs::PSMT4HL;
    else if(name=="high4hi")psm=gs::PSMT4HH;
    bool full=name=="full" || name=="interrupted" || name=="chunks";
    unsigned w=multi?128:full?64:1,h=full?32:1;
    // Masked writes target the already-rendered interior; all others origin0.
    unsigned x=psm==gs::PSMCT32?0:16,y=psm==gs::PSMCT32?0:16;
    reg(0x50,uint64_t(neighbor?4:0)<<32|uint64_t(bw)<<48|uint64_t(psm)<<56);
    reg(0x51,uint64_t(x)<<32|uint64_t(y)<<48);reg(0x52,w|uint64_t(h)<<32);reg(0x53,0);
    command(f,draw);draw.clear();
    if(name=="interrupted"){
        // A declared full-page transfer provides only one qword, then another
        // GIF path displays/reads the still-partial transfer state at vblank.
        image(f,std::vector<uint8_t>(16,0xff),512);
    }else if(name=="chunks"){
        // A complete IMAGE packet followed by another complete packet continues
        // the same transfer; each chunk is actual data, never a promise.
        image(f,std::vector<uint8_t>(16,0xff));
        image(f,std::vector<uint8_t>(64*32*4-16,0xff));
    }else if(multi){
        image(f,std::vector<uint8_t>(w*4,0xff));
    }else if(full){
        image(f,std::vector<uint8_t>(64*32*4,0xff));
    }else{
        reg(0x54,rgba(255,0,0,0x25));command(f,draw);
    }
    uint32_t end[4]={3,1,0,0};fwrite(end,1,16,f);fclose(f);free(gs::vram);
}
