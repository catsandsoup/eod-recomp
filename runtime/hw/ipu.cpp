// ipu.cpp - Image Processing Unit: the MPEG-2 decoding assist used by
// libmpeg (sceMpeg) for the game's movies.
//
// The IPU decodes from an 8-quadword input FIFO (fed by DMA channel 4,
// toIPU) into an 8-quadword output FIFO (drained by DMA channel 3,
// fromIPU).  Commands block when the input runs dry or the output fills
// up, exactly like the hardware; here a command runs on its own host fiber
// and is suspended in those situations, resuming when DMA or the EE moves
// data.  The EE software does the picture level work (headers, motion
// compensation); the IPU does bitstream parsing, VLC decoding, inverse
// quantisation, IDCT and colour space conversion.
//
// Registers (0x10002000):
//   IPU_CMD  +0x00  write: command;  read: DATA (+0x04 bit 31 = BUSY)
//   IPU_CTRL +0x10  IFC 0-3, OFC 4-7, CBP 8-13, ECD 14, SCD 15, IDP 16-17,
//                   AS 20, IVF 21, QST 22, MP1 23, PCT 24-26, RST 30, BUSY 31
//   IPU_BP   +0x20  BP 0-6, IFC 8-11, FP 16-17
//   IPU_TOP  +0x30  next 32 bits of the bitstream (+0x34 bit 31 = BUSY)
#include "ipu.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "../core/log.h"
#include "../kernel/kernel.h"
#include "../platform/platform.h"
#include "hw.h"

namespace {

// ------------------------------------------------------------ VLC tables
struct VlcCode {
    uint32_t code;
    int len, a, b;
};
#include "ipu_vlc.inc"

struct VlcEnt {
    int16_t a;
    int8_t b;
    uint8_t len;  // 0 = invalid code
};

struct Vlc {
    int bits = 0;
    std::vector<VlcEnt> t;
    void build(const VlcCode *c, int nbits) {
        bits = nbits;
        t.assign(1u << nbits, VlcEnt{0, 0, 0});
        for (; c->len; c++) {
            uint32_t base = c->code << (nbits - c->len), n = 1u << (nbits - c->len);
            for (uint32_t i = 0; i < n; i++) t[base + i] = VlcEnt{(int16_t)c->a, (int8_t)c->b, (uint8_t)c->len};
        }
    }
};

Vlc g_mb_i, g_mb_p, g_mb_b, g_motion, g_dmv, g_cbp_tab, g_mba, g_dc_lum, g_dc_chrom, g_dct_first, g_dct_next, g_dct_b15;

const uint8_t kZigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                             12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                             35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                             58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
const uint8_t kAltScan[64] = {0,  8,  16, 24, 1,  9,  2,  10, 17, 25, 32, 40, 48, 56, 57, 49,
                              41, 33, 26, 18, 3,  11, 4,  12, 19, 27, 34, 42, 50, 58, 35, 43,
                              51, 59, 20, 28, 5,  13, 6,  14, 21, 29, 36, 44, 52, 60, 37, 45,
                              53, 61, 22, 30, 7,  15, 23, 31, 38, 46, 54, 62, 39, 47, 55, 63};
const int kNonLinearQ[32] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  10, 12, 14, 16, 18,  20,  22,
                             24, 28, 32, 36, 40, 44, 48, 52, 56, 64, 72, 80, 88, 96, 104, 112};

enum { MB_INTRA = 1, MB_PATTERN = 2, MB_BACKWARD = 4, MB_FORWARD = 8, MB_QUANT = 16, DCT_TYPE_INTERLACED = 32 };
enum { MOTION_TYPE_BASE = 64, MC_FRAME = 128 };

// --------------------------------------------------------------- state
enum : uint32_t {
    CTRL_ECD = 1u << 14,
    CTRL_SCD = 1u << 15,
    CTRL_RST = 1u << 30,
};

uint32_t g_ctrl;  // IDP/AS/IVF/QST/MP1/PCT/ECD/SCD (the rest is computed)
uint32_t g_cmd;   // command in progress
uint32_t g_data;  // IPU_CMD.DATA
uint32_t g_top;
bool g_busy, g_top_busy;
int g_cbp;
uint16_t g_th0, g_th1;
uint16_t g_vqclut[16];
uint8_t g_iq[64], g_niq[64];

// decoder parameters of the current command
struct Decoder {
    bool mpeg1, q_scale_type, intra_vlc_format, alt_scan;
    int intra_dc_precision;
    int quantizer_scale;
    int dc_pred[3];
} g_dec;

