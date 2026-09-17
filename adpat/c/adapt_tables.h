/*
 * adapt_tables.h — the tables and helpers: stretch and squash, the maps, the run and recall cells, the arbiters (int32 and the second model's int16), the vector lanes.
 *
 * A section of adapt_model.h, which includes the sections in order into
 * one translation unit: every function stays static inline and the per-bit
 * path is specialized at compile time as before. Include adapt_model.h,
 * not this file. Moved out of adapt_model.h unchanged on 2026-09-17.
 */
#ifndef ADAPT_TABLES_H
#define ADAPT_TABLES_H

/* ------------------------------------------------------------------------
 * model.rs — tables and helpers
 * --------------------------------------------------------------------- */

/* The logistic function sampled at 33 points, d = -2048 + 128 i. */
static const int32_t SQUASH_POINTS[APM_BUCKETS] = {1,    2,    4,    6,    10,   17,   27,   45,   74,   120,  194,
                                                   311,  488,  747,  1102, 1546, 2048, 2550, 2994, 3349, 3608, 3785,
                                                   3902, 3976, 4022, 4051, 4069, 4079, 4086, 4090, 4092, 4094, 4095};

/* 1 / (1 + e^{-d/256}) in twelve bits, for d in -2047..=2047. */
static inline int32_t squash(int32_t d) {
    int32_t w, i;
    if (d > 2047) {
        return 4095;
    }
    if (d < -2047) {
        return 1;
    }
    w = d & 127;
    i = (d >> 7) + 16;
    return (SQUASH_POINTS[i] * (128 - w) + SQUASH_POINTS[i + 1] * w + 64) >> 7;
}

/* A probability cell for the recall and run confidences. */
#define CELL_INIT ((uint32_t)1 << 31)

static inline int32_t cell_p12(uint32_t c) {
    int32_t p = (int32_t)(c >> 20);
    return p > 1 ? p : 1;
}

static inline uint64_t mix64(uint64_t x) {
    x ^= x >> 31;
    x *= 0x9E3779B97F4A7C15ull;
    x ^= x >> 29;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 32;
    return x;
}

/* The shape of a byte for the class-history context. */
static inline uint64_t byte_class(uint8_t b) {
    if (b >= 'a' && b <= 'z') {
        return 1;
    }
    if (b >= 'A' && b <= 'Z') {
        return 2;
    }
    if (b >= '0' && b <= '9') {
        return 3;
    }
    if (b == ' ' || b == '\t') {
        return 4;
    }
    if (b == '\r' || b == '\n') {
        return 5;
    }
    if (b >= 128) {
        return 7;
    }
    return 6;
}

static inline int is_word_byte(uint8_t b) {
    return (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9') || b >= 0x80;
}

/* A first-layer arbiter is LANES(fast, max) weights and, after them, the
 * number of updates they have had: ARBITER_STRIDE ints per set, in a flat
 * array sized for the profile. */
#define ARBITER_STRIDE(fast, max) (LANES(fast, max) + 1)

typedef struct arbiter_final {
    int32_t w[FINAL_INPUTS];
    int32_t n;
} arbiter_final;

/* A mixer's output in the log-odds domain, clamped to the squash range:
 * the 64-bit sum of 32-bit products, shifted. */
static inline int32_t dot_clamp_shift(int64_t acc, int shift) {
    acc >>= shift;
    if (acc < -2047) {
        return -2047;
    }
    if (acc > 2047) {
        return 2047;
    }
    return (int32_t)acc;
}

static inline int32_t dot_clamp(int64_t acc) {
    return dot_clamp_shift(acc, 16);
}

static inline int32_t dot_scalar(const int32_t *w, const int32_t *x, size_t n) {
    int64_t acc = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        acc += (int64_t)w[i] * (int64_t)x[i];
    }
    return dot_clamp(acc);
}

