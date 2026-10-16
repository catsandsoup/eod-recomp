// pad_script.h - scripted controller input for automated testing.
//
//   EOD_PAD_SCRIPT  = port 0 (player 1)
//   EOD_PAD_SCRIPT1 = port 1 (player 2)
//
// Each is "frame:buttons_hex[:lx:ly[:cross:square]],..." with
// active-high PadButton bits and optional left stick bytes (0..255, 128 =
// centre); each entry holds from its vblank on. Two scripts together drive
// both player ports, which is how 2-player split screen is tested without
// two physical pads. Optional Cross/Square pressures are independent of the
// digital mask and must be 0..255; omitted fields retain digital 0/255.
// Invalid pressure fields reject that entry rather than wrapping the value.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "platform.h"

extern uint64_t kernel_vblank_count();

struct PadScriptEntry {
    uint64_t frame;
    uint16_t buttons;
    uint8_t lx, ly;
    int cross = -1, square = -1; // -1: derive pressure from the digital mask
};

// Fills out (buttons, left stick, pressure) from the port's script; false
// when no script is set for that port.
inline bool pad_script_state(int port, HostPadState *out) {
    static std::vector<PadScriptEntry> script[2];
    static bool parsed, active[2];
    if (!parsed) {
        parsed = true;
        const char *names[2] = {"EOD_PAD_SCRIPT", "EOD_PAD_SCRIPT1"};
        for (int p = 0; p < 2; p++) {
            const char *s = getenv(names[p]);
            if (!s) continue;
            active[p] = true;
            std::string str(s);
            size_t pos = 0;
            while (pos < str.size()) {
                size_t comma = str.find(',', pos);
                std::string tok = str.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                PadScriptEntry e = {0, 0, 128, 128};
                char *end = nullptr;
                e.frame = strtoull(tok.c_str(), &end, 10);
                if (end && *end == ':') {
                    e.buttons = (uint16_t)strtoul(end + 1, &end, 16);
                    if (end && *end == ':') {
                        e.lx = (uint8_t)strtoul(end + 1, &end, 10);
                        if (end && *end == ':') e.ly = (uint8_t)strtoul(end + 1, &end, 10);
                    }
                    bool valid = true;
                    auto pressure = [&](int &value) {
                        const char *start = end + 1;
                        long n = strtol(start, &end, 10);
                        if (end == start || n < 0 || n > 255 || (*end && *end != ':')) return false;
                        value = (int)n;
                        return true;
                    };
                    if (end && *end == ':') valid = pressure(e.cross);
                    if (valid && end && *end == ':') valid = pressure(e.square);
                    if (end && *end == ':') valid = false; // no additional pressure fields
                    if (valid) script[p].push_back(e);
                }
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
        }
    }
    if (port < 0 || port > 1 || !active[port]) return false;
    uint64_t f = kernel_vblank_count();
    PadScriptEntry cur = {0, 0, 128, 128};
    for (auto &e : script[port])
        if (f >= e.frame) cur = e;
    *out = HostPadState();
    out->connected = true;
    out->buttons = cur.buttons;
    out->lx = cur.lx;
    out->ly = cur.ly;
    static const uint16_t order[12] = {PAD_RIGHT, PAD_LEFT, PAD_UP, PAD_DOWN, PAD_TRIANGLE, PAD_CIRCLE,
                                       PAD_CROSS, PAD_SQUARE, PAD_L1, PAD_R1, PAD_L2, PAD_R2};
    for (int i = 0; i < 12; i++)
        if (out->buttons & order[i]) out->pressure[i] = 255;
    if (cur.cross >= 0) out->pressure[6] = (uint8_t)cur.cross;
    if (cur.square >= 0) out->pressure[7] = (uint8_t)cur.square;
    return true;
}

// Port 0's buttons only (the Win32 backend's original interface).
inline bool pad_script_buttons(uint16_t &buttons) {
    HostPadState s;
    if (!pad_script_state(0, &s)) return false;
    buttons = s.buttons;
    return true;
}