// FIFOs
uint8_t g_in[8][16];
int g_in_rd, g_in_n;
uint8_t g_out[8][16];
int g_out_rd, g_out_n;

// internal bit buffer: two quadwords taken from the input FIFO
uint8_t g_ib[48];
uint32_t g_bp;  // bit position in g_ib (0-255)
int g_fp;       // quadwords held in g_ib (0-2)

// command fiber
HostFiber *g_fiber, *g_caller;
bool g_running;  // a command is executing (possibly suspended)
bool g_waiting;  // the command is suspended waiting for data or FIFO space

void ipu_fiber_main(void *);

void wait() {
    g_waiting = true;
    fiber_switch(g_caller);
    g_waiting = false;
}

// ------------------------------------------------------------ FIFOs
void in_refill() {
    if (g_in_n >= 8 || !dma_ipu_active(DMA_IPU_TO)) return;
    uint8_t tmp[8 * 16];
    uint32_t n = dma_ipu_to_fetch(tmp, 8 - g_in_n);
    for (uint32_t i = 0; i < n; i++) {
        memcpy(g_in[(g_in_rd + g_in_n) & 7], tmp + i * 16, 16);
        g_in_n++;
    }
}

bool in_pop(uint8_t *dst) {
    if (!g_in_n) in_refill();
    if (!g_in_n) return false;
    memcpy(dst, g_in[g_in_rd], 16);
    g_in_rd = (g_in_rd + 1) & 7;
    g_in_n--;
    in_refill();
    return true;
}

void in_clear() {
    g_in_rd = g_in_n = 0;
    memset(g_ib, 0, sizeof g_ib);
    g_fp = 0;
    g_bp = 0;
}

void out_drain() {
    while (g_out_n && dma_ipu_active(DMA_IPU_FROM)) {
        int n = std::min(g_out_n, 8 - g_out_rd);
        uint32_t k = dma_ipu_from_store(g_out[g_out_rd], (uint32_t)n);
        if (!k) break;
        g_out_rd = (g_out_rd + (int)k) & 7;
        g_out_n -= (int)k;
    }
}

void out_write(const void *src, int nqw) {
    const uint8_t *p = static_cast<const uint8_t *>(src);
    for (int i = 0; i < nqw; i++) {
        while (g_out_n == 8) {
            out_drain();
            if (g_out_n == 8) wait();
        }
        memcpy(g_out[(g_out_rd + g_out_n) & 7], p + i * 16, 16);
        g_out_n++;
    }
    out_drain();
}

// ------------------------------------------------------------ bitstream
// Makes `bits` bits available after the bit pointer; false if the FIFO ran dry.
bool fill(uint32_t bits) {
    while ((uint32_t)g_fp * 128 < g_bp + bits) {
        if (!in_pop(g_ib + g_fp * 16)) return false;
        g_fp++;
    }
    return true;
}

void need(uint32_t bits) {
    while (!fill(bits)) wait();
}

inline uint32_t peek_raw(int n) {
    const uint8_t *p = g_ib + (g_bp >> 3);
    uint64_t v = ((uint64_t)p[0] << 32) | ((uint64_t)p[1] << 24) | ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 8) | p[4];
    v = (v << (g_bp & 7)) & 0xFFFFFFFFFFull;
    return (uint32_t)(v >> (40 - n));
}

uint32_t peek(int n) {
    need((uint32_t)n);
    return peek_raw(n);
}

int32_t speek(int n) {
    uint32_t v = peek(n);
    return (int32_t)(v << (32 - n)) >> (32 - n);
}

void skip(uint32_t n) {
    g_bp += n;
    while (g_bp >= 128) {
        g_bp -= 128;
        if (g_fp == 2) {
            memcpy(g_ib, g_ib + 16, 16);
            g_fp = 1;
        } else {
            g_fp = in_pop(g_ib) ? 1 : 0;
        }
    }
}

uint32_t getbits(int n) {
    uint32_t v = peek(n);
    skip((uint32_t)n);
    return v;
}

// Decodes one code; returns nullptr for an invalid code.
const VlcEnt *vlc(const Vlc &t) {
    const VlcEnt &e = t.t[peek(t.bits)];
    if (!e.len) return nullptr;
    skip(e.len);
    return &e;
}

void read_bytes(uint8_t *dst, int n) {
    while (n > 0) {
        need(8);
        if (g_bp & 7) {
            *dst++ = (uint8_t)getbits(8);
            n--;
            continue;
        }
        // byte aligned: copy what the internal buffer holds
        int k = std::min<int>(n, (int)((uint32_t)g_fp * 128 - g_bp) / 8);
        memcpy(dst, g_ib + g_bp / 8, (size_t)k);
        skip((uint32_t)k * 8);
        dst += k;
        n -= k;
    }
}

