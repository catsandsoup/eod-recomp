// ee_format.cpp - see ee_format.h
#include "ee_format.h"

#include <cstdio>
#include <cstring>
#include <functional>

#include "memory.h"

namespace {

std::string format(uint32_t fmt_addr, const std::function<uint64_t()> &next) {
    char fmt[1024];
    ee_read_string(fmt_addr, fmt, sizeof fmt);
    std::string out;
    for (const char *p = fmt; *p;) {
        if (*p != '%') {
            out += *p++;
            continue;
        }
        const char *start = p++;
        if (*p == '%') {
            out += '%';
            p++;
            continue;
        }
        while (*p && strchr("-+ #0", *p)) p++;
        if (*p == '*') {
            next();
            p++;
        } else {
            while (*p >= '0' && *p <= '9') p++;
        }
        if (*p == '.') {
            p++;
            if (*p == '*') {
                next();
                p++;
            } else {
                while (*p >= '0' && *p <= '9') p++;
            }
        }
        int longs = 0;
        while (*p && strchr("hlLqjzt", *p)) {
            if (*p == 'l' || *p == 'q' || *p == 'L') longs++;
            p++;
        }
        char conv = *p ? *p++ : 0;
        std::string spec(start, p - start);
        // strip length modifiers, we pass explicitly typed values
        std::string base;
        for (char c : spec)
            if (!strchr("hlLqjzt", c)) base += c;
        char buf[512];
        switch (conv) {
        case 'd':
        case 'i': {
            uint64_t v = next();
            long long sv = longs >= 2 ? (long long)v : (long long)(int32_t)v;
            base.insert(base.size() - 1, "ll");
            snprintf(buf, sizeof buf, base.c_str(), sv);
            out += buf;
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            uint64_t v = next();
            unsigned long long uv = longs >= 2 ? v : (uint32_t)v;
            base.insert(base.size() - 1, "ll");
            snprintf(buf, sizeof buf, base.c_str(), uv);
            out += buf;
            break;
        }
        case 'c':
            snprintf(buf, sizeof buf, base.c_str(), (int)(uint8_t)next());
            out += buf;
            break;
        case 'p':
            snprintf(buf, sizeof buf, "0x%08x", (uint32_t)next());
            out += buf;
            break;
        case 's': {
            uint32_t a = (uint32_t)next();
            char s[400];
            if (a)
                ee_read_string(a, s, sizeof s);
            else
                strcpy(s, "(null)");
            snprintf(buf, sizeof buf, base.c_str(), s);
            out += buf;
            break;
        }
        case 'f':
        case 'F':
        case 'g':
        case 'G':
        case 'e':
        case 'E': {
            uint64_t v = next();
            double d;
            memcpy(&d, &v, 8);
            snprintf(buf, sizeof buf, base.c_str(), d);
            out += buf;
            break;
        }
        default:
            out += spec;
        }
    }
    return out;
}

}  // namespace

std::string ee_format_regs(uint32_t fmt_addr, int first_reg) {
    int reg = first_reg + 1;
    uint32_t stack = g_ee.r[29].uw[0];
    return format(fmt_addr, [&]() -> uint64_t {
        if (reg <= 11) return g_ee.r[reg++].ud[0];
        uint64_t v = MEM_LD(stack);
        stack += 8;
        return v;
    });
}

std::string ee_format_valist(uint32_t fmt_addr, uint32_t va) {
    return format(fmt_addr, [&]() -> uint64_t {
        uint64_t v = MEM_LD(va);
        va += 8;
        return v;
    });
}
