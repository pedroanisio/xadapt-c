/*
 * adapt_model_byte.h — the per-byte path: the kinds' hashes, the run slots, the keys' rows, start_byte, prefetch_byte, end_byte.
 *
 * A section of adapt_model.h, which includes the sections in order into
 * one translation unit: every function stays static inline and the per-bit
 * path is specialized at compile time as before. Include adapt_model.h,
 * not this file. Moved out of adapt_model.h unchanged on 2026-09-17.
 */
#ifndef ADAPT_MODEL_BYTE_H
#define ADAPT_MODEL_BYTE_H

/* Every kind's hash from a view, the kinds this file does not run left
 * 0, and the word run's hash. */
static inline void hash_kinds(const model *m, const byte_view *v, int fast, uint64_t hk[HASHED], uint64_t *rh) {
    uint64_t c = v->c8;
    hk[H_O2] = mix64((c & 0xFFFF) | ((uint64_t)2 << 56));
    if (!fast) {
        hk[H_O1] = mix64((c & 0xFF) | ((uint64_t)1 << 56));
        hk[H_O3] = mix64((c & 0xFFFFFF) | ((uint64_t)3 << 56));
        hk[H_O4] = mix64((c & 0xFFFFFFFFull) | ((uint64_t)4 << 56));
        hk[H_O6] = mix64((c & 0xFFFFFFFFFFFFull) | ((uint64_t)6 << 56));
    } else {
        /* the fast sets reach order-1 and order-4 only */
        hk[H_O1] = m->on[H_O1] ? mix64((c & 0xFF) | ((uint64_t)1 << 56)) : 0;
        hk[H_O3] = 0;
        hk[H_O4] = m->on[H_O4] ? mix64((c & 0xFFFFFFFFull) | ((uint64_t)4 << 56)) : 0;
        hk[H_O6] = 0;
    }
    hk[H_WORD] = m->on[H_WORD] ? mix64(v->word != 0 ? v->word : ((c & 0xFF) | ((uint64_t)7 << 56))) : 0;
    if (v->periodic) {
        /* A record period: the byte one period back (above), the byte
         * before it (the corner) and the previous byte (left) stand in for
         * the line above; the gradient above + left - corner is the
         * predictor a raw image's pixels follow. */
        uint64_t up = v->up;
        uint64_t corner = v->corner;
        uint64_t left = c & 0xFF;
        int64_t g = (int64_t)up + (int64_t)left - (int64_t)corner;
        uint64_t gradient = (uint64_t)(g < 0 ? 0 : (g > 255 ? 255 : g));
        hk[H_FIELD] = m->on[H_FIELD] ? mix64((gradient >> 1) | (up >> 4) << 8 | (left >> 4) << 12 | ((uint64_t)8 << 56)) : 0;
        hk[H_RECORD] = m->on[H_RECORD] ? mix64(up | left << 8 | (corner >> 2) << 16 | ((uint64_t)15 << 56)) : 0;
    } else {
        hk[H_FIELD] = m->on[H_FIELD] ? mix64((uint64_t)(v->field_pos < 31 ? v->field_pos : 31) |
                                             (uint64_t)(v->field_idx < 63 ? v->field_idx : 63) << 5 | (c & 0xFF) << 11 |
                                             ((uint64_t)8 << 56))
                                     : 0;
        hk[H_RECORD] = m->on[H_RECORD] ? mix64((uint64_t)v->above0 | (uint64_t)v->above1 << 8 | (c & 0xFF) << 16 |
                                               (uint64_t)(v->field_idx < 63 ? v->field_idx : 63) << 24 | ((uint64_t)15 << 56))
                                       : 0;
    }
    hk[H_SPARSE] = m->on[H_SPARSE] ? mix64((c & 0xFFFF0000ull) | ((uint64_t)10 << 56)) : 0;
    hk[H_CLASS] = m->class_bits != 0 ? mix64((v->classes & (((uint64_t)1 << m->class_bits) - 1)) | ((uint64_t)11 << 56)) : 0;
    /* the max profile's kinds; off under the others, so never hashed */
    hk[H_WORD2] = m->on[H_WORD2] ? mix64((v->word_prev * 0x9E3779B97F4A7C15ull) ^ (v->word != 0 ? v->word : (c & 0xFF)) ^
                                         ((uint64_t)12 << 56))
                                 : 0;
    hk[H_SPARSE2] = m->on[H_SPARSE2] ? mix64((c & 0xFF00FFull) | ((uint64_t)14 << 56)) : 0;
    hk[H_INDIRECT] = m->on[H_INDIRECT] ? mix64((c & 0xFFFF) | (uint64_t)v->followed << 16 | ((uint64_t)16 << 56)) : 0;
    hk[H_NIBBLES] = m->on[H_NIBBLES] ? mix64((c & 0xF0F0F0F0ull) | ((uint64_t)18 << 56)) : 0;
    hk[H_NEST] = m->on[H_NEST]
                     ? mix64((uint64_t)(v->nest_link & 3) | (uint64_t)(v->nest_tmpl & 3) << 2 | (uint64_t)v->nest_intag << 4 |
                             ((v->nest_intag ? v->nest_tag : v->nest_last) & 0xFFFFFF) << 8 | (c & 0xFF) << 32 |
                             ((uint64_t)19 << 56))
                     : 0;
    hk[H_O5] = m->on[H_O5] ? mix64((c & 0xFFFFFFFFFFull) | ((uint64_t)5 << 56)) : 0;
    hk[H_O8] = m->on[H_O8] ? mix64(c ^ ((uint64_t)20 << 56)) : 0;
    hk[H_WORDLC] = m->on[H_WORDLC] ? mix64((v->word_lc != 0 ? v->word_lc : (c & 0xFF)) ^ ((uint64_t)22 << 56)) : 0;
    hk[H_WORD3] = m->on[H_WORD3] ? mix64((v->word_prev2 * 0x9E3779B97F4A7C15ull + v->word_prev) ^
                                         (v->word != 0 ? v->word : (c & 0xFF)) ^ ((uint64_t)23 << 56))
                                 : 0;
    hk[H_WORDC] = m->on[H_WORDC] ? mix64((v->word != 0 ? v->word : (c & 0xFF)) ^ (uint64_t)(v->classes & 0x1FF) << 40 ^
                                         ((uint64_t)24 << 56))
                                 : 0;
    if (m->model2) {
        /* four slots whose own contexts cost more than they gave (MODEL2
         * above) carry these instead; the set's mask still decides
         * whether the slot runs */
        uint64_t len = v->mlen < 15 ? v->mlen : 15;
        if (m->on[H_WORD3] && m->slots_on) {
            hk[H_WORD3] = v->mlen > 0 ? mix64((uint64_t)v->mbyte | len << 8 | (c & 0xFF) << 12 | ((uint64_t)25 << 56))
                                      : mix64((c & 0xFF) | ((uint64_t)26 << 56));
        }
        if (m->on[H_O5]) {
            hk[H_O5] = mix64((v->classes & (((uint64_t)1 << 63) - 1)) | ((uint64_t)27 << 56));
        }
        if (m->on[H_O8] && m->slots_on) {
            hk[H_O8] = mix64((c & 0xFF) | (uint64_t)v->followed1 << 8 | ((uint64_t)28 << 56));
        }
        if (m->on[H_SPARSE2]) {
            hk[H_SPARSE2] = v->mlen > 0 ? mix64((uint64_t)v->mbyte | (uint64_t)v->mbyte2 << 8 | (c & 0xFF) << 16 |
                                                len << 24 | ((uint64_t)31 << 56))
                                        : mix64((c & 0xFFFF) | ((uint64_t)32 << 56));
        }
    }
    *rh = mix64(v->word != 0 ? (v->word ^ ((uint64_t)13 << 56)) : ((c & 0xFF) | ((uint64_t)13 << 56)));
}

