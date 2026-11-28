// runtime/remaster/scene_bridge.h
// Pure EE Scene Graph Bridge (ZERO VU1 / VIF1 / GS dependency).
// Extracts camera, GRAF environment/lights, static world geometry, dynamic vehicles/props,
// live car dents, skeletal bone palettes, and 2D HUD overlays directly from EE RAM & C++ hooks.
#pragma once

#include "graf_loader.h"
#include "scene_types.h"
#include <cstdint>
#include <memory>
#include <string>

namespace editor_environment {
struct Override;
}

namespace remaster {

// Decompiled EE runtime global addresses (SLUS_209.10, GRAF subtypes 0x406B40):
namespace ee_addr {
constexpr uint32_t kGrafPtr         = 0x003D1C18; // Resident GRAF pointer (track.grf)
constexpr uint32_t kGrafCondFlag    = 0x003D1B34; // u32 GRAF 0x101/0x103 condition flag (0x214468)
constexpr uint32_t kSessionPtr      = 0x00495BE0; // Active session STRUCT directly (0x211150 returns &0x495BE0)
constexpr uint32_t kSessionEventMask= 0x00495C1C; // u32 active event mode bitmask = *(0x495BE0 + 0x3C) (0x223090)
constexpr uint32_t kCameraRotation  = 0x003E57A8; // float[9] row-major (right, down, forward)
constexpr uint32_t kCameraPosition  = 0x003E57CC; // float[3] world-space camera position
constexpr uint32_t kNearZ           = 0x004D7C10; // float near plane
constexpr uint32_t kFarZ            = 0x004D7C14; // float far plane
constexpr uint32_t kFovScale        = 0x004D7C1C; // float vertical/horizontal FOV scale
constexpr uint32_t kViewport        = 0x004D7C20; // float[4] x, y, w, h
constexpr uint32_t kAspect          = 0x003DD658; // float aspect ratio

// GRAF Environment & Light runtime globals (decompiled from vtables 0x408100..0x408348):
constexpr uint32_t kSunDir          = 0x004CD2E8; // float[3] normalized directional light vector (0x34E880)
constexpr uint32_t kSunColor        = 0x003DBB3C; // u32 0x00RRGGBB sun/moon color (0x3053C0)
constexpr uint32_t kSunIntensity    = 0x003DBB38; // float 0..1 sun brightness (0x305360 -> 0x3DBAF0+0x48)
constexpr uint32_t kSunIsDay        = 0x003DBB40; // u32 1=day, 0=night (0x305350)
constexpr uint32_t kAmbientColor    = 0x004CD478; // u32 0xAARRGGBB ambient color (0x34F5A0)
constexpr uint32_t kShadowAlpha     = 0x003DC064; // u32 0..255 shadow opacity (0x30AA00)
constexpr uint32_t kShadowDir       = 0x004AE970; // float[3] shadow projection direction (0x30AA10)
constexpr uint32_t kFogColor        = 0x004D8B80; // u32 0x00BBGGRR FOGCOL (0x365428)
constexpr uint32_t kFogRange        = 0x004D8B84; // float[3] fog start, end, density (0x3653F8)
constexpr uint32_t kFlareArray      = 0x003DBB48; // FlareSource[24], stride 0x2C (+0x04 float3 pos)
constexpr uint32_t kFlareCount      = 0x003DBF68; // u32 active flare source count (0..24)
constexpr uint32_t kModelTablePtr   = 0x004C96C0; // -> 0x14-byte model table entries (+0x08 mrModel header)
constexpr uint32_t kParticipantTbl  = 0x00447458; // CarObject* participant table (slot 0 = player 0)
} // namespace ee_addr

// Extract SceneEnvironment directly from EE RAM + optional World Editor override.
SceneEnvironment extract_environment_from_ram(const uint8_t *ram, uint32_t ram_size,
                                              const editor_environment::Override *editor_override = nullptr);

// Extract SceneCamera directly from EE RAM and build Vulkan-compatible view/proj matrices.
SceneCamera extract_camera_from_ram(const uint8_t *ram, uint32_t ram_size,
                                    uint32_t target_width, uint32_t target_height);

// Rebuild camera projection & view_proj for the actual render target dimensions.
void rebuild_camera_projection(SceneCamera &cam, uint32_t target_width, uint32_t target_height);

// Extract dynamic 3D lights (FlareSource stadium floodlights) directly from EE RAM.
void extract_flare_lights_from_ram(const uint8_t *ram, uint32_t ram_size,
                                   bool is_day, std::vector<DynamicLight> &out_lights);

// ---- EE Thread Hooks (called from src/render_queue.cpp, src/bone_pose.cpp, src/native_world.cpp) ----
void ee_begin_frame(uint64_t vblank);
void ee_sync_world_from_ram(const std::string &track_name);
void ee_submit_entity(uint32_t model_handle, const float world_matrix[12],
                      uint32_t material_packet, bool is_player_vehicle = false,
                      uint32_t layer_mask = 0xFFFFFFFFu);
void ee_record_bone_palette(uint32_t packet_addr, const float *matrices_4x4, uint32_t bone_count,
                            uint32_t first_bone = 0);
void ee_record_dent_offsets(uint32_t packet_addr, const float *offsets_xyz, uint32_t count);
void ee_submit_dynamic_light(const DynamicLight &light);
void ee_submit_hud_quad(const HudQuad &quad);
void ee_water_begin(uint32_t surface_addr, int32_t texture_slot);
bool ee_in_water();
void ee_water_vertex(const float xyz[3], const float st[2], uint32_t colour, uint32_t strip);
void ee_water_end(bool drawn);
void ee_end_frame(const editor_environment::Override *editor_override = nullptr);

// ---- Renderer Thread Consumers ----
std::shared_ptr<const SceneFrame> latest_scene_frame();
std::shared_ptr<const LoadedWorld> current_loaded_world();

} // namespace remaster
