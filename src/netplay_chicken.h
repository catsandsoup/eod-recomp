#pragma once

#include "netplay_wire.h"
#include "chicken_prop.h"
#include <array>
#include <cmath>
#include <deque>
#include <map>

namespace netplay {

enum class ChickenAction : uint8_t { Launch = 1, Blast, Cancel, Prop };
struct ChickenEvent {
    ChickenAction action = ChickenAction::Launch;
    uint8_t slot = 0;
    uint32_t life = 0, shot = 0;
    std::array<float, 3> position{}, velocity{};
    chicken_prop::Hit prop{};
};

inline void put_chicken(Writer &w, const ChickenEvent &e) {
    w.u8(static_cast<uint8_t>(e.action)); w.u8(e.slot); w.u32(e.life); w.u32(e.shot);
    if (e.action == ChickenAction::Prop) {
        const auto &h = e.prop;
        w.u8(static_cast<uint8_t>(h.type)); w.u16(h.object); w.u32(h.part); w.u32(h.donor_life);
        for (float v : h.pose) w.f32(v);
        for (const auto *vec : {&h.linear, &h.angular, &h.point, &h.impulse})
            for (float v : *vec) w.f32(v);
        return;
    }
    if (e.action != ChickenAction::Cancel)
        for (float v : e.position) w.f32(v);
    if (e.action == ChickenAction::Launch)
        for (float v : e.velocity) w.f32(v);
}

inline bool get_chicken(Reader &r, ChickenEvent &e) {
    ChickenEvent parsed;
    parsed.action = static_cast<ChickenAction>(r.u8());
    parsed.slot = r.u8(); parsed.life = r.u32(); parsed.shot = r.u32();
    if (!parsed.shot || parsed.slot >= kMaxPlayers || parsed.action < ChickenAction::Launch ||
        parsed.action > ChickenAction::Prop) return false;
    if (parsed.action == ChickenAction::Prop) {
        auto &h = parsed.prop;
        h.type = static_cast<chicken_prop::Type>(r.u8()); h.object = r.u16(); h.part = r.u32(); h.donor_life = r.u32();
        for (float &v : h.pose) v = r.f32();
        for (auto *vec : {&h.linear, &h.angular, &h.point, &h.impulse})
            for (float &v : *vec) v = r.f32();
        if (!chicken_prop::valid_hit(h)) return false;
    } else if (parsed.action != ChickenAction::Cancel) {
        for (auto &v : parsed.position) {
            v = r.f32();
            if (!std::isfinite(v) || std::abs(v) > 100000.f) return false;
        }
    }
    if (parsed.action == ChickenAction::Launch) {
        for (auto &v : parsed.velocity) {
            v = r.f32();
            if (!std::isfinite(v) || std::abs(v) > 2000.f) return false;
        }
    }
    if (!r.ok || r.p != r.e) return false;
    e = parsed;
    return true;
}

// The native game has one chicken body per launcher. Replaying a delayed older
// blast after a newer launch would destroy the wrong body. Keep state changes
// ordered, including launches which detonate within the same simulation step.
class ChickenChannel {
    struct Pending { uint32_t serial; ChickenEvent event; uint64_t last_sent = 0; };
    std::deque<Pending> outgoing_;
    std::map<uint32_t, ChickenEvent> incoming_;
    uint32_t sent_ = 0, received_ = 0;
    static constexpr unsigned kWindow = 32;
public:
    bool queue(const ChickenEvent &e) {
        if (outgoing_.size() >= 1024) return false;
        outgoing_.push_back({++sent_, e});
        return true;
    }
    void acknowledge(uint32_t serial) {
        if (serial > sent_) return;
        while (!outgoing_.empty() && outgoing_.front().serial <= serial) outgoing_.pop_front();
    }
    template<class Send> void transmit(uint64_t now, Send send) {
        unsigned count = 0;
        for (auto &item : outgoing_) {
            if (++count > kWindow) break;
            if (item.last_sent && now - item.last_sent < 100000) continue;
            send(item.serial, item.event);
            item.last_sent = now;
        }
    }
    void receive(uint32_t serial, const ChickenEvent &e) {
        if (serial > received_ && serial - received_ <= kWindow) incoming_.emplace(serial, e);
    }
    uint32_t received() const { return received_; }
    template<class Apply> void apply(Apply apply_event) {
        for (;;) {
            const auto it = incoming_.find(received_ + 1);
            if (it == incoming_.end() || !apply_event(it->second)) return;
            ++received_;
            incoming_.erase(it);
        }
    }
};

} // namespace netplay
