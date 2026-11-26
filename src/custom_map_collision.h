#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace eod::physics {
using MapPoint = std::array<float, 3>;

struct MapContact {
    MapPoint point{}, normal{};
    float fraction{}, penetration{};
};

// Mesh coordinates and all queries are in guest world metres (Y up).
class CustomMapCollision {
  public:
    bool build(const std::vector<MapPoint>& vertices, const std::vector<uint32_t>& indices);

    // Highest triangle beneath this point in the specified vertical interval.
    bool ground(const MapPoint& top, float bottom, MapContact& hit) const;

    // Translate an oriented chassis box from start to end, preserving its axes.
    // A starting overlap reports the minimum translation required for separation.
    bool sweep_box(const MapPoint& start, const MapPoint& end, const std::array<MapPoint, 3>& axes,
                   const MapPoint& half, MapContact& hit) const;

  private:
    struct Triangle {
        MapPoint vertices[3], normal;
        uint32_t ordinal;
    };
    struct Node {
        MapPoint low, high;
        uint32_t first, count, right;
    };
    std::vector<Triangle> triangles_;
    std::vector<Node> nodes_;
    uint32_t partition(uint32_t first, uint32_t count);
    template <class Visit> void query(const MapPoint& low, const MapPoint& high, Visit&& visit) const;
};
} // namespace eod::physics
