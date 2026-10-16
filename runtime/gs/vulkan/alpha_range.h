// Conservative bounds only: no texture, shader or visible pixel is changed.
#pragma once
#include <algorithm>
#include <cstdint>
namespace alpha_order {
struct Range { unsigned low=0,high=255; };
inline Range vertex_range(unsigned low,unsigned high) {
    // Cover UNORM float conversion/interpolation followed by shader truncation.
    // This deliberately rejects narrow threshold/equality cases near a byte.
    return {low ? low-1 : 0,std::min(255u,high+1)};
}
inline Range source_alpha(Range vertex,Range texture,bool texturing,bool tcc,unsigned tfx) {
    if(!texturing || !tcc)return vertex;
    if(tfx==0)return {std::min(255u,vertex.low*texture.low>>7),std::min(255u,vertex.high*texture.high>>7)};
    if(tfx==2)return {std::min(255u,vertex.low+texture.low),std::min(255u,vertex.high+texture.high)};
    return texture; // DECAL and HIGHLIGHT2
}
inline bool all_pass(unsigned test,unsigned ref,Range a) {
    switch(test) {
    case 0:return false;case 1:return true;case 2:return a.high<ref;case 3:return a.high<=ref;
    case 4:return a.low==ref && a.high==ref;case 5:return a.low>=ref;case 6:return a.low>ref;
    default:return ref<a.low || ref>a.high;
    }
}
inline bool all_fail(unsigned test,unsigned ref,Range a) {
    constexpr unsigned inverse[8]={1,0,5,6,7,2,3,4};
    return all_pass(inverse[test&7],ref,a);
}
}
