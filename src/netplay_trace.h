#pragma once
#include <cstdint>
// Diagnostic only: observes bytes returned by the native SDK pad read.
void netplay_trace_pad(int port, int slot, uint32_t length, const uint8_t *data);
