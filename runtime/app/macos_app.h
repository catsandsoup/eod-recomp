// macos_app.h - macOS app integration (macos_app.mm; EOD_SDL3 builds on macOS).
#pragma once
#include "disc_import.h"
#include <string>
// ~/Library/Application Support/TDEODRecomp (EOD_APP_SUPPORT overrides it).
std::string mac_app_support();
// Before platform_init: memory cards in mac_app_support()/memcard (unless eod.ini
// names a folder); `data` (--data, eod.ini or EOD_DATA) is checked, or else the
// disc installed in mac_app_support()/disc, or else the player is asked for an
// image or folder to install there (not in headless mode). false: quit.
bool mac_app_prepare(std::string &data, const std::string &config, bool headless, const DiscId &id);
// The settings dialog; true when eod.ini was saved (g_config updated).
bool mac_app_settings();
