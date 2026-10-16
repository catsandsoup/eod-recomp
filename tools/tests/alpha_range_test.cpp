#include "gs/vulkan/alpha_range.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
static void check(bool ok,const char *why) {if(!ok){std::fprintf(stderr,"FAIL %s\n",why);std::exit(1);}}
static unsigned alpha(unsigned v,unsigned t,bool tme,bool tcc,unsigned tfx) {
    if(!tme || !tcc)return v;
    if(tfx==0)return std::min(255u,v*t>>7);
    if(tfx==2)return std::min(255u,v+t);
    return t;
}
static bool pass(unsigned test,unsigned a,unsigned ref) {
    switch(test){case 0:return false;case 1:return true;case 2:return a<ref;case 3:return a<=ref;case 4:return a==ref;case 5:return a>=ref;case 6:return a>ref;default:return a!=ref;}
}
int main() {
    using namespace alpha_order;
    for(unsigned v=0;v<=255;v++) {
        auto r=vertex_range(v,v);
        unsigned converted=unsigned(std::floor((float(v)/255.0f)*255.0f+.0001f));
        check(converted>=r.low && converted<=r.high,"UNORM conversion/truncation included");
    }
    for(unsigned v=0;v<=255;v++)for(unsigned t=0;t<=255;t++)for(unsigned fx=0;fx<4;fx++)for(unsigned tcc=0;tcc<2;tcc++) {
        auto range=source_alpha({v,v},{t,t},true,tcc,fx);
        check(range.low==alpha(v,t,true,tcc,fx) && range.high==range.low,"every TCC/TFX byte pair has exact bound");
    }
    std::mt19937 random(0xA1FAABB);
    for(int n=0;n<100000;n++) {
        unsigned v0=random()%256,v1=random()%256,t0=random()%256,t1=random()%256;
        Range v{std::min(v0,v1),std::max(v0,v1)},t{std::min(t0,t1),std::max(t0,t1)};
        bool tme=random()%2,tcc=random()%2;unsigned fx=random()%4,test=random()%8,ref=random()%256;
        auto r=source_alpha(v,t,tme,tcc,fx);
        for(int k=0;k<8;k++) {
            unsigned va=v.low+random()%(v.high-v.low+1),ta=t.low+random()%(t.high-t.low+1),a=alpha(va,ta,tme,tcc,fx);
            check(a>=r.low && a<=r.high,"all interpolated alpha values bounded");
            if(all_pass(test,ref,r))check(pass(test,a,ref),"all-pass never accepts failing fragment");
            if(all_fail(test,ref,r))check(!pass(test,a,ref),"all-fail never rejects passing fragment");
        }
    }
    check(all_pass(5,96,source_alpha(vertex_range(128,128),{128,128},true,true,0)),"opaque MODULATE pass proven");
    check(!all_pass(5,96,source_alpha(vertex_range(128,128),{0,128},true,true,0)),"transparent texture keeps fail pass");
    check(all_pass(5,96,source_alpha(vertex_range(128,128),{},true,false,0)),"TCC0 ignores unknown texture alpha");
    check(!all_pass(4,128,vertex_range(128,128)),"interpolation guard rejects exact equality assumption");
    puts("524288 exact TCC/TFX pairs and 100000 alpha interval proofs passed");
}
