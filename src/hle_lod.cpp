// EE-thread-only diagnostics. Host locals preserve inputs across the original;
// helpers never assign g_ee, so native caller/callee register effects survive.
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include "core/cpu.h"
#include "core/memory.h"
#include "core/config.h"
#include "lod_thresholds.h"

extern "C" void f_0021ABF8_orig();
extern "C" void f_0021C6E0_orig();
extern "C" void f_00218E68_orig();
extern "C" void f_0034E308_orig();
extern "C" void f_00212B58_orig();
namespace {
bool trace() { static const bool enabled = std::getenv("EOD_LOD_TRACE") != nullptr; return enabled; }
bool ram(uint32_t a, uint32_t size) { return a && !(a & 3) && a < EE_RAM_SIZE && size <= EE_RAM_SIZE-a; }
float loadf(uint32_t a) { uint32_t u = ee_r32(a); float f; std::memcpy(&f,&u,4); return f; }
void storef(uint32_t a, float f) { uint32_t u; std::memcpy(&u,&f,4); ee_w32(a,u); }
struct Table { uint32_t records, handles, count_ptr, count; };
bool table(uint32_t object, Table &t) {
    if (!ram(object,0x554)) return false;
    t.records=ee_r32(object+0x220); t.handles=ee_r32(object+0x21c); t.count_ptr=ee_r32(object+0x224);
    if (!ram(t.count_ptr,4)) return false;
    t.count=ee_r32(t.count_ptr);
    return t.count && t.count<=eod_lod::max_levels && ram(t.records,t.count*16) && ram(t.handles,t.count*4);
}
void selected(const char *where, uint32_t object, uint32_t ra) {
    Table t{};
    if (!table(object,t)) return;
    const uint32_t index=ee_r32(object+0x54c);
    std::fprintf(stderr,"[lod] %s ra=%08x object=%08x records=%08x count=%u min=%u distance=%.9g index=%u handle=%08x near=%.9g far=%.9g view=%u\n",
        where,ra,object,t.records,t.count,ee_r32(object+0x550),double(loadf(object+0x548)),index,
        index<t.count ? ee_r32(t.handles+index*4) : 0xffffffffu,double(loadf(0x4cd370)),double(loadf(0x4cd374)),ee_r32(0x3d1a74));
}
bool maximum() {
    static const bool enabled = !std::getenv("EOD_CAR_LOD_MULTIPLIER") && g_config.detail=="maximum";
    return enabled;
}
float multiplier() {
    static const float value=[] {
        if (!std::getenv("EOD_CAR_LOD_MULTIPLIER")) return g_config.detail=="high" ? 3.0f : 1.0f;
        float v;
        if (!eod_lod::parse_multiplier(std::getenv("EOD_CAR_LOD_MULTIPLIER"),v))
            std::fprintf(stderr,"[lod] invalid EOD_CAR_LOD_MULTIPLIER; using 1\n");
        return v;
    }();
    return value;
}
}
extern "C" void f_0021ABF8() {
    const uint32_t object=arg_u32(0),ra=g_ee.r[31].uw[0];
    char car[64]{};
    const uint32_t name=arg_u32(1);
    if (trace() && name && name<EE_RAM_SIZE) {
        for (unsigned i=0;i+1<sizeof car && i<EE_RAM_SIZE-name;++i) {
            const unsigned char c=g_ram[name+i];
            if (!c) break;
            car[i]=c>=32 && c<127 ? char(c) : '?';
        }
    }
    f_0021ABF8_orig();
    float factor=multiplier();
    if (!trace() && factor==1 && !maximum()) return;
    static unsigned errors;
    Table t{};
    if (!table(object,t)) {
        if (errors++<64) std::fprintf(stderr,"[lod] load invalid object=%08x ra=%08x\n",object,ra);
        return;
    }
    // Constructor 00217DF0 sets embedded storage. 0021C130 aliases it for
    // clones. Only a loader's own storage may be scaled, once after reparsing.
    float before[eod_lod::max_levels],after[eod_lod::max_levels];
    bool valid=t.records==object+0xf8 && t.count_ptr==object+0x1f8 && ee_r32(object+0x1f8)==t.count;
    valid=valid && ee_r32(object+0x550)<t.count;
    for (unsigned i=0;i<t.count;++i) {
        before[i]=loadf(t.records+i*16);
        valid=valid && ee_r32(object+0x74+i*4)!=0xffffffffu;
    }
    // Keep the native minimum model (usually 1, not the incomplete model 0).
    // One million scaled distance units is > 800x the largest native far plane
    // (1150); retain ordered finite thresholds, handles and capability flags.
    if (valid && maximum()) {
        const float first=before[ee_r32(object+0x550)];
        valid=std::isfinite(first) && first>0;
        if (valid) factor=std::max(1.0f, 1000000.0f/first);
    }
    valid=valid && eod_lod::scale(before,after,t.count,factor);
    static unsigned loads;
    const bool log=trace() && loads++<64;
    if (log) std::fprintf(stderr,"[lod] load car=\"%s\" object=%08x ra=%08x records=%08x count_ptr=%08x handles=%08x count=%u min=%u multiplier=%.9g valid=%d\n",car,object,ra,t.records,t.count_ptr,t.handles,t.count,ee_r32(object+0x550),double(factor),int(valid));
    for (unsigned i=0;i<t.count;++i) {
        if (valid && factor!=1) storef(t.records+i*16,after[i]);
        if (log) std::fprintf(stderr,"[lod] level object=%08x i=%u threshold=%.9g effective=%.9g flags=%u,%u,%u original_handle=%08x active_handle=%08x\n",object,i,double(before[i]),double(loadf(t.records+i*16)),ee_r32(t.records+i*16+4),ee_r32(t.records+i*16+8),ee_r32(t.records+i*16+12),ee_r32(object+0x74+i*4),ee_r32(t.handles+i*4));
    }
    if (!valid && factor!=1 && errors++<64) std::fprintf(stderr,"[lod] multiplier rejected object=%08x (layout/count/min/handles/thresholds)\n",object);
}
extern "C" void f_0021C6E0() {
    const uint32_t object=arg_u32(0),ra=g_ee.r[31].uw[0];
    f_0021C6E0_orig();
    static unsigned calls,lines;
    if (trace() && lines<128 && (++calls<=16 || calls%120==0)) { ++lines; selected("single",object,ra); }
}
extern "C" void f_00218E68() {
    const uint32_t ra=g_ee.r[31].uw[0];
    static unsigned calls,lines;
    const bool log=trace() && lines<64 && (++calls<=2 || calls%120==0);
    uint32_t objects[20]{};
    if (log) for (unsigned i=0;i<20;++i) { objects[i]=ee_r32(0x3d1c90+i*4); if (!objects[i]) break; selected("batch-before",objects[i],ra); }
    f_00218E68_orig(); // native selection AND submission take place inside
    if (log) { ++lines; for (auto object:objects) { if (!object) break; selected("batch-after",object,ra); } }
}
extern "C" void f_0034E308() {
    static unsigned calls,lines;
    const bool log=trace() && lines<128 && (++calls<=16 || calls%4096==0);
    const uint32_t centre=arg_u32(0),classification=arg_u32(1),distance=arg_u32(2),ra=g_ee.r[31].uw[0];
    const float radius=g_ee.fpr[12].f;
    float xyz[3]{};
    if (log && ram(centre,12)) for (unsigned i=0;i<3;++i) xyz[i]=loadf(centre+i*4);
    f_0034E308_orig();
    if (log) {
        ++lines;
        // Optional native outputs can be unwritten on early rejection: label
        // them as raw post-call words rather than claiming a classification.
        std::fprintf(stderr,"[lod] visibility ra=%08x centre=%08x xyz=%.9g,%.9g,%.9g radius=%.9g result=%u class_ptr=%08x class_raw=%08x distance_ptr=%08x distance_raw=%08x near=%.9g far=%.9g tan=%.9g sphere=%.9g camera=%.9g,%.9g,%.9g\n",ra,centre,double(xyz[0]),double(xyz[1]),double(xyz[2]),double(radius),g_ee.r[2].uw[0],classification,ram(classification,4)?ee_r32(classification):0,distance,ram(distance,4)?ee_r32(distance):0,double(loadf(0x4cd370)),double(loadf(0x4cd374)),double(loadf(0x4cd378)),double(loadf(0x4cd380)),double(loadf(0x4cd31c)),double(loadf(0x4cd320)),double(loadf(0x4cd324)));
    }
}

// Native camera builder divides the configured distance by the view count and
// derives BOTH projection and fog from that quotient. Scope the source distance
// here rather than altering the global view count or projection alone.
extern "C" void f_00212B58() {
    const uint32_t original=ee_r32(0x495d58);
    const float distance=loadf(0x495d58);
    const bool extend=maximum() && ee_r32(0x3d1a74)==2 &&
        std::isfinite(distance) && distance>0 && distance<=1150;
    if (extend) storef(0x495d58,distance*2);
    f_00212B58_orig();
    if (extend) ee_w32(0x495d58,original);
}