// ------------------------------------------------------------ IDCT
// Integer IDCT (the classic 2048-scaled separable form), in place.
inline void butterfly(int &t0, int &t1, int w0, int w1, int d0, int d1) {
    int tmp = w0 * (d0 + d1);
    t0 = tmp + (w1 - w0) * d1;
    t1 = tmp - (w1 + w0) * d0;
}

void idct(int16_t *blk) {
    const int W1 = 2841, W2 = 2676, W3 = 2408, W5 = 1609, W6 = 1108, W7 = 565;
    for (int i = 0; i < 8; i++) {
        int16_t *r = blk + 8 * i;
        if (!(r[1] | r[2] | r[3] | r[4] | r[5] | r[6] | r[7])) {
            int16_t v = (int16_t)(r[0] << 3);
            for (int k = 0; k < 8; k++) r[k] = v;
            continue;
        }
        int a0, a1, a2, a3, b0, b1, b2, b3;
        {
            int d0 = (r[0] << 11) + 128, d1 = r[2], d2 = r[4] << 11, d3 = r[6];
            int t0 = d0 + d2, t1 = d0 - d2, t2, t3;
            butterfly(t2, t3, W6, W2, d3, d1);
            a0 = t0 + t2;
            a1 = t1 + t3;
            a2 = t1 - t3;
            a3 = t0 - t2;
        }
        {
            int d0 = r[1], d1 = r[3], d2 = r[5], d3 = r[7];
            int t0, t1, t2, t3;
            butterfly(t0, t1, W7, W1, d3, d0);
            butterfly(t2, t3, W3, W5, d1, d2);
            b0 = t0 + t2;
            b3 = t1 + t3;
            t0 -= t2;
            t1 -= t3;
            b1 = ((t0 + t1) * 181) >> 8;
            b2 = ((t0 - t1) * 181) >> 8;
        }
        r[0] = (int16_t)((a0 + b0) >> 8);
        r[1] = (int16_t)((a1 + b1) >> 8);
        r[2] = (int16_t)((a2 + b2) >> 8);
        r[3] = (int16_t)((a3 + b3) >> 8);
        r[4] = (int16_t)((a3 - b3) >> 8);
        r[5] = (int16_t)((a2 - b2) >> 8);
        r[6] = (int16_t)((a1 - b1) >> 8);
        r[7] = (int16_t)((a0 - b0) >> 8);
    }
    for (int i = 0; i < 8; i++) {
        int16_t *c = blk + i;
        int a0, a1, a2, a3, b0, b1, b2, b3;
        {
            int d0 = (c[0] << 11) + 65536, d1 = c[16], d2 = c[32] << 11, d3 = c[48];
            int t0 = d0 + d2, t1 = d0 - d2, t2, t3;
            butterfly(t2, t3, W6, W2, d3, d1);
            a0 = t0 + t2;
            a1 = t1 + t3;
            a2 = t1 - t3;
            a3 = t0 - t2;
        }
        {
            int d0 = c[8], d1 = c[24], d2 = c[40], d3 = c[56];
            int t0, t1, t2, t3;
            butterfly(t0, t1, W7, W1, d3, d0);
            butterfly(t2, t3, W3, W5, d1, d2);
            b0 = t0 + t2;
            b3 = t1 + t3;
            t0 = (t0 - t2) >> 8;
            t1 = (t1 - t3) >> 8;
            b1 = (t0 + t1) * 181;
            b2 = (t0 - t1) * 181;
        }
        c[0] = (int16_t)((a0 + b0) >> 17);
        c[8] = (int16_t)((a1 + b1) >> 17);
        c[16] = (int16_t)((a2 + b2) >> 17);
        c[24] = (int16_t)((a3 + b3) >> 17);
        c[32] = (int16_t)((a3 - b3) >> 17);
        c[40] = (int16_t)((a2 - b2) >> 17);
        c[48] = (int16_t)((a1 - b1) >> 17);
        c[56] = (int16_t)((a0 - b0) >> 17);
    }
}

// ------------------------------------------------------------ blocks
inline int saturate(int v) { return v < -2048 ? -2048 : v > 2047 ? 2047 : v; }

