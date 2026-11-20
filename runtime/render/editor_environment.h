#pragma once

#include <array>
#include <cmath>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>

namespace editor_environment {

struct Override {
    std::string track;
    std::array<float, 3> sun_dir{}, sun_color{}, fog_color{};
    float ambient_intensity = 0, fog_start = 0, fog_end = 0, fog_density = 0;
};

inline std::string_view trim(std::string_view value) {
    const size_t start = value.find_first_not_of(" \t\r");
    if (start == std::string_view::npos) return {};
    return value.substr(start, value.find_last_not_of(" \t\r") - start + 1);
}

inline std::string track_id(std::string_view name) {
    name.remove_prefix(name.find_last_of("/\\") + 1);
    if (name.empty() || name.size() > 64) return {};
    std::string id(name);
    for (char &ch : id) if (ch >= 'a' && ch <= 'z') ch += 'A' - 'a';
    if (id.size() > 4 && id.compare(id.size() - 4, 4, ".TRK") == 0) id.resize(id.size() - 4);
    if (id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos) return {};
    return id;
}

inline bool applies(const Override &settings, std::string_view archive, bool gameplay) {
    return gameplay && !settings.track.empty() && track_id(archive) == settings.track;
}

inline bool parse(std::istream &in, Override &out) {
    Override settings;
    bool section = false, active = false;
    unsigned fields = 0;
    for (std::string line; std::getline(in, line);) {
        const auto value = trim(line);
        if (value.empty() || value.front() == '#' || value.front() == ';') continue;
        if (value.front() == '[') { section = value == "[environment]"; continue; }
        const size_t eq = value.find('=');
        if (!section || eq == std::string_view::npos) continue;
        const auto key = trim(value.substr(0, eq)), text = trim(value.substr(eq + 1));
        if (key == "enabled") { active = text == "1"; continue; }
        if (key == "track") { settings.track = track_id(text); continue; }
        float *destination = nullptr;
        size_t count = 1;
        float min = 0, max = 1;
        unsigned bit = 0;
        if (key == "sun_dir") { destination = settings.sun_dir.data(); count = 3; min = -1; bit = 1; }
        else if (key == "sun_color") { destination = settings.sun_color.data(); count = 3; bit = 2; }
        else if (key == "fog_color") { destination = settings.fog_color.data(); count = 3; bit = 4; }
        else if (key == "ambient_intensity") { destination = &settings.ambient_intensity; max = 2; bit = 8; }
        else if (key == "fog_start") { destination = &settings.fog_start; max = 3000; bit = 16; }
        else if (key == "fog_end") { destination = &settings.fog_end; max = 6000; bit = 32; }
        else if (key == "fog_density") { destination = &settings.fog_density; bit = 64; }
        if (!destination) continue;
        std::istringstream numbers{std::string(text)};
        numbers.imbue(std::locale::classic());
        for (size_t i = 0; i < count; ++i) {
            char separator;
            if (i && (!(numbers >> separator) || separator != ',')) return false;
            if (!(numbers >> destination[i]) || !std::isfinite(destination[i]) ||
                destination[i] < min || destination[i] > max) return false;
        }
        if (!(numbers >> std::ws).eof()) return false;
        fields |= bit;
    }
    if (!active || settings.track.empty() || fields != 127 || settings.fog_end < settings.fog_start) return false;
    const float length = std::hypot(settings.sun_dir[0], settings.sun_dir[1], settings.sun_dir[2]);
    if (length < 0.001f) return false;
    for (float &value : settings.sun_dir) value /= length;
    settings.sun_dir[1] = -settings.sun_dir[1]; // editor Y-up to native Y-down
    out = settings;
    return true;
}

} // namespace editor_environment
