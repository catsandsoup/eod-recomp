#include "custom_map_collision.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace eod::physics {
namespace {
using P = MapPoint;
bool finite(P v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}
P sub(P a, P b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
P scaled(P a, float k) {
    return {a[0] * k, a[1] * k, a[2] * k};
}
P cross(P a, P b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
float dot(P a, P b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
float radius(P direction, const std::array<P, 3>& axes, P half) {
    return std::abs(dot(direction, axes[0])) * half[0] + std::abs(dot(direction, axes[1])) * half[1] +
           std::abs(dot(direction, axes[2])) * half[2];
}

bool box_face(const P* vertices, P normal, P start, P end, const std::array<P, 3>& axes, P half,
              MapContact& hit) {
    // Continuous separating-axis test: intersect the overlap time intervals on
    // the triangle normal, all three box normals, and all nine edge cross axes.
    const P move = sub(end, start);
    float enter = -std::numeric_limits<float>::infinity(), exit = 1.f;
    float shallowest = std::numeric_limits<float>::infinity();
    P entry_normal{}, overlap_normal{};
    auto interval = [&](P axis) {
        const float n2 = dot(axis, axis);
        if (n2 < 1e-12f) return true;
        axis = scaled(axis, 1.f / std::sqrt(n2));
        const float r = radius(axis, axes, half);
        const float a = dot(sub(vertices[0], start), axis);
        const float b = dot(sub(vertices[1], start), axis);
        const float c = dot(sub(vertices[2], start), axis);
        const float lo = std::min({a, b, c}) - r, hi = std::max({a, b, c}) + r;
        const float speed = dot(move, axis);
        const float depth = std::min(-lo, hi);
        if (depth < shallowest) {
            shallowest = depth;
            overlap_normal = scaled(axis, -lo < hi ? -1.f : 1.f);
        }
        if (std::abs(speed) < 1e-9f) return lo <= 0 && hi >= 0;
        float begin = lo / speed, finish = hi / speed;
        if (begin > finish) std::swap(begin, finish);
        if (begin > enter) {
            enter = begin;
            entry_normal = scaled(axis, speed > 0 ? -1.f : 1.f);
        }
        exit = std::min(exit, finish);
        return enter <= exit && exit >= 0 && enter <= 1;
    };
    if (!interval(normal)) return false;
    for (const P& axis : axes)
        if (!interval(axis)) return false;
    for (unsigned i = 0; i < 3; ++i) {
        const P edge = sub(vertices[(i + 1) % 3], vertices[i]);
        for (const P& axis : axes)
            if (!interval(cross(edge, axis))) return false;
    }
    hit.fraction = std::max(0.f, enter);
    hit.penetration = std::max(0.f, shallowest);
    hit.normal = enter < 0 ? overlap_normal : entry_normal;
    if (hit.penetration <= 1e-5f && dot(move, hit.normal) >= 0) return false;
    // Centre of the box support face/edge/vertex facing the contacted feature.
    for (unsigned i = 0; i < 3; ++i)
        hit.point[i] = start[i] + move[i] * hit.fraction;
    for (unsigned i = 0; i < 3; ++i) {
        const float projection = dot(axes[i], hit.normal);
        if (std::abs(projection) < 1e-5f) continue;
        const float offset = std::copysign(half[i], projection);
        for (unsigned d = 0; d < 3; ++d)
            hit.point[d] -= axes[i][d] * offset;
    }
    return true;
}
} // namespace

bool CustomMapCollision::build(const std::vector<MapPoint>& vertices, const std::vector<uint32_t>& indices) {
    if (indices.empty() || indices.size() % 3 || indices.size() / 3 >= UINT32_MAX / 2) return false;
    if (!std::all_of(vertices.begin(), vertices.end(), finite)) return false;
    CustomMapCollision next;
    next.triangles_.reserve(indices.size() / 3);
    for (size_t i = 0; i < indices.size(); i += 3) {
        if (indices[i] >= vertices.size() || indices[i + 1] >= vertices.size() ||
            indices[i + 2] >= vertices.size())
            return false;
        Triangle t{
            {vertices[indices[i]], vertices[indices[i + 1]], vertices[indices[i + 2]]}, {}, uint32_t(i / 3)};
        t.normal = cross(sub(t.vertices[1], t.vertices[0]), sub(t.vertices[2], t.vertices[0]));
        const float n = std::sqrt(dot(t.normal, t.normal));
        if (!std::isfinite(n)) return false;
        if (n <= 1e-9f) continue;
        t.normal = scaled(t.normal, 1.f / n);
        next.triangles_.push_back(t);
    }
    if (next.triangles_.empty()) return false;
    next.partition(0, static_cast<uint32_t>(next.triangles_.size()));
    *this = std::move(next);
    return true;
}

uint32_t CustomMapCollision::partition(uint32_t first, uint32_t count) {
    Node node{triangles_[first].vertices[0], triangles_[first].vertices[0], first, count, 0};
    for (uint32_t i = first; i < first + count; ++i)
        for (const P& v : triangles_[i].vertices)
            for (unsigned d = 0; d < 3; ++d) {
                node.low[d] = std::min(node.low[d], v[d]);
                node.high[d] = std::max(node.high[d], v[d]);
            }
    const uint32_t index = static_cast<uint32_t>(nodes_.size());
    nodes_.push_back(node);
    if (count <= 8) return index;
    const P span = sub(node.high, node.low);
    const size_t axis = std::max_element(span.begin(), span.end()) - span.begin();
    auto centroid = [axis](const Triangle& t) {
        return t.vertices[0][axis] + t.vertices[1][axis] + t.vertices[2][axis];
    };
    auto begin = triangles_.begin() + first;
    std::nth_element(begin, begin + count / 2, begin + count, [&](const Triangle& a, const Triangle& b) {
        const float x = centroid(a), y = centroid(b);
        return x == y ? a.ordinal < b.ordinal : x < y;
    });
    nodes_[index].count = 0;
    partition(first, count / 2); // left child immediately follows its parent
    const uint32_t right = partition(first + count / 2, count - count / 2);
    nodes_[index].right = right;
    return index;
}

template <class Visit> void CustomMapCollision::query(const P& low, const P& high, Visit&& visit) const {
    if (nodes_.empty() || !finite(low) || !finite(high)) return;
    // Median splitting with 32-bit triangle counts bounds tree depth to 32.
    uint32_t stack[33] = {0}, size = 1;
    while (size) {
        const uint32_t i = stack[--size];
        const Node& n = nodes_[i];
        if (low[0] > n.high[0] || low[1] > n.high[1] || low[2] > n.high[2] || high[0] < n.low[0] ||
            high[1] < n.low[1] || high[2] < n.low[2])
            continue;
        if (!n.count) {
            stack[size++] = n.right;
            stack[size++] = i + 1;
            continue;
        }
        for (uint32_t j = n.first; j < n.first + n.count; ++j)
            visit(triangles_[j]);
    }
}

bool CustomMapCollision::ground(const P& top, float bottom, MapContact& hit) const {
    if (bottom > top[1]) return false;
    const P low{top[0], bottom, top[2]};
    uint32_t best = UINT32_MAX;
    query(low, top, [&](const Triangle& t) {
        P normal = t.normal;
        if (normal[1] < 0) normal = scaled(normal, -1.f);
        if (normal[1] < .05f) return;
        const P& a = t.vertices[0];
        const float y = a[1] - ((top[0] - a[0]) * normal[0] + (top[2] - a[2]) * normal[2]) / normal[1];
        if (y < bottom || y > top[1] ||
            (best != UINT32_MAX && (y < hit.point[1] || (y == hit.point[1] && best < t.ordinal))))
            return;
        const P p{top[0], y, top[2]};
        for (unsigned i = 0; i < 3; ++i) {
            P edge = sub(t.vertices[(i + 1) % 3], t.vertices[i]);
            if (dot(cross(edge, sub(p, t.vertices[i])), t.normal) < -1e-5f) return;
        }
        best = t.ordinal;
        hit = {p, normal, top[1] > bottom ? (top[1] - y) / (top[1] - bottom) : 0.f, 0};
    });
    return best != UINT32_MAX;
}

bool CustomMapCollision::sweep_box(const P& start, const P& end, const std::array<P, 3>& axes, const P& half,
                                   MapContact& hit) const {
    P low, high;
    for (unsigned d = 0; d < 3; ++d) {
        const float r =
            std::abs(axes[0][d]) * half[0] + std::abs(axes[1][d]) * half[1] + std::abs(axes[2][d]) * half[2];
        low[d] = std::min(start[d], end[d]) - r;
        high[d] = std::max(start[d], end[d]) + r;
    }
    uint32_t best = UINT32_MAX;
    query(low, high, [&](const Triangle& t) {
        MapContact candidate;
        if (!box_face(t.vertices, t.normal, start, end, axes, half, candidate)) return;
        if (best != UINT32_MAX && (candidate.fraction > hit.fraction ||
                                   (candidate.fraction == hit.fraction &&
                                    (candidate.penetration < hit.penetration ||
                                     (candidate.penetration == hit.penetration && best < t.ordinal)))))
            return;
        best = t.ordinal;
        hit = candidate;
    });
    return best != UINT32_MAX;
}
} // namespace eod::physics
