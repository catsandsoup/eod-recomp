// CPU-only production wrapper contract; no generated game code or GPU.
#include "../../tests/test_env.h"
#include "core/cpu.h"
#include "core/diff_harness.h"
#include "lod_thresholds.h"
#include "visual_car_list.h"
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
static void simulate_original() {
    ++native_calls;
    // Deliberately overwrite every guest register, including upper GPR halves,
    // FACC/FCC/SP/RA, to distinguish the true native result from saved input.
    std::memset(&g_ee,0xa5,sizeof g_ee); native_after=g_ee;
    MEM_SW(0x200,0x12345678);
}
extern "C" void f_0021ABF8_orig() { simulate_original(); }
extern "C" void f_0021C6E0_orig() { simulate_original(); }
// Exercise the wrapper's handling of the fixture's original EE result.
void diff_run(uint32_t, void (*)(), void (*original)(), unsigned) { original(); }
void diff_add_exit_report(void (*)()) {}
void eod_visual_car_select_lod_index() { std::abort(); }
extern "C" void f_00218E68_orig() { simulate_original(); }
extern "C" void f_0020F4A0_orig() { std::abort(); }
extern "C" void f_0021C370_orig() { std::abort(); }
extern "C" void f_00218E68_cars(VisualCarTraversal *) { std::abort(); }
extern "C" void f_0020F4A0_cars(VisualCarTraversal *) { std::abort(); }
extern "C" void f_0021C370_cars(VisualCarTraversal *) { std::abort(); }
namespace native { bool enabled() { return false; } }
extern "C" void f_0034E308_orig() { simulate_original(); }
extern "C" void f_00212B58_orig() { uint32_t u=MEM_LW(0x495d58); std::memcpy(&distance_seen,&u,4); simulate_original(); }
extern "C" void f_00212B58();
uint8_t g_scratchpad[0x4000];                     // hle_draw_distance.cpp (DrawDistance off here)
static bool graf_needs_relocation = true;
constexpr uint32_t guest_stack = 0x20000;
extern "C" void f_00214678_orig() {
    // Simulate the version and relocation outputs written by the native resource lookup.
    MEM_SW(g_ee.r[29].uw[0] - 0xd0 + 0x30, 18);
    MEM_SW(g_ee.r[29].uw[0] - 0xd0 + 0x3c, graf_needs_relocation);
    simulate_original();
}
extern "C" void f_00214678();
extern "C" void f_003582B8_orig() { simulate_original(); }
extern "C" void f_0021ABF8();
extern "C" void f_0021C6E0();
extern "C" void f_00218E68();
extern "C" void f_0034E308();
static void invoke(void (*fn)()) {
    std::memset(&g_ee,0,sizeof g_ee); g_ee.r[4].uw[0]=object;
    g_ee.r[29].uw[0] = guest_stack;
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

static void check_world_lod(std::vector<uint8_t> &memory, bool maximum, bool extended) {
    constexpr uint32_t graf = 0x11000, mlist = 0x11100, groups = 0x12000, models = 0x13000;
    constexpr float open = 4e16f;
    MEM_SW(0x3d1c18, graf);
    MEM_SW(graf + 4, mlist);
    MEM_SW(graf + 8, mlist);
    MEM_SW(mlist + 0x18, 5);
    MEM_SW(mlist + 0x1c, groups);
    MEM_SW(mlist + 0x20, 9);
    MEM_SW(mlist + 0x24, models);
    float original[][3] = {{100, 400, open}, {25, 100, 225}, {100, open, open},
                           {100, 400, open}, {100, 100, open}};
    const uint32_t parents[] = {1, 1, 2, 2, 2, 3, 4, 5, 5};
    const uint32_t masks[] = {0, 1, 0, 1, 5, 0, 1, 0, 1};
    for (unsigned i = 0; i < 9; ++i) MEM_SW(models + 32 * i + 16, parents[i] | (masks[i] << 16));
    for (unsigned i = 0; i < 5; ++i) std::memcpy(memory.data() + groups + 32 * i, original[i], 12);
    auto expected = memory;
    const float factor = extended ? 64.0f : 1.0f;
    float wanted[][3] = {{maximum ? 400 * factor : 100, 400 * factor, open},
                         {maximum ? 225 * factor : 25, maximum ? 225 * factor : 100, 225 * factor},
                         {100 * factor, open, open}, {100, 400 * factor, open},
                         {100, 100 * factor, open}};
    for (unsigned i = 0; i < 5; ++i) std::memcpy(expected.data() + groups + 32 * i, wanted[i], 12);
    constexpr uint32_t version = 18, first_load = 1, resident = 0;
    const uint32_t graf_version = guest_stack - 0xd0 + 0x30;
    const uint32_t graf_relocate = guest_stack - 0xd0 + 0x3c;
    std::memcpy(expected.data() + graf_version, &version, 4);
    std::memcpy(expected.data() + graf_relocate, &first_load, 4);
    invoke(f_00214678);
    check(memory == expected, "only populated retail-shaped world LODs collapse at Maximum, with the cutoff scaled once");

    graf_needs_relocation = false;
    std::memcpy(expected.data() + graf_relocate, &resident, 4);
    invoke(f_00214678);
    check(memory == expected, "reusing the same relocated GRAF keeps all existing world LOD thresholds");

    // A freshly loaded GRAF can reuse the same guest addresses.
    for (unsigned i = 0; i < 5; ++i) std::memcpy(memory.data() + groups + 32 * i, original[i], 12);
    graf_needs_relocation = true;
    std::memcpy(expected.data() + graf_relocate, &first_load, 4);
    invoke(f_00214678);
    check(memory == expected, "world LOD policy applies again to fresh GRAF bytes at the same address");
}

int main(int argc,char **argv) {
    check(argc==2 || argc==3,"mode: off/one/two/trace/invalid/high/maximum/override [8]");
    const std::string mode=argv[1];
    const bool test_draw_distance = argc == 3;
    check(!test_draw_distance || ((mode == "maximum" || mode == "off") && std::strcmp(argv[2], "8") == 0),
          "extended draw distance requires maximum or off mode and factor 8");
    unsetenv("EOD_CAR_LOD_MULTIPLIER"); unsetenv("EOD_LOD_TRACE");
    unsetenv("EOD_DRAW_DISTANCE");
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
    auto selection_expected = memory;
    if (mode == "maximum") {
        const uint32_t minimum_lod = MEM_LW(object + 0x550);
        std::memcpy(selection_expected.data() + object + 0x54c, &minimum_lod, sizeof minimum_lod);
    }
    invoke(f_0021C6E0);
    check(memory == selection_expected, "only Maximum replaces the selected LOD with the native minimum");
    for (auto fn : {f_00218E68, f_0034E308}) {
        invoke(fn);
        check(memory == selection_expected, "diagnostic memory pass-through");
    }
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
    putf(0x495d58,1150);
    if (test_draw_distance) g_config.draw_distance = 8;
    const float maximum_source[] = {1150, 1150, 2300, 3450, 4600, 1150};
    for (uint32_t views : {0u, 1u, 2u, 3u, 4u, 5u}) {
        MEM_SW(0x3d1a74,views);
        auto snapshot = memory;
        if (test_draw_distance) {
            uint32_t fog_views = views;
            if (views == 0 || (mode == "maximum" && views <= 4)) fog_views = 1;
            if (views > 4) fog_views = 4;
            const float fog_distance = 1150.0f / float(fog_views);
            const float fog_start = fog_distance * 0.1f;
            const float fog_end = fog_distance * 2.0f;
            std::memcpy(snapshot.data() + 0x4d8b84, &fog_start, sizeof fog_start);
            std::memcpy(snapshot.data() + 0x4d8b88, &fog_end, sizeof fog_end);
        }
        invoke(f_00212B58);
        check(memory == snapshot, "camera scope restores native distance storage and unscaled fog");
        const float source = mode == "maximum" ? maximum_source[views] : 1150.0f;
        const float expected = test_draw_distance ? source * 8 : source;
        if (distance_seen != expected) {
            std::fprintf(stderr, "views=%u: expected camera source %.0f, got %.0f\n", views, expected, distance_seen);
            check(false, "Maximum preserves 1P camera distance across supported split views");
        }
    }
    if (!test_draw_distance) {
        MEM_SW(0x3d1a74,4);
        for(float distance:{-1.0f,0.0f,1151.0f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
            putf(0x495d58,distance); const auto snapshot=memory; invoke(f_00212B58);
            check(memory==snapshot,"unsupported camera source remains bit-identical");
            check(std::memcmp(&distance_seen,&distance,4)==0,"unsupported camera distance passed through");
        }
    }
    check_world_lod(memory, g_config.detail == "maximum", test_draw_distance);
    char path[]="/tmp/eod-detail-test-XXXXXX"; int fd=mkstemp(path); check(fd>=0,"config temp file"); close(fd);
    for (const char *detail : {"original","high","maximum"}) {
        Config c; c.detail=detail; check(config_save(path,c),"save detail");
        Config loaded; check(config_load(path,loaded) && loaded.detail==detail,"detail round trip");
    }
    FILE *f=fopen(path,"w"); std::fprintf(f,"[Video]\nDetail=INVALID\n"); fclose(f);
    Config c; check(config_load(path,c) && c.detail=="original","invalid detail remains original"); unlink(path);
    std::puts("LOD CPU contract passed");
}