/* The runs' slots and tags from the hashes: order-2 and the word for
 * every profile, the max profile's four more on order-1, -3, -4 and -6,
 * which every set keeps. */
static inline void run_slots(const model *m, const uint64_t hk[HASHED], uint64_t rh, uint64_t c8, size_t ridx[RUNS],
                             uint32_t rtag[RUNS]) {
    static const uint8_t RUN_KINDS[RUNS - BASE_RUNS] = {H_O1, H_O3, H_O4, H_O6};
    size_t i;
    ridx[0] = (size_t)(c8 & 0xFFFF);
    ridx[1] = RUN_SLOTS + ((size_t)rh & (RUN_SLOTS - 1));
    rtag[0] = 1;
    rtag[1] = ((uint32_t)(rh >> 40) & 0xFF) | 1;
    if (m->runs > BASE_RUNS) {
        for (i = 0; i < RUNS - BASE_RUNS; i++) {
            uint64_t h = hk[RUN_KINDS[i]];
            ridx[BASE_RUNS + i] = RUN_SLOTS * (BASE_RUNS + i) + ((size_t)h & (RUN_SLOTS - 1));
            rtag[BASE_RUNS + i] = ((uint32_t)(h >> 40) & 0xFF) | 1;
        }
    }
}

/* The third and fourth arbiters' rows for the byte after `classes`,
 * `nest_*` and the recall state, under the key policies (adapt.h,
 * ADAPT_MAX_KEYS_VERSION): both policies key the third by the classes of
 * the last five bytes; the fourth ADAPT_KEYS_TEXT keys by the nesting
 * state — link and template depth, in a tag, the last tag — with the
 * last two classes, ADAPT_KEYS_BINARY by recall's expected byte (256 for
 * none) and its state. Rows, not sets: the partial byte is added per bit. */
