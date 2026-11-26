// Compare the strip uploaded by the EE at 0x2EF6E0 with the original VU0
// interpreter, including masked qword stores, subdivision and EE readback.
#include "vu.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

void vu_init();

namespace {
using Program = std::array<uint8_t, 65 * 8>;
using Memory = std::array<uint8_t, 4096>;
struct Result {
    VUState returned;
    Memory local;
    std::vector<VUReg> vertices;
};

Result run(const Program& program, int fixture, bool compiled, bool changed) {
    vu_init();
    auto& vu = g_vu0;
    std::memcpy(vu.micro, program.data(), program.size());
    // The remainder of VU0 can contain a previous unrelated upload.
    std::memset(vu.micro + program.size(), 0x7e, 4096 - program.size());
    if (changed) {
        // Use a valid short E-bit NOP program to check dispatch on a changed image.
        const uint32_t nop[] = {0x8000033c, 0x400002ff, 0x8000033c, 0x000002ff};
        std::memcpy(vu.micro, nop, sizeof nop);
    }
    for (int i = 0; i < 4096; ++i) vu.mem[i] = uint8_t(i + fixture);
    for (int i = 1; i < 32; ++i)
        for (int lane = 0; lane < 4; ++lane)
            vu.vf[i].f[lane] = float((13 * i + 29 * lane + 7 * fixture) % 39 - 19) / 7.f;
    vu.vf[10] = {{30.f, .14f, 4.f, 0.f}};
    vu.vf[1].f[3] = 35.f - float(fixture * 7);
    vu.cycle = 10000u * (fixture + 1);
    vu.q_busy = vu.p_busy = 1;
    vu.q_ready = vu.cycle + 2;
    vu.p_ready = vu.cycle + 3;
    vu.q_pending = .7f;
    vu.p_pending = .25f;
    vu.fq_head = fixture % 8;
    if (compiled) {
        if (bool(eod_ribbon_strip_build()) == changed) {
            std::fprintf(stderr, "Unexpected VU0 image dispatch for fixture %d\n", fixture);
            return {};
        }
    } else {
        vu0_macro(0x4a000038);
    }
    Result result{};
    result.returned = vu;
    std::memcpy(result.local.data(), vu.mem, result.local.size());
    vu.vi[2] = 0;
    for (unsigned i = 0; i < vu.vi[3]; ++i) {
        if (compiled) {
            eod_ribbon_next_vertex();
        } else {
            vu0_macro(0x4be1137c);
            vu0_macro(0x4be2137c);
        }
        result.vertices.push_back(vu.vf[1]);
        result.vertices.push_back(vu.vf[2]);
    }
    return result;
}
}

int test_ribbon_strip(const char* elf_path) {
    // File offset of the checked SLUS_209.10 VIF0 MPG payload at VA 0x003BD2C8.
    Program program;
    FILE* elf = std::fopen(elf_path, "rb");
    if (!elf) return 1;
    const bool read = std::fseek(elf, 0x2be2c8, SEEK_SET) == 0 &&
                      std::fread(program.data(), 1, program.size(), elf) == program.size();
    std::fclose(elf);
    if (!read) return 1;
    for (int n = 0; n < 10; ++n) {
        const bool changed = n == 9;
        const Result reference = run(program, n, false, changed);
        const Result actual = run(program, n, true, changed);
        if (actual.returned.tpc != reference.returned.tpc ||
            std::memcmp(&actual.returned, &reference.returned, sizeof(VUState)) ||
            actual.local != reference.local || actual.vertices.size() != reference.vertices.size() ||
            (!actual.vertices.empty() && std::memcmp(actual.vertices.data(), reference.vertices.data(),
                                                     actual.vertices.size() * sizeof(VUReg)))) {
            std::fprintf(stderr, "Ribbon fixture %d differs from the VU0 interpreter\n", n);
            return 1;
        }
    }
    return 0;
}
