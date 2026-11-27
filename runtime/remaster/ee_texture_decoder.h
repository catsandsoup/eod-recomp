// runtime/remaster/ee_texture_decoder.h
// Decodes real retail RGBA8 textures directly from the EE Texture Slot Table (*(0x4D2D30),
// 512 slots, stride 0x44) in EE RAM.
// Self-contained unswizzling arithmetic — ZERO #include of runtime/gs/*, runtime/vu/*, or vif_mesh.h.
#pragma once

#include "asset_registry.h"
#include <cstdint>
#include <string>

namespace remaster {

// Search the active EE Texture Slot Table (*(0x4D2D30)) by texture name (case-insensitive)
// and decode mip 0 into a host RGBA8 image (with PS2 0..128 alpha expanded to 0..255).
bool decode_ee_texture_by_name(const uint8_t *ram, uint32_t ram_size,
                               const std::string &name, CpuImageRgba8 &out);

// Decode a specific slot index (0..511) in the EE Texture Slot Table (*(0x4D2D30)).
// Used for per-instance car liveries (CT.RES .tm0) and dynamic models.
bool decode_ee_texture_by_slot(const uint8_t *ram, uint32_t ram_size,
                               int32_t slot, CpuImageRgba8 &out,
                               std::string *out_name = nullptr);

// Query the texture name of a slot index (0..511) in *(0x4D2D30).
std::string get_ee_texture_slot_name(const uint8_t *ram, uint32_t ram_size, int32_t slot);

// Decode a specific EE texture header address into RGBA8.
bool decode_ee_texture_header(const uint8_t *ram, uint32_t ram_size,
                              uint32_t tex_hdr_addr, CpuImageRgba8 &out);

} // namespace remaster
