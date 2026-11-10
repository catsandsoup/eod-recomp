// Host texture replacement: identity keys (against tools/texture_identity.py
// reference values), .eodhtx validation, manifest parsing, alpha conversion
// and CPU prefetch. Disposable files only; no retail data.
#include "gs/texture_replacements.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace gs::hosttex;

static int g_fail;
static void check(bool ok, const char *why) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", why);
        g_fail++;
    }
}

static std::vector<uint8_t> htx(uint32_t w, uint32_t h, uint32_t levels, uint8_t alpha = 255) {
    std::vector<uint8_t> f(htx_total_bytes(w, h, levels));
    memcpy(f.data(), "EODHTX1\0", 8);
    f[8] = (uint8_t)w, f[9] = (uint8_t)(w >> 8), f[10] = (uint8_t)h, f[11] = (uint8_t)(h >> 8);
    f[12] = (uint8_t)levels, f[13] = 0, f[14] = f[15] = 0;
    for (size_t i = kHtxHeaderBytes; i + 3 < f.size(); i += 4) {
        f[i] = (uint8_t)i, f[i + 1] = (uint8_t)(i >> 8), f[i + 2] = 7, f[i + 3] = alpha;
    }
    return f;
}

static void write(const std::string &path, const std::vector<uint8_t> &d) {
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::perror(path.c_str());
        std::exit(2);
    }
    std::fwrite(d.data(), 1, d.size(), f);
    std::fclose(f);
}
static void write(const std::string &path, const std::string &s) {
    write(path, std::vector<uint8_t>(s.begin(), s.end()));
}

