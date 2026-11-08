#pragma once
// Online play: lobby, event flow and car-state sync (state sync, host star topology).
// Everything runs on the EE thread: poll() is called every vblank and from the blocking
// waits inside the game hooks, so there are no locks and no network threads.
// Design and references: docs/NETPLAY_STATE_SYNC.md.
#include "../runtime/app/game_frontend.h"
#include "netplay_car.h"
#include "netplay_hooks.h"
#include "netplay_wire.h"
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace netplay {

using StartInfo = netplay_hooks::StartInfo;

struct RosterSlot { uint32_t driver = 0, vehicle = 0; uint8_t owner = 0xFF; };   // owner 0xFF = AI

class Session : public game_frontend::Network {
public:
    Session();
    ~Session() override;
    // game_frontend::Network
    game_frontend::Snapshot snapshot() const override;
    void submit(game_frontend::Command c) override;

    // ---- pump (EE thread)
    void poll();
    // blocks (pumping window events, the overlay and the network) until done() or timeout_ms
    bool wait_until(const std::function<bool()> &done, int timeout_ms, const char *what);

    // ---- game hooks
    bool take_start(StartInfo &out);              // an event was started by the lobby
    void menu_enter();                            // the native main menu loop starts / ends
    void menu_leave();
    uint16_t menu_buttons();                      // pad buttons injected into the main menu to open ACTION
    void car_chosen(uint32_t vehicle_number, const std::string &name);
    void track_chosen(const std::string &track);  // host: after the native track screen
    bool wait_track(std::string &track);          // guest: blocks for the host's pick
    void event_ended();                           // GameDoSingle returned
    bool event_active() const { return in_event_; }
    bool is_host() const;
    uint32_t remote_humans() const;               // extra cars the roster needs (Battle size)
    void roster_pre(uint32_t description);        // before 0x1116E0 resolves names
    void roster_post(uint32_t description);
    void step_begin();                            // world-step hooks
    void step_end();
    const std::string &last_error() const { return error_; }
    std::string strip() const;

private:
    void auto_drive(uint64_t t);
    struct Impl;
    std::unique_ptr<Impl> d_;
    bool in_event_ = false;
    std::string error_;
};

Session &session();
bool parse_endpoint(const std::string &text, uint32_t &ip_host_order, uint16_t &port, uint16_t default_port);

}  // namespace netplay
