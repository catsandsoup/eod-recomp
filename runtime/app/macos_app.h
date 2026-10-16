#pragma once
#include "disc_import.h"
#include <string>
std::string mac_app_support();
bool mac_app_prepare(std::string &data, const std::string &config, bool headless, const DiscId &id);
bool mac_app_settings();
