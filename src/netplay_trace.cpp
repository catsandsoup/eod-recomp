// Default-off native diagnostics; separate explicit test controls/seed options
// modify native inputs only when requested. Normal offline behavior is retained.
#include "netplay_trace.h"
#include "netplay_car.h"
#include "netplay_session.h"
#include "../runtime/core/memory.h"
#include "../runtime/kernel/kernel.h"
#include <cstdio>
#include <cstdlib>

namespace {
FILE *trace() {
 static FILE *file = []() -> FILE * {
  const char *path = std::getenv("EOD_NETPLAY_TRACE");
  if (!path || !*path) return nullptr;
  FILE *f = std::fopen(path,"wb");
  if (!f) { std::perror("EOD_NETPLAY_TRACE"); std::abort(); }
  std::setvbuf(f,nullptr,_IOFBF,1024*1024);
  std::fputs("{\"event\":\"schema\",\"version\":2,\"boundary\":\"00177718-return\",\"car_bytes\":7168,\"block_bytes\":64}\n",f);
  return f;
 }();
 return file;
}
uint64_t step=0;
uint32_t race_epoch=0;
void record_step() {
 FILE *f=trace(); if(!f)return;
 std::fprintf(f,"{\"event\":\"tick\",\"step\":%llu,\"vblank\":%llu,\"native_frame\":%u,\"clock_bits\":%u,\"phase\":%u,\"cars\":[",
  (unsigned long long)step,(unsigned long long)kernel_vblank_count(),ee_r32(0x3CDA8C),ee_r32(0x3CDACC),ee_r32(0x3CDAE8));
 uint32_t count=ee_r32(0x48522C);
 if(step==1) {
  const char *path=std::getenv("EOD_NETPLAY_INITIAL_RAM");
  if(path && *path) {
   FILE *snapshot=std::fopen(path,"wb");
   if(!snapshot) {std::perror("EOD_NETPLAY_INITIAL_RAM");std::abort();}
   uint8_t page[4096];
   for(uint32_t a=0;a<EE_RAM_SIZE;a+=sizeof(page)) {
    ee_read_block(a,page,sizeof(page));
    if(std::fwrite(page,1,sizeof(page),snapshot)!=sizeof(page))std::abort();
   }
   std::fclose(snapshot);
  }
 }
 if(count>16) { std::fprintf(stderr,"netplay trace invalid driver count %u\n",count); std::abort(); }
 for(uint32_t i=0;i<count;++i) {
  uint32_t driver=ee_r32(0x485230+4*i);
  uint32_t car=(driver && driver<EE_RAM_SIZE-8 && !(driver&3))?ee_r32(driver+4):0;
  std::fprintf(f,"%s{\"driver\":%u,\"car\":%u,\"blocks\":[",i?",":"",driver,car);
  if(car && !(car&3) && car<EE_RAM_SIZE-0x1c00) {
   for(uint32_t off=0;off<0x1c00;off+=64) {
    // All bytes in this diagnostic range, including unknown fields, participate.
    uint64_t hash=14695981039346656037ull;
    for(uint32_t j=0;j<64;++j) {hash^=ee_r8(car+off+j);hash*=1099511628211ull;}
    std::fprintf(f,"%s\"%016llx\"",off?",":"",(unsigned long long)hash);
   }
  }
  std::fputs("],\"probe_c40\":[",f);
  if(car && !(car&3) && car<EE_RAM_SIZE-0x1c00) {
   for(uint32_t j=0;j<64;j+=4) std::fprintf(f,"%s%u",j?",":"",ee_r32(car+0xc40+j));
  }
  std::fputs("]}",f);
 }
 std::fprintf(f,"],\"driver_count\":%u,\"seed_state\":[",count);
 for(uint32_t i=0;i<6;++i)std::fprintf(f,"%s%u",i?",":"",ee_r32(0x4473b0+i*4));
 std::fputs("],\"rng_streams\":[",f);
 for(uint32_t stream=0;stream<6;++stream) {
  uint64_t hash=14695981039346656037ull;
  for(uint32_t j=0;j<0x19c;++j){hash^=ee_r8(0x3cc470+stream*0x19c+j);hash*=1099511628211ull;}
  std::fprintf(f,"%s\"%016llx\"",stream?",":"",(unsigned long long)hash);
 }
 std::fputs("]}\n",f);
}
}
void netplay_trace_pad(int port,int slot,uint32_t length,const uint8_t *data) {
 FILE *f=trace();if(!f)return;
 std::fprintf(f,"{\"event\":\"pad_read\",\"step\":%llu,\"vblank\":%llu,\"port\":%d,\"slot\":%d,\"length\":%u,\"bytes\":\"",
 (unsigned long long)step,(unsigned long long)kernel_vblank_count(),port,slot,length);
 for(int i=0;i<20;++i)std::fprintf(f,"%02x",data[i]);
 std::fputs("\"}\n",f);
}
extern "C" void f_00177718_orig(void);
extern "C" void f_00177718(void) { netplay_car::before_step(); netplay::session().step_begin(); f_00177718_orig(); ++step; record_step(); netplay_car::after_step(); netplay::session().step_end(); }

