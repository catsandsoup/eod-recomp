// Compare every byte of the guest chain and its untouched tail against the
// retail EE builder, including DMA termination independently of GIF EOP.
#include "ee.h"
#include "gs/texture_upload.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

EEState g_ee{};
uint8_t *g_ram;
extern "C" {
void f_0035FC40();
void f_0035FC40_orig();
void f_00389500();
void f_003896E8();
void f_003B1BD0() { std::abort(); }
void ee_write128_slow(uint32_t, const R128 *) { std::abort(); }
void f_00365878();
void f_003896E8_orig() { R[2].sd[0] = -1; }
void ee_jump(uint32_t) { std::abort(); }
void f_00331DF0() { std::abort(); }
uint32_t ee_read32_slow(uint32_t) { std::abort(); }
uint64_t ee_read64_slow(uint32_t) { std::abort(); }
void ee_write32_slow(uint32_t, uint32_t) { std::abort(); }
void ee_write64_slow(uint32_t, uint64_t) { std::abort(); }
}
void diff_run(uint32_t, void (*replacement)(), void (*)(), unsigned) { replacement(); }
bool diff_in_original_side() { return false; }

static bool admit_boot;
static unsigned boot_calls;
bool gs::try_boot_texture_upload(const TextureUpload &upload, uint32_t) {
    ++boot_calls;
    return admit_boot && upload.bootstrap && upload.source == 0x30000;
}

static void check(bool ok, const char *what) {
    if (!ok) { std::fprintf(stderr, "texture upload: %s\n", what); std::exit(1); }
}

int main() {
    g_ram = static_cast<uint8_t *>(std::calloc(0x500000, 1));
    check(g_ram, "allocate fixture RAM");
    constexpr uint32_t cases[] = {0x35fcd0,0x35fcd0,0x35fcc8,0x35fcd8,0x35fcc0,
                                 0x35fcd8,0x35fcd8,0x35fcd8,0x35fcb8};
    std::memcpy(g_ram + 0x429f00, cases, sizeof cases);
    constexpr uint32_t packet = 0x10000;
    for (unsigned type : {0,1,2,4,8}) {
        for (unsigned flags = 0; flags < 4; ++flags) {
            for (unsigned width : {2,17,128}) {
                R[4].ud[0] = packet; R[5].ud[0] = 0x6bc4; R[6].ud[0] = 0x30000;
                R[7].ud[0] = width; R[8].ud[0] = 0x42; R[9].ud[0] = 13;
                R[10].ud[0] = 0x815; R[11].ud[0] = type;
                R[29].ud[0] = 0x80000; R[31].ud[0] = 0x35f184;
                MEM_SW(0x80000, flags & 1); MEM_SW(0x80008, flags & 2);
                auto input = g_ee;
                std::array<uint8_t,256> reference;
                std::memset(g_ram + packet, 0xa5, reference.size());
                f_0035FC40_orig();
                const auto count = R[2].ud[0], lo = g_ee.lo.ud[0], hi = g_ee.hi.ud[0];
                std::memcpy(reference.data(), g_ram + packet, reference.size());
                g_ee = input;
                std::memset(g_ram + packet, 0xa5, reference.size());
                f_0035FC40();
                check(!std::memcmp(reference.data(), g_ram + packet, reference.size()), "retail bytes and untouched tail");
                check(R[2].ud[0] == count && g_ee.lo.ud[0] == lo && g_ee.hi.ud[0] == hi, "EE return and multiplier");
                gs::TextureUpload u;
                check(gs::find_texture_upload(packet, g_ram + packet, u), "recipe registered");
                bool eop;
                check(gs::find_texture_fence(packet + 144, g_ram + packet + 144, eop) &&
                      eop == bool(flags & 2), "retail chain fence");
                check(gs::find_texture_fence(packet + 176, g_ram + packet + 176, eop) ==
                      bool(flags & 2), "optional second fence");
                g_ram[packet + 160 + 8] ^= 1;
                check(!gs::find_texture_fence(packet + 144, g_ram + packet + 144, eop), "modified fence");
                g_ram[packet + 160 + 8] ^= 1;
                // Mutating one byte anywhere in the setup or source reference must fall back.
                for (size_t i = 0; i < gs::kUploadPrefixQwords * 16; ++i) {
                    g_ram[packet + i] ^= 1;
                    check(!gs::find_texture_upload(packet, g_ram + packet, u), "stale recipe");
                    g_ram[packet + i] ^= 1;
                }
            }
        }
    }
    // Build the six SDK qwords with the retail 0x389500, then ask 0x3896E8
    // for its typed operation using each of the four real caller registers.
    constexpr uint32_t callers[] = {0x35b0d8, 0x35b124, 0x35b170, 0x35b1c0};
    MEM_SW(0x42f910, 0x38956c); // CT32 switch target
    for (unsigned page = 0; page < 16; ++page) {
        R[4].ud[0] = packet; R[5].ud[0] = page << 10; R[6].ud[0] = 16;
        R[7].ud[0] = R[8].ud[0] = R[9].ud[0] = 0;
        R[10].ud[0] = 1024; R[11].ud[0] = 64;
        f_00389500();
        check(R[2].ud[0] == 6, "retail SDK setup size");
        R[4].ud[0] = packet; R[5].ud[0] = 0x30000;
        R[17].ud[0] = (page & ~3u) + (page % 4 == 3 ? 4 : 0);
        R[31].ud[0] = callers[page % 4];
        admit_boot = true;
        f_003896E8();
        check(R[2].sd[0] == 0 && boot_calls == page * 2 + 1, "retail boot page admitted");
        admit_boot = false;
        f_003896E8();
        check(R[2].sd[0] == -1 && boot_calls == page * 2 + 2, "busy channel falls back");
        g_ram[packet + 16] ^= 1;
        f_003896E8();
        check(R[2].sd[0] == -1 && boot_calls == page * 2 + 2, "modified SDK setup falls back");
    }
    constexpr uint32_t manager = 0x1000;
    MEM_SW(0x3e597c, manager);
    MEM_SD(0x3e5980, 0x70000000);
    for (unsigned bank = 0; bank < 2; ++bank) {
        const uint32_t base = 0x20000 + bank * 1024;
        const uint32_t desc = manager + bank * 20;
        MEM_SW(manager + 40, bank);
        MEM_SW(desc + 4, base); MEM_SW(desc + 8, 4); MEM_SW(desc + 16, 7);
        f_00365878();
        bool eop;
        check(MEM_LW(desc + 8) == 7 && MEM_LW(base + 6*16) == 0x70000000 &&
              gs::find_texture_fence(base + 64, g_ram + base + 64, eop) && eop,
              "retail bank completion fence and END");
    }
    std::free(g_ram);
}