int dc_diff(bool chroma) {
    const VlcEnt *e = vlc(chroma ? g_dc_chrom : g_dc_lum);
    if (!e) {
        g_ctrl |= CTRL_ECD;
        return 0;
    }
    int size = e->a;
    if (!size) return 0;
    int d = (int)getbits(size);
    if (!(d & (1 << (size - 1)))) d -= (1 << size) - 1;
    return d;
}

// Intra block: DC + AC coefficients (dequantised) into blk.
void intra_block(int cc, int16_t *blk) {
    memset(blk, 0, 64 * sizeof(int16_t));
    Decoder &d = g_dec;
    d.dc_pred[cc] += dc_diff(cc != 0);
    blk[0] = (int16_t)(d.dc_pred[cc] << (3 - d.intra_dc_precision));
    const uint8_t *scan = d.alt_scan ? kAltScan : kZigzag;
    const Vlc &tab = (d.intra_vlc_format && !d.mpeg1) ? g_dct_b15 : g_dct_next;
    int qs = d.quantizer_scale;
    for (int i = 1;; i++) {
        const VlcEnt *e = vlc(tab);
        if (!e) {
            g_ctrl |= CTRL_ECD;
            return;
        }
        if (e->a == 64) return;  // end of block
        int val;
        if (e->a == 65) {  // escape
            i += (int)getbits(6);
            if (i >= 64) return;
            if (!d.mpeg1) {
                val = (speek(12) * qs * g_iq[i]) >> 4;
                skip(12);
            } else {
                val = speek(8);
                skip(8);
                if (!(val & 0x7F)) val = (int)getbits(8) + 2 * val;
                val = (val * qs * g_iq[i]) >> 4;
                val = (val + ~(val >> 31)) | 1;
            }
        } else {
            i += e->a;
            if (i >= 64) return;
            val = (e->b * qs * g_iq[i]) >> 4;
            if (d.mpeg1) val = (val - 1) | 1;
            if (getbits(1)) val = -val;
        }
        blk[scan[i]] = (int16_t)saturate(val);
    }
}

// Non-intra block: AC coefficients (dequantised) into blk.
void non_intra_block(int16_t *blk) {
    memset(blk, 0, 64 * sizeof(int16_t));
    Decoder &d = g_dec;
    const uint8_t *scan = d.alt_scan ? kAltScan : kZigzag;
    int qs = d.quantizer_scale;
    for (int i = 0;; i++) {
        const VlcEnt *e = vlc(i == 0 ? g_dct_first : g_dct_next);
        if (!e) {
            g_ctrl |= CTRL_ECD;
            return;
        }
        if (e->a == 64) return;
        int val;
        if (e->a == 65) {
            i += (int)getbits(6);
            if (i >= 64) return;
            if (!d.mpeg1) {
                int lv = speek(12);
                val = ((2 * lv + (lv < 0 ? -1 : 0) + 1) * qs * g_niq[i]) >> 5;
                skip(12);
            } else {
                val = speek(8);
                skip(8);
                if (!(val & 0x7F)) val = (int)getbits(8) + 2 * val;
                val = ((2 * (val + (val >> 31)) + 1) * qs * g_niq[i]) / 32;
                val = (val + ~(val >> 31)) | 1;
            }
        } else {
            i += e->a;
            if (i >= 64) return;
            val = ((2 * e->b + 1) * qs * g_niq[i]) >> 5;
            if (getbits(1)) val = -val;
        }
        blk[scan[i]] = (int16_t)saturate(val);
    }
}

// ------------------------------------------------------------ macroblocks
struct MB8 {
    uint8_t y[16][16], cb[8][8], cr[8][8];
};
struct MB16 {
    int16_t y[16][16], cb[8][8], cr[8][8];
};

inline uint8_t clip8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

// Places a decoded luma block (n = 0..3) of a macroblock.
template <class T, class F>
void put_luma(T (&y)[16][16], int n, bool field_dct, const int16_t *blk, F conv) {
    int x0 = (n & 1) * 8;
    for (int r = 0; r < 8; r++) {
        int row = field_dct ? (n >> 1) + r * 2 : (n >> 1) * 8 + r;
        for (int c = 0; c < 8; c++) y[row][x0 + c] = conv(blk[r * 8 + c]);
    }
}

void decode_intra_mb(MB8 &mb, bool field_dct) {
    int16_t blk[64];
    auto conv = [](int16_t v) { return clip8(v); };
    for (int n = 0; n < 4; n++) {
        intra_block(0, blk);
        idct(blk);
        put_luma(mb.y, n, field_dct, blk, conv);
    }
    intra_block(1, blk);
    idct(blk);
    for (int i = 0; i < 64; i++) mb.cb[i >> 3][i & 7] = clip8(blk[i]);
    intra_block(2, blk);
    idct(blk);
    for (int i = 0; i < 64; i++) mb.cr[i >> 3][i & 7] = clip8(blk[i]);
}

