/*
 * adapt_coder.h — the binary arithmetic coder, both sides.
 *
 * A section of adapt_model.h, which includes the sections in order into
 * one translation unit: every function stays static inline and the per-bit
 * path is specialized at compile time. Include adapt_model.h, not this file.
 */
#ifndef ADAPT_CODER_H
#define ADAPT_CODER_H

/* ------------------------------------------------------------------------
 * Arithmetic coder — the binary arithmetic decoder
 * --------------------------------------------------------------------- */

typedef struct arith {
    uint32_t x1, x2, x;
    const uint8_t *src;
    size_t len;
    size_t pos;
} arith;

/* Reads past the end yield 0xFF, which keeps every truncated stream inside
 * the interval so damage is caught by the container's checks. */
static inline uint8_t arith_next(arith *d) {
    uint8_t b = d->pos < d->len ? d->src[d->pos] : 0xFF;
    d->pos++;
    return b;
}

static inline void arith_init(arith *d, const uint8_t *src, size_t len) {
    int i;
    d->x1 = 0;
    d->x2 = UINT32_MAX;
    d->x = 0;
    d->src = src;
    d->len = len;
    d->pos = 0;
    for (i = 0; i < 4; i++) {
        d->x = (d->x << 8) | arith_next(d);
    }
}

static inline uint32_t arith_split(uint32_t x1, uint32_t x2, uint32_t p1) {
    uint32_t r = x2 - x1;
    return x1 + (r >> PROB_BITS) * p1 + (((r & (PROB_ONE - 1)) * p1) >> PROB_BITS);
}

static inline uint32_t arith_decode(arith *d, uint32_t p1) {
    uint32_t xmid = arith_split(d->x1, d->x2, p1);
    uint32_t bit;
    if (d->x <= xmid) {
        d->x2 = xmid;
        bit = 1;
    } else {
        d->x1 = xmid + 1;
        bit = 0;
    }
    while (((d->x1 ^ d->x2) & 0xFF000000u) == 0) {
        d->x1 <<= 8;
        d->x2 = (d->x2 << 8) | 0xFF;
        d->x = (d->x << 8) | arith_next(d);
    }
    return bit;
}

/* True when the decoder consumed exactly the stream the encoder wrote:
 * every byte read, none invented. */
static inline int arith_consumed_exactly(const arith *d) {
    return d->pos == d->len;
}

/* The writing side: narrows the interval by each bit and emits settled
 * bytes into a caller-owned buffer. The container bounds a block's output
 * at its length plus 36 — at most four settled bytes per bit, eight bits
 * for the byte that crosses the line, then four final bytes — so `cap` is
 * never reached; `overflow` records it if a caller sized the buffer
 * smaller. */
typedef struct arith_enc {
    uint32_t x1, x2;
    uint8_t *out;
    size_t len;
    size_t cap;
    int overflow;
} arith_enc;

static inline void arith_enc_init(arith_enc *e, uint8_t *out, size_t cap) {
    e->x1 = 0;
    e->x2 = UINT32_MAX;
    e->out = out;
    e->len = 0;
    e->cap = cap;
    e->overflow = 0;
}

static inline void arith_enc_push(arith_enc *e, uint8_t byte) {
    if (e->len < e->cap) {
        e->out[e->len] = byte;
    } else {
        e->overflow = 1;
    }
    e->len++;
}

/* Code `bit` given p1 = P(bit = 1) in 1..=4095. */
static inline void arith_encode(arith_enc *e, uint32_t bit, uint32_t p1) {
    uint32_t xmid = arith_split(e->x1, e->x2, p1);
    if (bit != 0) {
        e->x2 = xmid;
    } else {
        e->x1 = xmid + 1;
    }
    while (((e->x1 ^ e->x2) & 0xFF000000u) == 0) {
        arith_enc_push(e, (uint8_t)(e->x2 >> 24));
        e->x1 <<= 8;
        e->x2 = (e->x2 << 8) | 0xFF;
    }
}

/* Flush the lower bound in full: four bytes, so a decoder never has to
 * read past the end of the stream to finish. Returns the payload length. */
static inline size_t arith_enc_finish(arith_enc *e) {
    arith_enc_push(e, (uint8_t)(e->x1 >> 24));
    arith_enc_push(e, (uint8_t)(e->x1 >> 16));
    arith_enc_push(e, (uint8_t)(e->x1 >> 8));
    arith_enc_push(e, (uint8_t)e->x1);
    return e->len;
}

#endif /* ADAPT_CODER_H */
