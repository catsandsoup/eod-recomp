#include "battle_navigation.h"
#include <cstdlib>
#include <limits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {
struct World {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x400000);
    static uint32_t read(void *ctx, uint32_t at) {
        auto &w = *static_cast<World *>(ctx);
        uint32_t v = 0;
        if (at <= w.bytes.size() - 4) std::memcpy(&v, &w.bytes[at], 4);
        return v;
    }
    void word(uint32_t at, uint32_t v) { std::memcpy(&bytes[at], &v, 4); }
    void number(uint32_t at, float f) { uint32_t v; std::memcpy(&v, &f, 4); word(at, v); }
    battle_navigation::Reader reader() { return {this, read}; }
    World() {
        // Four 100m sides; clockwise in X/Z. Header count is HIGH halfword.
        word(0x3CD1B4, 1); word(0x3CD190, 0x10000);
        word(0x3CD194, 0x101); word(0x3CD198, 4);
        word(0x10000, 0x104F128B); word(0x10004, 48); word(0x10008, 60);
        word(0x1001C, 4u << 16); word(0x10020, 0x11000);
        const float x[] = {0,100,100,0}, z[] = {0,0,100,100};
        for (uint32_t i=0;i<4;++i) {
            uint32_t p=0x11000+i*60,j=(i+1)%4;
            number(p,x[i]);number(p+4,z[i]);
            number(p+8,(x[j]-x[i])/100);number(p+12,(z[j]-z[i])/100);
            // Metadata deliberately isn't a previous pointer.
            word(p+16, i << 16); word(p+20,0x11000+j*60);number(p+24,100);
        }
    }
};
void check(bool b) { if (!b) std::abort(); }
}
int main() {
    using namespace battle_navigation;
    World w; Navigation nav;
    check(nav.refresh(w.reader()));
    Point out;
    // Short arc crosses the last->first node, in reverse from the first side.
    check(nav.waypoint(0,1,{30,0},{1,0},10,{0,40},out));
    check(out.x < 30 && std::fabs(out.z)<.01f);
    // Opposite trip follows the forward arc and projects onto a segment.
    check(nav.waypoint(1,0,{0,40},{0,-1},10,{30,0},out));
    check(out.z < 40 && std::fabs(out.x)<.01f);
    // Nearby opponents on the same section retain direct interception.
    check(!nav.waypoint(2,0,{30,0},{1,0},10,{40,0},out));
    // Off-course departure is outside the supported routing contract.
    check(!nav.waypoint(2,0,{50,50},{1,0},10,{100,70},out));
    check(!nav.waypoint(2,0,{30,0},{1,0},10,{150,70},out));
    // A near-equal target movement must not flip a retained direction.
    check(nav.waypoint(2,1,{0,0},{1,0},10,{100,95},out));
    check(out.x>0);
    check(nav.waypoint(2,1,{0,0},{1,0},10,{95,100},out));
    check(out.x>0);
    // Changing opponent clears that hysteresis and chooses the shorter arc.
    check(nav.waypoint(2,3,{0,0},{1,0},10,{95,100},out));
    check(out.z>0);
    // World-near opponents across a figure-eight crossing must not cause an
    // immediate direct turn across the unrelated course segment.
    World crossing;
    const float cx[]={-100,100,-100,100},cz[]={-100,100,100,-100};
    for(uint32_t i=0;i<4;++i) {
        uint32_t p=0x11000+i*60,j=(i+1)%4;
        float length=std::hypot(cx[j]-cx[i],cz[j]-cz[i]);
        crossing.number(p,cx[i]);crossing.number(p+4,cz[i]);
        crossing.number(p+8,(cx[j]-cx[i])/length);crossing.number(p+12,(cz[j]-cz[i])/length);
        crossing.number(p+24,length);
    }
    Navigation eight;check(eight.refresh(crossing.reader()));
    check(eight.waypoint(0,1,{-5,-5},{.7071f,.7071f},8,{-5,5},out));
    // Native unrelocated headers and inactive/negative ownership cannot be used.
    // Cache disappearance must discard the formerly valid snapshot.
    w.word(0x3CD1B4,0);check(!nav.refresh(w.reader()));
    check(!nav.waypoint(0,1,{30,0},{1,0},10,{0,40},out));
    for (int failure=0; failure<13; ++failure) {
        World bad;
        switch(failure) {
        case 0: bad.word(0x1001C,1u<<16);break;
        case 1: bad.word(0x1001C,4097u<<16);break;
        case 2: bad.word(0x10020,0x1FFFFE0);break;
        case 3: bad.word(0x11014,0x123456);break;
        case 4: bad.word(0x11000,0x7FC00000);break;
        case 5: bad.word(0x3CD198,0);break;
        case 6: bad.word(0x3CD194,0x201);break;
        case 7: bad.word(0x10000,0x104F128A);break;
        case 8: bad.word(0x10008,64);break;
        case 9: bad.word(0x3CD198,UINT32_MAX);break;
        case 10: bad.word(0x10004,44);break;
        case 11: bad.word(0x11018,0);break;
        case 12: bad.word(0x3CD1B4,4);break;
        }
        Navigation reject;check(!reject.refresh(bad.reader()));
    }
    // New identity replaces a valid snapshot and its retained direction.
    World replacement;Navigation changed;check(changed.refresh(replacement.reader()));
    replacement.word(0x10020,3);check(!changed.refresh(replacement.reader()));
    // Nonfinite caller coordinates never produce native control values.
    World finite; check(nav.refresh(finite.reader()));
    check(!nav.waypoint(0,1,{std::numeric_limits<float>::quiet_NaN(),0},{1,0},10,{0,40},out));
    nav.reset();check(!nav.waypoint(0,1,{30,0},{1,0},10,{0,40},out));
}
