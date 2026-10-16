// Offline end-to-end SPU2 AutoDMA -> SDL stream. No audio device is opened.
// The output stream is unbound: samples are retrieved into memory only.
#include <SDL3/SDL.h>
#include "../runtime/platform/sdl3/audio_gain.h"
#include "../runtime/iop/spu2.cpp"
#include <cmath>
uint8_t *g_iop_ram;
bool g_log_enabled[LOG_CAT_COUNT] = {};
void log_write(LogCat, const char *, ...) {}
uint64_t kernel_vblank_count() { return 0; }

static uint64_t fake_now;
static SDL_AudioStream *stream;
uint64_t platform_time_us() { return fake_now; }
bool platform_audio_open(int) { return false; } // no physical audio device
int platform_audio_queued_frames() { return SDL_GetAudioStreamQueued(stream)/4; }
void platform_audio_submit(const int16_t *samples, int frames) { SDL_PutAudioStreamData(stream,samples,frames*4); }

static int render(int volume) {
    g_config.volume=volume; g_config.audio=true;
    SDL_AudioSpec format{SDL_AUDIO_S16LE,2,48000};
    stream=SDL_CreateAudioStream(&format,&format);
    if (!stream) { fprintf(stderr,"offline stream: %s\n",SDL_GetError()); return -1; }
    SDL_SetAudioStreamGain(stream,sdl_output_gain(g_config));
    spu2_init();
    spu2_set_param(0x0980,0x3fff); // master left/right
    spu2_set_param(0x0a80,0x3fff);
    spu2_block_trans(0,0x10,0,1024,0); // looping planar stereo PCM
    g_t0=1000; g_produced=0; g_audio_open=true; fake_now=3000;
    spu2_update();
    int16_t samples[192] = {};
    int bytes=SDL_GetAudioStreamData(stream,samples,sizeof(samples));
    SDL_DestroyAudioStream(stream); stream=nullptr;
    if (bytes != sizeof(samples)) { fprintf(stderr,"expected192 samples, got%d bytes\n",bytes); return -1; }
    if (samples[0] != -samples[1] && std::abs(samples[0]+samples[1])>2) return -1;
    return samples[0];
}
int main() {
    setenv("EOD_HEADLESS","1",1); setenv("EOD_MUTE","1",1);
    // SDL streams run offline; no SDL audio subsystem or device is started.
    std::vector<uint8_t> ram(IOP_RAM_SIZE,0); g_iop_ram=ram.data();
    for (int block=0;block<2;block++) for (int i=0;i<128;i++) {
        int16_t left=12000,right=-12000;
        memcpy(g_iop_ram+block*512+i*2,&left,2);
        memcpy(g_iop_ram+block*512+256+i*2,&right,2);
    }
    int full=render(100),half=render(50),zero=render(0);
    printf("offline SPU2/SDL output amplitudes: 100%%=%d 50%%=%d 0%%=%d\n",full,half,zero);
    if (full<11000 || std::abs(half*2-full)>2 || zero!=0) return 1;
    g_config.audio=false;
    if(sdl_output_gain(g_config)!=0) return 1;
    return 0;
}