extern "C" void f_0038FF40_orig(void);
extern "C" void f_0038FF40(void) {
 const uint32_t port=R[4].uw[0], slot=R[5].uw[0], dst=R[6].uw[0];
 f_0038FF40_orig();
 if(port==0 && dst<EE_RAM_SIZE-32 && R[2].uw[0]>=4) {
  // online lobby START while the native main menu waits: press ACTION for the player
  const uint16_t press=netplay::session().menu_buttons();
  if(press){uint16_t keys=uint16_t(~(ee_r8(dst+2)|(ee_r8(dst+3)<<8)));keys|=press;ee_w8(dst+2,uint8_t(~keys));ee_w8(dst+3,uint8_t(~keys>>8));}
 }
 if(trace() && R[2].uw[0] && dst<EE_RAM_SIZE-32) {
  uint8_t bytes[20]={}; const uint32_t n=R[2].uw[0]<20?R[2].uw[0]:20;
  ee_read_block(dst,bytes,n);netplay_trace_pad(port,slot,R[2].uw[0],bytes);
 }
}

#include "netplay_control.h"
extern "C" void f_00191548_orig(void);
extern "C" void f_00191548(void) {
 const uint32_t input=R[4].uw[0], car=R[5].uw[0];
 f_00191548_orig();
 if(!input || input>=EE_RAM_SIZE-48)return;
 const uint32_t record=ee_r32(input+8);
 if(!record || record>=EE_RAM_SIZE-6)return;
 static bool pattern=[](){const char *s=std::getenv("EOD_NETPLAY_TEST_CONTROLS");return s && *s=='1';}();
 if(pattern) {
  uint32_t player=0;
  const uint32_t count=ee_r32(0x48522c);
  if(count>16)std::abort();
  for(uint32_t i=0;i<count;++i) {
   const uint32_t d=ee_r32(0x485230+i*4);
   if(!d || d>=EE_RAM_SIZE-8 || (d&3))std::abort();
   const uint32_t c=ee_r32(d+4);
   if(c==car)break;
   if(c && c<EE_RAM_SIZE-0x1e4 && ee_r32(c+0x1e0)==0x50)++player;
  }
  uint8_t data[4];netplay::encode_control(netplay::test_control(uint64_t(ee_r32(0x3cda8c))+1,player),data);
  ee_write_block(record,data,sizeof(data));
 }
 FILE *f=trace();if(!f)return;
 std::fprintf(f,"{\"event\":\"driver_input\",\"step\":%llu,\"native_frame\":%u,\"car\":%u,\"record\":\"",(unsigned long long)(step+1),ee_r32(0x3cda8c),car);
 for(int i=0;i<6;++i)std::fprintf(f,"%02x",ee_r8(record+i));
 std::fprintf(f,"\",\"test_pattern\":%s}\n",pattern?"true":"false");
}

