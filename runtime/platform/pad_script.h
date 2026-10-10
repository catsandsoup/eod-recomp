// pad_script.h - scripted controller input for automated testing:
// EOD_PAD_SCRIPT="frame:buttons_hex,frame:buttons_hex,..." (active-high
// PadButton bits; each entry holds from its vblank on).
#pragma once
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

extern uint64_t kernel_vblank_count();

// Buttons the script holds now; false when no script is set.
inline bool pad_script_buttons(uint16_t &buttons) {
    static std::vector<std::pair<uint64_t, uint16_t>> script;
    static bool parsed, active;
    if (!parsed) {
        parsed = true;
        if (const char *s = getenv("EOD_PAD_SCRIPT")) {
            active = true;
            std::string str(s);
            size_t pos = 0;
            while (pos < str.size()) {
                size_t comma = str.find(',', pos);
                std::string tok = str.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                size_t colon = tok.find(':');
                if (colon != std::string::npos)
                    script.push_back({strtoull(tok.c_str(), nullptr, 10),
                                      (uint16_t)strtoul(tok.c_str() + colon + 1, nullptr, 16)});
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
        }
    }
    if (!active) return false;
    uint64_t f = kernel_vblank_count();
    buttons = 0;
    for (auto &e : script)
        if (f >= e.first) buttons = e.second;
    return true;
}