#if ADAPT_X86_V3
/* The same sum on eight lanes at a time: the even lanes' products
 * straight from the sign-extending 32 x 32 -> 64 multiply, the odd
 * lanes' after a shift down, and the two added into four 64-bit
 * accumulators. The compiler's own vectorization of dot_scalar
 * sign-extends every lane through two shuffles before the multiply,
 * which was a fifth of predict(); this is the same arithmetic in half
 * the instructions. Two accumulators, so consecutive adds do not wait
 * on each other. The lanes past a multiple of eight go the scalar way. */
static inline ADAPT_TARGET_V3 int32_t dot_v3(const int32_t *w, const int32_t *x, size_t n) {
    __m256i acc0 = _mm256_setzero_si256(), acc1 = _mm256_setzero_si256();
    __m128i lo;
    int64_t sum;
    size_t i, vec = n & ~(size_t)7;
    for (i = 0; i < vec; i += 8) {
        __m256i a = _mm256_loadu_si256((const __m256i *)(const void *)(w + i));
        __m256i b = _mm256_loadu_si256((const __m256i *)(const void *)(x + i));
        acc0 = _mm256_add_epi64(acc0, _mm256_mul_epi32(a, b));
        acc1 = _mm256_add_epi64(acc1, _mm256_mul_epi32(_mm256_srli_epi64(a, 32), _mm256_srli_epi64(b, 32)));
    }
    acc0 = _mm256_add_epi64(acc0, acc1);
    lo = _mm_add_epi64(_mm256_castsi256_si128(acc0), _mm256_extracti128_si256(acc0, 1));
    lo = _mm_add_epi64(lo, _mm_unpackhi_epi64(lo, lo));
    sum = _mm_cvtsi128_si64(lo);
    for (; i < n; i++) {
        sum += (int64_t)w[i] * (int64_t)x[i];
    }
    return dot_clamp(sum);
}
#endif

#if ADAPT_X86_V3
/* Four arbiters' dot products over one input vector in a single pass:
 * the inputs are loaded and split once for all four, and eight
 * accumulators keep the adds independent. */
static inline ADAPT_TARGET_V3 void dot4_v3(const int32_t *w1, const int32_t *w2, const int32_t *w3, const int32_t *w4,
                                           const int32_t *x, size_t n, int32_t out[4]) {
    __m256i a1 = _mm256_setzero_si256(), b1 = a1, a2 = a1, b2 = a1, a3 = a1, b3 = a1, a4 = a1, b4 = a1;
    size_t i, vec = n & ~(size_t)7;
    int64_t s1, s2, s3, s4;
    for (i = 0; i < vec; i += 8) {
        __m256i xe = _mm256_loadu_si256((const __m256i *)(const void *)(x + i));
        __m256i xo = _mm256_srli_epi64(xe, 32);
        __m256i w;
        w = _mm256_loadu_si256((const __m256i *)(const void *)(w1 + i));
        a1 = _mm256_add_epi64(a1, _mm256_mul_epi32(w, xe));
        b1 = _mm256_add_epi64(b1, _mm256_mul_epi32(_mm256_srli_epi64(w, 32), xo));
        w = _mm256_loadu_si256((const __m256i *)(const void *)(w2 + i));
        a2 = _mm256_add_epi64(a2, _mm256_mul_epi32(w, xe));
        b2 = _mm256_add_epi64(b2, _mm256_mul_epi32(_mm256_srli_epi64(w, 32), xo));
        w = _mm256_loadu_si256((const __m256i *)(const void *)(w3 + i));
        a3 = _mm256_add_epi64(a3, _mm256_mul_epi32(w, xe));
        b3 = _mm256_add_epi64(b3, _mm256_mul_epi32(_mm256_srli_epi64(w, 32), xo));
        w = _mm256_loadu_si256((const __m256i *)(const void *)(w4 + i));
        a4 = _mm256_add_epi64(a4, _mm256_mul_epi32(w, xe));
        b4 = _mm256_add_epi64(b4, _mm256_mul_epi32(_mm256_srli_epi64(w, 32), xo));
    }
#define ADAPT_HSUM64(v, out_)                                                                   \
    do {                                                                                        \
        __m128i lo_ = _mm_add_epi64(_mm256_castsi256_si128(v), _mm256_extracti128_si256(v, 1)); \
        lo_ = _mm_add_epi64(lo_, _mm_unpackhi_epi64(lo_, lo_));                                 \
        out_ = _mm_cvtsi128_si64(lo_);                                                          \
    } while (0)
    ADAPT_HSUM64(_mm256_add_epi64(a1, b1), s1);
    ADAPT_HSUM64(_mm256_add_epi64(a2, b2), s2);
    ADAPT_HSUM64(_mm256_add_epi64(a3, b3), s3);
    ADAPT_HSUM64(_mm256_add_epi64(a4, b4), s4);
#undef ADAPT_HSUM64
    for (; i < n; i++) {
        s1 += (int64_t)w1[i] * (int64_t)x[i];
        s2 += (int64_t)w2[i] * (int64_t)x[i];
        s3 += (int64_t)w3[i] * (int64_t)x[i];
        s4 += (int64_t)w4[i] * (int64_t)x[i];
    }
    out[0] = dot_clamp(s1);
    out[1] = dot_clamp(s2);
    out[2] = dot_clamp(s3);
    out[3] = dot_clamp(s4);
}
#endif

