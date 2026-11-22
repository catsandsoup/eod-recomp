// Optional source NASCAR fast wheel geometry resolved via VehicleClassRegistry
// (cup, truck, modified, dirt, pace + arbitrary custom wheel classes).
// Native classes and absent extensions retain the exact original path.
// No extra model submission or texture/depth workaround.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "core/cpu.h"
#include "core/memory.h"
#include "hle_nascar_class.h"
extern "C" void f_001326F8();
extern "C" void f_0034CEF0_orig();
namespace {
bool trace(){static const bool on=std::getenv("EOD_WHEEL_TRACE")!=nullptr;return on;}
}
extern "C" void f_0034CEF0(){
    const uint32_t address=arg_u32(0);
    // 21f6d4 jal 34cef0, delay slot 21f6d8; only the second (fast) handle.
    if(g_ee.r[31].uw[0]!=0x0021f6dc || address>=EE_RAM_SIZE-16){
        f_0034CEF0_orig();return;
    }
    char before[16];std::memcpy(before,g_ram+address,16);
    char slow[16];std::memcpy(slow,before,16);slow[15]='\0';
    std::string fast_mesh;
    if(!eod::nascar::VehicleClassRegistry::instance().resolve_fast_wheel_mesh(slow, fast_mesh) ||
       fast_mesh.size() >= sizeof(before)){
        f_0034CEF0_orig();return;
    }
    std::memcpy(g_ram+address,fast_mesh.c_str(),fast_mesh.size()+1);
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
        exists?"source":"fallback",exists?fast_mesh.c_str():slow,g_ee.r[2].uw[0]);
}

extern "C" void f_00220788_orig();
extern "C" void f_00220788(){
    const uint32_t wheel=arg_u32(0),lod=arg_u32(1);
    if(trace() && wheel && wheel<EE_RAM_SIZE-0x44 && lod<5){
        const uint32_t klass=ee_r32(wheel);
        if(klass && klass<EE_RAM_SIZE-0xb4){
            char prefix[4];
            std::memcpy(prefix,g_ram+klass,4);
            if(eod::nascar::VehicleClassRegistry::instance().is_registered_wheel_prefix(prefix)){
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
    }
    f_00220788_orig();
}
