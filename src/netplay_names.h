#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace netplay::names {

using Point = std::array<float, 3>;

struct Tag {
    std::string name;
    float x, y, distance; // normalized game viewport and camera distance in metres
};

struct Camera {
    Point position, axes[3];
    float horizontal_scale, vertical_scale, width, height;
};

struct TerrainFace {
    Point edges[3]; // coefficients of the three XZ half-planes
    Point normal;
    float offset;
    uint16_t mask;
};

struct Box {
    Point center, axes[3], half_extents;
};

struct Tube {
    Point center, axis;
    float radius, half_length;
};

// The online session installs this read-only listener for the world visibility pass.
inline void (*on_world_view)() = nullptr;
Camera read_world_camera();
Point roof_anchor(uint32_t car);
bool project(const Camera &camera, const Point &world, Tag &out);
bool intersects(const Box &box, const Point &from, const Point &to);
bool intersects(const Tube &tube, const Point &from, const Point &to);
bool intersects(const TerrainFace &face, const Point &from, const Point &to);

// Native SCOL terrain and SOBL collision solids are fixed for the lifetime of the event.
class Occluders {
public:
    void load();
    bool blocked(const Point &from, const Point &to) const;
    std::size_t terrain_count() const { return terrain_.size(); }
    std::size_t box_count() const { return boxes_.size(); }
    std::size_t tube_count() const { return tubes_.size(); }
private:
    std::vector<TerrainFace> terrain_;
    std::vector<Box> boxes_;
    std::vector<Tube> tubes_;
};

inline uint8_t toggle_bit(const std::string &mode) { return mode == "Demolition Derby" ? 64 : 32; }
inline bool enabled(const std::string &mode, uint8_t rules) {
    return (mode != "Demolition Derby") != ((rules & toggle_bit(mode)) != 0);
}

} // namespace netplay::names