/* ---- the second model's int16 arbiters ----
 * Weights in 1/32768 (WGT16_SHIFT), inputs as they are (every lane fits
 * int16: |x| <= 2047, the bias 256, recall's fixed lane +-2048). A dot
 * product is the exact integer sum the scalar form makes, so the bytes
 * do not depend on the path taken. */
#define WGT16_SHIFT 15

static inline int32_t dot16_scalar(const int16_t *w, const int32_t *x, size_t n) {
    int64_t acc = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        acc += (int64_t)w[i] * (int64_t)x[i];
    }
    return dot_clamp_shift(acc, WGT16_SHIFT);
}

/* The train step, rounded: the weight moves by round(x err / 2048),
 * formed as (16 x) err in a 16-bit high multiply with rounding
 * (vpmulhrsw: (a b + 2^14) >> 15), the input times sixteen saturated to
 * int16 so the vector and scalar forms agree; the sum saturates at
 * +-32767. */
static inline int32_t train16_delta(int32_t x, int32_t err) {
    /* Lanes are in [-2048, 2048]; multiplication is defined for negative
     * inputs too, and the scaled value fits int32 before saturation. */
    int32_t xs = x * 16;
    xs = xs > 32767 ? 32767 : (xs < -32768 ? -32768 : xs);
    return (xs * err + 16384) >> 15;
}

