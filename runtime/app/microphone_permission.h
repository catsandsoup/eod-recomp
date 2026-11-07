#pragma once
#include <functional>
namespace microphone_permission {
enum class State {NotRequested,Granted,Denied,Restricted,Unavailable};
State state();
// Explicit UI action only. Does not create capture devices or transmit audio.
void request(std::function<void(State)>completed);
void open_settings();
}
