#include "gs/vulkan/alpha_order.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
using alpha_order::Test;
struct Pixel { int color; uint32_t z; };
struct Fragment { int color,alpha; uint32_t z; bool covered; };
static bool zpass(Test test,uint32_t z,uint32_t dst) {
    return test==Test::Always || (test==Test::GreaterEqual ? z>=dst : z>dst);
}
static int blend(int src,int dst,int alpha) { return std::clamp(((src-dst)*alpha>>7)+dst,0,255); }
static bool alpha_pass(int test,int a,int ref) {
    switch(test) {
    case 0:return false;case 1:return true;case 2:return a<ref;case 3:return a<=ref;
    case 4:return a==ref;case 5:return a>=ref;case 6:return a>ref;default:return a!=ref;
    }
}
static Pixel native(Pixel p,const std::vector<Fragment> &frags,Test test,int atst,int ref) {
    for(auto f:frags) if(f.covered && zpass(test,f.z,p.z)) {
        p.color=blend(f.color,p.color,f.alpha);
        if(alpha_pass(atst,f.alpha,ref)) p.z=f.z;
    }
    return p;
}
static Pixel deferred(Pixel p,const std::vector<Fragment> &frags,Test test,int atst,int ref) {
    for(auto f:frags) if(f.covered && zpass(test,f.z,p.z)) p.color=blend(f.color,p.color,f.alpha);
    for(auto f:frags) if(f.covered && zpass(test,f.z,p.z) && alpha_pass(atst,f.alpha,ref)) p.z=f.z;
    return p;
}
static void check(bool ok,const char *why) { if(!ok){std::fprintf(stderr,"FAIL %s\n",why);std::exit(1);} }
int main() {
    std::mt19937 random(0xAFFA110);
    for(int iteration=0;iteration<50000;iteration++) {
        auto test=iteration%2 ? Test::GreaterEqual : Test::Always;
        uint32_t z=random();if(iteration%3==0) z&=65535;
        Pixel initial{int(random()%256),iteration%3==0 ? z : random()};
        int atst=random()%8,ref=random()%256;
        std::vector<Fragment> frags;
        for(unsigned n=0,count=1+random()%32;n<count;n++) frags.push_back({int(random()%256),int(random()%256),z,bool(random()%2)});
        check(alpha_order::defer_depth(true,true,test,z,z),"eligible constant-Z FB_ONLY path");
        auto expected=native(initial,frags,test,atst,ref),actual=deferred(initial,frags,test,atst,ref);
        check(expected.color==actual.color && expected.z==actual.z,"deferred color/depth equals native primitive order");
    }
    std::vector<Fragment> strict={{200,128,1,true},{50,128,1,true}};
    auto a=native({0,0},strict,Test::Greater,5,96),b=deferred({0,0},strict,Test::Greater,5,96);
    check(a.color!=b.color && !alpha_order::defer_depth(true,true,Test::Greater,1,1),"strict greater counterexample falls back");
    std::vector<Fragment> varying={{200,128,2,true},{50,128,1,true}};
    a=native({0,0},varying,Test::GreaterEqual,5,96);b=deferred({0,0},varying,Test::GreaterEqual,5,96);
    check(a.color!=b.color && !alpha_order::defer_depth(true,true,Test::GreaterEqual,1,2),"varying-Z counterexample falls back");
    check(!alpha_order::defer_depth(false,true,Test::Always,1,1),"other alpha-fail modes excluded");
    check(!alpha_order::defer_depth(true,false,Test::Always,1,1),"no depth writes excluded");
    std::puts("50000 FB_ONLY color+depth differentials passed; strict/varying-depth counterexamples rejected");
}
