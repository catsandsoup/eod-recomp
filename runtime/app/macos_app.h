#pragma once
#include "disc_import.h"
#include <string>
std::string mac_app_support();
bool mac_app_prepare(std::string &data, const std::string &config, bool headless, const DiscId &id);
bool mac_app_settings();
// Profiles live under Application Support/mods. Selection happens before streaming starts.
bool mac_app_set_mod_profile(const std::string &profile);
bool mac_app_select_mods(bool headless);
