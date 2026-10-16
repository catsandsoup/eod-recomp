#pragma once
#include <cstdint>
#include <string>

// Existing values are stable: 0 original 4:3, 1 16:9, 2 stretch; 3 adds 16:10.
inline int display_aspect(const std::string &aspect) {
    return aspect == "16:9" ? 1 : aspect == "stretch" ? 2 : aspect == "16:10" ? 3 : 0;
}
struct DisplayRect { int x, y, width, height; };
inline DisplayRect display_rect(int aspect, int width, int height) {
    if (width <= 0 || height <= 0) return {0,0,0,0};
    int w=width,h=height;
    if (aspect != 2) {
        int numerator = aspect == 1 || aspect == 3 ? 16 : 4;
        int denominator = aspect == 1 ? 9 : aspect == 3 ? 10 : 3;
        h=(int)((int64_t)width*denominator/numerator);
        if(h>height) { h=height;w=(int)((int64_t)height*numerator/denominator); }
    }
    return {(width-w)/2,(height-h)/2,w,h};
}
