// CPU-only production wrapper contract; no generated game code or GPU.
#include "core/cpu.h"
#include "lod_thresholds.h"
#include "core/config.h"
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
EEState g_ee{};
uint8_t *g_ram;
constexpr uint32_t object=0x10000;
static EEState native_after;
static unsigned native_calls;
static float distance_seen;
extern "C" uint32_t ee_read32_slow(uint32_t) { std::abort(); }
extern "C" void ee_write32_slow(uint32_t,uint32_t) { std::abort(); }
static void check(bool ok,const char *s) { if (!ok) { std::fprintf(stderr,"FAIL: %s\n",s); std::exit(1); } }
static void putf(uint32_t a,float f) { uint32_t u; std::memcpy(&u,&f,4); MEM_SW(a,u); }
static void native() {
    ++native_calls;
    // Deliberately overwrite every guest register, including upper GPR halves,
    // FACC/FCC/SP/RA, to distinguish the true native result from saved input.
    std::memset(&g_ee,0xa5,sizeof g_ee); native_after=g_ee;
    MEM_SW(0x200,0x12345678);
}
extern "C" void f_0021ABF8_orig() { native(); }
extern "C" void f_0021C6E0_orig() { native(); }
extern "C" void f_00218E68_orig() { native(); }
extern "C" void f_0034E308_orig() { native(); }
extern "C" void f_00212B58_orig() { uint32_t u=MEM_LW(0x495d58); std::memcpy(&distance_seen,&u,4); native(); }
extern "C" void f_00212B58();
extern "C" void f_0021ABF8();
extern "C" void f_0021C6E0();
extern "C" void f_00218E68();
extern "C" void f_0034E308();
static void invoke(void (*fn)()) {
    std::memset(&g_ee,0,sizeof g_ee); g_ee.r[4].uw[0]=object;
    fn();
    check(std::memcmp(&g_ee,&native_after,sizeof g_ee)==0,"full native EE result preserved");
    check(MEM_LW(0x200)==0x12345678,"native side effect preserved");
}
static void fixture() {
    MEM_SW(object+0x220,object+0xf8); MEM_SW(object+0x224,object+0x1f8);
    MEM_SW(object+0x21c,object+0xb4); MEM_SW(object+0x1f8,5); MEM_SW(object+0x550,1);
    float values[]={-0.0f,10,15,30,500};
    for (unsigned i=0;i<5;++i) {
        putf(object+0xf8+i*16,values[i]);
        for (unsigned j=1;j<4;++j) MEM_SW(object+0xf8+i*16+j*4,0x12340000+i*4+j);
        MEM_SW(object+0x74+i*4,i); MEM_SW(object+0xb4+i*4,i);
    }
}
int main(int argc,char **argv) {
    check(argc==2,"mode: off/one/two/trace/invalid");
    const std::string mode=argv[1];
    unsetenv("EOD_CAR_LOD_MULTIPLIER"); unsetenv("EOD_LOD_TRACE");
    if (mode=="one") setenv("EOD_CAR_LOD_MULTIPLIER","1",1);
    if (mode=="two") setenv("EOD_CAR_LOD_MULTIPLIER","2",1);
    if (mode=="trace") setenv("EOD_LOD_TRACE","1",1);
    if (mode=="invalid") setenv("EOD_CAR_LOD_MULTIPLIER","nan",1);
    if (mode=="high") g_config.detail="high";
    if (mode=="maximum") g_config.detail="maximum";
    if (mode=="override") { g_config.detail="maximum"; setenv("EOD_CAR_LOD_MULTIPLIER","1",1); }
    float parsed=0;
    for (const char *s:{"", "0", "-2", "nan", "inf", "2oops", "1e90"}) check(!eod_lod::parse_multiplier(s,parsed) && parsed==1,"invalid multiplier fails to identity");
    float in[]={-0.0f,10,15,30,500},out[5];
    check(eod_lod::scale(in,out,5,1) && std::memcmp(in,out,sizeof in)==0,"identity bit equality including negative zero");
    check(eod_lod::scale(in,out,5,2) && out[1]==20 && out[4]==1000,"positive scale");
    float bad[]={10,5}; float guard[]={77,88};
    check(!eod_lod::scale(bad,guard,2,2) && guard[0]==77 && guard[1]==88,"unordered failure transactional");
    float huge[]={std::numeric_limits<float>::max()};
    check(!eod_lod::scale(huge,guard,1,2) && guard[0]==77,"overflow failure transactional");
    check(!eod_lod::scale(in,out,0,2) && !eod_lod::scale(in,out,17,2),"bounded counts");
    std::vector<uint8_t> memory(EE_RAM_SIZE); g_ram=memory.data(); fixture(); MEM_SW(0x200,0x12345678);
    const auto before=memory; invoke(f_0021ABF8);
    auto expected=before;
    if (mode=="two") for (unsigned i=1;i<5;++i) { const float v=in[i]*2; std::memcpy(expected.data()+object+0xf8+i*16,&v,4); }
    if (mode=="high") for (unsigned i=1;i<5;++i) { const float v=in[i]*3; std::memcpy(expected.data()+object+0xf8+i*16,&v,4); }
    if (mode=="maximum") for (unsigned i=1;i<5;++i) { const float v=in[i]*100000; std::memcpy(expected.data()+object+0xf8+i*16,&v,4); }
    check(memory==expected,"only positive threshold words change; off/1 byte identity");
    // A clone alias, an invalid handle, and overflow must all retain bytes.
    for (unsigned which=0;which<3;++which) {
        fixture();
        if (which==0) MEM_SW(object+0x220,object+0x100);
        if (which==1) MEM_SW(object+0x74+4,0xffffffffu);
        if (which==2) putf(object+0xf8+64,std::numeric_limits<float>::max());
        const auto rejected=memory; invoke(f_0021ABF8);
        check(memory==rejected,"unsafe table does not partially change");
    }
    fixture(); MEM_SW(0x3d1c90,object);
    for (auto fn:{f_0021C6E0,f_00218E68,f_0034E308}) { const auto snapshot=memory; invoke(fn); check(memory==snapshot,"diagnostic memory pass-through"); }
    check(native_calls==7,"each original called exactly once");
    if (mode=="maximum") {
        fixture(); MEM_SW(object+0x550,2);
        auto min_expected=memory; const float factor=1000000.0f/15.0f;
        for(unsigned i=1;i<5;++i) { float v=float(double(in[i])*factor); std::memcpy(min_expected.data()+object+0xf8+i*16,&v,4); }
        invoke(f_0021ABF8); check(memory==min_expected,"Maximum respects non-default native minimum and flags");
        for(unsigned min:{0u,5u,0xffffffffu}) {
            fixture(); MEM_SW(object+0x550,min); const auto snapshot=memory;
            invoke(f_0021ABF8); check(memory==snapshot,"invalid/zero-distance minimum cannot rewrite table");
        }
    }
    putf(0x495d58,1150); MEM_SW(0x3d1a74,2);
    const auto distance_before=memory; invoke(f_00212B58);
    check(memory==distance_before,"camera scope restores native distance storage");
    check(distance_seen==(mode=="maximum" ? 2300.0f : 1150.0f),"Maximum gives both projection and fog the 1P distance");
    for (uint32_t views:{0u,1u,3u,4u}) {
        MEM_SW(0x3d1a74,views); invoke(f_00212B58);
        check(distance_seen==1150,"only supported 2P cameras change");
    }
    MEM_SW(0x3d1a74,2);
    for(float distance:{-1.0f,0.0f,1151.0f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
        putf(0x495d58,distance); const auto snapshot=memory; invoke(f_00212B58);
        check(memory==snapshot,"unsupported camera source remains bit-identical");
        check(std::memcmp(&distance_seen,&distance,4)==0,"unsupported camera distance passed through");
    }
    char path[]="/tmp/eod-detail-test-XXXXXX"; int fd=mkstemp(path); check(fd>=0,"config temp file"); close(fd);
    for (const char *detail : {"original","high","maximum"}) {
        Config c; c.detail=detail; check(config_save(path,c),"save detail");
        Config loaded; check(config_load(path,loaded) && loaded.detail==detail,"detail round trip");
    }
    FILE *f=fopen(path,"w"); std::fprintf(f,"[Video]\nDetail=INVALID\n"); fclose(f);
    Config c; check(config_load(path,c) && c.detail=="original","invalid detail remains original"); unlink(path);
    std::puts("LOD CPU contract passed");
}
