#pragma once
namespace gs {
// Fog is a rounded interpolation about the fog colour (GS F is /256).
constexpr int fog_channel(int source, int fog, int factor) { return fog + (((source - fog) * factor + 128) >> 8); }
} // namespace gs
