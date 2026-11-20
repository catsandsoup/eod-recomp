#include "visual_car_list.h"
#include "ee.h"
#include "render/native_renderer.h"

#include <cstdlib>
#include <map>

namespace {
std::map<uint32_t, uint32_t> overflow_slots;

constexpr uint32_t slot_for_address(uint32_t address) {
    return (address - kVisualCarSlots) / sizeof(uint32_t);
}
}

bool visual_car_adaptive_enabled() {
    // Differential snapshots restore guest RAM, so they use the guest list alone.
    static const bool diff_off = [] {
        const char *diff = std::getenv("EOD_DIFF");
        return !diff || !*diff;
    }();
    return diff_off && native::enabled();
}

VisualCarTraversal::VisualCarTraversal(bool batch)
    : adaptive_(visual_car_adaptive_enabled()) {
    if (adaptive_) {
        for (uint32_t slot = 0; slot < kLegacyVisualCarSlots; ++slot) {
            if (const uint32_t object = MEM_LW(kVisualCarSlots + slot * 4))
                inline_[count_++] = object;
        }
        if (count_ + overflow_slots.size() > inline_.size())
            overflow_.reserve(count_ + overflow_slots.size() - inline_.size());
        for (const auto &record : overflow_slots) {
            if (count_ < inline_.size())
                inline_[count_] = record.second;
            else
                overflow_.push_back(record.second);
            ++count_;
        }
        if (batch)
            batch_overflow_.resize(overflow_.size());
    }
}

uint32_t VisualCarTraversal::limit() const {
    return adaptive_ ? static_cast<uint32_t>(count_) : kLegacyVisualCarSlots;
}

uint32_t VisualCarTraversal::entry(size_t ordinal) const {
    if (ordinal >= limit()) return 0;
    if (!adaptive_) return MEM_LW(kVisualCarSlots + static_cast<uint32_t>(ordinal) * 4);
    return ordinal < inline_.size() ? inline_[ordinal] : overflow_[ordinal - inline_.size()];
}

uint32_t VisualCarTraversal::batch_read(uint32_t address) const {
    const uint32_t ordinal = (address - R[29].uw[0]) / sizeof(uint32_t);
    if (!adaptive_ || ordinal < kLegacyVisualCarSlots)
        return MEM_LW(address);
    return batch_overflow_.at(ordinal - kLegacyVisualCarSlots);
}

void VisualCarTraversal::batch_write(uint32_t address, uint32_t object) {
    const uint32_t ordinal = (address - R[29].uw[0]) / sizeof(uint32_t);
    if (!adaptive_ || ordinal < kLegacyVisualCarSlots)
        MEM_SW(address, object);
    else
        batch_overflow_.at(ordinal - kLegacyVisualCarSlots) = object;
}

extern "C" uint32_t eod_visual_car_slot_read(uint32_t address) {
    const uint32_t slot = slot_for_address(address);
    if (slot < kLegacyVisualCarSlots || !visual_car_adaptive_enabled())
        return MEM_LW(address);
    const auto it = overflow_slots.find(slot);
    return it != overflow_slots.end() ? it->second : 0;
}

extern "C" void eod_visual_car_register(uint32_t address, uint32_t object) {
    const uint32_t slot = slot_for_address(address);
    if (slot < kLegacyVisualCarSlots || !visual_car_adaptive_enabled())
        MEM_SW(address, object);
    else
        overflow_slots.insert_or_assign(slot, object);
}

extern "C" void eod_visual_car_unregister(uint32_t address, uint32_t object) {
    if (!visual_car_adaptive_enabled()) {
        MEM_SW(address, 0);
        return;
    }
    // A late destructor from the previous occupant must leave the replacement alive.
    if (eod_visual_car_slot_read(address) != object) return;
    const uint32_t slot = slot_for_address(address);
    if (slot < kLegacyVisualCarSlots)
        MEM_SW(address, 0);
    else
        overflow_slots.erase(slot);
}

extern "C" void eod_visual_car_reset() {
    if (!visual_car_adaptive_enabled()) return;
    overflow_slots.clear();
    for (uint32_t slot = 0; slot < kLegacyVisualCarSlots; ++slot)
        MEM_SW(kVisualCarSlots + 4 * slot, 0);
}

extern "C" uint32_t eod_visual_car_batch_owner_slot(uint32_t object) {
    if (!visual_car_adaptive_enabled()) return MEM_LW(object + 0x40);
    // The scene dispatcher invokes this callback on every visual car. Let the
    // first remaining slot lead the batch if slot 0 has been removed.
    for (uint32_t slot = 0; slot < kLegacyVisualCarSlots; ++slot) {
        if (const uint32_t first = MEM_LW(kVisualCarSlots + 4 * slot))
            return first != object;
    }
    return overflow_slots.empty() || overflow_slots.begin()->second != object;
}

extern "C" uint32_t eod_visual_car_ordered_read(const VisualCarTraversal *cars, uint32_t address) {
    if (cars) return cars->entry(slot_for_address(address));
    return MEM_LW(address);
}

extern "C" int32_t eod_visual_car_order_limit(const VisualCarTraversal *cars) {
    return cars ? static_cast<int32_t>(cars->limit()) : kLegacyVisualCarSlots;
}

extern "C" uint32_t eod_visual_car_batch_read(const VisualCarTraversal *cars, uint32_t address) {
    return cars ? cars->batch_read(address) : MEM_LW(address);
}

extern "C" void eod_visual_car_batch_write(VisualCarTraversal *cars, uint32_t address, uint32_t object) {
    if (cars)
        cars->batch_write(address, object);
    else
        MEM_SW(address, object);
}

extern "C" void f_0020F4A0_orig();
extern "C" void f_0021C370_orig();

extern "C" void f_0020F4A0() {
    VisualCarTraversal cars;
    if (cars.adaptive())
        f_0020F4A0_cars(&cars);
    else
        f_0020F4A0_orig();
}

extern "C" void f_0021C370() {
    VisualCarTraversal cars;
    if (cars.adaptive())
        f_0021C370_cars(&cars);
    else
        f_0021C370_orig();
}