void reset_dc() {
    int v = 128 << g_dec.intra_dc_precision;
    g_dec.dc_pred[0] = g_dec.dc_pred[1] = g_dec.dc_pred[2] = v;
}

int qscale(int code) { return g_dec.q_scale_type ? kNonLinearQ[code & 31] : (code & 31) << 1; }

void load_decoder_ctrl() {
    g_dec.mpeg1 = (g_ctrl >> 23) & 1;
    g_dec.q_scale_type = (g_ctrl >> 22) & 1;
    g_dec.intra_vlc_format = (g_ctrl >> 21) & 1;
    g_dec.alt_scan = (g_ctrl >> 20) & 1;
    g_dec.intra_dc_precision = (g_ctrl >> 16) & 3;
}

// YCbCr -> RGBA32 with the IPU's coefficients and alpha thresholds.
void csc(const MB8 &mb, uint32_t (&rgb)[16][16], bool sgn) {
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            int cb = mb.cb[y >> 1][x >> 1] - 128, cr = mb.cr[y >> 1][x >> 1] - 128;
            int lum = (0x95 * std::max(0, mb.y[y][x] - 16)) >> 6;
            int r = (lum + ((0xCC * cr) >> 6) + 1) >> 1;
            int g = (lum + ((-0x68 * cr) >> 6) + ((-0x32 * cb) >> 6) + 1) >> 1;
            int b = (lum + ((0x102 * cb) >> 6) + 1) >> 1;
            uint32_t R = clip8(r), G = clip8(g), B = clip8(b), A = 0x80;
            if (g_th0 && R < g_th0 && G < g_th0 && B < g_th0) R = G = B = A = 0;
            else if (g_th1 && R < g_th1 && G < g_th1 && B < g_th1) A = 0x40;
            uint32_t px = R | (G << 8) | (B << 16) | (A << 24);
            if (sgn) px ^= 0x808080;
            rgb[y][x] = px;
        }
}

void to_rgb16(const uint32_t (&rgb)[16][16], uint16_t (&out)[16][16], bool dither) {
    static const int kDither[4][4] = {{-4, 2, -3, 3}, {0, -2, 1, -1}, {-3, 3, -4, 2}, {1, -1, 0, -2}};
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            uint32_t p = rgb[y][x];
            int r = p & 255, g = (p >> 8) & 255, b = (p >> 16) & 255, a = p >> 24;
            if (dither) {
                int d = kDither[y & 3][x & 3];
                r = clip8(r + d);
                g = clip8(g + d);
                b = clip8(b + d);
            }
            out[y][x] = (uint16_t)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | (a ? 0x8000 : 0));
        }
}

// After IDEC/BDEC: look for a start code if the next byte is zero, then
// latch the next 32 bits into TOP.
void end_of_decode() {
    if (peek(8) == 0) {
        skip((8 - (g_bp & 7)) & 7);  // byte align
        for (;;) {
            uint32_t v = peek(24);
            if (v) {
                g_ctrl |= v == 1 ? CTRL_SCD : CTRL_ECD;
                break;
            }
            skip(8);
        }
    }
    g_top = peek(32);
}

// ------------------------------------------------------------ commands
void cmd_idec(uint32_t v) {
    load_decoder_ctrl();
    g_ctrl = (g_ctrl & ~(7u << 24)) | (1u << 24);  // PCT = I
    g_dec.quantizer_scale = qscale((int)((v >> 16) & 31));
    bool dtd = (v >> 24) & 1, sgn = (v >> 25) & 1, dte = (v >> 26) & 1, ofm = (v >> 27) & 1;
    reset_dc();
    g_top = 0;
    for (;;) {
        const VlcEnt *e = vlc(g_mb_i);
        if (!e) {
            g_ctrl |= CTRL_ECD;
            break;
        }
        int modes = e->a;
        bool field_dct = dtd && getbits(1);
        if (modes & MB_QUANT) g_dec.quantizer_scale = qscale((int)getbits(5));
        MB8 mb;
        decode_intra_mb(mb, field_dct);
        uint32_t rgb[16][16];
        csc(mb, rgb, sgn);
        if (ofm) {
            uint16_t rgb16[16][16];
            to_rgb16(rgb, rgb16, dte);
            out_write(rgb16, 32);
        } else {
            out_write(rgb, 64);
        }
        // macroblock_address_increment (escape/stuffing), or the end of the slice
        int incr = 0;
        bool end = false;
        for (;;) {
            uint32_t code = peek(16);
            if (code >= 0x0300) {
                const VlcEnt *m = vlc(g_mba);
                incr += m ? m->a : 0;
                break;
            }
            uint32_t c11 = peek(11);
            if (c11 == 8) {
                incr += 33;
                skip(11);
            } else if (c11 == 15) {
                skip(11);
            } else {
                end = true;
                break;
            }
        }
        if (end) break;
        if (incr) reset_dc();  // skipped macroblocks
    }
    g_cbp = 0x3F;
    g_ctrl &= ~CTRL_SCD;
    end_of_decode();
}

