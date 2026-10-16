#pragma once
#include "../../core/config.h"
// SPU2 already applies the user's percentage to every mixed sample, shared
// with the Windows backend. SDL must only mute, never apply it a second time.
inline float sdl_output_gain(const Config &config) { return config.audio ? 1.0f : 0.0f; }
