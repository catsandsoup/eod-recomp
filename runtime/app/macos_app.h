#pragma once
#include "disc_import.h"
#include <string>
std::string mac_app_support();
bool mac_app_prepare(std::string &data, const std::string &config, bool headless, const DiscId &id);
// As above, but with problem set no Cocoa dialog opens: the reason is returned for the in-game
// error screen (and its controller disc chooser).
bool mac_app_prepare(std::string &data, const std::string &config, bool headless, const DiscId &id, std::string *problem);
// Installs the disc image/folder chosen in-game into Application Support/disc; "" or a message.
std::string mac_app_install_disc(const std::string &source, const std::string &config, const DiscId &id);
bool mac_app_settings();
// Integrated content (Phantom, Battle, NASCAR Cup + native) is activated automatically before
// streaming starts; there is no player-facing chooser. EOD_MOD_DIR is the developer override and
// EOD_NATIVE_ONLY=1 the native-only diagnostic. Returns "" when the root is complete and intact.
std::string mac_app_validate_integrated(const std::string &root);
bool mac_app_activate_integrated(std::string *why = nullptr);
bool mac_app_select_mods(bool headless);  // false: error shown, do not start the game
bool mac_app_select_mods(bool headless, std::string *problem);  // problem: in-game error instead of an alert
