#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void eod_matrix_product_column(void);
void eod_matrix_identity_rows(void);
void eod_matrix_seed_camera_row(void);
uint32_t eod_vif1_worker_status(void);

#ifdef __cplusplus
}
#endif