void cmd_bdec(uint32_t v) {
    load_decoder_ctrl();
    g_dec.quantizer_scale = qscale((int)((v >> 16) & 31));
    bool dt = (v >> 25) & 1, dcr = (v >> 26) & 1, mbi = (v >> 27) & 1;
    if (dcr) reset_dc();
    g_top = 0;
    MB16 out;
    memset(&out, 0, sizeof out);
    int16_t blk[64];
    if (mbi) {
        MB8 mb;
        decode_intra_mb(mb, dt);
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) out.y[y][x] = mb.y[y][x];
        for (int i = 0; i < 64; i++) {
            out.cb[i >> 3][i & 7] = mb.cb[i >> 3][i & 7];
            out.cr[i >> 3][i & 7] = mb.cr[i >> 3][i & 7];
        }
        g_cbp = 0x3F;
    } else {
        const VlcEnt *e = vlc(g_cbp_tab);
        int cbp = e ? e->a : 0;
        if (!e) g_ctrl |= CTRL_ECD;
        auto conv = [](int16_t s) { return s; };
        for (int n = 0; n < 4; n++)
            if (cbp & (0x20 >> n)) {
                non_intra_block(blk);
                idct(blk);
                put_luma(out.y, n, dt, blk, conv);
            }
        if (cbp & 2) {
            non_intra_block(blk);
            idct(blk);
            memcpy(out.cb, blk, sizeof out.cb);
        }
        if (cbp & 1) {
            non_intra_block(blk);
            idct(blk);
            memcpy(out.cr, blk, sizeof out.cr);
        }
        g_cbp = cbp;
    }
    g_ctrl &= ~CTRL_SCD;
    out_write(&out, (int)(sizeof out / 16));
    end_of_decode();
}

void cmd_vdec(uint32_t v) {
    int tbl = (v >> 26) & 3;
    load_decoder_ctrl();
    uint32_t data = 0;
    switch (tbl) {
    case 0: {  // macroblock address increment
        uint32_t code = peek(16);
        if (code >= 0x0300) {
            const VlcEnt *e = vlc(g_mba);
            data = e ? (uint32_t)(e->a + 1) | ((uint32_t)e->len << 16) : 0;
        } else {
            uint32_t c11 = peek(11);
            if (c11 == 8) {
                skip(11);
                data = 0xB0023;  // escape: +33
            } else if (c11 == 15 && g_dec.mpeg1) {
                skip(11);
                data = 0xB0022;  // stuffing
            }
        }
        break;
    }
    case 1: {  // macroblock type for the picture type in CTRL.PCT
        int pct = (g_ctrl >> 24) & 7;
        if (!pct) pct = 1;
        const Vlc *t = pct == 1 ? &g_mb_i : pct == 2 ? &g_mb_p : pct == 3 ? &g_mb_b : nullptr;
        if (!t) {  // D picture
            data = getbits(1) ? (MB_INTRA | (1u << 16)) : 0;
            break;
        }
        const VlcEnt *e = vlc(*t);
        if (!e) break;
        // frame pictures with frame_pred_frame_dct: frame motion for predicted
        // macroblocks; only B types report the code length
        int modes = e->a;
        if (pct == 2 && (modes & MB_FORWARD)) modes |= MC_FRAME;
        if (pct == 3) modes |= MC_FRAME | (e->len << 16);
        data = (uint32_t)modes;
        break;
    }
    case 2: {  // motion code
        if (peek(1)) {
            skip(1);
            data = 0x00010000;
            break;
        }
        const VlcEnt *e = vlc(g_motion);
        if (!e) break;
        int delta = e->a + 1;
        int len = e->len;
        if (getbits(1)) delta = -delta;
        data = (uint32_t)delta | ((uint32_t)len << 16);  // negative values keep their sign bits
        break;
    }
    case 3: {  // dmvector
        const VlcEnt *e = vlc(g_dmv);
        if (e) data = (uint32_t)(int32_t)e->a | ((uint32_t)e->len << 16);
        break;
    }
    }
    g_data = data;
    if (!data) g_ctrl |= CTRL_ECD;
    g_top = peek(32);
}

