#pragma once

#include "battle_layout.h"
#include "netplay_wire.h"
#include <array>

namespace netplay {

struct BattleLife {
    uint32_t kills = 0, deaths = 0, penalties = 0;
    bool dead = false;
    uint32_t spawn_tick = 0;
    netplay_car::State spawn{};
};

struct BattleFrame {
    uint32_t tick = 0;
    std::array<BattleLife, battle_layout::kCapacity> seats{};
};

inline void put_battle_frame(Writer &w, const BattleFrame &frame, size_t count) {
    w.u32(frame.tick);
    w.u8(count);
    for (size_t i = 0; i < count; ++i) {
        const auto &s = frame.seats[i];
        w.u32(s.kills); w.u32(s.deaths); w.u32(s.penalties); w.u8(s.dead);
        w.u8(s.spawn_tick != 0);
        if (s.spawn_tick) put_car(w, i, s.spawn_tick, s.spawn);
    }
}

inline bool get_battle_frame(Reader &r, size_t count, BattleFrame &out) {
    BattleFrame frame;
    frame.tick = r.u32();
    if (!frame.tick || r.u8() != count || !count || count > frame.seats.size()) return false;
    for (size_t i = 0; i < count; ++i) {
        auto &s = frame.seats[i];
        s.kills = r.u32(); s.deaths = r.u32(); s.penalties = r.u32();
        unsigned dead = r.u8(), spawned = r.u8();
        if (dead > 1 || spawned > 1 || s.kills > INT32_MAX || s.deaths > INT32_MAX || s.penalties > INT32_MAX) return false;
        s.dead = dead != 0;
        if (spawned) {
            uint8_t slot;
            if (!get_car(r, slot, s.spawn_tick, s.spawn) || slot != i || !s.spawn_tick ||
                s.spawn_tick > frame.tick || !s.deaths) return false;
        }
    }
    if (!r.ok || r.p != r.e) return false;
    out = frame;
    return true;
}

}  // namespace netplay
