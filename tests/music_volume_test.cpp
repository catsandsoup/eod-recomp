// Exercise the real SPU2 mixer without devices or disc data. Including the
// implementation keeps the mixer internals out of the production public API.
#include "../runtime/iop/spu2.cpp"
#include <cassert>

bool g_log_enabled[LOG_CAT_COUNT]{};
void log_write(LogCat, const char *, ...) {}
uint8_t* g_iop_ram=nullptr;
ProfCounter g_prof[P_COUNT];
thread_local ProfThread t_prof;
uint64_t platform_time_us(){return 0;}
bool platform_audio_open(int){return false;}
void platform_audio_submit(const int16_t*,int){}
int platform_audio_queued_frames(){return 0;}
void overlay_mix_audio(int16_t*,int){}
namespace custom_music {
static bool test_host=false;
void host_begin_block(){}
bool host_active(int slot){return slot==0&&test_host;}
bool host_frame(int slot,int& l,int& r){l=r=12000;return host_active(slot);}
}

static int sample(int volume,int voice,bool song,bool custom=false) {
 spu2_init();g_config.music_volume=volume;custom_music::test_host=custom;
 Voice& v=g_core[0].v[voice];v.phase=SUSTAIN;v.env.level=0x7fff;
 v.env.increment=0;v.voll=v.volr=0x3fff;
 v.cur=music_blocks::first;v.s_prev=v.s_cur=12000;v.pitch=0;
 music_blocks::set(v.cur,song);
 int16_t out[2]{};mix(out,1);assert(out[0]==out[1]);return out[0];
}
int main() {
 const int stock=sample(100,0,true);assert(stock>1000);
 assert(sample(0,0,true)==0);
 assert(std::abs(sample(50,0,true)*2-stock)<=4);
 // Speech on the same streaming voice and engines/effects on other voices
 // retain their exact PCM contribution when the music slider is muted.
 assert(sample(0,0,false)==sample(100,0,false));
 assert(sample(0,2,true)==sample(100,2,true));
 const int custom=sample(100,0,false,true);assert(custom>1000);
 assert(sample(0,0,false,true)==0);
 assert(std::abs(sample(50,0,false,true)*2-custom)<=4);
 // Reusing a song ring for another sound removes old classification.
 sample(100,0,true);const uint8_t zero[16]{};
 spu2_sram_write(music_blocks::first,zero,sizeof zero);
 assert(!music_blocks::contains(music_blocks::first));
 std::puts("Music volume real mixer checks passed");
}
