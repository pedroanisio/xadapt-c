/* Adaptive byte models. Fixed integer rules, no corpus-derived data.
 * All experts consume the same decoded prefix. Prediction precedes update;
 * a completed 4096-byte descriptor routes only subsequent predictions.
 * See docs/ADAPTIVE.md for arithmetic and accounting. */
#ifndef ADAPT_ADAPTIVE_H
#define ADAPT_ADAPTIVE_H

#include <stdint.h>
#include <string.h>

#define ADAPT_WEIGHT_ONE (1u << 24)
#define ADAPT_CAL_CONTEXTS 16384u
#define ADAPT_RETAIN_CONTEXTS 2048u
#define ADAPT_SPARSE_CONTEXTS 16384u
#define ADAPT_EXPERTS 6u

typedef struct adaptive_model {
    uint16_t calibration[2][ADAPT_CAL_CONTEXTS][33];
    uint16_t sparse[2][ADAPT_SPARSE_CONTEXTS][2];
    uint16_t retained[4][ADAPT_RETAIN_CONTEXTS][33];
    uint16_t prototype[4][256], histogram[256];
    uint32_t age[4], weights[8][ADAPT_EXPERTS], prediction[ADAPT_EXPERTS];
    unsigned mode, experts, active, used, observed;
    unsigned context, bin, fraction, sparse_key[2], bitpos, partial;
} adaptive_model;

static inline uint32_t adaptive_hash(uint32_t x) {
    x ^= x >> 16;
    x *= UINT32_C(0x7feb352d);
    x ^= x >> 15;
    x *= UINT32_C(0x846ca68b);
    return x ^ (x >> 16);
}

static inline void adaptive_init(adaptive_model *a, unsigned mode) {
    unsigned i, j;
    memset(a, 0, sizeof(*a));
    a->mode = mode;
    a->experts = mode == 1 ? 3 : (mode == 2 ? 5 : 6);
    for (i = 0; i < 8; i++) {
        for (j = 0; j < a->experts; j++) a->weights[i][j] = ADAPT_WEIGHT_ONE / a->experts;
        a->weights[i][0] += ADAPT_WEIGHT_ONE % a->experts;
    }
}

static inline uint32_t adaptive_calibrate(uint16_t *row, unsigned bin, unsigned fraction,
                                         const int16_t *squashed) {
    unsigned j;
    for (j = bin; j <= bin + 1; j++) {
        if (row[j] == 0) {
            int s = ((int)j - 16) * 128;
            if (s < -2047) s = -2047;
            if (s > 2047) s = 2047;
            row[j] = (uint16_t)(squashed[s + 2047] * 16);
        }
    }
    return ((uint32_t)row[bin] * (128 - fraction) + (uint32_t)row[bin + 1] * fraction) >> 11;
}

static inline void adaptive_learn(uint16_t *row, unsigned bin, uint32_t bit, unsigned shift) {
    unsigned j;
    int32_t target = bit ? 65535 : 1;
    for (j = bin; j <= bin + 1; j++) {
        int32_t v = row[j];
        /* Division, not signed right shift: C99 defines truncation to zero. */
        row[j] = (uint16_t)(v + (target - v) / (int32_t)(1u << shift));
    }
}

static inline void adaptive_observe_byte(adaptive_model *a, unsigned byte) {
    unsigned i, j, selected = 0, distance = UINT32_MAX;
    if (a->mode != 3) return;
    a->histogram[byte]++;
    if (++a->observed != 4096) return;
    for (i = 0; i < a->used; i++) {
        unsigned d = 0;
        for (j = 0; j < 256; j++) {
            int delta = (int)a->histogram[j] - a->prototype[i][j];
            d += (unsigned)(delta < 0 ? -delta : delta);
        }
        if (d < distance) { distance = d; selected = i; }
        if (a->age[i] < UINT32_MAX) a->age[i]++;
    }
    if (distance > 2048) {
        if (a->used < 4) selected = a->used++;
        else {
            selected = 0;
            for (i = 1; i < 4; i++) if (a->age[i] > a->age[selected]) selected = i;
            memset(a->retained[selected], 0, sizeof(a->retained[selected]));
        }
        memcpy(a->prototype[selected], a->histogram, sizeof(a->histogram));
    }
    a->active = selected;
    a->age[selected] = 0;
    a->observed = 0;
    memset(a->histogram, 0, sizeof(a->histogram));
}

