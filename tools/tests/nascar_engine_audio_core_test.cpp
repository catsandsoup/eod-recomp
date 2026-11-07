#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>
#include "nascar_engine_audio.h"
int main(int argc,char**argv){
    if(argc!=2)return 1;
    std::ifstream f(argv[1],std::ios::binary);
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(f)),{});
    nascar_audio::Profile p;
    if(!nascar_audio::parse(data.data(),data.size(),p))return 2;
    const float rpms[]={99,100,825,3000,4275,6400,6750,7000,7806,8100,44050,80000};
    for(float rpm:rpms)for(float throttle:{0.f,.2f,.8f,1.f})for(int focus=0;focus<3;++focus){
        auto voices=nascar_audio::mix(p,rpm,throttle,.3f,focus!=0,focus==2,false);
        for(unsigned i=0;i<voices.size();++i){const auto&v=voices[i];
            std::printf("%.0f %.2f %d %u %d %.9g %.9g\n",rpm,throttle,focus,i,int(v.active),v.pitch,v.volume);
        }
    }
    for(unsigned n=0;n<4;++n)if(nascar_audio::parse(data.data(),n,p))return 3;
    data[0]=9;if(nascar_audio::parse(data.data(),data.size(),p))return 4;
    return 0;
}