static inline void keys_rows(int keys, size_t row_mask, uint64_t classes, uint32_t nest_link, uint32_t nest_tmpl,
                             uint32_t nest_intag, uint64_t nest_last, uint32_t mlen, uint32_t mmiss, uint8_t mbyte,
                             size_t *row3, size_t *row4) {
    uint64_t k4;
    *row3 = ((size_t)(mix64((classes & 0x7FFF) | ((uint64_t)33 << 56)) >> 48) & row_mask) * 256;
    if (keys == ADAPT_KEYS_TEXT) {
        k4 = mix64((uint64_t)(nest_link & 3) | (uint64_t)(nest_tmpl & 3) << 2 | (uint64_t)(nest_intag != 0) << 4 |
                   (nest_last & 0x7FF) << 5 | (classes & 0x3F) << 16 | ((uint64_t)43 << 56));
    } else {
        uint64_t live = mlen > 0 && !mmiss;
        k4 = mix64((live ? (uint64_t)mbyte : 256) | (uint64_t)(!live ? 0 : mlen < 16 ? 1 : 2) << 9 | ((uint64_t)31 << 56));
    }
    *row4 = ((size_t)(k4 >> 48) & row_mask) * 256;
}

static inline void start_byte(model *m, int fast) {
    uint64_t c = m->c8;
    uint64_t rh;
    size_t i;
    byte_view v;
    /* Every kind's hash, then the kinds this file runs packed into their
     * slots; a kind without a slot is not hashed. */
    uint64_t hk[HASHED];
    m->c0 = 1;
    m->bitpos = 0;
    m->node = 1;
    m->o0_blk = 0;
    view_now(m, &v);
    hash_kinds(m, &v, fast, hk, &rh);
#ifdef ADAPT_SHADOW_CHECK
    /* A check build: the view prefetch_byte() advanced for this byte must
     * agree with the real one, hash for hash, or the prefetch is aimed
     * wrong — a speed fault, never a bytes fault, but one to catch. */
    if (m->shadow_valid) {
        for (i = 0; i < m->hashed; i++) {
            /* model2's two recall-derived slots are guessed after a byte
             * that breaks a match (view_after: the index may offer another
             * candidate), so their lines are aimed wrong at match
             * boundaries — a wasted prefetch there, by design */
            if (m->model2 && (m->kind[i] == H_WORD3 || m->kind[i] == H_SPARSE2)) {
                continue;
            }
            if (m->shadow_hk[m->kind[i]] != hk[m->kind[i]]) {
                fprintf(stderr, "shadow hash mismatch: kind %s at byte %llu\n", CONTEXT_NAMES[m->kind[i]],
                        (unsigned long long)m->n);
                abort();
            }
        }
        if (m->shadow_rh != rh) {
            fprintf(stderr, "shadow run hash mismatch at byte %llu\n", (unsigned long long)m->n);
            abort();
        }
        m->shadow_valid = 0;
    }
#endif
    for (i = 0; i < m->hashed; i++) {
        m->h[i] = hk[m->kind[i]];
    }
    run_slots(m, hk, rh, c, m->ridx, m->rtag);
    for (i = 0; i < m->runs; i++) {
        uint32_t e;
        if (!RUN_ON(fast, i)) {
            m->rcnt[i] = 0;
            continue;
        }
        e = m->rtab[m->ridx[i]];
        if ((e >> 16) == m->rtag[i]) {
            m->rbyte[i] = e & 0xFF;
            m->rcnt[i] = (e >> 8) & 0xFF;
        } else {
            m->rbyte[i] = 0;
            m->rcnt[i] = 0;
        }
    }
    claim_chain(m);
    slots_refresh(m);
    m->apm2_row = ((size_t)(mix64((c & 0xFFFF) | ((uint64_t)21 << 56)) >> 46) & (m->apm2_mask >> 8)) << 8;
    m->wset4_row = ((size_t)(mix64(m->word + 1) >> 48) & m->mix34_row_mask) * 256;
    if (m->keys != ADAPT_KEYS_CLASSIC) {
        keys_rows(m->keys, m->mix34_row_mask, m->classes, m->nest_link, m->nest_tmpl, m->nest_intag, m->nest_last, m->mlen,
                  m->mmiss, m->mbyte, &m->keys_row3, &m->keys_row4);
        m->wset4_row = m->keys_row4;
    }
}

