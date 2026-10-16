// sse_compat.h - the SSE/SSE2 intrinsics the runtime uses, on hosts without
// them (arm64: Apple Silicon), implemented with NEON.
//
// Only the intrinsics this code base calls are here, and each one keeps the
// x86 result bit for bit where the two architectures differ: cvttps returns
// 0x80000000 for NaN and out-of-range lanes (NEON saturates and gives 0 for
// NaN), and the MXCSR calls map DAZ/FTZ onto FPCR.FZ.
#pragma once

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#include <xmmintrin.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#include <stdint.h>

typedef float32x4_t __m128;
typedef int32x4_t __m128i;

#define SSEC_INLINE static inline __attribute__((always_inline))

// ---- loads, stores, constants
SSEC_INLINE __m128 _mm_loadu_ps(const float *p) { return vld1q_f32(p); }
SSEC_INLINE __m128 _mm_load_ps(const float *p) { return vld1q_f32(p); }
SSEC_INLINE void _mm_storeu_ps(float *p, __m128 v) { vst1q_f32(p, v); }
SSEC_INLINE __m128i _mm_loadu_si128(const __m128i *p) { return vld1q_s32((const int32_t *)p); }
SSEC_INLINE __m128 _mm_set1_ps(float f) { return vdupq_n_f32(f); }
SSEC_INLINE __m128i _mm_set1_epi32(int v) { return vdupq_n_s32(v); }
SSEC_INLINE __m128i _mm_set1_epi16(short v) { return vreinterpretq_s32_s16(vdupq_n_s16(v)); }
SSEC_INLINE __m128i _mm_setzero_si128(void) { return vdupq_n_s32(0); }
SSEC_INLINE __m128i _mm_cvtsi32_si128(int v) { return vsetq_lane_s32(v, vdupq_n_s32(0), 0); }
SSEC_INLINE int _mm_cvtsi128_si32(__m128i v) { return vgetq_lane_s32(v, 0); }

// ---- casts
SSEC_INLINE __m128 _mm_castsi128_ps(__m128i v) { return vreinterpretq_f32_s32(v); }
SSEC_INLINE __m128i _mm_castps_si128(__m128 v) { return vreinterpretq_s32_f32(v); }

// ---- float arithmetic (IEEE single, as SSE; FZ follows _mm_setcsr below)
SSEC_INLINE __m128 _mm_add_ps(__m128 a, __m128 b) { return vaddq_f32(a, b); }
SSEC_INLINE __m128 _mm_sub_ps(__m128 a, __m128 b) { return vsubq_f32(a, b); }
SSEC_INLINE __m128 _mm_mul_ps(__m128 a, __m128 b) { return vmulq_f32(a, b); }

// ---- bitwise
#define SSEC_U(v) vreinterpretq_u32_f32(v)
#define SSEC_F(v) vreinterpretq_f32_u32(v)
SSEC_INLINE __m128 _mm_and_ps(__m128 a, __m128 b) { return SSEC_F(vandq_u32(SSEC_U(a), SSEC_U(b))); }
SSEC_INLINE __m128 _mm_or_ps(__m128 a, __m128 b) { return SSEC_F(vorrq_u32(SSEC_U(a), SSEC_U(b))); }
SSEC_INLINE __m128 _mm_andnot_ps(__m128 a, __m128 b) { return SSEC_F(vbicq_u32(SSEC_U(b), SSEC_U(a))); }
SSEC_INLINE __m128i _mm_xor_si128(__m128i a, __m128i b) { return veorq_s32(a, b); }

// ---- compares (all-ones lanes), movemask
SSEC_INLINE __m128 _mm_cmpeq_ps(__m128 a, __m128 b) { return SSEC_F(vceqq_f32(a, b)); }
SSEC_INLINE __m128 _mm_cmpge_ps(__m128 a, __m128 b) { return SSEC_F(vcgeq_f32(a, b)); }
SSEC_INLINE int _mm_movemask_ps(__m128 v) {
    uint32x4_t s = vshrq_n_u32(SSEC_U(v), 31);
    return (int)(vgetq_lane_u32(s, 0) | (vgetq_lane_u32(s, 1) << 1) |
                 (vgetq_lane_u32(s, 2) << 2) | (vgetq_lane_u32(s, 3) << 3));
}

// ---- conversions
SSEC_INLINE __m128 _mm_cvtepi32_ps(__m128i v) { return vcvtq_f32_s32(v); }
// x86: NaN and anything outside [-2^31, 2^31) become 0x80000000.
SSEC_INLINE __m128i _mm_cvttps_epi32(__m128 f) {
    int32x4_t r = vcvtq_s32_f32(f);
    uint32x4_t ok = vandq_u32(vcltq_f32(f, vdupq_n_f32(2147483648.0f)),
                              vcgeq_f32(f, vdupq_n_f32(-2147483648.0f)));
    return vbslq_s32(ok, r, vdupq_n_s32((int32_t)0x80000000));
}

// ---- 16-bit integer lanes
#define SSEC_S16(v) vreinterpretq_s16_s32(v)
#define SSEC_I32(v) vreinterpretq_s32_s16(v)
SSEC_INLINE __m128i _mm_add_epi16(__m128i a, __m128i b) { return SSEC_I32(vaddq_s16(SSEC_S16(a), SSEC_S16(b))); }
SSEC_INLINE __m128i _mm_mullo_epi16(__m128i a, __m128i b) { return SSEC_I32(vmulq_s16(SSEC_S16(a), SSEC_S16(b))); }
#define _mm_srli_epi16(a, n) \
    vreinterpretq_s32_u16(vshrq_n_u16(vreinterpretq_u16_s32(a), (n)))
SSEC_INLINE __m128i _mm_packus_epi16(__m128i a, __m128i b) {
    return vreinterpretq_s32_u8(vcombine_u8(vqmovun_s16(SSEC_S16(a)), vqmovun_s16(SSEC_S16(b))));
}
// byte shift of the whole register
#define _mm_srli_si128(a, n) \
    vreinterpretq_s32_u8(vextq_u8(vreinterpretq_u8_s32(a), vdupq_n_u8(0), (n)))

// ---- unpacks (interleave the low halves)
SSEC_INLINE __m128i _mm_unpacklo_epi8(__m128i a, __m128i b) {
    return vreinterpretq_s32_u8(vzip1q_u8(vreinterpretq_u8_s32(a), vreinterpretq_u8_s32(b)));
}
SSEC_INLINE __m128i _mm_unpacklo_epi32(__m128i a, __m128i b) { return vzip1q_s32(a, b); }
SSEC_INLINE __m128i _mm_unpacklo_epi64(__m128i a, __m128i b) {
    return vreinterpretq_s32_s64(vzip1q_s64(vreinterpretq_s64_s32(a), vreinterpretq_s64_s32(b)));
}

// ---- MXCSR: only DAZ (0x0040) and FTZ (0x8000) are used; both are FPCR.FZ
SSEC_INLINE unsigned _mm_getcsr(void) {
    uint64_t fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    return (fpcr & (1u << 24)) ? 0x8040u : 0u;
}
SSEC_INLINE void _mm_setcsr(unsigned csr) {
    uint64_t fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    if (csr & 0x8040u) fpcr |= (1u << 24);
    else fpcr &= ~(uint64_t)(1u << 24);
    __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
}
#undef SSEC_U
#undef SSEC_F
#else
#error "no SSE2 and no NEON: add this host to sse_compat.h"
#endif
