#pragma once

#include "game_frontend.h"
#include <optional>
#include <string>
#include <vector>

namespace online_profile {
// Session-scoped recovery. Persistent standalone profiles and settings remain the user's.
void initialize(char **argv);
bool selected_for_session();
std::optional<game_frontend::Command> take_invite();
std::string standalone_overlay();
std::vector<std::string> installed_overlays();
void restart_with(const std::string &overlay, const game_frontend::Command &invite);
}