static inline size_t recall_key(uint64_t c8, size_t mmask);

/* Bring in what the byte after `b` will touch first: the line of every
 * hashed context's claim, the runs' slots, the recall slot, the first
 * rows of both estimation stages and the first sets of the arbiters
 * keyed by hashes — all from the view after b, none of it changing the
 * model. The encoder calls this with the byte it is coding, once its
 * high nibble has been coded; the decoder with both values the last bit
 * can take. */
static inline void prefetch_byte(model *m, uint8_t b, int fast, int now) {
    byte_view v;
    uint64_t hk[HASHED], rh;
    size_t ridx[RUNS];
    uint32_t rtag[RUNS];
    size_t i;
    view_after(m, b, fast, &v);
    hash_kinds(m, &v, fast, hk, &rh);
#ifdef ADAPT_SHADOW_CHECK
    if (m->coding_known) {
        memcpy(m->shadow_hk, hk, sizeof(hk));
        m->shadow_rh = rh;
        m->shadow_valid = 1;
    }
#endif
    /* The claims' lines: issued now, or kept for the caller to issue a
     * few at a time — a burst of forty outstanding misses is more than
     * the core's fill buffers hold, and the rest of a burst is lost. */
    for (i = 0; i < m->hashed; i++) {
        uint64_t h = hk[m->kind[i]];
        m->planned[i] = m->table + block_line(m, h) * BLOCK;
        if (now) {
            ADAPT_PREFETCH(m->planned[i]);
        }
    }
    run_slots(m, hk, rh, v.c8, ridx, rtag);
    for (i = 0; i < m->runs; i++) {
        if (RUN_ON(fast, i)) {
            ADAPT_PREFETCH(&m->rtab[ridx[i]]);
        }
    }
    if (v.n >= MATCH_MIN) {
        ADAPT_PREFETCH(&m->mtab[recall_key(v.c8, m->mmask)]);
    }
    ADAPT_PREFETCH(&m->followed[v.c8 & 0xFFFF]);
    if (!fast) {
        size_t row2 = ((size_t)(mix64((v.c8 & 0xFFFF) | ((uint64_t)21 << 56)) >> 46) & (m->apm2_mask >> 8)) << 8;
        size_t w3 = (((size_t)(hk[H_O2] >> 48) & m->mix34_row_mask) * 256 + 1) * (size_t)ARBITER_STRIDE(0, m->max);
        size_t w4 = (((size_t)(mix64(v.word + 1) >> 48) & m->mix34_row_mask) * 256 + 1) * (size_t)ARBITER_STRIDE(0, m->max);
        size_t k;
        if (m->keys != ADAPT_KEYS_CLASSIC) {
            /* the view's recall state after `b`: a match `b` confirms
             * goes on, one it breaks is guessed absent */
            size_t row3, row4;
            keys_rows(m->keys, m->mix34_row_mask, v.classes, v.nest_link, v.nest_tmpl, v.nest_intag, v.nest_last, v.mlen, 0,
                      (uint8_t)v.mbyte, &row3, &row4);
            w3 = (row3 + 1) * (size_t)ARBITER_STRIDE(0, m->max);
            w4 = (row4 + 1) * (size_t)ARBITER_STRIDE(0, m->max);
        }
        ADAPT_PREFETCH(m->apm[((size_t)b << 8 | 1) & (APM_CONTEXTS - 1)]);
        ADAPT_PREFETCH((const char *)m->apm[((size_t)b << 8 | 1) & (APM_CONTEXTS - 1)] + 64);
        ADAPT_PREFETCH(m->apm2[(row2 | 1) & m->apm2_mask]);
        ADAPT_PREFETCH((const char *)m->apm2[(row2 | 1) & m->apm2_mask] + 64);
        for (k = 0; k < (size_t)ARBITER_STRIDE(0, m->max) * sizeof(int32_t); k += 64) {
            ADAPT_PREFETCH((const char *)(m->weights3 + w3) + k);
            ADAPT_PREFETCH((const char *)(m->weights4 + w4) + k);
        }
    }
}