void execute(uint32_t v) {
    switch (v >> 28) {
    case 1:  // IDEC
        need(v & 63);
        skip(v & 63);
        cmd_idec(v);
        break;
    case 2:  // BDEC
        need(v & 63);
        skip(v & 63);
        cmd_bdec(v);
        break;
    case 3:  // VDEC
        need(v & 63);
        skip(v & 63);
        cmd_vdec(v);
        break;
    case 4:  // FDEC
        need(v & 63);
        skip(v & 63);
        g_data = g_top = peek(32);
        break;
    case 5:  // SETIQ
        need(v & 63);
        skip(v & 63);
        read_bytes((v >> 27) & 1 ? g_niq : g_iq, 64);
        break;
    case 6: {  // SETVQ
        uint8_t b[32];
        read_bytes(b, 32);
        memcpy(g_vqclut, b, 32);
        break;
    }
    case 7: {  // CSC: RAW8 macroblocks -> RGB32/RGB16
        int mbc = (int)(v & 0x7FF);
        bool dte = (v >> 26) & 1, ofm = (v >> 27) & 1;
        for (int i = 0; i < mbc; i++) {
            MB8 mb;
            read_bytes(&mb.y[0][0], 384);
            uint32_t rgb[16][16];
            csc(mb, rgb, false);
            if (ofm) {
                uint16_t rgb16[16][16];
                to_rgb16(rgb, rgb16, dte);
                out_write(rgb16, 32);
            } else {
                out_write(rgb, 64);
            }
        }
        break;
    }
    case 8: {  // PACK: RGB32 -> RGB16 / INDX4
        int mbc = (int)(v & 0x7FF);
        bool dte = (v >> 26) & 1, ofm = (v >> 27) & 1;
        for (int i = 0; i < mbc; i++) {
            uint32_t rgb[16][16];
            read_bytes(reinterpret_cast<uint8_t *>(rgb), 1024);
            uint16_t rgb16[16][16];
            to_rgb16(rgb, rgb16, dte);
            if (ofm) {
                out_write(rgb16, 32);
                continue;
            }
            uint8_t idx[128];
            for (int y = 0; y < 16; y++)
                for (int x = 0; x < 16; x++) {
                    uint16_t p = rgb16[y][x];
                    int best = 0, bd = 1 << 30;
                    for (int k = 0; k < 16; k++) {
                        int dr = (p & 31) - (g_vqclut[k] & 31), dg = ((p >> 5) & 31) - ((g_vqclut[k] >> 5) & 31),
                            db = ((p >> 10) & 31) - ((g_vqclut[k] >> 10) & 31);
                        int dist = dr * dr + dg * dg + db * db;
                        if (dist < bd) bd = dist, best = k;
                    }
                    if (x & 1) idx[y * 8 + (x >> 1)] |= (uint8_t)(best << 4);
                    else idx[y * 8 + (x >> 1)] = (uint8_t)best;
                }
            out_write(idx, 8);
        }
        break;
    }
    default:
        WARN("IPU: command %08x not implemented", v);
        break;
    }
}

void ipu_fiber_main(void *) {
    for (;;) {
        execute(g_cmd);
        g_busy = false;
        g_top_busy = false;
        g_running = false;
        intc_raise(INTC_IPU);
        fiber_switch(g_caller);
    }
}

// Runs the command fiber until it finishes or blocks.
void run() {
    if (!g_running || fiber_current() == g_fiber) return;
    g_caller = fiber_current();
    fiber_switch(g_fiber);
}

void abort_command() {
    if (!g_running) return;
    WARN("IPU: command %08x interrupted", g_cmd);
    fiber_delete(g_fiber);
    g_fiber = fiber_create(256 * 1024, ipu_fiber_main, nullptr);
    g_running = g_waiting = false;
    g_busy = g_top_busy = false;
}

void reset() {
    abort_command();
    in_clear();
    g_out_rd = g_out_n = 0;
    g_cbp = 0;
    g_th0 = g_th1 = 0;
    g_ctrl &= 0x7F33F00u & ~(CTRL_ECD | CTRL_SCD);
    g_top = 0;
    g_data = 0;
    reset_dc();
}

}  // namespace