static inline void train16(int16_t *w, int16_t *count, const int32_t *x, size_t n, int32_t err, const int32_t *rates) {
    int32_t rate = rates[*count];
    size_t i;
    *count += (*count < MIXER_COUNT_MAX);
    if ((err < 0 ? -err : err) < MIXER_SKIP) {
        return;
    }
    err = (err * rate) >> 6;
    for (i = 0; i < n; i++) {
        int32_t v = (int32_t)w[i] + train16_delta(x[i], err);
        w[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
    }
}

#if ADAPT_X86_V3
/* The eight int32 sums of a madd accumulator, widened to 64 bits before
 * they meet: each lane holds n/16 pair sums of at most 2 x 32767 x 2047,
 * inside int32 for any lane count a profile has. */
static inline ADAPT_TARGET_V3 int64_t hsum_epi32_64(__m256i v) {
    __m256i lo = _mm256_cvtepi32_epi64(_mm256_castsi256_si128(v));
    __m256i hi = _mm256_cvtepi32_epi64(_mm256_extracti128_si256(v, 1));
    __m256i sum = _mm256_add_epi64(lo, hi);
    __m128i t = _mm_add_epi64(_mm256_castsi256_si128(sum), _mm256_extracti128_si256(sum, 1));
    t = _mm_add_epi64(t, _mm_unpackhi_epi64(t, t));
    return _mm_cvtsi128_si64(t);
}

/* A 128-bit value in the low half of a 256-bit one and zeros above: the
 * cast intrinsic leaves the upper half undefined, which one compiler
 * zeroed and another did not. */
static inline ADAPT_TARGET_V3 __m256i zext128(__m128i v) {
    return _mm256_inserti128_si256(_mm256_setzero_si256(), v, 0);
}

/* The inputs as int16 (x16) and as int16 times sixteen, saturated (x16s),
 * sixteen a step: the two eight-lane halves packed with saturation and
 * the pack's interleave undone by a quad-word permute. */
static inline ADAPT_TARGET_V3 void pack_inputs16(const int32_t *x, int16_t *x16, int16_t *x16s, size_t n) {
    size_t i;
    for (i = 0; i < n; i += 16) {
        __m256i a = _mm256_loadu_si256((const __m256i *)(const void *)(x + i));
        __m256i b = _mm256_loadu_si256((const __m256i *)(const void *)(x + i + 8));
        _mm256_storeu_si256((__m256i *)(void *)(x16 + i), _mm256_permute4x64_epi64(_mm256_packs_epi32(a, b), 0xD8));
        _mm256_storeu_si256((__m256i *)(void *)(x16s + i),
                            _mm256_permute4x64_epi64(_mm256_packs_epi32(_mm256_slli_epi32(a, 4), _mm256_slli_epi32(b, 4)), 0xD8));
    }
}

/* Four arbiters' int16 dot products over one packed input vector: sixteen
 * lanes a step by vpmaddwd, one 128-bit step for eight more, the rest
 * the scalar way. */
static inline ADAPT_TARGET_V3 void dot4_16_v3(const int16_t *w1, const int16_t *w2, const int16_t *w3, const int16_t *w4,
                                              const int32_t *x, const int16_t *x16, size_t n, int32_t out[4]) {
    __m256i a1 = _mm256_setzero_si256(), a2 = a1, a3 = a1, a4 = a1;
    size_t i, vec = n & ~(size_t)15;
    int64_t s1, s2, s3, s4;
    for (i = 0; i < vec; i += 16) {
        __m256i xv = _mm256_loadu_si256((const __m256i *)(const void *)(x16 + i));
        a1 = _mm256_add_epi32(a1, _mm256_madd_epi16(_mm256_loadu_si256((const __m256i *)(const void *)(w1 + i)), xv));
        a2 = _mm256_add_epi32(a2, _mm256_madd_epi16(_mm256_loadu_si256((const __m256i *)(const void *)(w2 + i)), xv));
        a3 = _mm256_add_epi32(a3, _mm256_madd_epi16(_mm256_loadu_si256((const __m256i *)(const void *)(w3 + i)), xv));
        a4 = _mm256_add_epi32(a4, _mm256_madd_epi16(_mm256_loadu_si256((const __m256i *)(const void *)(w4 + i)), xv));
    }
    if (n & 8) {
        __m128i xv = _mm_loadu_si128((const __m128i *)(const void *)(x16 + i));
        a1 = _mm256_add_epi32(a1, zext128(_mm_madd_epi16(_mm_loadu_si128((const __m128i *)(const void *)(w1 + i)), xv)));
        a2 = _mm256_add_epi32(a2, zext128(_mm_madd_epi16(_mm_loadu_si128((const __m128i *)(const void *)(w2 + i)), xv)));
        a3 = _mm256_add_epi32(a3, zext128(_mm_madd_epi16(_mm_loadu_si128((const __m128i *)(const void *)(w3 + i)), xv)));
        a4 = _mm256_add_epi32(a4, zext128(_mm_madd_epi16(_mm_loadu_si128((const __m128i *)(const void *)(w4 + i)), xv)));
        i += 8;
    }
    s1 = hsum_epi32_64(a1);
    s2 = hsum_epi32_64(a2);
    s3 = hsum_epi32_64(a3);
    s4 = hsum_epi32_64(a4);
    for (; i < n; i++) {
        s1 += (int64_t)w1[i] * (int64_t)x[i];
        s2 += (int64_t)w2[i] * (int64_t)x[i];
        s3 += (int64_t)w3[i] * (int64_t)x[i];
        s4 += (int64_t)w4[i] * (int64_t)x[i];
    }
    out[0] = dot_clamp_shift(s1, WGT16_SHIFT);
    out[1] = dot_clamp_shift(s2, WGT16_SHIFT);
    out[2] = dot_clamp_shift(s3, WGT16_SHIFT);
    out[3] = dot_clamp_shift(s4, WGT16_SHIFT);
}

static inline ADAPT_TARGET_V3 int32_t dot1_16_v3(const int16_t *w, const int32_t *x, const int16_t *x16, size_t n) {
    __m256i a = _mm256_setzero_si256();
    size_t i, vec = n & ~(size_t)15;
    int64_t sum;
    for (i = 0; i < vec; i += 16) {
        a = _mm256_add_epi32(a, _mm256_madd_epi16(_mm256_loadu_si256((const __m256i *)(const void *)(w + i)),
                                                  _mm256_loadu_si256((const __m256i *)(const void *)(x16 + i))));
    }
    if (n & 8) {
        a = _mm256_add_epi32(a, zext128(_mm_madd_epi16(_mm_loadu_si128((const __m128i *)(const void *)(w + i)),
                                                                      _mm_loadu_si128((const __m128i *)(const void *)(x16 + i)))));
        i += 8;
    }
    sum = hsum_epi32_64(a);
    for (; i < n; i++) {
        sum += (int64_t)w[i] * (int64_t)x[i];
    }
    return dot_clamp_shift(sum, WGT16_SHIFT);
}

/* train16, sixteen lanes a step. */
static inline ADAPT_TARGET_V3 void train16_v3(int16_t *w, int16_t *count, const int16_t *x16s, const int32_t *x, size_t n,
                                              int32_t err, const int32_t *rates) {
    int32_t rate = rates[*count];
    size_t i, vec;
    __m256i e;
    *count += (*count < MIXER_COUNT_MAX);
    if ((err < 0 ? -err : err) < MIXER_SKIP) {
        return;
    }
    err = (err * rate) >> 6;
    e = _mm256_set1_epi16((short)err);
    vec = n & ~(size_t)15;
    for (i = 0; i < vec; i += 16) {
        __m256i d = _mm256_mulhrs_epi16(_mm256_loadu_si256((const __m256i *)(const void *)(x16s + i)), e);
        _mm256_storeu_si256((__m256i *)(void *)(w + i),
                            _mm256_adds_epi16(_mm256_loadu_si256((const __m256i *)(const void *)(w + i)), d));
    }
    if (n & 8) {
        __m128i d = _mm_mulhrs_epi16(_mm_loadu_si128((const __m128i *)(const void *)(x16s + i)), _mm256_castsi256_si128(e));
        _mm_storeu_si128((__m128i *)(void *)(w + i), _mm_adds_epi16(_mm_loadu_si128((const __m128i *)(const void *)(w + i)), d));
        i += 8;
    }
    for (; i < n; i++) {
        int32_t v = (int32_t)w[i] + train16_delta(x[i], err);
        w[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
    }
}
#endif

/* The dot product of an instantiation: `v3` is a constant of the
 * per-bit path, so one body or the other remains. */
static inline ADAPT_ALWAYS_INLINE int32_t dot(const int32_t *w, const int32_t *x, size_t n, const int v3) {
#if ADAPT_X86_V3
    if (v3) {
        return dot_v3(w, x, n);
    }
#else
    (void)v3;
#endif
    return dot_scalar(w, x, n);
}

/* Move a mixer's weights along the gradient of the coding cost, unless
 * the error is too small to be worth the work. */
static inline void train(int32_t *w, int32_t *count, const int32_t *x, size_t n, int32_t err, const int32_t *rates) {
    int32_t rate = rates[*count];
    size_t i;
    *count += (*count < MIXER_COUNT_MAX);
    if ((err < 0 ? -err : err) < MIXER_SKIP) {
        return;
    }
    err = (err * rate) >> 6;
    for (i = 0; i < n; i++) {
        w[i] = (int32_t)((uint32_t)w[i] + (uint32_t)((x[i] * err) >> 10));
    }
}

#endif /* ADAPT_TABLES_H */