static inline size_t recall_key(uint64_t c8, size_t mmask) {
    return (size_t)mix64(c8 & 0xFFFFFFFFFFull) & mmask;
}

static inline void end_byte(model *m, uint8_t b, int fast) {
    size_t i;
    uint64_t n;
    for (i = 0; i < m->runs; i++) {
        uint32_t tag = m->rtag[i];
        uint32_t *e = &m->rtab[m->ridx[i]];
        int same;
        uint32_t count;
        if (!RUN_ON(fast, i)) {
            continue;
        }
        same = (*e >> 16) == tag && (*e & 0xFF) == b;
        count = same ? (((*e >> 8) & 0xFF) < 254 ? ((*e >> 8) & 0xFF) : 254) + 1 : 1;
        *e = tag << 16 | count << 8 | b;
    }
    m->hist[(size_t)m->n & m->hmask] = b;
    m->n += 1;
    m->c8 = (m->c8 << 8) | b;
    /* the order-2 context before this byte, and what followed it */
    m->followed[(m->c8 >> 8) & 0xFFFF] = (uint16_t)(m->followed[(m->c8 >> 8) & 0xFFFF] << 8 | b);
    m->followed1[(m->c8 >> 8) & 0xFF] = (uint16_t)(m->followed1[(m->c8 >> 8) & 0xFF] << 8 | b);
    m->since_space = (b == ' ' || b == '\n') ? 0 : (m->since_space < 15 ? m->since_space + 1 : 15);
    if (!is_word_byte(b) && m->word != 0) {
        m->word_prev2 = m->word_prev;
        m->word_prev = m->word;
    }
    m->word = is_word_byte(b) ? mix64(m->word ^ ((uint64_t)b + 1)) + b : 0;
    {
        uint8_t lc = (uint8_t)((b >= 'A' && b <= 'Z') ? b + 32 : b);
        m->word_lc = is_word_byte(b) ? mix64(m->word_lc ^ ((uint64_t)lc + 1)) + lc : 0;
    }
    m->classes = (m->classes << 3) | byte_class(b);
    {
        /* markup nesting: wiki links and templates by their brackets, a tag
         * by its angle brackets, its name the word after '<' */
        uint8_t prev = (uint8_t)(m->c8 >> 8);
        if (b == '[' && prev == '[' && m->nest_link < 3) {
            m->nest_link++;
        } else if (b == ']' && prev == ']' && m->nest_link > 0) {
            m->nest_link--;
        } else if (b == '{' && prev == '{' && m->nest_tmpl < 3) {
            m->nest_tmpl++;
        } else if (b == '}' && prev == '}' && m->nest_tmpl > 0) {
            m->nest_tmpl--;
        } else if (b == '<') {
            m->nest_intag = 1;
            m->nest_tag = 0;
        } else if (b == '>') {
            if (m->nest_intag) {
                m->nest_last = m->nest_tag;
            }
            m->nest_intag = 0;
        } else if (m->nest_intag && m->nest_tag != 0xFFFFFFFF && is_word_byte(b)) {
            m->nest_tag = mix64(m->nest_tag ^ ((uint64_t)b + 1)) + b;
        } else if (m->nest_intag && (b == ' ' || b == '/')) {
            m->nest_tag = m->nest_tag != 0 ? m->nest_tag : 0xFFFFFFFF; /* the name is complete */
        }
    }
    if (fast) {
        /* neither the field nor the record context: no line to track */
        goto recall;
    }
    if (m->line_pos < ROW_MAX) {
        m->row[m->line_pos] = b;
    }
    m->line_pos += 1;
    if (b == '\n') {
        uint8_t *swap = m->row;
        m->row = m->prow;
        m->prow = swap;
        if (m->line_pos < ROW_MAX) {
            memset(m->row + m->line_pos, 0, ROW_MAX - (size_t)m->line_pos);
        }
        m->line_pos = 0;
        m->field_pos = 0;
        m->field_idx = 0;
    } else if (b == ',' || b == '\t' || b == ';' || b == '|') {
        m->field_pos = 0;
        m->field_idx += 1;
    } else {
        m->field_pos += 1;
    }
    /* recall: extend or drop the current match, then look for a new one */
recall:
    if (m->mlen > 0 && !m->mmiss) {
        m->mlen = m->mlen + 1 < 65535 ? m->mlen + 1 : 65535;
        m->mptr += 1;
    } else {
        m->mlen = 0;
    }
    n = m->n;
    if (n >= MATCH_MIN) {
        size_t key = recall_key(m->c8, m->mmask);
        size_t ways = (size_t)RECALL_WAYS(m->model2);
        size_t bucket = key & ~(ways - 1);
        if (m->mlen == 0) {
            /* The table keeps the low 32 bits of a position; the age
             * recovers it when the candidate is still in the window, and
             * the byte check rejects anything older that aliases. Under
             * model2 a bucket holds the four most recent positions and
             * the longest verified candidate wins, the most recent on a
             * tie. */
            size_t w, best_len = 0;
            uint64_t best_cand = 0;
            for (w = 0; w < ways; w++) {
                uint64_t age = (uint32_t)((uint32_t)n - m->mtab[bucket | w]);
                if (age > 0 && age + MATCH_VERIFY < (uint64_t)m->hmask + 1) {
                    uint64_t cand = n - age;
                    size_t len = 0;
                    while (len < MATCH_VERIFY && (uint64_t)len < cand &&
                           m->hist[(size_t)(cand - 1 - len) & m->hmask] == m->hist[(size_t)(n - 1 - len) & m->hmask]) {
                        len++;
                    }
                    if (len > best_len) {
                        best_len = len;
                        best_cand = cand;
                    }
                }
            }
            if (best_len >= MATCH_MIN) {
                m->mlen = (uint32_t)best_len;
                m->mptr = best_cand;
            }
        }
        {
            size_t w;
            for (w = ways - 1; w > 0; w--) {
                m->mtab[bucket | w] = m->mtab[bucket | (w - 1)];
            }
            m->mtab[bucket] = (uint32_t)n;
        }
    }
    m->mmiss = 0;
    if (m->mlen > 0) {
        m->mbyte = m->hist[(size_t)m->mptr & m->hmask];
    }
}

