// texture_replacements.h - host high-resolution texture replacement (overlay
// HOSTTEX/, docs/CREATION_KIT_TEXTURE_CONTRACTS.md sections 2 and 4).
//
// Renderer-independent part: manifest parsing, .eodhtx validation and CPU
// residency, the runtime identity keys and alpha-domain conversion. The GPU
// residency budget and binding live with the renderer (gs_vulkan.cpp).
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gs {
namespace hosttex {

// --------------------------------------------------------------- identity
constexpr uint64_t kFnvOffset = 0xCBF29CE484222325ull;
constexpr uint64_t kFnvPrime = 0x100000001B3ull;

inline uint64_t fnv1a64(const void *data, size_t n, uint64_t h = kFnvOffset) {
    const uint8_t *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * kFnvPrime;
    return h;
}
// FNV-1a 64 over "EODTEX1", u8 bits, u32le width, u32le height, then one byte
// per mip-zero palette index in row-major order.
uint64_t index_key(uint32_t bits, uint32_t width, uint32_t height, const uint8_t *indices);
// FNV-1a 64 over R,G,B of entries 0..n-1. Entries are GS RGBA words (R in bits
// 0-7, as the renderer uploads R8G8B8A8_UNORM).
uint64_t palette_key(const uint32_t *pal, uint32_t n);

// ------------------------------------------------------------ file format
struct HtxHeader {
    uint32_t width = 0, height = 0, levels = 0, flags = 0;
};
constexpr size_t kHtxHeaderBytes = 16;
// Byte offset of each level after the header (levels entries) and the total.
size_t htx_level_offset(uint32_t w, uint32_t h, uint32_t level);
size_t htx_total_bytes(uint32_t w, uint32_t h, uint32_t levels);
uint32_t full_chain_levels(uint32_t w, uint32_t h);
// Validates a header against the file size; on failure sets why.
bool parse_htx_header(const uint8_t *p, size_t file_bytes, HtxHeader &out, std::string &why);

// ------------------------------------------------------------ alpha domain
// Native indexed textures decode to the GS alpha domain (0x80 = 1.0) when the
// palette's largest alpha is <= 0x80; replacement alpha is straight 0..255.
inline bool gs_alpha_domain(uint32_t native_alpha_high) { return native_alpha_high <= 0x80; }
inline uint8_t to_gs_alpha(uint8_t a) { return (uint8_t)(((uint32_t)a * 128 + 127) / 255); }
// Copies n RGBA8 texels, converting alpha when gs_domain, and widens
// [alpha_lo, alpha_hi] by the written alpha values.
void convert_texels(const uint8_t *src, uint8_t *dst, size_t n, bool gs_domain, uint32_t &alpha_lo,
                    uint32_t &alpha_hi);

// --------------------------------------------------------------- manifest
struct Entry {
    uint64_t index_key = 0, palette_key = 0;
    bool any_palette = false;
    uint32_t orig_w = 0, orig_h = 0;
    uint32_t w = 0, h = 0, levels = 0, scale = 1;
    std::string file, path, asset_id, package_id;
    int line = 0;
    // file contents (mapped or read); data + kHtxHeaderBytes is level 0
    const uint8_t *data = nullptr;
    size_t bytes = 0;
    bool mapped = false;
};

using LogFn = std::function<void(const std::string &)>;
// Resolves a path relative to a directory (case-insensitively when the
// platform needs it). Returns false when not found.
using FindFn = std::function<bool(const std::string &dir, const std::string &rel, std::string &out)>;

struct Stats {
    uint64_t hits = 0;       // texture binds served by a replacement
    uint64_t misses = 0;     // newly keyed textures with candidate dimensions but no entry
    uint64_t fallbacks = 0;  // matched but native kept (palette, budget, levels, upload)
    uint64_t keyed = 0;      // keys computed
};

class Store {
public:
    ~Store();
    // Parses <root>/HOSTTEX/replacements.tsv. Each accepted entry's file is
    // validated and opened (mapped). Malformed lines are logged and skipped.
    // Returns the number of accepted entries.
    size_t load(const std::string &root, const LogFn &log, const FindFn &find = FindFn());
    // Parses manifest text (exposed for tests). hosttex_dir resolves files.
    size_t parse(const std::string &text, const std::string &hosttex_dir, const LogFn &log, const FindFn &find);
    // Starts prefaulting mapped files into memory in manifest order until
    // cpu_budget bytes are resident (background thread).
    void prefetch(uint64_t cpu_budget, const LogFn &log);
    void wait_prefetch();

    bool empty() const { return entries_.empty(); }
    const std::vector<Entry> &entries() const { return entries_; }
    // Whether any entry has these native dimensions (cheap pre-filter).
    bool has_dims(uint32_t w, uint32_t h) const;
    // Entry for the keys or -1. Exact palette entries win over '*'. On a key
    // match with no usable palette, why names the reason.
    int find(uint64_t index_key, uint64_t palette_key, uint32_t w, uint32_t h, std::string &why) const;

    uint64_t prefetched_bytes() const { return prefetched_; }
    double prefetch_ms() const { return prefetch_ms_; }

private:
    bool open_file(Entry &e, std::string &why);
    void close_all();
    std::vector<Entry> entries_;
    std::vector<uint64_t> dims_;
    std::atomic<uint64_t> prefetched_{0};
    std::atomic<double> prefetch_ms_{0};
    void *thread_ = nullptr;
};

// Hex helpers (16 lowercase digits).
std::string hex64(uint64_t v);
bool parse_hex64(const std::string &s, uint64_t &out);

}  // namespace hosttex
}  // namespace gs
