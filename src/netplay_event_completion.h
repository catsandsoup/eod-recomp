#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace netplay {

enum class EventCompletionMode { Native, Race, Derby, Battle };
enum class EventFinishState : uint8_t { Active, Finished, Totalled, Disconnected, TimedOut, Survivor, Withdrawn, Completed, BattleScore };

// Native session event types at SessionDescription + 0x9A4 (SLUS_209.10).
inline EventCompletionMode event_completion_mode(uint32_t event_type) {
    switch (event_type) {
    case 0x00042108: // Forward-Backward Race
    case 0x02002109: // Backward Race
    case 0x0001210A: // Suicide Race
    case 0x0010210B: // Shortcut Race
    case 0x0002210C: // Flagpole Race
    case 0x0020210D: // Reverse Race
    case 0x00402112: // Stop-and-Go Race
    case 0x00042119: // Whip Around Race
    case 0x00002110: // No-Rules Race
    case 0x0000210E: // Jump Race
    case 0x01002105: // Figure-8
    case 0x0100210F: // Figure-8 Jump Race
        return EventCompletionMode::Race;
    case 0x1880221E: // Demolition Derby
        return EventCompletionMode::Derby;
    case 0x18000224: // Battle
        return EventCompletionMode::Battle;
    default:
        return EventCompletionMode::Native;
    }
}

// All wire timestamps are milliseconds modulo 2^32. Signed differences
// stay valid across wrap for the bounded intervals used by a single event.
class EventHostClock {
public:
    void anchor(uint32_t host, uint32_t local) {
        if (!sampled_) offset_ = host - local;
    }
    void observe(uint32_t sent, uint32_t received, uint32_t host) {
        const uint32_t rtt = received - sent;
        if (rtt > 2000) return;
        if (sampled_ && received - sampled_at_ < 30000 && rtt > best_rtt_) return;
        sampled_ = true;
        sampled_at_ = received;
        best_rtt_ = rtt;
        offset_ = host - (sent + rtt / 2);
    }
    uint32_t host_ms(uint32_t local) const { return local + offset_; }
    uint32_t round_trip_ms() const { return best_rtt_; }

private:
    bool sampled_ = false;
    uint32_t sampled_at_ = 0, best_rtt_ = 0, offset_ = 0;
};

struct EventResultSlot {
    EventFinishState state = EventFinishState::Active;
    uint8_t rank = 0;
    uint64_t observed_at_ms = 0;
};

struct EventResultRow {
    uint8_t slot;
    EventFinishState state;
    uint8_t rank;
    std::string name;
    int32_t score = 0;
};

class EventCompletion {
public:
    static constexpr std::size_t kSlots = 16;
    static constexpr uint64_t kCountdownUs = 30000000;
    // Finish reports repeat every 100 ms. Allow lost retries and a high
    // one-way delay while still presenting the outcome within two seconds.
    static constexpr uint64_t kReportSettleUs = 750000;
    static constexpr uint64_t kDerbySettleUs = 250000;

    void reset(EventCompletionMode mode, uint16_t participants) {
        *this = {};
        mode_ = mode;
        participants_ = participants;
    }

    bool participates(std::size_t slot) const {
        return slot < kSlots && ((participants_ >> slot) & 1);
    }

    void record(std::size_t slot, EventFinishState state, uint64_t now, uint64_t observed_at = 0) {
        if (complete_ || !participates(slot) || slots_[slot].state != EventFinishState::Active) return;
        // Battle deaths are respawnable. Its native host arbiter owns victory.
        if (mode_ == EventCompletionMode::Battle && state != EventFinishState::Disconnected &&
            state != EventFinishState::Withdrawn) return;
        const uint64_t at = observed_at ? std::min(observed_at, now) : now;
        if (deadline_ && (at >= deadline_ || now >= deadline_ + kReportSettleUs)) return;
        const bool native_end = mode_ == EventCompletionMode::Native && state == EventFinishState::Completed;
        if (!native_end && state != EventFinishState::Finished && state != EventFinishState::Totalled &&
            state != EventFinishState::Disconnected && state != EventFinishState::Withdrawn) return;
        if (state == EventFinishState::Finished) {
            if (mode_ != EventCompletionMode::Race) return;
            // Retries and different network paths can reverse report order.
            const uint64_t deadline = (at / 1000) * 1000 + kCountdownUs;
            if (!deadline_ || deadline < deadline_) deadline_ = deadline;
        }
        slots_[slot].state = state;
        slots_[slot].observed_at_ms = at / 1000;
    }

    bool update(uint64_t now, bool mode_timed_out = false, bool host_objective_ended = false) {
        if (complete_ || participants_ == 0) return false;
        std::size_t active = 0, last_slot = 0;
        for (std::size_t slot = 0; slot < kSlots; ++slot) {
            if (participates(slot) && slots_[slot].state == EventFinishState::Active) {
                ++active;
                last_slot = slot;
            }
        }
        if (mode_ == EventCompletionMode::Battle) {
            if (!host_objective_ended) return false;
        } else if (mode_ == EventCompletionMode::Native) {
            if (active && !mode_timed_out && !host_objective_ended) return false;
        } else if (mode_ == EventCompletionMode::Derby) {
            if (active > 1 && !mode_timed_out) return false;
            // The other owner's wreck report can arrive a few frames later.
            // A short settle window lets a final mutual wreck end without a winner.
            if (active == 1 && !mode_timed_out) {
                if (!derby_settle_until_) derby_settle_until_ = now + kDerbySettleUs;
                if (now < derby_settle_until_) return false;
            }
            if (active == 1) slots_[last_slot] = {EventFinishState::Survivor, 1};
        } else if (active != 0 && (!deadline_ || now < deadline_ + kReportSettleUs)) {
            return false;
        }
        if (active != 0) {
            for (std::size_t slot = 0; slot < kSlots; ++slot) {
                if (participates(slot) && slots_[slot].state == EventFinishState::Active)
                    slots_[slot].state = mode_ == EventCompletionMode::Battle ? EventFinishState::BattleScore :
                        mode_ == EventCompletionMode::Native ?
                        EventFinishState::Completed : EventFinishState::TimedOut;
            }
        }
        // Only native line crossings receive a race rank. A retransmission
        // arriving later cannot move an earlier crossing down the standings.
        if (mode_ == EventCompletionMode::Race && deadline_) {
            for (auto &slot : slots_)
                if (slot.observed_at_ms >= deadline_ / 1000)
                    slot.state = EventFinishState::TimedOut;
        }
        for (std::size_t i = 0; i < kSlots; ++i) {
            if (slots_[i].state != EventFinishState::Finished) continue;
            uint8_t rank = 1;
            for (std::size_t j = 0; j < kSlots; ++j) {
                if (slots_[j].state != EventFinishState::Finished) continue;
                if (slots_[j].observed_at_ms < slots_[i].observed_at_ms ||
                    (slots_[j].observed_at_ms == slots_[i].observed_at_ms && j < i)) ++rank;
            }
            slots_[i].rank = rank;
        }
        complete_ = true;
        return true;
    }

    uint64_t deadline() const { return deadline_; }
    EventFinishState state(std::size_t slot) const { return slots_[slot].state; }
    uint8_t rank(std::size_t slot) const { return slots_[slot].rank; }

private:
    EventCompletionMode mode_ = EventCompletionMode::Native;
    uint16_t participants_ = 0;
    uint64_t deadline_ = 0;
    uint64_t derby_settle_until_ = 0;
    bool complete_ = false;
    std::array<EventResultSlot, kSlots> slots_{};
};

}  // namespace netplay
