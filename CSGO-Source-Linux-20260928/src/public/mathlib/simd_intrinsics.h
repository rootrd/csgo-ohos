#ifndef SOURCE_SIMD_INTRINSICS_H
#define SOURCE_SIMD_INTRINSICS_H

#if defined(__aarch64__)
// Pinned sse2neon is supplied by build-android.sh. Keep IEEE edge cases in the
// common SIMD layer; do not compile it with finite-math-only or FP contraction.
#if defined(__FAST_MATH__)
#error "ARM64 SIMD requires strict floating-point semantics"
#endif
#define SSE2NEON_PRECISE_MINMAX 1
#define SSE2NEON_PRECISE_DIV 1
#define SSE2NEON_PRECISE_SQRT 1
#define SSE2NEON_PRECISE_DP 1
#include <sse2neon.h>

// AArch64 FCVT saturates invalid inputs; SSE returns INT32_MIN. Preserve that
// contract for game code that uses the conversion result as a sentinel.
inline __m128i SourceCvtpsEpi32(__m128 value)
{
    const float32x4_t rounded = vrndiq_f32(vreinterpretq_f32_m128(value));
    const uint32x4_t valid = vandq_u32(vcgeq_f32(rounded, vdupq_n_f32(-2147483648.0f)),
                                     vcltq_f32(rounded, vdupq_n_f32(2147483648.0f)));
    return vreinterpretq_m128i_s32(vbslq_s32(valid, vcvtq_s32_f32(rounded), vdupq_n_s32(INT32_MIN)));
}
inline __m128i SourceCvttpsEpi32(__m128 value)
{
    const float32x4_t input = vreinterpretq_f32_m128(value);
    const uint32x4_t valid = vandq_u32(vcgeq_f32(input, vdupq_n_f32(-2147483648.0f)),
                                     vcltq_f32(input, vdupq_n_f32(2147483648.0f)));
    return vreinterpretq_m128i_s32(vbslq_s32(valid, vcvtq_s32_f32(input), vdupq_n_s32(INT32_MIN)));
}
#undef _mm_cvtps_epi32
#undef _mm_cvttps_epi32
#undef _mm_cvtss_si32
#undef _mm_cvttss_si32
#undef _mm_cvt_ss2si
#undef _mm_cvtt_ss2si
#define _mm_cvtps_epi32(value) SourceCvtpsEpi32(value)
#define _mm_cvttps_epi32(value) SourceCvttpsEpi32(value)
#define _mm_cvtss_si32(value) vgetq_lane_s32(vreinterpretq_s32_m128i(SourceCvtpsEpi32(value)), 0)
#define _mm_cvttss_si32(value) vgetq_lane_s32(vreinterpretq_s32_m128i(SourceCvttpsEpi32(value)), 0)
#define _mm_cvt_ss2si(value) _mm_cvtss_si32(value)
#define _mm_cvtt_ss2si(value) _mm_cvttss_si32(value)
#else
#include <xmmintrin.h>
#include <emmintrin.h>
#endif

#endif
