#pragma once

#include "camera.h"
#include <vector>
#include <cstdint>

struct ImDrawList;

namespace eod::editor {

struct ViewportVertex {
    Vector3f position;
    uint32_t color; // ABGR / RGBA
};

struct ViewportLine {
    Vector3f a;
    Vector3f b;
    uint32_t color;
};

class ViewportRenderer {
public:
    ViewportRenderer();
    ~ViewportRenderer() = default;

    void SetGridSize(float size_meters, float step_meters = 1.0f, int major_interval = 5);
    void SetGridVisible(bool visible) { show_grid_ = visible; }
    bool IsGridVisible() const { return show_grid_; }

    void SetAxesVisible(bool visible) { show_axes_ = visible; }
    bool IsAxesVisible() const { return show_axes_; }

    void SetBounds(const Vector3f& min_bounds, const Vector3f& max_bounds);
    void SetBoundsVisible(bool visible) { show_bounds_ = visible; }

    // Primitive submission
    void AddLine(const Vector3f& a, const Vector3f& b, uint32_t color);
    void AddBox(const Vector3f& center, const Vector3f& extents, uint32_t color);
    void AddDiamondSocket(const Vector3f& position, float size, uint32_t color);
    void ClearPrimitives();

    // Render pass
    void Render(ImDrawList* draw_list,
                float screen_x, float screen_y,
                float width, float height,
                const EditorCamera& camera);

private:
    void BuildGridLines();
    bool ProjectPoint(const Vector3f& world_pt,
                      const Matrix4x4f& view,
                      const Matrix4x4f& proj,
                      float screen_x, float screen_y,
                      float width, float height,
                      float& out_sx, float& out_sy) const;

    bool show_grid_ = true;
    bool show_axes_ = true;
    bool show_bounds_ = false;

    float grid_size_ = 40.0f;
    float grid_step_ = 1.0f;
    int major_interval_ = 5;

    Vector3f bounds_min_{-5.0f, 0.0f, -5.0f};
    Vector3f bounds_max_{5.0f, 2.0f, 5.0f};

    std::vector<ViewportLine> grid_lines_;
    std::vector<ViewportLine> custom_lines_;
};

} // namespace eod::editor
