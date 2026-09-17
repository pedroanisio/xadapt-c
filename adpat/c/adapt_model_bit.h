/*
 * adapt_model_bit.h — the per-bit path: predict and update, per profile, and the decoder's prefetch plans.
 *
 * A section of adapt_model.h, which includes the sections in order into
 * one translation unit: every function stays static inline and the per-bit
 * path is specialized at compile time as before. Include adapt_model.h,
 * not this file. Moved out of adapt_model.h unchanged on 2026-09-17.
 */
#ifndef ADAPT_MODEL_BIT_H
#define ADAPT_MODEL_BIT_H

/* Probability, in 1..=4095, that the next bit is one. The body, with the
 * profile a constant; predict() and predict_fast() below are its two
 * instantiations, each compiled per instruction set. */
static inline ADAPT_ALWAYS_INLINE uint32_t predict_impl(model *m, const int fast, const int max, const int v3, const int m2) {
    size_t node = m->node & (BLOCK - 1);
    const int16_t *st = m->stretch;
    size_t i, mstate, ctx, j, key;
    int32_t m_sm = 0, m_fixed = 0, s, w, a, b, papm, p;
    const uint16_t *row;
#if ADAPT_X86_V3
    if (max && v3 && !m->aliased) {
        context_lanes_v3(m);
    } else
#endif
    {
        context_lanes(m->st, st, m->maps[0], &m->states, m->o0[m->o0_blk & 31][node], max);
        for (i = 0; i < m->hashed; i++) {
            uint8_t *cell = m->base[i] + node;
            m->cell[i] = cell;
            context_lanes(m->st + 1 + i, st, m->maps[1 + i], &m->states, *cell, max);
        }
    }
    /* runs: the byte that followed this context last time, while its bits
     * so far agree with the byte being coded */
    for (i = 0; i < m->runs; i++) {
        int32_t v = 0;
        if (!RUN_ON(fast, i)) {
            continue;
        }
        if (m->rcnt[i] > 0) {
            uint32_t expected_prefix = m->rbyte[i] >> (8 - m->bitpos);
            if (expected_prefix == (m->c0 ^ (1u << m->bitpos))) {
                uint32_t expected = (m->rbyte[i] >> (7 - m->bitpos)) & 1;
                size_t bucket = m->rcnt[i] < RUN_BUCKETS - 1 ? m->rcnt[i] : RUN_BUCKETS - 1;
                int32_t pr;
                m->rsm_idx[i] = i * RUN_BUCKETS + bucket;
                pr = st[cell_p12(m->rsm[m->rsm_idx[i]])];
                v = expected ? pr : -pr;
            } else {
                m->rcnt[i] = 0;
            }
        }
        m->st[RUN_BASE(fast, max) + i] = v;
    }
    if (fast) {
        /* the direct order-1 memory, in the lane the order-2 run would take */
        m->st[O1_LANE] = st[map_p12(m->maps[O1_MAP], m->o1[o1_blk(m)][node])];
    }
    /* recall */
    if (m->mlen > 0 && !m->mmiss) {
        uint32_t expected_prefix = m->mbyte >> (8 - m->bitpos);
        if (expected_prefix == (m->c0 ^ (1u << m->bitpos))) {
            uint32_t expected = (m->mbyte >> (7 - m->bitpos)) & 1;
            size_t bucket = m->mlen < MATCH_BUCKETS - 1 ? m->mlen : MATCH_BUCKETS - 1;
            int32_t k = (int32_t)((m->mlen < 32 ? m->mlen : 32) * 64);
            m->msm_idx = bucket * 2 + expected;
            m_sm = st[cell_p12(m->msm[m->msm_idx])];
            m_fixed = expected ? k : -k;
        } else {
            m->mmiss = 1;
        }
    }
    m->st[RUN_BASE(fast, max) + RUNS_OF(max)] = m_sm;
    m->st[RUN_BASE(fast, max) + RUNS_OF(max) + 1] = m_fixed;
    m->st[RUN_BASE(fast, max) + RUNS_OF(max) + 2] = 256;
    /* first arbiter: weights by partial byte, the top three bits of the
     * previous byte, and recall state */
    mstate = (m->mlen == 0 || m->mmiss) ? 0 : (m->mlen < 16 ? 1 : 2);
    /* The normal profile's first arbiter keys on the previous byte's top
     * bits; the fast profile's only arbiter on whether the word run has an
     * answer and two of those bits. */
    key = fast ? ((size_t)(m->rcnt[1] > 0) << 2 | (size_t)((m->c8 >> 6) & 3)) : (size_t)((m->c8 >> 5) & 7);
    m->wset = (mstate * 8 + key) * 256 + (m->c0 & 255);
    if (fast) {
        /* the fast profile: one arbiter is the whole verdict */
        m->pmix = m->squashed[dot(m->weights + m->wset * ARBITER_STRIDE(fast, max), m->st, LANES(fast, max), v3) + 2047];
        m->pfinal = m->pmix;
        p = m->pfinal;
        if (p < 1) {
            p = 1;
        }
        if (p > (int32_t)PROB_ONE - 1) {
            p = (int32_t)PROB_ONE - 1;
        }
        return (uint32_t)p;
    }
    /* second arbiter: weights by the previous byte; third and fourth: by
     * eight bits of the order-2 hash (slot 1 holds order-2 under every
     * set) and of the word hash, each with the partial byte */
    m->wset2 = (mstate * 2 + (m->rcnt[0] > 0)) * 256 + (size_t)(m->c8 & 0xFF);
    m->wset3 = ((size_t)(m->h[1] >> 48) & m->mix34_row_mask) * 256 + (m->c0 & 255);
    if (m->keys != ADAPT_KEYS_CLASSIC) {
        /* the key policies (keys_rows): the third arbiter's row by the
         * classes, the second's set by them too under ADAPT_KEYS_TEXT */
        m->wset3 = m->keys_row3 + (m->c0 & 255);
        if (m->keys == ADAPT_KEYS_TEXT) {
            m->wset2 = (mstate * 2 + (m->rcnt[0] > 0)) * 256 + (size_t)(m->classes & 0xFF);
        }
    }
    m->wset4 = m->wset4_row + (m->c0 & 255);
    if (!fast && !max && m->mid) {
        /* the mid profile: the first arbiter alone is the verdict, the
         * estimation stages take it as they take the final arbiter's */
        m->pmix = m->squashed[dot(m->weights + m->wset * ARBITER_STRIDE(fast, max), m->st, LANES(fast, max), v3) + 2047];
        m->pfinal = m->pmix;
        goto estimate;
    }
    if (m2) {
        /* model2: the int16 arbiters, and the fifth keyed by the bytes
         * since a space and the previous byte's class */
        const size_t stride = (size_t)ARBITER_STRIDE(0, max);
        m->wsetx = ((size_t)m->since_space * 8 + (size_t)byte_class((uint8_t)(m->c8 & 0xFF))) * 256 + (m->c0 & 255);
#if ADAPT_X86_V3
        if (v3) {
            int32_t d[4];
            pack_inputs16(m->st, m->st16, m->st16s, LANES(0, max));
            dot4_16_v3(m->w16 + m->wset * stride, m->w16_2 + m->wset2 * stride, m->w16_3 + m->wset3 * stride,
                       m->w16_4 + m->wset4 * stride, m->st, m->st16, LANES(0, max), d);
            m->pmix = m->squashed[d[0] + 2047];
            m->pmix2 = m->squashed[d[1] + 2047];
            m->pmix3 = m->squashed[d[2] + 2047];
            m->pmix4 = m->squashed[d[3] + 2047];
            if (m->extra_on) {
                m->pmixx = m->squashed[dot1_16_v3(m->w16_x + m->wsetx * stride, m->st, m->st16, LANES(0, max)) + 2047];
            }
        } else
#endif
        {
            m->pmix = m->squashed[dot16_scalar(m->w16 + m->wset * stride, m->st, LANES(0, max)) + 2047];
            m->pmix2 = m->squashed[dot16_scalar(m->w16_2 + m->wset2 * stride, m->st, LANES(0, max)) + 2047];
            m->pmix3 = m->squashed[dot16_scalar(m->w16_3 + m->wset3 * stride, m->st, LANES(0, max)) + 2047];
            m->pmix4 = m->squashed[dot16_scalar(m->w16_4 + m->wset4 * stride, m->st, LANES(0, max)) + 2047];
            if (m->extra_on) {
                m->pmixx = m->squashed[dot16_scalar(m->w16_x + m->wsetx * stride, m->st, LANES(0, max)) + 2047];
            }
        }
    } else {
#if ADAPT_X86_V3
        if (v3) {
            int32_t d[4];
            dot4_v3(m->weights + m->wset * ARBITER_STRIDE(0, max), m->weights2 + m->wset2 * ARBITER_STRIDE(0, max),
                    m->weights3 + m->wset3 * ARBITER_STRIDE(0, max), m->weights4 + m->wset4 * ARBITER_STRIDE(0, max), m->st,
                    LANES(0, max), d);
            m->pmix = m->squashed[d[0] + 2047];
            m->pmix2 = m->squashed[d[1] + 2047];
            m->pmix3 = m->squashed[d[2] + 2047];
            m->pmix4 = m->squashed[d[3] + 2047];
        } else
#endif
        {
            m->pmix = m->squashed[dot(m->weights + m->wset * ARBITER_STRIDE(0, max), m->st, LANES(0, max), v3) + 2047];
            m->pmix2 = m->squashed[dot(m->weights2 + m->wset2 * ARBITER_STRIDE(0, max), m->st, LANES(0, max), v3) + 2047];
            m->pmix3 = m->squashed[dot(m->weights3 + m->wset3 * ARBITER_STRIDE(0, max), m->st, LANES(0, max), v3) + 2047];
            m->pmix4 = m->squashed[dot(m->weights4 + m->wset4 * ARBITER_STRIDE(0, max), m->st, LANES(0, max), v3) + 2047];
        }
    }
    /* final arbiter: weights by the partial byte */
    m->stf[0] = st[m->pmix];
    m->stf[1] = st[m->pmix2];
    m->stf[2] = st[m->pmix3];
    m->stf[3] = st[m->pmix4];
    if (m2 && m->extra_on) {
        m->stf[4] = st[m->pmixx];
    }
    m->stf[FINAL_INPUTS_ON(m, m2) - 1] = 256;
    m->wsetf = (mstate * 2 + (m->rcnt[0] > 0)) * 256 + (m->c0 & 255);
    m->pfinal = m->squashed[dot(m->wfinal[m->wsetf].w, m->stf, FINAL_INPUTS_ON(m, m2), 0) + 2047];
estimate:
    /* secondary estimation keyed by the previous byte and the partial
     * byte, and a second stage keyed by an order-2 hash with the partial
     * byte, averaged; three parts of it to one of the arbiters' verdict */
    s = st[m->pfinal] + 2048;
    j = ((size_t)s >> 7) & 31;
    w = s & 127;
    ctx = (((size_t)(m->c8 & 0xFF) << 8) | m->c0) & (APM_CONTEXTS - 1);
    m->apm_ctx = ctx;
    m->apm_j = j;
    row = m->apm[ctx];
    a = row[j];
    b = row[j + 1];
    papm = (a * (128 - w) + b * w) >> 11;
    {
        size_t ctx2 = (m->apm2_row | m->c0) & m->apm2_mask;
        const uint16_t *row2 = m->apm2[ctx2];
        int32_t papm2 = ((int32_t)row2[j] * (128 - w) + (int32_t)row2[j + 1] * w) >> 11;
        m->apm2_ctx = ctx2;
        m->apm2_j = j;
        papm = (papm + papm2 + 1) >> 1;
    }
    p = (m->pfinal + 3 * papm + 2) >> 2;
    if (p < 1) {
        p = 1;
    }
    if (p > (int32_t)PROB_ONE - 1) {
        p = (int32_t)PROB_ONE - 1;
    }
    return (uint32_t)p;
}

