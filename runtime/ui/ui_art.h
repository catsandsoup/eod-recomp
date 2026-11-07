// ui_art.h - the game's own front-end art and menu sounds, decoded at runtime
// from the player's disc (DATA/RES/UIACTION.RES). Nothing is committed or cached:
// the archive is read once when the front end first opens.
//
//   0TSR v6 archive (tools/rst0.py) -> TEX v10 ".art" entries. UI art is 4-bit
//   (TEX format 0x200; 8-bit 0x201 is also handled) with a 256-entry ARGB palette
//   (alpha 0..255). Large textures are stored in PSMCT32 upload order: the bytes
//   are a CT32 rectangle (w/2 x h/4 for 4-bit, w/2 x h/2 for 8-bit) that the GS
//   then samples as PSMT4/PSMT8 (docs/TEXTURE_FORMAT_NOTES.md); the decode uses
//   the GS address functions of runtime/gs/gs_mem.cpp on a private buffer.
//   SFX0 entries (a_press/a_accept/a_back/a_error.sfx) are PS-ADPCM (VAG) mono.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ui_art {

struct Image {
    int w = 0, h = 0;
    std::vector<uint32_t> rgba;  // bytes R,G,B,A
};

// One 0TSR v6 archive in memory.
class Archive {
public:
    bool load(const std::string &path);
    bool get(const std::string &name, std::vector<uint8_t> &out) const;
    std::vector<std::string> names() const;
private:
    std::vector<uint8_t> data_;
    struct Entry { std::string name; uint32_t offset, size; };
    std::vector<Entry> entries_;
};

// TEX v10 -> RGBA (mip 0). false on an unsupported/truncated file.
bool decode_tex(const std::vector<uint8_t> &tex, Image &out);
// SFX0 (PS-ADPCM) -> 16-bit mono samples at *rate.
bool decode_sfx(const std::vector<uint8_t> &sfx, std::vector<int16_t> &out, int &rate);

// Sprites used by the themed front end (texture id 0 = not available: draw the procedural look).
struct Sprite {
    uint32_t tex = 0;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    float w = 0, h = 0;  // source size in texels
    bool ok() const { return tex != 0; }
};

enum class Spr {
    CornerTL, CornerTR, CornerBL, CornerBR, BarH, BarV, Plate, Metal, Chain, Hazard,
    PadDpad, PadCross, PadSquare, PadCircle, PadTriangle, PadL1, PadR1, PadL2, PadR2,
    Count
};

// Loads UIACTION.RES from the game data (once). add_texture registers an RGBA image with the
// overlay and returns its texture id. Returns false when the disc art is unavailable.
using AddTexture = uint32_t (*)(int w, int h, const uint32_t *rgba);
bool load(const std::string &game_root, AddTexture add_texture);
Sprite sprite(Spr s);
// Which game assets were decoded and which are still drawn procedurally (for the report/log).
std::string summary();

// Menu sounds (mono, rate in Hz). Empty when unavailable.
enum class Sfx { Move, Select, Back, Error, Count };
const std::vector<int16_t> &sound(Sfx s, int &rate);

}  // namespace ui_art
