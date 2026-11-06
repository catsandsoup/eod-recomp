// CPU contract: optional Cup fast mesh; ordinary/native call semantics survive.
#include "core/cpu.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
EEState g_ee{};
uint8_t *g_ram;
static EEState before,after;
static std::string seen;
static unsigned probes,calls;
static bool resident=true;
static constexpr uint32_t name=0x10000;
extern "C" uint32_t ee_read32_slow(uint32_t){std::abort();}
extern "C" void ee_write32_slow(uint32_t,uint32_t){std::abort();}
static void check(bool yes,const char* s){if(!yes){std::fprintf(stderr,"FAIL: %s\n",s);std::exit(1);}}
extern "C" void f_001326F8(){
 ++probes;check(!std::strcmp((char*)g_ram+name,"ncu2f.mod"),"probe actual optional fast filename");
 std::memset(&g_ee,0x77,sizeof g_ee);g_ee.r[2].uw[0]=resident?1:0;
}
extern "C" void f_0034CEF0_orig(){
 ++calls;seen=(char*)g_ram+arg_u32(0);
 check(!std::memcmp(&g_ee,&before,sizeof g_ee),"probe must preserve all native input registers");
 std::memset(&g_ee,0xa5,sizeof g_ee);after=g_ee;g_ram[0x200]=0x42;
}
extern "C" void f_00220788_orig(){
 ++calls;check(!std::memcmp(&g_ee,&before,sizeof g_ee),"wheel draw trace preserves all input registers");
 std::memset(&g_ee,0xa5,sizeof g_ee);after=g_ee;g_ram[0x200]=0x42;
}
extern "C" void f_00220788();
extern "C" void f_0034CEF0();
static void invoke(const char* text,uint32_t ra,bool exists,const char* wanted,unsigned want_probes){
 resident=exists;probes=calls=0;
 std::memset(&g_ee,0x11,sizeof g_ee);g_ee.r[4].uw[0]=name;g_ee.r[5].uw[0]=3;g_ee.r[31].uw[0]=ra;before=g_ee;
 std::memset(g_ram+name,0xcc,16);std::strcpy((char*)g_ram+name,text);
 uint8_t bytes[16];std::memcpy(bytes,g_ram+name,16);
 f_0034CEF0();
 check(seen==wanted,"native loader must receive the correct geometry resource");
 check(calls==1&&probes==want_probes,"only one draw resource load, bounded optional probe");
 check(!std::memcmp(g_ram+name,bytes,16),"temporary caller filename restored byte for byte");
 check(!std::memcmp(&g_ee,&after,sizeof g_ee),"full original EE result survives");
 check(g_ram[0x200]==0x42,"original side effect survives");
}
int main(){
 std::vector<uint8_t> ram(EE_RAM_SIZE);g_ram=ram.data();
 invoke("ncu2.mod",0x21f6dc,true,"ncu2f.mod",1);
 invoke("ncu2.mod",0x21f6dc,false,"ncu2.mod",1);
 invoke("muscle2.mod",0x21f6dc,true,"muscle2.mod",0);
 invoke("ncu2.mod",0x21f6c0,true,"ncu2.mod",0);
 invoke("ncu2.mod",0x123456,true,"ncu2.mod",0);
 invoke("ncu8.mod",0x21f6dc,true,"ncu8.mod",0);
 setenv("EOD_WHEEL_TRACE","1",1);
 std::memset(&g_ee,0,sizeof g_ee);g_ee.r[4].uw[0]=0x12000;g_ee.r[5].uw[0]=0;
 MEM_SW(0x12000,0x13000);MEM_SW(0x12004,0x10);MEM_SW(0x12008,0);MEM_SW(0x1200c,2);
 std::memcpy(g_ram+0x13000,"ncu",4);MEM_SW(0x1302c,0x1234);
 before=g_ee;const auto memory=ram;calls=0;
 f_00220788();
 check(calls==1&&!std::memcmp(&g_ee,&after,sizeof g_ee),"traced native draw result preserved");
 check(memory==ram,"wheel selection trace does not change guest memory");
 std::puts("Cup fast wheel hook contract passed");
}