/* Pass a stored, skipped block through: the bytes enter the window and the
 * recall table, and nothing else moves. */
static inline void skip_bytes(model *m, const uint8_t *bytes, size_t len) {
    size_t i;
    for (i = 0; i < len; i++) {
        uint8_t b = bytes[i];
        if (m->adaptive != NULL) adaptive_observe_byte(m->adaptive, b);
        m->hist[(size_t)m->n & m->hmask] = b;
        m->n += 1;
        m->c8 = (m->c8 << 8) | b;
        if (m->n >= MATCH_MIN) {
            m->mtab[recall_key(m->c8, m->mmask)] = (uint32_t)m->n;
        }
    }
    m->mlen = 0;
    m->mmiss = 0;
}

#if ADAPT_X86_V3
/* The max profile's context inputs, eight slots at a time: the cell of
 * every slot's state gathered from its map, the probability and its
 * stretch, the linear lane and the third lane, into the three planes of
 * st[] — the stretched probabilities of the slots first, then their
 * linear lanes, then their third lanes. The three groups cover 24 slots,
 * so each plane's stores run three past the last slot into the next
 * plane, which is stored after it and puts its own values there; the
 * runs' lanes after the last plane are written by the caller, after
 * this. Slots a file's context set leaves out are zeroed at the end, as
 * the scalar path leaves them. */