void ipu_init() {
    VlcCode motion[32];
    int n = 0;
    for (const VlcCode *c = kMotion; c->len; c++) motion[n++] = *c;
    motion[n] = VlcCode{0, 0, 0, 0};
    g_mb_i.build(kMbI, 2);
    g_mb_p.build(kMbP, 6);
    g_mb_b.build(kMbB, 6);
    g_motion.build(motion, 10);
    g_dmv.build(kDmv, 2);
    g_cbp_tab.build(kCbp, 9);
    g_mba.build(kMba, 11);
    g_dc_lum.build(kDcLum, 9);
    g_dc_chrom.build(kDcChrom, 10);
    g_dct_first.build(kDctFirst, 16);
    g_dct_next.build(kDctNext, 16);
    g_dct_b15.build(kDctB15, 16);
    g_fiber = fiber_create(256 * 1024, ipu_fiber_main, nullptr);
    reset();
}

uint32_t ipu_read32(uint32_t off) {
    switch (off & 0xFF) {
    case 0x00: {  // IPU_CMD.DATA: VDEC/FDEC result, else the next 32 bits of the bitstream
        uint32_t op = g_cmd >> 28;
        if (op != 3 && op != 4 && !g_running && fill(32)) g_data = peek_raw(32);
        return g_data;
    }
    case 0x04:
        return (g_busy && ((g_cmd >> 28) == 3 || (g_cmd >> 28) == 4)) ? 0x80000000u : 0;
    case 0x10: {  // IPU_CTRL
        uint32_t v = g_ctrl & ~0xFFFFu;
        v |= (uint32_t)g_in_n | ((uint32_t)g_out_n << 4) | ((uint32_t)g_cbp << 8) | (g_ctrl & (CTRL_ECD | CTRL_SCD));
        if (g_busy) v |= 0x80000000u;
        return v;
    }
    case 0x20:  // IPU_BP
        return (g_bp & 0x7F) | ((uint32_t)g_in_n << 8) | ((uint32_t)g_fp << 16);
    case 0x30:  // IPU_TOP
        if (!g_running) {
            if (fill(32)) {
                g_top = peek_raw(32);
                g_top_busy = false;
            } else {
                g_top_busy = true;
            }
        }
        return g_top;
    case 0x34:
        return (g_top_busy || g_running) ? 0x80000000u : 0;
    }
    return 0;
}

void ipu_write32(uint32_t off, uint32_t v) {
    switch (off & 0xFF) {
    case 0x00: {  // IPU_CMD
        abort_command();
        g_ctrl &= ~(CTRL_ECD | CTRL_SCD);
        g_cmd = v;
        uint32_t op = v >> 28;
        LOG(LOG_HW, "IPU command %08x", v);
        if (op == 0) {  // BCLR
            in_clear();
            g_bp = v & 0x7F;
            in_refill();
            intc_raise(INTC_IPU);
            return;
        }
        if (op == 9) {  // SETTH
            g_th0 = v & 0x1FF;
            g_th1 = (v >> 16) & 0x1FF;
            intc_raise(INTC_IPU);
            return;
        }
        if (op > 9) {
            WARN("IPU: invalid command %08x", v);
            return;
        }
        g_busy = true;
        g_top_busy = true;
        g_running = true;
        run();
        return;
    }
    case 0x10:  // IPU_CTRL
        g_ctrl = (v & 0x47F30000u) | (g_ctrl & 0x8000FFFFu);
        if (((g_ctrl >> 16) & 3) == 3) g_ctrl &= ~(2u << 16);
        if (v & CTRL_RST) {
            reset();
            g_ctrl &= ~CTRL_RST;
        }
        return;
    }
    LOG(LOG_HW, "IPU write %04x = %08x", off, v);
}

void ipu_in_fifo(const uint8_t *data, uint32_t qwc) {
    for (uint32_t i = 0; i < qwc && g_in_n < 8; i++) {
        memcpy(g_in[(g_in_rd + g_in_n) & 7], data + i * 16, 16);
        g_in_n++;
    }
    if (g_waiting) run();
}

uint32_t ipu_out_fifo(uint8_t *dst, uint32_t qwc) {
    uint32_t n = 0;
    while (n < qwc && g_out_n) {
        memcpy(dst + n * 16, g_out[g_out_rd], 16);
        g_out_rd = (g_out_rd + 1) & 7;
        g_out_n--;
        n++;
    }
    if (g_waiting) run();
    return n;
}

void ipu_dma_kick() {
    in_refill();
    out_drain();
    if (g_waiting) run();
}
