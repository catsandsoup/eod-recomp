// Visual-car scene slots used by the recompiled EE readers and writers.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VisualCarTraversal VisualCarTraversal;

uint32_t eod_visual_car_slot_read(uint32_t address);
void eod_visual_car_register(uint32_t address, uint32_t object);
void eod_visual_car_unregister(uint32_t address, uint32_t object);
void eod_visual_car_reset(void);
uint32_t eod_visual_car_batch_owner_slot(uint32_t object);
uint32_t eod_visual_car_ordered_read(const VisualCarTraversal *cars, uint32_t address);
int32_t eod_visual_car_order_limit(const VisualCarTraversal *cars);
uint32_t eod_visual_car_batch_read(const VisualCarTraversal *cars, uint32_t address);
void eod_visual_car_batch_write(VisualCarTraversal *cars, uint32_t address, uint32_t object);
void f_0020F4A0_cars(VisualCarTraversal *cars);
void f_00218E68_cars(VisualCarTraversal *cars);
void f_0021C370_cars(VisualCarTraversal *cars);

#ifdef __cplusplus
}

#include <array>
#include <cstddef>
#include <vector>

inline constexpr uint32_t kVisualCarSlots = 0x003D1C90;
inline constexpr uint32_t kLegacyVisualCarSlots = 20;
bool visual_car_adaptive_enabled();

// The EE thread consumes entries in scene-slot order. Each traversal has its own
// snapshot and (for the LOD batch) its own space for the guest array's overflow.
struct VisualCarTraversal {
    explicit VisualCarTraversal(bool batch = false);
    VisualCarTraversal(const VisualCarTraversal &) = delete;
    VisualCarTraversal &operator=(const VisualCarTraversal &) = delete;

    uint32_t entry(size_t ordinal) const;
    uint32_t limit() const;
    bool adaptive() const { return adaptive_; }
    uint32_t batch_read(uint32_t address) const;
    void batch_write(uint32_t address, uint32_t object);

private:
    bool adaptive_;
    size_t count_ = 0;
    std::array<uint32_t, kLegacyVisualCarSlots> inline_{};
    std::vector<uint32_t> overflow_;
    std::vector<uint32_t> batch_overflow_;
};
#endif
