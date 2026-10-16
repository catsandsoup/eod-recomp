#pragma once
#include <cstdlib>
#include <cstdint>
struct ScreenshotSize { int width, height; bool composed, valid; };
inline ScreenshotSize screenshot_size(int source_width,int source_height,int scale,
                                      const char *output_width,const char *output_height) {
    if(output_width || output_height) {
        auto dimension=[](const char *value)->int {
            if(!value || !*value) return 0;
            char *end=nullptr;long number=std::strtol(value,&end,10);
            return *end || number<1 || number>8192 ? 0 : (int)number;
        };
        int width=dimension(output_width),height=dimension(output_height);
        return {width,height,true,width>0 && height>0};
    }
    int64_t width=(int64_t)source_width*scale,height=(int64_t)source_height*scale;
    bool valid=source_width>0 && source_height>0 && scale>0 && width<=16384 && height<=16384;
    return {valid?(int)width:0,valid?(int)height:0,false,valid};
}
