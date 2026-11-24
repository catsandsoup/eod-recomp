// Release-only fault entries. Generated code can still name an unconverted
// guest producer. Include the EE caller and exact operation when one executes.
#include "ee.h"
#include "../core/log.h"

extern "C" void eod_unretired_vu0_macro(uint32_t word) {
    fatal("R7: unretired VU0 macro %08x (EE return %08x)", word, g_ee.r[31].uw[0]);
}
extern "C" uint32_t eod_unretired_vu0_read(int reg) {
    fatal("R7: unretired COP2 control read VI%d (EE return %08x)",reg,g_ee.r[31].uw[0]);
}
extern "C" void eod_unretired_vu0_write(int reg,uint32_t value) {
    fatal("R7: unretired COP2 control write VI%d=%08x (EE return %08x)",reg,value,g_ee.r[31].uw[0]);
}
void vu_reject_launch(const VUState& vu, uint32_t address, VuLaunch launch) {
    fatal("R7: unretired VU%d microprogram at %04x (launch %u, EE return %08x)",
          vu.index,address,static_cast<unsigned>(launch),g_ee.r[31].uw[0]);
}