static inline ADAPT_TARGET_V3 void context_lanes_v3(model *m) {
    const int *cells = (const int *)&m->maps[0][0];
    const __m256i one = _mm256_set1_epi32(1), c512 = _mm256_set1_epi32(512), c4096 = _mm256_set1_epi32(4096);
    const __m256i zero = _mm256_setzero_si256(), two = _mm256_set1_epi32(2), three = _mm256_set1_epi32(3);
    __m256i l1[3], l2[3];
    /* the groups the file's slots fill: a set that leaves kinds out ends
     * short of the third, and its lanes past the last slot stay zero */
    const int groups = m->lane_groups;
    int g;
    size_t i;
    for (g = 0; g < groups; g++) {
        /* the eight cells by scalar loads: a gather is microcoded on the
         * cores this runs on, and dearer than the inserts */
        const uint8_t *cur = m->cur + 8 * g;
        const int *row = cells + g * 8 * 256;
        __m256i c = _mm256_setr_epi32(row[0 * 256 + cur[0]], row[1 * 256 + cur[1]], row[2 * 256 + cur[2]],
                                      row[3 * 256 + cur[3]], row[4 * 256 + cur[4]], row[5 * 256 + cur[5]],
                                      row[6 * 256 + cur[6]], row[7 * 256 + cur[7]]);
        __m256i p = _mm256_max_epi32(_mm256_srli_epi32(c, 20), one);
        __m256i q = _mm256_srai_epi32(p, 2);
        __m256i mode = _mm256_and_si256(_mm256_srli_epi32(c, CELL_MODE_SHIFT), three);
        __m256i down = _mm256_sub_epi32(zero, _mm256_srai_epi32(_mm256_sub_epi32(c4096, p), 2));
        _mm256_storeu_si256((__m256i *)(void *)(m->cellv + 8 * g), c);
        _mm256_storeu_si256((__m256i *)(void *)(m->st + 8 * g), _mm256_i32gather_epi32(m->stretch32, p, 4));
        l1[g] = _mm256_sub_epi32(q, c512);
        l2[g] = _mm256_or_si256(_mm256_and_si256(q, _mm256_cmpeq_epi32(mode, one)),
                                _mm256_and_si256(down, _mm256_cmpeq_epi32(mode, two)));
    }
    for (g = 0; g < groups; g++) {
        _mm256_storeu_si256((__m256i *)(void *)(m->st + (1 + HASHED) + 8 * g), l1[g]);
    }
    for (g = 0; g < groups; g++) {
        _mm256_storeu_si256((__m256i *)(void *)(m->st + 2 * (1 + HASHED) + 8 * g), l2[g]);
    }
    {
        /* the last group's lanes past the last slot, in each plane, up
         * to the plane's end (a third group spills into the next plane,
         * which is stored after it); the lanes beyond the groups were
         * zeroed once, at model_new, and no store reaches them */
        int32_t *st_ = m->st;
        const size_t from = 1 + m->hashed;
        const size_t to = (size_t)groups * 8 < 1 + HASHED ? (size_t)groups * 8 : 1 + HASHED;
        for (i = from; i < to; i++) {
            st_[i] = 0;
            st_[(1 + HASHED) + i] = 0;
            st_[2 * (1 + HASHED) + i] = 0;
        }
    }
}