static uint32_t predict(model *m) {
    return predict_impl(m, 0, 0, 0, 0);
}

/* The max profile's path, compiled like the normal one; the second max
 * model's beside it. */
static uint32_t predict_max(model *m) {
    return predict_impl(m, 0, 1, 0, 0);
}

static uint32_t predict_max2(model *m) {
    return predict_impl(m, 0, 1, 0, 1);
}

#if ADAPT_X86_V3
static ADAPT_TARGET_V3 uint32_t predict_v3(model *m) {
    return predict_impl(m, 0, 0, 1, 0);
}

static ADAPT_TARGET_V3 uint32_t predict_max_v3(model *m) {
    return predict_impl(m, 0, 1, 1, 0);
}

static ADAPT_TARGET_V3 uint32_t predict_max2_v3(model *m) {
    return predict_impl(m, 0, 1, 1, 1);
}
#endif

/* The fast profile's path is short and scalar: compiled for the wide
 * instruction set it ran 15 % slower here (eight lanes do not pay for the
 * widening multiplies), so it is built once, for the baseline. */
static uint32_t predict_fast(model *m) {
    return predict_impl(m, 1, 0, 0, 0);
}

/* The lines the second nibble's claims will take when the nibble is
 * known — the encoder's case — into `planned`, from the same salt the
 * claims will use. */
