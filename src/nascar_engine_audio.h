// Source ESFX mixer core. No EE/native hooks are installed by this header.
// Source SLUS_204.41:1EE9E8 count/stride,1EEEC8 gates/gain,1EF3F8 curve.
// Keep the audited native voice backend; its smoothstep preset mixer is different.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
#include <utility>
namespace nascar_audio {
struct Record {
    std::array<char,16> sample{};
    float sample_rpm{},gain_low{},gain_high{},outer_low{},inner_low{},inner_high{},outer_high{};
};
using Profile=std::vector<Record>;
struct Voice {bool active;float pitch,volume;};
inline uint32_t u32(const unsigned char*p){
    return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
}
inline float f32(const unsigned char*p){auto bits=u32(p);float f;std::memcpy(&f,&bits,4);return f;}
inline bool parse(const unsigned char*data,size_t size,Profile&out){
    if(size<4 || !data)return false;
    const auto count=u32(data);
    if(count>8 || size<4+size_t(count)*44)return false;
    Profile result;
    for(uint32_t i=0;i<count;++i){
        const auto*p=data+4+i*44;Record r;
        std::memcpy(r.sample.data(),p,16);
        const auto end=std::find(r.sample.begin(),r.sample.end(),'\0');
        const auto len=size_t(end-r.sample.begin());
        if(end==r.sample.end() || len<5 || std::memcmp(r.sample.data()+len-4,".sfx",4))return false;
        for(size_t n=0;n<len-4;++n){const auto c=r.sample[n];
            if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'))return false;
        }
        std::array<float,7> values{};
        for(unsigned n=0;n<7;++n){values[n]=f32(p+16+4*n);if(!std::isfinite(values[n]))return false;}
        r.sample_rpm=values[0];r.gain_low=values[1];r.gain_high=values[2];
        r.outer_low=values[3];r.inner_low=values[4];r.inner_high=values[5];r.outer_high=values[6];
        if(!(r.sample_rpm>0 && r.outer_low<r.inner_low && r.inner_low<r.inner_high && r.inner_high<r.outer_high))return false;
        result.push_back(r);
    }
    out=std::move(result);return true; // Serialized unused-capacity tail is deliberately ignored.
}
inline Voice sample(const Record&r,float rpm,float gain){
    if(!(r.outer_low<=rpm && rpm<r.outer_high))return {false,0,0};
    const float x=std::clamp((rpm-r.inner_low)/(r.inner_high-r.inner_low),0.f,1.f);
    const float interpolated=r.gain_low+x*(r.gain_high-r.gain_low);
    const float envelope=rpm<r.inner_low ? (rpm-r.outer_low)/(r.inner_low-r.outer_low)
        : rpm>r.inner_high ? (rpm-r.outer_high)/(r.inner_high-r.outer_high) : 1.f;
    const float level=interpolated*envelope*gain;
    return level>0 ? Voice{true,rpm/r.sample_rpm,level*.6f} : Voice{false,0,0};
}
inline float caller_gain(float throttle,float audio_random,bool viewed,bool local_mix){
    // Caller owns a separate source-audio RNG stream. No target RNG calls here.
    const float slope=throttle>.2f ? 1.25f*(throttle-.2f) : 5.f*(.2f-throttle)*audio_random;
    float gain=.09375f+.15625f*slope;
    if(viewed)gain*=local_mix?2.32f:8.f/3.f;
    return gain*2.2f;
}
inline std::vector<Voice> mix(const Profile&p,float rpm,float throttle,float audio_random,
                              bool viewed,bool local_mix,bool silenced){
    const float gain=caller_gain(throttle,audio_random,viewed,local_mix);
    std::vector<Voice> out;out.reserve(p.size());
    for(size_t i=0;i<p.size();++i){
        const bool suppressed=silenced || (p.size()>=2 && ((i==0)==local_mix));
        out.push_back(suppressed?Voice{false,0,0}:sample(p[i],rpm,i<2||throttle>.2f?gain:0.f));
    }
    return out;
}
} // namespace nascar_audio
