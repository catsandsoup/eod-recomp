// ee_format.h - printf-style formatting of EE strings/arguments (for HLE
// logging hooks).
#pragma once
#include <cstdint>
#include <string>

// Formats with arguments taken from GPRs starting at register first_reg
// (4 = a0), continuing on the EE stack after t3 (EABI: 8-byte slots).
std::string ee_format_regs(uint32_t fmt_addr, int first_reg);
// Formats with an EE va_list (pointer to consecutive 8-byte slots).
std::string ee_format_valist(uint32_t fmt_addr, uint32_t va);
