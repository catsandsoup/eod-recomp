#include "viewport_renderer.h"
#include "../../third_party/imgui/imgui.h"
#include <cmath>

namespace eod::editor {

ViewportRenderer::ViewportRenderer() {
    BuildGridLines();
}

void ViewportRenderer::SetGridSize(float size_meters, float step_meters, int major_interval) {
    grid_size_ = size_meters;
    grid_step_ = step_meters;
    major_interval_ = major_interval;
    BuildGridLines();
}

void ViewportRenderer::SetBounds(const Vector3f& min_bounds, const Vector3f& max_bounds) {
    bounds_min_ = min_bounds;
    bounds_max_ = max_bounds;
}

void ViewportRenderer::BuildGridLines() {
    grid_lines_.clear();

    float half = grid_size_ * 0.5f;
    int steps = static_cast<int>(grid_size_ / grid_step_);

    for (int i = -steps / 2; i <= steps / 2; ++i) {
        float pos = i * grid_step_;
        bool is_major = (i % major_interval_ == 0);
        bool is_axis = (i == 0);

        uint32_t color;
        if (is_axis) {
            color = 0x88666666; // subtle center line
        } else if (is_major) {
            color = 0x55888888; // major grid line
        } else {
            color = 0x25666666; // minor grid line
        }

        // Lines parallel to Z (varying X)
        grid_lines_.push_back({{pos, 0.0f, -half}, {pos, 0.0f, half}, color});

        // Lines parallel to X (varying Z)
        grid_lines_.push_back({{-half, 0.0f, pos}, {half, 0.0f, pos}, color});
    }
}

void ViewportRenderer::AddLine(const Vector3f& a, const Vector3f& b, uint32_t color) {
    custom_lines_.push_back({a, b, color});
}

void ViewportRenderer::AddBox(const Vector3f& center, const Vector3f& extents, uint32_t color) {
    Vector3f min_pt = center - extents;
    Vector3f max_pt = center + extents;

    // 12 edges of a box
    Vector3f v[8] = {
        {min_pt.x, min_pt.y, min_pt.z},
        {max_pt.x, min_pt.y, min_pt.z},
        {max_pt.x, min_pt.y, max_pt.z},
        {min_pt.x, min_pt.y, max_pt.z},
        {min_pt.x, max_pt.y, min_pt.z},
        {max_pt.x, max_pt.y, min_pt.z},
        {max_pt.x, max_pt.y, max_pt.z},
        {min_pt.x, max_pt.y, max_pt.z}
    };

    // Bottom
    AddLine(v[0], v[1], color); AddLine(v[1], v[2], color);
    AddLine(v[2], v[3], color); AddLine(v[3], v[0], color);
    // Top
    AddLine(v[4], v[5], color); AddLine(v[5], v[6], color);
    AddLine(v[6], v[7], color); AddLine(v[7], v[4], color);
    // Pillars
    AddLine(v[0], v[4], color); AddLine(v[1], v[5], color);
    AddLine(v[2], v[6], color); AddLine(v[3], v[7], color);
}

void ViewportRenderer::AddDiamondSocket(const Vector3f& position, float size, uint32_t color) {
    Vector3f top = position + Vector3f{0.0f, size, 0.0f};
    Vector3f bot = position - Vector3f{0.0f, size, 0.0f};
    Vector3f r = position + Vector3f{size * 0.7f, 0.0f, 0.0f};
    Vector3f l = position - Vector3f{size * 0.7f, 0.0f, 0.0f};
    Vector3f f = position + Vector3f{0.0f, 0.0f, size * 0.7f};
    Vector3f b = position - Vector3f{0.0f, 0.0f, size * 0.7f};

    // Top pyramid
    AddLine(top, r, color); AddLine(top, l, color);
    AddLine(top, f, color); AddLine(top, b, color);
    // Bottom pyramid
    AddLine(bot, r, color); AddLine(bot, l, color);
    AddLine(bot, f, color); AddLine(bot, b, color);
    // Equator
    AddLine(r, f, color); AddLine(f, l, color);
    AddLine(l, b, color); AddLine(b, r, color);
}

void ViewportRenderer::ClearPrimitives() {
    custom_lines_.clear();
}

bool ViewportRenderer::ProjectPoint(const Vector3f& world_pt,
                                   const Matrix4x4f& view,
                                   const Matrix4x4f& proj,
                                   float screen_x, float screen_y,
                                   float width, float height,
                                   float& out_sx, float& out_sy) const {
    // 1. Transform to view space
    float vx = view.m[0][0] * world_pt.x + view.m[0][1] * world_pt.y + view.m[0][2] * world_pt.z + view.m[0][3];
    float vy = view.m[1][0] * world_pt.x + view.m[1][1] * world_pt.y + view.m[1][2] * world_pt.z + view.m[1][3];
    float vz = view.m[2][0] * world_pt.x + view.m[2][1] * world_pt.y + view.m[2][2] * world_pt.z + view.m[2][3];
    float vw = 1.0f;

    // 2. Transform to clip space
    float cx = proj.m[0][0] * vx + proj.m[0][1] * vy + proj.m[0][2] * vz + proj.m[0][3] * vw;
    float cy = proj.m[1][0] * vx + proj.m[1][1] * vy + proj.m[1][2] * vz + proj.m[1][3] * vw;
    float cz = proj.m[2][0] * vx + proj.m[2][1] * vy + proj.m[2][2] * vz + proj.m[2][3] * vw;
    float cw = proj.m[3][0] * vx + proj.m[3][1] * vy + proj.m[3][2] * vz + proj.m[3][3] * vw;

    if (cw <= 0.001f) {
        return false; // Behind camera near plane
    }

    // 3. Perspective divide to NDC [-1, 1]
    float inv_w = 1.0f / cw;
    float ndc_x = cx * inv_w;
    float ndc_y = cy * inv_w;

    // 4. Map NDC to screen coordinates
    out_sx = screen_x + (ndc_x * 0.5f + 0.5f) * width;
    out_sy = screen_y + (-ndc_y * 0.5f + 0.5f) * height; // Y inverted for screen

    return true;
}

void ViewportRenderer::Render(ImDrawList* draw_list,
                              float screen_x, float screen_y,
                              float width, float height,
                              const EditorCamera& camera) {
    if (!draw_list || width <= 0.0f || height <= 0.0f) return;

    Matrix4x4f view = camera.GetViewMatrix();
    Matrix4x4f proj = camera.GetProjectionMatrix(width / height);

    // Draw grid
    if (show_grid_) {
        for (const auto& line : grid_lines_) {
            float sx0, sy0, sx1, sy1;
            if (ProjectPoint(line.a, view, proj, screen_x, screen_y, width, height, sx0, sy0) &&
                ProjectPoint(line.b, view, proj, screen_x, screen_y, width, height, sx1, sy1)) {
                draw_list->AddLine(ImVec2(sx0, sy0), ImVec2(sx1, sy1), line.color, 1.0f);
            }
        }
    }

    // Draw coordinate axes (+X Red, +Y Green, +Z Blue)
    if (show_axes_) {
        float axis_len = 2.0f;
        Vector3f origin{0.0f, 0.0f, 0.0f};
        Vector3f axis_x{axis_len, 0.0f, 0.0f};
        Vector3f axis_y{0.0f, axis_len, 0.0f};
        Vector3f axis_z{0.0f, 0.0f, axis_len};

        float sox, soy, sxx, sxy, syx, syy, szx, szy;
        if (ProjectPoint(origin, view, proj, screen_x, screen_y, width, height, sox, soy)) {
            // X Axis - Red
            if (ProjectPoint(axis_x, view, proj, screen_x, screen_y, width, height, sxx, sxy)) {
                draw_list->AddLine(ImVec2(sox, soy), ImVec2(sxx, sxy), 0xFF3333E5, 2.5f);
            }
            // Y Axis - Green
            if (ProjectPoint(axis_y, view, proj, screen_x, screen_y, width, height, syx, syy)) {
                draw_list->AddLine(ImVec2(sox, soy), ImVec2(syx, syy), 0xFF33E533, 2.5f);
            }
            // Z Axis - Blue
            if (ProjectPoint(axis_z, view, proj, screen_x, screen_y, width, height, szx, szy)) {
                draw_list->AddLine(ImVec2(sox, soy), ImVec2(szx, szy), 0xFFE53333, 2.5f);
            }
        }
    }

    // Draw custom lines (sockets, bounding boxes, wireframes)
    for (const auto& line : custom_lines_) {
        float sx0, sy0, sx1, sy1;
        if (ProjectPoint(line.a, view, proj, screen_x, screen_y, width, height, sx0, sy0) &&
            ProjectPoint(line.b, view, proj, screen_x, screen_y, width, height, sx1, sy1)) {
            draw_list->AddLine(ImVec2(sx0, sy0), ImVec2(sx1, sy1), line.color, 1.5f);
        }
    }
}

} // namespace eod::editor