int main() {
    // ---- identity: values from tools/texture_identity.py fnv1a64 over the same bytes
    std::vector<uint8_t> p8, p4;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 16; x++) p8.push_back((uint8_t)((x * 7 + y * 3) & 255));
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 8; x++) p4.push_back((uint8_t)((x + y) & 15));
    check(hex64(index_key(8, 16, 8, p8.data())) == "6030558c8bdae8d9", "8-bit indexKey matches the Kit");
    check(hex64(index_key(4, 8, 4, p4.data())) == "6135115c206eea51", "4-bit indexKey matches the Kit");
    uint32_t pal[256];
    for (uint32_t i = 0; i < 256; i++)
        pal[i] = i | (255 - i) << 8 | ((i * 3) & 255) << 16 | 0x80u << 24;  // R in bits 0-7
    check(hex64(palette_key(pal, 256)) == "b514f2c74cca0125", "256-entry paletteKey matches the Kit");
    check(hex64(palette_key(pal, 16)) == "8cabbc78671964b5", "16-entry paletteKey matches the Kit");
    pal[3] ^= 0x7Fu << 24;
    check(hex64(palette_key(pal, 16)) == "8cabbc78671964b5", "paletteKey ignores alpha");
    uint64_t v = 0;
    check(parse_hex64("b0dfc9176c7f868a", v) && v == 0xb0dfc9176c7f868aull, "hex parse");
    check(!parse_hex64("B0DFC9176C7F868A", v), "uppercase rejected per contract");
    check(!parse_hex64("b0dfc9176c7f868", v), "short hex rejected");

    // ---- format
    check(full_chain_levels(4096, 4096) == 13 && full_chain_levels(64, 32) == 7 && full_chain_levels(1, 1) == 1,
          "full chain level counts");
    check(htx_level_offset(4, 4, 1) == 16 + 64 && htx_total_bytes(4, 4, 3) == 16 + 64 + 16 + 4, "level offsets");
    HtxHeader h;
    std::string why;
    auto good = htx(8, 4, 4);
    check(parse_htx_header(good.data(), good.size(), h, why) && h.width == 8 && h.height == 4 && h.levels == 4,
          "valid header");
    check(!parse_htx_header(good.data(), good.size() - 1, h, why), "truncated file rejected");
    auto partial = htx(8, 4, 2);
    check(!parse_htx_header(partial.data(), partial.size(), h, why), "partial chain rejected");
    auto bad = good;
    bad[0] = 'X';
    check(!parse_htx_header(bad.data(), bad.size(), h, why), "bad magic rejected");

    // ---- alpha domain
    check(to_gs_alpha(255) == 128 && to_gs_alpha(0) == 0 && to_gs_alpha(128) == 64 && to_gs_alpha(254) == 127 && to_gs_alpha(1) == 1,
          "straight -> GS alpha");
    check(gs_alpha_domain(0x80) && !gs_alpha_domain(0xFF), "domain from native alpha high");
    uint8_t src[8] = {1, 2, 3, 255, 4, 5, 6, 0}, dst[8];
    uint32_t lo = 255, hi = 0;
    convert_texels(src, dst, 2, true, lo, hi);
    check(dst[0] == 1 && dst[3] == 128 && dst[7] == 0 && lo == 0 && hi == 128, "GS conversion keeps RGB");
    lo = 255, hi = 0;
    convert_texels(src, dst, 2, false, lo, hi);
    check(dst[3] == 255 && lo == 0 && hi == 255, "straight passthrough");

    // ---- manifest
    char tmpl[] = "/tmp/eod_hosttex_XXXXXX";
    const char *dir = mkdtemp(tmpl);
    check(dir != nullptr, "temp dir");
    std::string root = dir, hosttex = root + "/HOSTTEX";
    mkdir(hosttex.c_str(), 0755);
    mkdir((hosttex + "/sky").c_str(), 0755);
    write(hosttex + "/sky/a.eodhtx", htx(32, 32, 6));     // 16x16 x2
    write(hosttex + "/sky/b.eodhtx", htx(48, 48, 6));     // 16x16 x3: not a power of two
    write(hosttex + "/sky/c.eodhtx", htx(32, 16, 6));     // 16x16 aspect changed
    write(hosttex + "/sky/w.eodhtx", htx(64, 64, 7, 9));  // wildcard
    std::string m = "eod-host-textures 1\n";
    m += "b0dfc9176c7f868a\t205f0e8a058538d6\t16\t16\tsky/a.eodhtx\tDATA/TRACKS/ANOKA.TRK#sky01A.tex\tpkg\n";
    m += "b0dfc9176c7f868a\t*\t16\t16\tsky/w.eodhtx\tDATA/TRACKS/ANOKA.TRK#sky01A.tex\twild\n";
    m += "1111111111111111\t*\t16\t16\tsky/missing.eodhtx\tA#missing\tpkg\n";
    m += "2222222222222222\t*\t16\t16\tsky/b.eodhtx\tA#b\tpkg\n";
    m += "3333333333333333\t*\t16\t16\tsky/c.eodhtx\tA#c\tpkg\n";
    m += "4444444444444444\t*\t16\t16\t../escape.eodhtx\tA#esc\tpkg\n";
    m += "5555555555555555\t*\t16\tsky/a.eodhtx\tA#short\tpkg\n";
    m += "BADBADBADBADBADB\t*\t16\t16\tsky/a.eodhtx\tA#upper\tpkg\n";
    m += "6666666666666666\t*\t0\t16\tsky/a.eodhtx\tA#zero\tpkg\n";
    m += "# comment\n\n";
    write(hosttex + "/replacements.tsv", m);
    std::vector<std::string> logs;
    LogFn log = [&](const std::string &s) { logs.push_back(s); };
    Store st;
    size_t n = st.load(root, log);
    check(n == 2, "two valid entries accepted");
    int skipped = 0;
    for (auto &l : logs) skipped += l.find("skipped") != std::string::npos;
    check(skipped == 7, "seven malformed lines logged as skipped");
    bool missing_logged = false;
    for (auto &l : logs) missing_logged |= l.find("file not found") != std::string::npos;
    check(missing_logged, "missing file reason logged");
    check(st.entries()[0].scale == 2 && st.entries()[0].w == 32 && st.entries()[1].scale == 4, "scale factors");
    check(st.has_dims(16, 16) && !st.has_dims(32, 32), "dimension pre-filter");
    why.clear();
    check(st.find(0xb0dfc9176c7f868aull, 0x205f0e8a058538d6ull, 16, 16, why) == 0, "exact palette wins");
    check(st.find(0xb0dfc9176c7f868aull, 0x1234ull, 16, 16, why) == 1, "wildcard for another palette");
    check(st.find(0xb0dfc9176c7f868aull, 0x205f0e8a058538d6ull, 32, 32, why) == -1, "dimensions are part of identity");
    check(st.find(0x9999ull, 0, 16, 16, why) == -1 && why.empty(), "unknown key is a plain miss");

    // palette mismatch without wildcard: reason reported
    Store st2;
    std::vector<std::string> logs2;
    write(hosttex + "/replacements.tsv",
          "eod-host-textures 1\nb0dfc9176c7f868a\t205f0e8a058538d6\t16\t16\tsky/a.eodhtx\tX#a\tpkg\n");
    check(st2.load(root, [&](const std::string &s) { logs2.push_back(s); }) == 1, "single entry");
    why.clear();
    check(st2.find(0xb0dfc9176c7f868aull, 1, 16, 16, why) == -1 && why == "paletteKey differs",
          "palette mismatch not bound, reason given");

    // CPU prefetch
    st2.prefetch(1ull << 30, [&](const std::string &s) { logs2.push_back(s); });
    st2.wait_prefetch();
    check(st2.prefetched_bytes() == st2.entries()[0].bytes, "prefetch makes the file resident");
    Store st3;
    st3.load(root, log);
    st3.prefetch(10, log);  // below one file: nothing prefetched
    st3.wait_prefetch();
    check(st3.prefetched_bytes() == 0, "CPU budget respected");

    // wrong header line: nothing loaded
    Store st4;
    write(hosttex + "/replacements.tsv", "eod-host-textures 2\n");
    check(st4.load(root, log) == 0, "unknown manifest version rejected");

    for (const char *f : {"sky/a.eodhtx", "sky/b.eodhtx", "sky/c.eodhtx", "sky/w.eodhtx", "replacements.tsv"})
        unlink((hosttex + "/" + f).c_str());
    rmdir((hosttex + "/sky").c_str());
    rmdir(hosttex.c_str());
    rmdir(root.c_str());
    if (g_fail) return 1;
    std::printf("texture replacements: all checks passed\n");
    return 0;
}
