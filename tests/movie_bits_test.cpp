// Differential oracle comes from the owner's generated function in the build
// directory. This source contains only synthetic inputs and authored checks.
#include "ee.h"
#include "../src/movie_bits.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <fstream>
#include <iterator>
#include <vector>
extern "C" {
EEState g_ee;
uint8_t *g_ram;
volatile int g_ee_irq_pending;
#ifdef EE_TRACE_CALLS
uint32_t g_ee_trace[256],g_ee_trace_pos;
#endif
void reference_movie_bits_advance();
}
static int edge, trigger, mutation, services;
static std::vector<EEState> irq_states;
static std::vector<std::array<uint8_t,48>> irq_memory;
extern "C" void movie_bits_test_safe_point() { if (++edge == trigger) g_ee_irq_pending=1; }
extern "C" void ee_service_interrupts() {
    ++services; irq_states.push_back(g_ee);
    std::array<uint8_t,48> snapshot;
    memcpy(snapshot.data(),g_ram+(R[6].uw[0] & (EE_RAM_SIZE-1)),snapshot.size()); irq_memory.push_back(snapshot);
    g_ee_irq_pending=0;
    if(mutation==1) { // interrupt producer changes the next source byte
        uint32_t cursor=MEM_LW(R[6].uw[0]+12); g_ram[cursor]^=0xa7;
    } else if(mutation==2) { // interrupt changes buffered bits / read cursor
        MEM_SD(R[6].uw[0],0x9182736455463728ull);
        MEM_SW(R[6].uw[0]+12,MEM_LW(R[6].uw[0]+32));
    } else if(mutation==3) { // changed GPR address must use slow accessors
        R[6].ud[0]=0x20001000;
    } else if(mutation==4) {
        R[10].ud[0]^=0xfeedbeef; R[9].ud[0]+=7;
    }
}
extern "C" uint8_t ee_read8_slow(uint32_t a) { return g_ram[a & (EE_RAM_SIZE-1)]; }
extern "C" uint32_t ee_read32_slow(uint32_t a) { uint32_t x; memcpy(&x,g_ram+(a & (EE_RAM_SIZE-1)),4);return x; }
extern "C" uint64_t ee_read64_slow(uint32_t a) { uint64_t x; memcpy(&x,g_ram+(a & (EE_RAM_SIZE-1)),8);return x; }
extern "C" void ee_write32_slow(uint32_t a,uint32_t x) { memcpy(g_ram+(a & (EE_RAM_SIZE-1)),&x,4); }
extern "C" void ee_write64_slow(uint32_t a,uint64_t x) { memcpy(g_ram+(a & (EE_RAM_SIZE-1)),&x,8); }