static inline uint32_t adaptive_predict(adaptive_model *a, uint32_t base, int stretched,
                                       const int16_t *squashed, uint64_t history,
                                       unsigned partial, unsigned bitpos) {
    uint64_t sum = 0;
    unsigned i;
    a->context = adaptive_hash(((uint32_t)history & 255) * 256 + partial) & (ADAPT_CAL_CONTEXTS - 1);
    a->bin = (unsigned)(stretched + 2048) >> 7;
    a->fraction = (unsigned)(stretched + 2048) & 127;
    a->partial = partial;
    a->bitpos = bitpos;
    a->prediction[0] = base;
    for (i = 0; i < 2; i++)
        a->prediction[i + 1] = adaptive_calibrate(a->calibration[i][a->context], a->bin, a->fraction, squashed);
    if (a->mode >= 2) {
        /* Lags (1,3) and (2,4), with the already decoded bit prefix. */
        uint32_t h = (uint32_t)history;
        a->sparse_key[0] = adaptive_hash((h & 255) | ((h >> 8) & 65280) | (partial << 16)) & (ADAPT_SPARSE_CONTEXTS - 1);
        a->sparse_key[1] = adaptive_hash(((h >> 8) & 255) | ((h >> 16) & 65280) | (partial << 16)) & (ADAPT_SPARSE_CONTEXTS - 1);
        for (i = 0; i < 2; i++) {
            uint16_t *counts = a->sparse[i][a->sparse_key[i]];
            a->prediction[i + 3] = (2u * counts[1] + 1) * 4096 / (2u * (counts[0] + counts[1] + 1));
        }
    }
    if (a->mode == 3)
        a->prediction[5] = adaptive_calibrate(a->retained[a->active][a->context & (ADAPT_RETAIN_CONTEXTS - 1)],
                                             a->bin, a->fraction, squashed);
    for (i = 0; i < a->experts; i++) {
        if (a->prediction[i] == 0) a->prediction[i] = 1;
        if (a->prediction[i] > 4095) a->prediction[i] = 4095;
        sum += (uint64_t)a->weights[bitpos][i] * a->prediction[i];
    }
    return (uint32_t)((sum + ADAPT_WEIGHT_ONE / 2) >> 24);
}

static inline void adaptive_update(adaptive_model *a, uint32_t bit) {
    uint64_t evidence[ADAPT_EXPERTS], sum = 0;
    uint32_t posterior[ADAPT_EXPERTS], total = 0;
    uint32_t *weights = a->weights[a->bitpos];
    unsigned i;
    for (i = 0; i < a->experts; i++) {
        evidence[i] = (uint64_t)weights[i] * (bit ? a->prediction[i] : 4096 - a->prediction[i]);
        sum += evidence[i];
    }
    for (i = 0; i < a->experts; i++) {
        posterior[i] = (uint32_t)((evidence[i] << 24) / sum);
        total += posterior[i];
    }
    posterior[0] += ADAPT_WEIGHT_ONE - total;
    total = 0;
    for (i = 0; i < a->experts; i++) {
        /* No-self switch alpha=1/65536. Rounded deficit goes to base. */
        weights[i] = (uint32_t)(((uint64_t)65535 * posterior[i] +
                                (ADAPT_WEIGHT_ONE - posterior[i]) / (a->experts - 1)) >> 16);
        total += weights[i];
    }
    weights[0] += ADAPT_WEIGHT_ONE - total;
    adaptive_learn(a->calibration[0][a->context], a->bin, bit, 5);
    adaptive_learn(a->calibration[1][a->context], a->bin, bit, 9);
    if (a->mode >= 2) {
        for (i = 0; i < 2; i++) {
            uint16_t *counts = a->sparse[i][a->sparse_key[i]];
            if (counts[0] + counts[1] >= 512) { counts[0] /= 2; counts[1] /= 2; }
            counts[bit]++;
        }
    }
    if (a->mode == 3) adaptive_learn(a->retained[a->active][a->context & (ADAPT_RETAIN_CONTEXTS - 1)], a->bin, bit, 6);
    if (a->bitpos == 7) adaptive_observe_byte(a, (a->partial * 2 + bit) & 255);
}
#endif
