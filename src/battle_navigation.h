#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace battle_navigation {
struct Point { float x=0,z=0; };
// Reader is read-only. Production uses EE RAM; fixtures supply isolated memory.
struct Reader { void *context; uint32_t (*word)(void *,uint32_t); };
class Navigation {
public:
    void reset();
    bool refresh(Reader reader);
    // Returns false when direct pursuit should supply the steering target.
    bool waypoint(uint32_t slot,uint32_t opponent,Point car,Point heading,
                  float speed,Point target,Point &out);
private:
    struct Node { Point position,tangent; float length=0,start=0; };
    struct Choice { uint32_t opponent=UINT32_MAX; int direction=0; };
    std::vector<Node> nodes_;
    std::array<Choice,4> choices_{};
    uint32_t resource_=0,root_=0,count_=0;
    float length_=0;
};
} // namespace battle_navigation