extern "C" void f_00176918_orig(void);
extern "C" void f_00176918(void) {
 const uint32_t dst=R[4].uw[0];
 f_00176918_orig();
 const char *env=std::getenv("EOD_NETPLAY_SEED");
 if(!env || !*env)return;
 char *end=nullptr;const unsigned long value=std::strtoul(env,&end,10);
 if(*end || value>0xfffffffful || dst<0x4473b0 || dst>0x4473c0 || (dst-0x4473b0)%8) {
  std::fputs("invalid netplay seed or unexpected seed destination\n",stderr);std::abort();
 }
 const auto seeds=netplay::session_seeds(uint32_t(value),(dst-0x4473b0)/8);
 ee_w32(dst,seeds.first);ee_w32(dst+4,seeds.second);
 FILE *f=trace();if(f)std::fprintf(f,"{\"event\":\"session_seed\",\"dst\":%u,\"first\":%u,\"second\":%u}\n",dst,seeds.first,seeds.second);
}

extern "C" void f_00176698_orig(void);
extern "C" void f_0012EF50(void);
extern "C" void f_00176698(void) {
 const uint32_t previous=ee_r32(0x3cda8c), next=R[4].uw[0];
 f_00176698_orig();
 static const bool align=[](){const char *s=std::getenv("EOD_NETPLAY_ALIGN_START");return s && *s=='1';}();
 if(!align || !netplay::begins_epoch(previous,next,step>0))return;
 const char *env=std::getenv("EOD_NETPLAY_SEED");
 if(!env || !*env){std::fputs("start alignment requires explicit session seed\n",stderr);std::abort();}
 char *end=nullptr;const unsigned long seed=std::strtoul(env,&end,10);
 if(*end || seed>0xfffffffful)std::abort();
 // Race seed-pair slots B0/B8/C0 correspond to native streams4/3/5.
 // Preserve those meanings while giving remaining streams agreed session seeds.
 const uint32_t ordinal[6]={3,4,5,1,0,2};
 const EEState saved=g_ee;
 for(uint32_t stream=0;stream<6;++stream) {
  const auto pair=netplay::session_seeds(uint32_t(seed),ordinal[stream]);
  R[4].ud[0]=stream;R[5].ud[0]=pair.first;R[6].ud[0]=pair.second;
  f_0012EF50();
 }
 g_ee=saved;
 ee_w32(0x3cdae8,0);
 ++race_epoch;
 FILE *f=trace();if(f)std::fprintf(f,"{\"event\":\"race_start_epoch\",\"after_step\":%llu,\"previous_native_frame\":%u,\"seed\":%lu,\"input_phase\":0}\n",(unsigned long long)step,previous,seed);
}

namespace {
void trace_rng0(const char *kind,uint32_t stream,uint32_t caller) {
 static const bool enabled=[](){const char *s=std::getenv("EOD_NETPLAY_TRACE_RNG");return s && *s=='1';}();
 static uint32_t count=0;
 if(!enabled || stream!=0 || !race_epoch)return;
 FILE *f=trace();if(!f)return;
 if(count++>=100000) {if(count==100001)std::fputs("{\"event\":\"rng_trace_limit\"}\n",f);return;}
 std::fprintf(f,"{\"event\":\"rng0_call\",\"kind\":\"%s\",\"step\":%llu,\"native_frame\":%u,\"caller\":%u,\"float_bits\":%u,\"integer\":%u}\n",kind,(unsigned long long)step,ee_r32(0x3cda8c),caller,FPR[0].u,R[2].uw[0]);
}
}
extern "C" void f_0012F3C8_orig(void);
extern "C" void f_0012F3C8(void) {const uint32_t s=R[4].uw[0],c=R[31].uw[0];f_0012F3C8_orig();trace_rng0("float",s,c);}
extern "C" void f_0012F470_orig(void);
extern "C" void f_0012F470(void) {const uint32_t s=R[4].uw[0],c=R[31].uw[0];f_0012F470_orig();trace_rng0("range",s,c);}
extern "C" void f_0012F4F0_orig(void);
extern "C" void f_0012F4F0(void) {const uint32_t s=R[4].uw[0],c=R[31].uw[0];f_0012F4F0_orig();trace_rng0("bound",s,c);}
