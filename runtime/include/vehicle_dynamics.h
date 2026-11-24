#pragma once
#include "vu.h"

#ifdef __cplusplus
// Precondition: the retail staged body, wheel and tuning records occupy qwords 0..65.
void vehicle_dynamics_step(VUState &state);
extern "C" {
#endif

void eod_vehicle_physics_step(void);

#ifdef __cplusplus
}
#endif
