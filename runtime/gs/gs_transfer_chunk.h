// Rectangles occupied by pixels actually supplied in one atomic CPU write chunk.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
namespace gs {
inline uint64_t transfer_pixels_supplied(unsigned bits_per_pixel, uint32_t bytes, unsigned pending_bytes) {
    return bits_per_pixel ? (uint64_t(bytes) * 8 + (bits_per_pixel == 24 ? uint64_t(pending_bytes) * 8 : 0)) / bits_per_pixel : 0;
}
struct TransferWriteRect { int x, y, w, h; };
struct TransferWriteRegions { std::array<TransferWriteRect,3> rects{}; unsigned count=0; };
inline TransferWriteRegions transfer_write_regions(int x0,int y0,int width,int height,
                                                   int cursor_x,int cursor_y,uint64_t pixels) {
    TransferWriteRegions out;
    if(width<=0 || height<=0 || cursor_x<0 || cursor_x>=width || cursor_y<0 || cursor_y>=height)return out;
    pixels=std::min(pixels,uint64_t(height-cursor_y)*width-cursor_x);
    if(cursor_x && pixels) {
        int head=int(std::min(pixels,uint64_t(width-cursor_x)));
        out.rects[out.count++]={x0+cursor_x,y0+cursor_y,head,1};
        pixels-=head;
        if(!pixels)return out;
        ++cursor_y;
    }
    if(pixels>=unsigned(width)) {
        int rows=int(pixels/width);
        out.rects[out.count++]={x0,y0+cursor_y,width,rows};
        pixels%=width;cursor_y+=rows;
    }
    if(pixels)out.rects[out.count++]={x0,y0+cursor_y,int(pixels),1};
    return out;
}
} // namespace gs
