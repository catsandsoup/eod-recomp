// Conservative screen-space proof: primitives touching different physical
// pixel tiles cannot affect each other's color/depth/alpha test results.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
namespace alpha_order {
struct Bounds { int x0,y0,x1,y1; }; // half-open physical pixels after scissor
inline Bounds bounds(float min_x,float min_y,float max_x,float max_y,int scale,Bounds clip) {
    // VS adds half a logical pixel before viewport scaling. Expand one physical
    // pixel for raster precision and aliased point/line edge coverage. Clip
    // before integer conversion so offscreen coordinates cannot overflow.
    if(!std::isfinite(min_x) || !std::isfinite(min_y) || !std::isfinite(max_x) || !std::isfinite(max_y)) return clip;
    auto low=[](double value,int first,int last) { return int(std::floor(std::clamp(value,double(first),double(last)))); };
    auto high=[](double value,int first,int last) { return int(std::ceil(std::clamp(value,double(first),double(last)))); };
    return {low((double(min_x)+.5)*scale-1,clip.x0,clip.x1),low((double(min_y)+.5)*scale-1,clip.y0,clip.y1),
            high((double(max_x)+.5)*scale+1,clip.x0,clip.x1),high((double(max_y)+.5)*scale+1,clip.y0,clip.y1)};
}
class Tiles {
    static constexpr int size=16;
    int columns_,rows_;
    std::vector<uint64_t> occupied_;
    std::vector<size_t> touched_;
    template<class F> void masks(Bounds b,F callback) const {
        if(b.x0>=b.x1 || b.y0>=b.y1) return;
        int x0=std::clamp(b.x0/size,0,columns_-1),x1=std::clamp((b.x1-1)/size,0,columns_-1);
        int y0=std::clamp(b.y0/size,0,rows_-1),y1=std::clamp((b.y1-1)/size,0,rows_-1);
        for(int y=y0;y<=y1;y++) {
            size_t first=size_t(y)*columns_+x0,last=size_t(y)*columns_+x1;
            for(size_t word=first/64;word<=last/64;word++) {
                uint64_t mask=~uint64_t(0);
                if(word==first/64) mask&=~uint64_t(0)<<(first%64);
                if(word==last/64) mask&=~uint64_t(0)>>(63-last%64);
                callback(word,mask);
            }
        }
    }
public:
    Tiles(int width,int height):columns_(std::max(1,(width+size-1)/size)),rows_(std::max(1,(height+size-1)/size)),
        occupied_((size_t(columns_)*rows_+63)/64) {}
    bool append(Bounds b) {
        bool overlap=false;
        masks(b,[&](size_t i,uint64_t mask){overlap=overlap || (occupied_[i]&mask)!=0;});
        if(overlap) return false; // rejected primitive never mutates the proof
        masks(b,[&](size_t i,uint64_t mask){if(!occupied_[i]) touched_.push_back(i);occupied_[i]|=mask;});
        return true;
    }
    void clear() { for(auto i:touched_) occupied_[i]=0;touched_.clear(); }
};
}