int main(int argc, char **argv) {
    std::mt19937_64 random(0x6d6f766965);
    std::vector<uint8_t> initial(0x10000), expected(0x10000), actual(0x10000);
    for(auto &b:initial) b=(uint8_t)random();
    std::vector<uint8_t> media;
    if(argc>1) {
        std::ifstream input(argv[1],std::ios::binary);
        media.assign(std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>());
        if(media.empty()) { fputs("Could not read movie input\n",stderr);return 1; }
        printf("Using read-only movie source (%zu bytes); no fixtures are written\n",media.size());
    }
    unsigned fast_count=0, fallback_count=0, linear_fast_count=0;
    for(int trial=0;trial<12000;trial++) {
        const uint32_t state=0x1000, ring=0x8000;
        EEState input;
        for(size_t i=0;i<sizeof(input);i++) ((uint8_t *)&input)[i]=(uint8_t)random();
        input.r[4].uw[0]=state;
        const uint32_t count=trial<65*65 ? trial/65 : random()%81;
        const uint32_t request=trial<65*65 ? trial%65 : random()%81;
        input.r[5].uw[0]=request;
        const uint32_t length=1+random()%257;
        uint32_t cursor=ring+random()%length, begin=ring,end=ring+length;
        if(!media.empty()) {
            size_t offset=random()%media.size();
            for(uint32_t i=0;i<length;i++) initial[ring+i]=media[(offset+i)%media.size()];
        }
        if(trial%11==0) cursor=end-1; // wrap on the first byte
        if(trial%7==0) { begin=0;end=UINT32_MAX; } // real demuxer's linear sentinel
        if(trial%191==0) { begin=0;end=UINT32_MAX;cursor=state-8; } // adjacent, safe
        if(trial%193==0) { begin=0;end=UINT32_MAX;cursor=state-7; } // overlap for an 8-byte refill
        if(trial%197==0) { begin=0;end=UINT32_MAX;cursor=state+48; } // adjacent, safe
        if(trial%37==0) { begin=state;end=state+48;cursor=state+7; } // overlap -> fallback
        if(trial%53==0) input.r[4].uw[0]=0x20000000+state; // RAM alias -> fallback
        uint64_t buffered=random(),consumed=random();
        memcpy(initial.data()+state,&buffered,8);
        memcpy(initial.data()+state+24,&consumed,8);
        memcpy(initial.data()+state+12,&cursor,4);
        memcpy(initial.data()+state+16,&count,4);
        memcpy(initial.data()+state+32,&begin,4);
        memcpy(initial.data()+state+36,&end,4);
        expected=initial; actual=initial;
        mutation=(trial/5)%5;
        trigger=trial%5==0 ? 1+trial%7 : -1;
        g_ram=expected.data();g_ee=input;edge=0;services=0;g_ee_irq_pending=trial%17==0;
        irq_states.clear();irq_memory.clear();
        reference_movie_bits_advance();
        EEState expected_state=g_ee;
        auto expected_irq_states=irq_states;auto expected_irq_memory=irq_memory;
        int expected_edges=edge,expected_services=services,expected_pending=g_ee_irq_pending;
        g_ram=actual.data();g_ee=input;edge=0;services=0;g_ee_irq_pending=trial%17==0;
        irq_states.clear();irq_memory.clear();
        bool fast=movie_bits_advance_fast();
        if(!fast) {
            // A rejected layout must be untouched before the generated fallback.
            if(memcmp(&g_ee,&input,sizeof(input)) || actual!=initial) return 2;
            reference_movie_bits_advance();++fallback_count;
        } else { ++fast_count; if(end==UINT32_MAX) ++linear_fast_count; }
        if(memcmp(&g_ee,&expected_state,sizeof(g_ee)) || actual!=expected ||
            edge!=expected_edges || services!=expected_services || g_ee_irq_pending!=expected_pending || irq_memory!=expected_irq_memory ||
            irq_states.size()!=expected_irq_states.size() ||
            (!irq_states.empty() && memcmp(irq_states.data(),expected_irq_states.data(),irq_states.size()*sizeof(EEState)))) {
            fprintf(stderr,"mismatch trial%d count%u request%u fast%d irq%d mutation%d\n",trial,count,request,fast,trigger,mutation);
            for(int i=0;i<32;i++) if(memcmp(&g_ee.r[i],&expected_state.r[i],16)) fprintf(stderr," GPR%d actual%016llx expected%016llx\n",i,(unsigned long long)g_ee.r[i].ud[0],(unsigned long long)expected_state.r[i].ud[0]);
            return 1;
        }
    }
    printf("12000 cases: full EE state, RAM, IRQ-edge states match (%u fast incl%u linear sentinel, %u fallback)\n",fast_count,linear_fast_count,fallback_count);
    // A sentinel is not permission to read past the host RAM backing.
    g_ram=actual.data();g_ee={};R[4].ud[0]=0x1000;R[5].ud[0]=0;
    MEM_SW(0x1000+12,EE_RAM_SIZE-7);MEM_SW(0x1000+16,0);
    MEM_SW(0x1000+32,0);MEM_SW(0x1000+36,UINT32_MAX);
    EEState boundary_state=g_ee;auto boundary_ram=actual;
    if(movie_bits_advance_fast() || memcmp(&g_ee,&boundary_state,sizeof(g_ee)) || actual!=boundary_ram) return 3;
    // Same fixed input/call sequence, only the implementation changes.
    auto benchmark=[&](bool native) {
        actual=initial;g_ram=actual.data(); memset(&g_ee,0,sizeof(g_ee));
        MEM_SD(0x1000,0x8172635445362718ull);MEM_SD(0x1000+24,0);
        MEM_SW(0x1000+12,0x8000); MEM_SW(0x1000+16,64);
        MEM_SW(0x1000+32,0x8000); MEM_SW(0x1000+36,0x8100);
        trigger=-1;edge=0;g_ee_irq_pending=0;
        auto start=std::chrono::steady_clock::now();
        const unsigned requests[]={1,1,1,1,4,8,16,32,24,0,8,7,9,16,1,2};
        for(int i=0;i<2000000;i++) {
            R[4].ud[0]=0x1000;R[5].ud[0]=requests[i%16];
            if(!native || !movie_bits_advance_fast()) reference_movie_bits_advance();
        }
        return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    };
    double generated=benchmark(false),native=benchmark(true);
    printf("same-input 2M consume/refill calls: generated%.2fms native%.2fms (%.2fx)\n",generated,native,generated/native);
    return 0;
}
