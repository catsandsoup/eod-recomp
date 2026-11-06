// Optional source Cup fast geometry. Native classes and absent extensions retain
// the exact original path. No extra model submission or texture/depth workaround.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "core/cpu.h"
#include "core/memory.h"
extern "C" void f_001326F8();
extern "C" void f_0034CEF0_orig();
namespace {
bool trace(){static const bool on=std::getenv("EOD_WHEEL_TRACE")!=nullptr;return on;}
}
extern "C" void f_0034CEF0(){
    const uint32_t address=arg_u32(0);
    // 21f6d4 jal34cef0, delay slot21f6d8; only the second (fast) handle.
    if(g_ee.r[31].uw[0]!=0x0021f6dc || address>=EE_RAM_SIZE-16){
        f_0034CEF0_orig();return;
    }
    char before[16];std::memcpy(before,g_ram+address,16);
    if(std::memcmp(before,"ncu",3) || before[3]<'0' || before[3]>'4' ||
       std::memcmp(before+4,".mod\0",5)){
        f_0034CEF0_orig();return;
    }
    char fast[10]={'n','c','u',before[3],'f','.','m','o','d',0};
    std::memcpy(g_ram+address,fast,sizeof fast);
    // Use the native resident-resource lookup. It may overwrite every EE
    // register; restore all input state before the one actual model load.
    const EEState input=g_ee;
    f_001326F8();
    const bool exists=g_ee.r[2].uw[0]!=0;
    g_ee=input;
    if(!exists)std::memcpy(g_ram+address,before,16);
    f_0034CEF0_orig();
    std::memcpy(g_ram+address,before,16);
    if(trace())std::fprintf(stderr,"[wheel] fast-geometry %s resource=%s handle=%08x\n",
        exists?"source":"fallback",exists?fast:before,g_ee.r[2].uw[0]);
}

extern "C" void f_00220788_orig();
extern "C" void f_00220788(){
    const uint32_t wheel=arg_u32(0),lod=arg_u32(1);
    if(trace() && wheel && wheel<EE_RAM_SIZE-0x44 && lod<5){
        const uint32_t klass=ee_r32(wheel);
        if(klass && klass<EE_RAM_SIZE-0xb4 && !std::memcmp(g_ram+klass,"ncu\0",4)){
            const uint32_t car=ee_r32(wheel+8),side=ee_r32(wheel+12);
            const bool fast=(ee_r32(wheel+4)&0x10)!=0;
            static uint32_t states[160]{};static unsigned lines;
            const uint32_t state=1+lod*2+unsigned(fast),slot=car*4+side;
            if(car<40 && side<4 && states[slot]!=state && lines<96){
                states[slot]=state;++lines;
                std::fprintf(stderr,"[wheel] select car=%u wheel=%u lod=%u fast=%u handle=%08x\n",
                    car,side,lod,unsigned(fast),ee_r32(klass+0x28+lod*16+(fast?4:0)));
            }
        }
    }
    f_00220788_orig();
}
