// ui_preview.h - pictures for the online lobby, decoded at runtime from the player's own disc
// (nothing is stored or committed):
//   tracks: the Eve loading-screen art in DATA/RES/UIBIG.RES (l-<track>.img: 512x512, 8-bit; the
//           lower part is the map with the track's photo and name label),
//   cars:   the car-select portraits in DATA/RES/UICAR.RES (uicar_<vehicle number>.tm0).
//           Native special vehicles without art and unverified custom portrait identities
//           return an empty Pic; flat livery/HUD swatches are never shown as portraits.
#pragma once
#include <cstdint>
#include <string>
#include "ui_art.h"

namespace ui_preview {
struct Pic {
    uint32_t tex = 0;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    float aspect = 1;   // width / height of the visible part
    bool ok() const { return tex != 0; }
};
void init(const std::string &game_root, ui_art::AddTexture add);
Pic track(const std::string &name);        // by display name ("Anoka"); the default art when the track has none
Pic car(uint32_t vehicle_number);
}