static inline void nibble_plan(model *m, uint64_t hi) {
    uint64_t salt = (hi + 1) * 0x2545F4914F6CDD1Dull;
    size_t i;
    for (i = 0; i < m->hashed; i++) {
        uint64_t x = (m->h[i] + salt) * 0x9E3779B97F4A7C15ull;
        m->planned[i] = m->table + block_line(m, x ^ (x >> 29)) * BLOCK;
    }
}

/* Issue half of the planned lines: the first half or the second. */
static inline void issue_planned(const model *m, int second) {
    size_t half = (m->hashed + 1) / 2;
    size_t i, from = second ? half : 0, to = second ? m->hashed : half;
    for (i = from; i < to; i++) {
        ADAPT_PREFETCH(m->planned[i]);
    }
}

/* Learn from `bit`, which must be the bit predict() was asked about. The
 * body, as for predict_impl. */
static inline ADAPT_ALWAYS_INLINE void update_impl(model *m, uint32_t bit, const int fast, const int max, const int v3, const int m2) {
    int32_t target, g, v;
    uint16_t *row;
    size_t j, node, o0_blk, i;
    uint8_t s0;
    (void)v3;
    bit &= 1;
    target = (int32_t)(bit << 12);
    if (!fast && m->bitpos < 7) {
        /* The next bit's partial byte is known now, and with it the sets
         * of the arbiters keyed by it and the rows of both estimation
         * stages: bring them in while this bit's learning runs. The third
         * and fourth arbiters' tables are 19 MiB each and the second
         * stage's 17 MiB, so a set or row is rarely still in cache. */
        size_t c0n = (m->c0 << 1) | bit;
        size_t wbytes = m2 ? sizeof(int16_t) : sizeof(int32_t);
        const char *w3 = m2 ? (const char *)(m->w16_3 + (m->wset3 - (m->c0 & 255) + c0n) * ARBITER_STRIDE(0, max))
                            : (const char *)(m->weights3 + (m->wset3 - (m->c0 & 255) + c0n) * ARBITER_STRIDE(0, max));
        const char *w4 = m2 ? (const char *)(m->w16_4 + (m->wset4_row + c0n) * ARBITER_STRIDE(0, max))
                            : (const char *)(m->weights4 + (m->wset4_row + c0n) * ARBITER_STRIDE(0, max));
        const char *w1 = m2 ? (const char *)(m->w16 + (m->wset - (m->c0 & 255) + c0n) * ARBITER_STRIDE(fast, max))
                            : (const char *)(m->weights + (m->wset - (m->c0 & 255) + c0n) * ARBITER_STRIDE(fast, max));
        size_t k;
        for (k = 0; k < (size_t)ARBITER_STRIDE(0, max) * wbytes; k += 64) {
            ADAPT_PREFETCH(w3 + k);
            ADAPT_PREFETCH(w4 + k);
            ADAPT_PREFETCH(w1 + k);
        }
        ADAPT_PREFETCH(m->apm[(((size_t)(m->c8 & 0xFF) << 8) | c0n) & (APM_CONTEXTS - 1)]);
        ADAPT_PREFETCH((const char *)m->apm[(((size_t)(m->c8 & 0xFF) << 8) | c0n) & (APM_CONTEXTS - 1)] + 64);
        ADAPT_PREFETCH(m->apm2[(m->apm2_row | c0n) & m->apm2_mask]);
        ADAPT_PREFETCH((const char *)m->apm2[(m->apm2_row | c0n) & m->apm2_mask] + 64);
    }
    /* arbiters */
    if (m2) {
        const size_t stride = (size_t)ARBITER_STRIDE(0, max), lanes = (size_t)LANES(0, max);
        int16_t *w1 = m->w16 + m->wset * stride, *w2 = m->w16_2 + m->wset2 * stride, *w3 = m->w16_3 + m->wset3 * stride,
                *w4 = m->w16_4 + m->wset4 * stride, *wx = m->w16_x + m->wsetx * stride;
#if ADAPT_X86_V3
        if (v3) {
            train16_v3(w1, w1 + lanes, m->st16s, m->st, lanes, target - m->pmix, m->mixer_rate);
            train16_v3(w2, w2 + lanes, m->st16s, m->st, lanes, target - m->pmix2, m->mixer_rate);
            train16_v3(w3, w3 + lanes, m->st16s, m->st, lanes, target - m->pmix3, m->mixer_rate);
            train16_v3(w4, w4 + lanes, m->st16s, m->st, lanes, target - m->pmix4, m->mixer_rate);
            if (m->extra_on) {
                train16_v3(wx, wx + lanes, m->st16s, m->st, lanes, target - m->pmixx, m->mixer_rate);
            }
        } else
#endif
        {
            train16(w1, w1 + lanes, m->st, lanes, target - m->pmix, m->mixer_rate);
            train16(w2, w2 + lanes, m->st, lanes, target - m->pmix2, m->mixer_rate);
            train16(w3, w3 + lanes, m->st, lanes, target - m->pmix3, m->mixer_rate);
            train16(w4, w4 + lanes, m->st, lanes, target - m->pmix4, m->mixer_rate);
            if (m->extra_on) {
                train16(wx, wx + lanes, m->st, lanes, target - m->pmixx, m->mixer_rate);
            }
        }
    } else {
        int32_t *w = m->weights + m->wset * ARBITER_STRIDE(fast, max);
        train(w, w + LANES(fast, max), m->st, LANES(fast, max), target - m->pmix, m->mixer_rate);
        if (!fast && !m->mid) {
            int32_t *w2 = m->weights2 + m->wset2 * ARBITER_STRIDE(0, max);
            int32_t *w3 = m->weights3 + m->wset3 * ARBITER_STRIDE(0, max);
            int32_t *w4 = m->weights4 + m->wset4 * ARBITER_STRIDE(0, max);
            train(w2, w2 + LANES(0, max), m->st, LANES(0, max), target - m->pmix2, m->mixer_rate);
            train(w3, w3 + LANES(0, max), m->st, LANES(0, max), target - m->pmix3, m->mixer_rate);
            train(w4, w4 + LANES(0, max), m->st, LANES(0, max), target - m->pmix4, m->mixer_rate);
        }
    }
    if (!fast) {
        if (!m->mid) {
            train(m->wfinal[m->wsetf].w, &m->wfinal[m->wsetf].n, m->stf, FINAL_INPUTS_ON(m, m2), target - m->pfinal, m->mixer_rate);
        }
        /* secondary estimation: move both neighbours toward the outcome,
         * in both stages */
        g = (int32_t)(bit << 16) + (int32_t)(bit << APM_RATE) - (int32_t)bit * 2;
        row = m->apm[m->apm_ctx & (APM_CONTEXTS - 1)];
        j = m->apm_j & 31;
        v = row[j] + ((g - row[j]) >> APM_RATE);
        row[j] = (uint16_t)(v < 65535 ? v : 65535);
        v = row[j + 1] + ((g - row[j + 1]) >> APM_RATE);
        row[j + 1] = (uint16_t)(v < 65535 ? v : 65535);
        row = m->apm2[m->apm2_ctx & m->apm2_mask];
        j = m->apm2_j & 31;
        v = row[j] + ((g - row[j]) >> APM_RATE);
        row[j] = (uint16_t)(v < 65535 ? v : 65535);
        v = row[j + 1] + ((g - row[j + 1]) >> APM_RATE);
        row[j + 1] = (uint16_t)(v < 65535 ? v : 65535);
    }
    /* memories */
    node = m->node & (BLOCK - 1);
    o0_blk = m->o0_blk & 31;
#if ADAPT_X86_V3
    if (max && v3 && !m->aliased) {
        memories_learn_v3(m, bit, node, ((node << 1) | bit) & (BLOCK - 1));
    } else
#endif
    {
        s0 = m->o0[o0_blk][node];
        map_update(&m->maps[0][s0], bit, m->rate, MAP_LIMIT);
        m->o0[o0_blk][node] = m->states.next[s0][bit];
        if (fast) {
            size_t o1b = o1_blk(m);
            uint8_t s1 = m->o1[o1b][node];
            map_update(&m->maps[O1_MAP][s1], bit, m->rate, MAP_LIMIT);
            m->o1[o1b][node] = m->states.next[s1][bit];
        }
        for (i = 0; i < m->hashed; i++) {
            uint8_t *cell = m->cell[i];
            uint8_t s = *cell;
            map_update(&m->maps[1 + i][s], bit, m->rate, MAP_LIMIT);
            *cell = m->states.next[s][bit];
        }
    }
    for (i = 0; i < m->runs; i++) {
        if (RUN_ON(fast, i) && m->rcnt[i] > 0) {
            uint32_t expected = (m->rbyte[i] >> (7 - m->bitpos)) & 1;
            map_update(&m->rsm[m->rsm_idx[i]], bit == expected, m->rate, CELL_LIMIT);
        }
    }
    if (m->mlen > 0 && !m->mmiss) {
        map_update(&m->msm[m->msm_idx], bit, m->rate, CELL_LIMIT);
    }
    /* advance */
    m->c0 = (m->c0 << 1) | bit;
    m->node = ((node << 1) | bit) & (BLOCK - 1);
    m->bitpos += 1;
    if (m->bitpos == 4) {
        uint64_t hi = m->c0 & 15;
        uint64_t salt = (hi + 1) * 0x2545F4914F6CDD1Dull;
        m->o0_blk = (size_t)hi + 1;
        for (i = 0; i < m->hashed; i++) {
            uint64_t x = (m->h[i] + salt) * 0x9E3779B97F4A7C15ull;
            m->h[i] = x ^ (x >> 29);
        }
        claim_chain(m);
        m->node = 1;
        slots_refresh(m);
        if (!fast && m->coding_known) {
            /* the next byte's lines, planned now and issued over bits 5 and 6 */
            prefetch_byte(m, (uint8_t)m->coding, fast, 0);
        }
        return;
    }
    if (m->bitpos == 8) {
        end_byte(m, (uint8_t)(m->c0 & 0xFF), fast);
        start_byte(m, fast);
        return;
    }
    if (!fast) {
        /* Ahead of the claims at each boundary — every one a random line
         * of the table — the lines are brought in early. When the byte is
         * known, the twenty exact lines of the second nibble go out over
         * bits 1 and 2 and the next byte's over bits 5 and 6, ten at a
         * time, which the fill buffers hold; when it is not, the two
         * candidates' lines go out at bits 3 and 7 (the nibble's, worth
         * 3 % to the normal profile when it was the only prefetch).
         * Nothing for the fast profile, whose two contexts' lines the core
         * keeps up with. */
        if (m->coding_known) {
            if (m->bitpos == 1) {
                nibble_plan(m, (m->coding >> 4) & 15);
                issue_planned(m, 0);
            } else if (m->bitpos == 2) {
                issue_planned(m, 1);
            } else if (m->bitpos == 5) {
                issue_planned(m, 0);
            } else if (m->bitpos == 6) {
                issue_planned(m, 1);
            }
        } else if (m->bitpos == 3) {
            uint64_t hi0 = (m->c0 << 1) & 15;
            uint64_t salt0 = (hi0 + 1) * 0x2545F4914F6CDD1Dull;
            uint64_t salt1 = (hi0 + 2) * 0x2545F4914F6CDD1Dull;
            for (i = 0; i < m->hashed; i++) {
                uint64_t x0 = (m->h[i] + salt0) * 0x9E3779B97F4A7C15ull;
                uint64_t x1 = (m->h[i] + salt1) * 0x9E3779B97F4A7C15ull;
                ADAPT_PREFETCH(m->table + block_line(m, x0 ^ (x0 >> 29)) * BLOCK);
                ADAPT_PREFETCH(m->table + block_line(m, x1 ^ (x1 >> 29)) * BLOCK);
            }
        } else if (m->bitpos == 7) {
            uint8_t low = (uint8_t)((m->c0 << 1) & 0xFE);
            prefetch_byte(m, low, fast, 1);
            prefetch_byte(m, (uint8_t)(low | 1), fast, 1);
        }
    }
}

static void update(model *m, uint32_t bit) {
    update_impl(m, bit, 0, 0, 0, 0);
}

static void update_max(model *m, uint32_t bit) {
    update_impl(m, bit, 0, 1, 0, 0);
}

static void update_max2(model *m, uint32_t bit) {
    update_impl(m, bit, 0, 1, 0, 1);
}

#if ADAPT_X86_V3
static ADAPT_TARGET_V3 void update_v3(model *m, uint32_t bit) {
    update_impl(m, bit, 0, 0, 1, 0);
}

static ADAPT_TARGET_V3 void update_max_v3(model *m, uint32_t bit) {
    update_impl(m, bit, 0, 1, 1, 0);
}

static ADAPT_TARGET_V3 void update_max2_v3(model *m, uint32_t bit) {
    update_impl(m, bit, 0, 1, 1, 1);
}
#endif

static void update_fast(model *m, uint32_t bit) {
    update_impl(m, bit, 1, 0, 0, 0);
}

#endif /* ADAPT_MODEL_BIT_H */