/* Learn `bit` into every slot's cell, eight at a time from the words
 * predict() gathered, and move each slot's state on: the same arithmetic
 * as map_update() and the transition table, the results written back
 * one slot at a time. */
static inline ADAPT_TARGET_V3 void memories_learn_v3(model *m, uint32_t bit, size_t node, size_t node_next) {
    uint32_t *cells = &m->maps[0][0];
    const __m256i target = _mm256_set1_epi32(bit ? (1 << 22) - 1 : 0);
    const __m256i m255 = _mm256_set1_epi32((int)CELL_COUNT_MASK), c255 = _mm256_set1_epi32(255);
    const __m256i modebits = _mm256_set1_epi32(3 << CELL_MODE_SHIFT);
    const uint8_t *next = m->states.next8[bit];
    uint32_t learned[SLOTS];
    uint8_t *const *cbase = m->cbase;
    uint8_t *cur = m->cur;
    const size_t slots = 1 + m->hashed;
    const int groups = m->lane_groups;
    int g;
    size_t i;
    for (g = 0; g < groups; g++) {
        __m256i c = _mm256_loadu_si256((const __m256i *)(const void *)(m->cellv + 8 * g));
        __m256i n = _mm256_and_si256(c, m255);
        __m256i p = _mm256_srli_epi32(c, 10);
        __m256i d = _mm256_srai_epi32(_mm256_sub_epi32(target, p), 6);
        __m256i r = _mm256_i32gather_epi32(m->rate, n, 4);
        __m256i p2 = _mm256_add_epi32(p, _mm256_srai_epi32(_mm256_mullo_epi32(d, r), 9));
        __m256i n2 = _mm256_sub_epi32(n, _mm256_cmpgt_epi32(c255, n));
        __m256i keep = _mm256_and_si256(c, modebits);
        _mm256_storeu_si256((__m256i *)(void *)(learned + 8 * g),
                            _mm256_or_si256(_mm256_or_si256(_mm256_slli_epi32(p2, 10), keep), n2));
    }
    /* Write back, and read each slot's state at the next node for the
     * next bit — at a boundary slots_refresh() replaces it. The next
     * states are gathered into a register and stored eight at a time:
     * eight byte stores followed by the eight-byte load predict() makes
     * of them cannot be forwarded and would wait for every one to
     * commit. The state stores are bytes, which could alias anything,
     * so the bounds and pointers are locals. */
    for (g = 0; g < groups; g++) {
        uint64_t word = 0;
        size_t base = (size_t)g * 8, k, n;
        if (base >= slots) {
            break;
        }
        n = slots - base < 8 ? slots - base : 8;
        for (k = 0; k < n; k++) {
            uint8_t *blk = cbase[base + k];
            uint8_t s = cur[base + k];
            cells[(base + k) * 256 + s] = learned[base + k];
            blk[node] = next[s];
            word |= (uint64_t)blk[node_next] << (8 * k);
        }
        memcpy(cur + base, &word, 8);
    }
    (void)i;
}
#endif

#endif /* ADAPT_MODEL_BYTE_H */
