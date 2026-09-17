/*
 * adapt_blocks.h — the block methods: framing, the block decoder.
 *
 * A section of adapt_model.h, which includes the sections in order into
 * one translation unit: every function stays static inline and the per-bit
 * path is specialized at compile time as before. Include adapt_model.h,
 * not this file. Moved out of adapt_model.h unchanged on 2026-09-17.
 */
#ifndef ADAPT_BLOCKS_H
#define ADAPT_BLOCKS_H

/* ------------------------------------------------------------------------
 * container.rs — the block methods
 * --------------------------------------------------------------------- */

static uint32_t predict_adaptive(model *m) {
    uint32_t base = m->max ? predict_impl(m, 0, 1, 0, 0) : predict_impl(m, 0, 0, 0, 0);
    return adaptive_predict(m->adaptive, base, m->stretch[base], m->squashed, m->c8, m->c0, m->bitpos);
}
static void update_adaptive(model *m, uint32_t bit) {
    adaptive_update(m->adaptive, bit);
    if (m->max) update_impl(m, bit, 0, 1, 0, 0);
    else update_impl(m, bit, 0, 0, 0, 0);
}
#if ADAPT_X86_V3
static ADAPT_TARGET_V3 uint32_t predict_adaptive_v3(model *m) {
    uint32_t base = m->max ? predict_impl(m, 0, 1, 1, 0) : predict_impl(m, 0, 0, 1, 0);
    return adaptive_predict(m->adaptive, base, m->stretch[base], m->squashed, m->c8, m->c0, m->bitpos);
}
static ADAPT_TARGET_V3 void update_adaptive_v3(model *m, uint32_t bit) {
    adaptive_update(m->adaptive, bit);
    if (m->max) update_impl(m, bit, 0, 1, 1, 0);
    else update_impl(m, bit, 0, 0, 1, 0);
}
#endif

/* The per-bit pair of the model's profile, chosen once per block: the
 * x86-64-v3 instantiation on a CPU that runs it (m->cpu_v3, set by
 * model_new), the baseline one otherwise. */
typedef uint32_t (*predict_fn)(model *m);
typedef void (*update_fn)(model *m, uint32_t bit);
#if ADAPT_X86_V3
#define ADAPT_PROFILE(m, predict_p, update_p)                                                                       \
    predict_fn predict_p = (m)->adaptive ? ((m)->cpu_v3 ? predict_adaptive_v3 : predict_adaptive)                    \
                           : (m)->fast   ? predict_fast                                                              \
                           : (m)->max    ? ((m)->model2 ? ((m)->cpu_v3 ? predict_max2_v3 : predict_max2)               \
                                                        : ((m)->cpu_v3 ? predict_max_v3 : predict_max))              \
                                         : ((m)->cpu_v3 ? predict_v3 : predict);                                    \
    update_fn update_p = (m)->adaptive ? ((m)->cpu_v3 ? update_adaptive_v3 : update_adaptive)                        \
                         : (m)->fast   ? update_fast                                                                  \
                         : (m)->max    ? ((m)->model2 ? ((m)->cpu_v3 ? update_max2_v3 : update_max2)                  \
                                                      : ((m)->cpu_v3 ? update_max_v3 : update_max))                  \
                                       : ((m)->cpu_v3 ? update_v3 : update)
#else
#define ADAPT_PROFILE(m, predict_p, update_p)                                                   \
    predict_fn predict_p = (m)->adaptive ? predict_adaptive                                              \
                           : (m)->fast   ? predict_fast                                                  \
                           : (m)->max    ? ((m)->model2 ? predict_max2 : predict_max)                    \
                                         : predict;                                                      \
    update_fn update_p = (m)->adaptive ? update_adaptive                                                 \
                         : (m)->fast   ? update_fast                                                      \
                         : (m)->max    ? ((m)->model2 ? update_max2 : update_max)                        \
                                       : update
#endif

/* Decode `raw` bytes of one modeled block. */
static inline void decode_bytes(model *m, arith *d, size_t raw, uint8_t *out) {
    size_t i;
    int k;
    ADAPT_PROFILE(m, predict_p, update_p);
    m->coding_known = 0;
    for (i = 0; i < raw; i++) {
        uint32_t byte = 0;
        for (k = 0; k < 8; k++) {
            uint32_t p = predict_p(m);
            uint32_t bit = arith_decode(d, p);
            update_p(m, bit);
            byte = (byte << 1) | bit;
        }
        out[i] = (uint8_t)byte;
    }
}

/* Code one byte the encoder knows: predict each bit, code it, learn. */
static inline void encode_byte(model *m, arith_enc *e, uint8_t byte) {
    int k;
    ADAPT_PROFILE(m, predict_p, update_p);
    m->coding = byte;
    m->coding_known = 1;
    for (k = 7; k >= 0; k--) {
        uint32_t bit = ((uint32_t)byte >> k) & 1;
        uint32_t p = predict_p(m);
        arith_encode(e, bit, p);
        update_p(m, bit);
    }
}

/* Feed a stored, observed block through the model without decoding. */
static inline void observe_bytes(model *m, const uint8_t *bytes, size_t len) {
    size_t i;
    int k;
    ADAPT_PROFILE(m, predict_p, update_p);
    for (i = 0; i < len; i++) {
        uint32_t byte = bytes[i];
        m->coding = byte;
        m->coding_known = 1;
        for (k = 7; k >= 0; k--) {
            uint32_t bit = (byte >> k) & 1;
            (void)predict_p(m);
            update_p(m, bit);
        }
    }
}

#endif /* ADAPT_BLOCKS_H */
