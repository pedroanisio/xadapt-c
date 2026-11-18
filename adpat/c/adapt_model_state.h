/*
 * adapt_model_state.h — the model: its struct, the arena and the tables in it, the claims, the byte view.
 *
 * A section of adapt_model.h, which includes the sections in order into
 * one translation unit: every function stays static inline and the per-bit
 * path is specialized at compile time. Include adapt_model.h, not this file.
 */
#ifndef ADAPT_MODEL_STATE_H
#define ADAPT_MODEL_STATE_H

/* ------------------------------------------------------------------------
 * Predictor — the predictor
 * --------------------------------------------------------------------- */

typedef struct model {
    state_table states;
    int16_t stretch[4096];
    /* squash(d) for d in -2047..=2047, at d + 2047: the mixers' outputs
     * are clamped to that range, so a lookup replaces the interpolation. */
    int16_t squashed[4095];
    int32_t rate[RATE_LEN];
    /* Observations a state stands for: the replacement priority of a
     * block whose first node is in that state. */
    uint8_t seen[256];
    int32_t mixer_rate[MIXER_COUNT_MAX + 1];
    uint32_t maps[HASHED + 1][256];
    /* 2^table_log blocks of 16 bytes: slot 0 a tag, slots 1..15 the states
     * of one nibble's fifteen binary decisions. */
    uint8_t *table;
    size_t table_mask;
    /* The table's lines (blocks / WAYS) when its block count is not a
     * power of two (a fraction, see adapt_table_blocks): a hash then maps
     * to a line by a range multiply; 0 for a power of two, where the mask
     * above does it and the bytes are those of the unsized formats. */
    size_t table_lines;
    uint8_t o0[32][BLOCK];
    /* The fast profile's direct order-1 memory; untouched under the normal
     * profile. */
    uint8_t o1[O1_BLOCKS][BLOCK];
    size_t blk[HASHED];
    /* The block of each context for the nibble being coded, as a
     * pointer, set beside blk by claim_chain(). */
    uint8_t *base[HASHED];
    /* The cell of each context for the bit being coded, found by predict()
     * and updated by update(). */
    uint8_t *cell[HASHED];
    /* The memories by slot — 0 the order-0 memory, 1.. the hashed
     * contexts in slot order — as the vector path reads them: each
     * slot's block for the nibble, its state for the bit being coded,
     * and the cell word predict() gathered, which update() learns from.
     * SLOTS is the count rounded up to the eight-lane loads. */
#define SLOTS 24
    uint8_t *cbase[SLOTS];
    uint8_t cur[SLOTS];
    uint32_t cellv[SLOTS];
    /* Whether two hashed contexts share a block this nibble, in which
     * case the per-bit path runs its scalar loops; see slots_refresh()
     * and claim_chain(), whose stamp table and stamp these are. */
    int aliased;
#define ALIAS_SLOTS 4096
    uint8_t alias_seen[ALIAS_SLOTS];
    uint8_t alias_stamp;
    /* The stretch again as 32-bit words, for the vector path's gather. */
    int32_t stretch32[4096];
    size_t o0_blk;
    uint64_t h[HASHED];
    /* Three bits per byte, the newest lowest. */
    uint64_t classes;
    uint32_t class_bits;
    /* The hashed contexts this file runs, packed into the first `hashed`
     * slots in kind order: kind[k] is the H_* index slot k holds; on[i]
     * says whether kind i has a slot. */
    uint8_t kind[HASHED];
    int on[HASHED];
    size_t hashed;
    /* Run models this file runs: RUNS, or 0 when its set leaves them out. */
    size_t runs;
    /* The profile: 1 for the fast profile (format version 11), 0 for the
     * normal one. Read once per block; the per-bit path has it constant. */
    int fast;
    /* 1 for the max profile (format version 12); never with fast. */
    int max;
    /* 1 for the compact side tables (ADAPT_MAX_COMPACT_VERSION): the
     * masks below size the third and fourth arbiters' sets and the
     * second estimation stage's rows, read once per bit. */
    int compact;
    /* Immutable dispatch choice owned by this model, never shared by workers. */
    int cpu_v3;
    /* 1 when the arbiters' starting weights are 1/32 rather than 1/4
     * (ADAPT_SIZING_LOW_START); read once, at model_new. */
    int low_start;
    /* 1 for the second max model (ADAPT_SIZING_MODEL2, see MODEL2 above):
     * int16 arbiters, four substituted context slots, four-way recall
     * buckets and a fifth arbiter. Read once per block; the per-bit path
     * has it constant. */
    int model2;
    /* 1 under model2 with the precise coder (ADAPT_MAX_PRECISE_VERSION):
     * predict() leaves the probability in sixteen bits in p16 beside the
     * twelve it returns, and a block is coded by either. */
    int precise;
    uint32_t p16;
    /* model2's pressure-gated parts (MODEL2_PRESSURE_LOG): the o8 and
     * word3 slots re-purposed, and the fifth arbiter */
    int slots_on, extra_on;
    /* the mid profile: the normal path with its first arbiter alone */
    int mid;
    /* the arbiters' key policy (adapt_sizes.keys): under ADAPT_KEYS_TEXT
     * and ADAPT_KEYS_BINARY the third and fourth arbiters' rows are set
     * per byte (keys_row3, keys_row4, see keys_rows) and the second's set
     * is by the classes under ADAPT_KEYS_TEXT */
    int keys;
    size_t keys_row3, keys_row4;
    /* groups of eight slots the vector lane path runs: ceil((1 + hashed) / 8) */
    int lane_groups;
    size_t mix34_row_mask;
    size_t apm2_mask;
    /* The record period in bytes, or 0: when set, the record and field
     * contexts read the byte one period back and its neighbours in place
     * of the line above (Core::period). */
    size_t period;
    /* The last 2^window_log bytes, a ring indexed by absolute position. */
    uint8_t *hist;
    size_t hmask;
    /* Bytes seen so far, skipped ones included. */
    uint64_t n;
    uint64_t c8;
    uint64_t word;
    /* The hash of the last complete word, for the word-pair context. */
    uint64_t word_prev;
    /* What followed each order-2 context the last two times. */
    uint16_t followed[1 << 16];
    /* What followed each order-1 context the last two times (model2). */
    uint16_t followed1[256];
    /* Bytes since the last space or newline, at most 15 (model2). */
    uint32_t since_space;
    /* markup nesting: open [[ and {{ counts, inside a <tag>, the tag's name
     * hash while inside it (fixed once the name ends) and the last one
     * closed */
    uint32_t nest_link, nest_tmpl, nest_intag;
    uint64_t nest_tag, nest_last;
    /* the case-folded word, and the word before the previous one */
    uint64_t word_lc, word_prev2;
    /* runs: tag << 16 | count << 8 | byte, directly indexed */
    uint32_t *rtab;
    uint32_t rtag[RUNS];
    size_t ridx[RUNS];
    uint32_t rbyte[RUNS];
    uint32_t rcnt[RUNS];
    uint32_t rsm[RUNS * RUN_BUCKETS];
    size_t rsm_idx[RUNS];
    /* recall */
    uint32_t *mtab;
    size_t mmask;
    uint64_t mptr;
    uint32_t mlen;
    uint32_t mbyte;
    int mmiss;
    uint32_t msm[MATCH_BUCKETS * 2];
    size_t msm_idx;
    /* arbiters */
    /* The inputs, padded to sixteen lanes for the int16 pack; under model2
     * also packed as int16 and as int16 times sixteen. */
    int32_t st[INPUTS + 16];
    int16_t st16[INPUTS + 16];
    int16_t st16s[INPUTS + 16];
    int32_t *weights;
    /* model2's int16 arbiters, in the same sets as the int32 ones, and its
     * fifth (EXTRA_SETS sets keyed by the bytes since a space and the
     * previous byte's class, with the partial byte). */
    int16_t *w16, *w16_2, *w16_3, *w16_4, *w16_x;
    size_t wsetx;
    int32_t pmixx;
    size_t wset;
    int32_t pmix;
    int32_t *weights2;
    size_t wset2;
    int32_t pmix2;
    int32_t *weights3;
    size_t wset3;
    int32_t pmix3;
    int32_t *weights4;
    size_t wset4;
    int32_t pmix4;
    arbiter_final *wfinal;
    int32_t stf[FINAL_INPUTS];
    size_t wsetf;
    int32_t pfinal;
    uint16_t (*apm)[APM_BUCKETS];
    size_t apm_ctx;
    size_t apm_j;
    /* the second estimation stage, keyed by an order-2 hash and the
     * partial byte, averaged with the first: 2^18 rows */
    uint16_t (*apm2)[APM_BUCKETS];
    size_t apm2_ctx;
    size_t apm2_j;
    /* The byte's part of the second stage's row and of the fourth
     * arbiter's set — an order-2 hash and the word hash — computed once
     * in start_byte() rather than on every bit. */
    size_t apm2_row, wset4_row;
    /* the current line's bytes and the previous line's, for the record context */
    uint8_t *row;
    uint8_t *prow;
    uint64_t line_pos;
    uint32_t field_pos;
    uint32_t field_idx;
    /* bit state */
    uint32_t c0;
    uint32_t bitpos;
    size_t node;
    /* The byte being coded, when the caller knows it (the encoder does,
     * the decoder does not): prefetch_byte() takes it at the nibble;
     * otherwise both values the last bit leaves possible, at bit seven. */
    uint32_t coding;
    int coding_known;
    /* Lines prefetch_byte() and nibble_plan() chose, issued a half at a
     * time over the following bits. */
    const uint8_t *planned[HASHED];
    /* The hashes prefetch_byte() made for the byte to come, kept for
     * start_byte(): one set for the encoder's known byte, two for the
     * decoder's candidates, filed by the byte's low bit and stamped with
     * the position and the bytes they are of (see ahead_take). */
    uint64_t ahead_hk[2][HASHED];
    uint64_t ahead_rh[2];
    uint64_t ahead_c8[2];
    uint64_t ahead_n[2];
    int ahead_valid[2];
    /* The arena every table above points into: what was mapped or
     * allocated, and its length, for model_free. */
    void *arena_base;
    size_t arena_len;
    adaptive_model *adaptive;
} model;

static inline void model_free(model *m) {
    if (m == NULL) {
        return;
    }
    if (m->arena_base != NULL) {
#ifdef ADAPT_ARENA_MMAP
        munmap(m->arena_base, m->arena_len);
#else
        free(m->arena_base);
#endif
    }
    free(m->adaptive);
    free(m);
}

/* Blocks of the context table: a power of two, or with a fraction
 * (8 + f) << (table_log - 3); the public adapt_table_blocks. */
static inline uint64_t table_blocks(adapt_sizes sizes) {
    return ((uint64_t)8 + sizes.table_frac) << (sizes.table_log - 3);
}

/* Place the tables in an arena starting at `base`, largest and most
 * randomly accessed first, each on its own page, and return the bytes
 * they span; with base NULL it only measures. */
static inline size_t model_layout(model *m, uint8_t *base, adapt_sizes sizes) {
    size_t at = 0;
#define ADAPT_PLACE(field, type, bytes, align)                                                     \
    do {                                                                                            \
        at = (at + ((align) - 1)) & ~(size_t)((align) - 1);                                         \
        m->field = (type)(base == NULL ? NULL : (void *)(base + at));                               \
        at += (bytes);                                                                              \
    } while (0)
    ADAPT_PLACE(table, uint8_t *, (size_t)table_blocks(sizes) * BLOCK, 4096);
    ADAPT_PLACE(apm, uint16_t(*)[APM_BUCKETS], APM_CONTEXTS * sizeof(*m->apm), 4096);
    ADAPT_PLACE(apm2, uint16_t(*)[APM_BUCKETS], APM2_CONTEXTS(sizes.max, sizes.compact) * sizeof(*m->apm2), 4096);
    ADAPT_PLACE(mtab, uint32_t *, ((size_t)1 << sizes.match_log) * sizeof(uint32_t), 4096);
    ADAPT_PLACE(rtab, uint32_t *, (size_t)RUNS_OF(sizes.max) * RUN_SLOTS * sizeof(uint32_t), 4096);
    ADAPT_PLACE(hist, uint8_t *, (size_t)1 << sizes.window_log, 4096);
    /* the int32 arbiters of every model but model2, whose int16 ones follow */
    ADAPT_PLACE(weights, int32_t *, sizes.model2 ? 0 : MIXER_SETS * (size_t)ARBITER_STRIDE(sizes.fast, sizes.max) * sizeof(int32_t), 64);
    ADAPT_PLACE(weights2, int32_t *, sizes.model2 ? 0 : MIXER2_SETS * (size_t)ARBITER_STRIDE(0, sizes.max) * sizeof(int32_t), 64);
    ADAPT_PLACE(weights3, int32_t *, sizes.model2 ? 0 : MIXER34_SETS(sizes.compact) * (size_t)ARBITER_STRIDE(0, sizes.max) * sizeof(int32_t), 4096);
    ADAPT_PLACE(weights4, int32_t *, sizes.model2 ? 0 : MIXER34_SETS(sizes.compact) * (size_t)ARBITER_STRIDE(0, sizes.max) * sizeof(int32_t), 4096);
    ADAPT_PLACE(w16, int16_t *, sizes.model2 ? MIXER_SETS * (size_t)ARBITER_STRIDE(0, 1) * sizeof(int16_t) : 0, 64);
    ADAPT_PLACE(w16_2, int16_t *, sizes.model2 ? MIXER2_SETS * (size_t)ARBITER_STRIDE(0, 1) * sizeof(int16_t) : 0, 64);
    ADAPT_PLACE(w16_3, int16_t *, sizes.model2 ? MIXER34_SETS(sizes.compact) * (size_t)ARBITER_STRIDE(0, 1) * sizeof(int16_t) : 0, 4096);
    ADAPT_PLACE(w16_4, int16_t *, sizes.model2 ? MIXER34_SETS(sizes.compact) * (size_t)ARBITER_STRIDE(0, 1) * sizeof(int16_t) : 0, 4096);
    ADAPT_PLACE(w16_x, int16_t *, (size_t)EXTRA_SETS_OF(sizes.model2) * (size_t)ARBITER_STRIDE(0, 1) * sizeof(int16_t), 4096);
    ADAPT_PLACE(wfinal, arbiter_final *, FINAL_SETS * sizeof(arbiter_final), 64);
    ADAPT_PLACE(row, uint8_t *, ROW_MAX, 64);
    ADAPT_PLACE(prow, uint8_t *, ROW_MAX, 64);
#undef ADAPT_PLACE
    return at;
}

/* Map or allocate the arena, zeroed, and point the tables into it. */
static inline int model_arena(model *m, adapt_sizes sizes) {
    size_t bytes = model_layout(m, NULL, sizes);
    uint8_t *base;
#ifdef ADAPT_ARENA_MMAP
    size_t len = bytes + ARENA_ALIGN;
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        return 0;
    }
    base = (uint8_t *)(((uintptr_t)p + (ARENA_ALIGN - 1)) & ~(uintptr_t)(ARENA_ALIGN - 1));
    /* Advisory: a kernel that declines leaves ordinary pages, and the
     * model is unchanged. */
    (void)madvise(base, bytes, MADV_HUGEPAGE);
#else
    size_t len = bytes + 64;
    void *p = calloc(len, 1);
    if (p == NULL) {
        return 0;
    }
    base = (uint8_t *)(((uintptr_t)p + 63) & ~(uintptr_t)63);
#endif
    m->arena_base = p;
    m->arena_len = len;
    model_layout(m, base, sizes);
    return 1;
}

/* Find or claim the block for hash h: WAYS blocks of one cache line, a
 * miss evicting the candidate whose first node has seen the least. */
/* The first block of the line hash h selects: for a power-of-two table
 * the masked low bits, for any other size the low 32 bits scaled to the
 * line count (a multiply and a shift, uniform over the lines). */
static inline size_t block_line(const model *m, uint64_t h) {
    if (m->table_lines != 0) {
        return (size_t)(((uint64_t)(uint32_t)h * (uint64_t)m->table_lines) >> 32) * WAYS;
    }
    return ((size_t)h & m->table_mask) & ~(size_t)(WAYS - 1);
}

static inline size_t claim(model *m, uint64_t h) {
    size_t base = block_line(m, h);
    uint8_t tag = (uint8_t)(h >> 56);
    uint8_t *line = m->table + base * BLOCK;
    /* Compare the four tags in one cache line before branching, avoiding
     * a data-dependent branch per way. The first matching way must win
     * to preserve deterministic replacement order. */
    unsigned hits = (unsigned)(line[0 * BLOCK] == tag) | (unsigned)(line[1 * BLOCK] == tag) << 1 |
                    (unsigned)(line[2 * BLOCK] == tag) << 2 | (unsigned)(line[3 * BLOCK] == tag) << 3;
    if (hits != 0) {
        size_t k = 0;
        k += (hits & 1) == 0;
        k += (hits & 3) == 0;
        k += (hits & 7) == 0;
        return base + k;
    }
    {
        /* A miss: evict the way whose first node has seen the least, the
         * earliest of equals — the same choice the loop made with its
         * strict compare against a falling minimum. */
        unsigned s0 = m->seen[line[0 * BLOCK + 1]], s1 = m->seen[line[1 * BLOCK + 1]],
                 s2 = m->seen[line[2 * BLOCK + 1]], s3 = m->seen[line[3 * BLOCK + 1]];
        size_t victim = 0;
        unsigned fewest = s0 < 255 ? s0 : 255;
        if (s1 < fewest) {
            fewest = s1;
            victim = 1;
        }
        if (s2 < fewest) {
            fewest = s2;
            victim = 2;
        }
        if (s3 < fewest) {
            victim = 3;
        }
        uint8_t *b = line + victim * BLOCK;
        memset(b, 0, BLOCK);
        b[0] = tag;
        return base + victim;
    }
}

/* Point the slots at their blocks for the nibble and read each one's
 * state at the first node: after every claim_chain(). Two hashed
 * contexts can claim one block — their hashes fall in one line and both
 * miss, so both evict the way with the fewest observations, which the
 * first claim has just zeroed — and then the scalar path's order shows:
 * the second context reads the state the first has already moved on.
 * The vector path reads every state at once, so a nibble with such a
 * pair takes the scalar path; claim_chain() sets `aliased` to say so. */
static inline void slots_refresh(model *m) {
    size_t i;
    const size_t hashed = m->hashed;
    uint8_t **cbase = m->cbase;
    uint8_t *cur = m->cur;
    cbase[0] = m->o0[m->o0_blk & 31];
    for (i = 0; i < hashed; i++) {
        cbase[1 + i] = m->base[i];
    }
    /* Eight states at a time into one word, one store each: the vector
     * path loads them eight at a time, which eight byte stores could not
     * forward to. */
    for (i = 0; i <= hashed; i += 8) {
        uint64_t word = 0;
        size_t k, n = hashed + 1 - i < 8 ? hashed + 1 - i : 8;
        for (k = 0; k < n; k++) {
            word |= (uint64_t)cbase[i + k][1] << (8 * k);
        }
        memcpy(cur + i, &word, 8);
    }
}



/* Claim every hashed context's block, and find whether two of them are
 * one block (see slots_refresh): each index stamps a small table on its
 * low bits with the nibble's stamp, and a stamp already there names a
 * candidate pair, which the exact scan then settles. A true alias always
 * stamps twice, so none escapes; a false candidate costs one scan. The
 * stamps are a byte, cleared when they wrap. */
static inline void claim_chain(model *m) {
    size_t i, j;
    uint8_t stamp;
    int candidate = 0;
    if (++m->alias_stamp == 0) {
        memset(m->alias_seen, 0, sizeof(m->alias_seen));
        m->alias_stamp = 1;
    }
    stamp = m->alias_stamp;
    for (i = 0; i < m->hashed; i++) {
        size_t blk = claim(m, m->h[i]);
        uint8_t *slot = &m->alias_seen[blk & (ALIAS_SLOTS - 1)];
        m->blk[i] = blk;
        m->base[i] = m->table + blk * BLOCK;
        candidate |= *slot == stamp;
        *slot = stamp;
    }
    m->aliased = 0;
    if (candidate) {
        for (i = 1; i < m->hashed; i++) {
            for (j = 0; j < i; j++) {
                m->aliased |= m->blk[i] == m->blk[j];
            }
        }
    }
}

/* The order-1 memory's block: the previous byte's 32, at the order-0
 * memory's nibble position. */
static inline size_t o1_blk(const model *m) {
    return ((size_t)(m->c8 & 0xFF) << 5 | (m->o0_blk & 31)) & (O1_BLOCKS - 1);
}

/* What the context hashes of a byte are computed from: the byte-level
 * state end_byte() advances, and the few table reads start_byte() makes
 * of it. Filled either from the model as it stands (view_now) or as it
 * will stand after a byte the encoder already knows (view_after), so the
 * lines the next byte's claims will take can be brought in early. The
 * second only guides prefetches: a mismatch there costs a wasted line
 * and nothing else, since the model's bytes never depend on it. */
typedef struct byte_view {
    uint64_t c8, n, word, word_lc, word_prev, word_prev2, classes;
    uint32_t nest_link, nest_tmpl, nest_intag;
    uint64_t nest_tag, nest_last;
    uint64_t line_pos;
    uint32_t field_pos, field_idx;
    /* what followed the order-2 context c8 & 0xFFFF the last two times */
    uint16_t followed;
    /* the line above at line_pos and line_pos + 1, both clamped to the row */
    uint8_t above0, above1;
    /* the record period's neighbours, when `periodic` */
    uint8_t up, corner;
    int periodic;
    /* model2: recall's expected byte, the one after it and its length
     * after the byte (0 for none), and what followed the previous byte
     * the last two times */
    uint32_t mbyte, mbyte2, mlen;
    uint16_t followed1;
} byte_view;

static inline void view_now(const model *m, byte_view *v) {
    v->c8 = m->c8;
    v->n = m->n;
    v->word = m->word;
    v->word_lc = m->word_lc;
    v->word_prev = m->word_prev;
    v->word_prev2 = m->word_prev2;
    v->classes = m->classes;
    v->nest_link = m->nest_link;
    v->nest_tmpl = m->nest_tmpl;
    v->nest_intag = m->nest_intag;
    v->nest_tag = m->nest_tag;
    v->nest_last = m->nest_last;
    v->line_pos = m->line_pos;
    v->field_pos = m->field_pos;
    v->field_idx = m->field_idx;
    v->followed = m->followed[m->c8 & 0xFFFF];
    v->above0 = m->prow[m->line_pos < ROW_MAX - 1 ? m->line_pos : ROW_MAX - 1];
    v->above1 = m->prow[m->line_pos + 1 < ROW_MAX - 1 ? m->line_pos + 1 : ROW_MAX - 1];
    v->periodic = m->period != 0 && (size_t)m->n > m->period;
    v->up = v->periodic ? m->hist[((size_t)m->n - m->period) & m->hmask] : 0;
    v->corner = v->periodic ? m->hist[((size_t)m->n - m->period - 1) & m->hmask] : 0;
    v->mlen = m->mlen > 0 && !m->mmiss ? m->mlen : 0;
    v->mbyte = m->mbyte;
    v->mbyte2 = v->mlen > 0 ? m->hist[(size_t)(m->mptr + 1) & m->hmask] : 0;
    v->followed1 = m->followed1[m->c8 & 0xFF];
}

/* The view after end_byte(b): each field moved as end_byte() moves it,
 * in the same order, the table reads taken from what end_byte() will
 * have written. */
static inline void view_after(const model *m, uint8_t b, int fast, byte_view *v) {
    uint64_t c8 = (m->c8 << 8) | b;
    uint64_t n = m->n + 1;
    uint8_t prev = (uint8_t)(m->c8 & 0xFF);
    size_t idx_written = (size_t)((c8 >> 8) & 0xFFFF), idx_read = (size_t)(c8 & 0xFFFF);
    uint16_t f = m->followed[idx_read];
    v->c8 = c8;
    v->n = n;
    v->word_prev = m->word_prev;
    v->word_prev2 = m->word_prev2;
    if (!is_word_byte(b) && m->word != 0) {
        v->word_prev2 = m->word_prev;
        v->word_prev = m->word;
    }
    v->word = is_word_byte(b) ? mix64(m->word ^ ((uint64_t)b + 1)) + b : 0;
    {
        uint8_t lc = (uint8_t)((b >= 'A' && b <= 'Z') ? b + 32 : b);
        v->word_lc = is_word_byte(b) ? mix64(m->word_lc ^ ((uint64_t)lc + 1)) + lc : 0;
    }
    v->classes = (m->classes << 3) | byte_class(b);
    v->nest_link = m->nest_link;
    v->nest_tmpl = m->nest_tmpl;
    v->nest_intag = m->nest_intag;
    v->nest_tag = m->nest_tag;
    v->nest_last = m->nest_last;
    if (b == '[' && prev == '[' && m->nest_link < 3) {
        v->nest_link++;
    } else if (b == ']' && prev == ']' && m->nest_link > 0) {
        v->nest_link--;
    } else if (b == '{' && prev == '{' && m->nest_tmpl < 3) {
        v->nest_tmpl++;
    } else if (b == '}' && prev == '}' && m->nest_tmpl > 0) {
        v->nest_tmpl--;
    } else if (b == '<') {
        v->nest_intag = 1;
        v->nest_tag = 0;
    } else if (b == '>') {
        if (m->nest_intag) {
            v->nest_last = m->nest_tag;
        }
        v->nest_intag = 0;
    } else if (m->nest_intag && m->nest_tag != 0xFFFFFFFF && is_word_byte(b)) {
        v->nest_tag = mix64(m->nest_tag ^ ((uint64_t)b + 1)) + b;
    } else if (m->nest_intag && (b == ' ' || b == '/')) {
        v->nest_tag = m->nest_tag != 0 ? m->nest_tag : 0xFFFFFFFF;
    }
    v->line_pos = m->line_pos;
    v->field_pos = m->field_pos;
    v->field_idx = m->field_idx;
    v->above0 = v->above1 = 0;
    if (!fast) {
        /* the row after b is written at line_pos: on a newline it becomes
         * the line above, read at 0 and 1; otherwise the line above stays */
        if (b == '\n') {
            uint64_t k0 = 0 < ROW_MAX - 1 ? 0 : ROW_MAX - 1, k1 = 1 < ROW_MAX - 1 ? 1 : ROW_MAX - 1;
            v->above0 = (m->line_pos == k0 && m->line_pos < ROW_MAX) ? b : m->row[k0];
            v->above1 = (m->line_pos == k1 && m->line_pos < ROW_MAX) ? b : m->row[k1];
            v->line_pos = 0;
            v->field_pos = 0;
            v->field_idx = 0;
        } else {
            uint64_t lp = m->line_pos + 1;
            v->line_pos = lp;
            v->above0 = m->prow[lp < ROW_MAX - 1 ? lp : ROW_MAX - 1];
            v->above1 = m->prow[lp + 1 < ROW_MAX - 1 ? lp + 1 : ROW_MAX - 1];
            if (b == ',' || b == '\t' || b == ';' || b == '|') {
                v->field_pos = 0;
                v->field_idx += 1;
            } else {
                v->field_pos += 1;
            }
        }
    }
    /* end_byte() writes what followed the order-2 context before b; the
     * hash reads the context after it, the same slot when the last three
     * bytes are one value */
    if (idx_read == idx_written) {
        f = (uint16_t)(f << 8 | b);
    }
    v->followed = f;
    {
        /* the order-1 history end_byte() writes is the previous byte's;
         * the hash reads b's: the same slot when they are one byte */
        uint16_t f1 = m->followed1[b];
        if ((m->c8 & 0xFF) == b) {
            f1 = (uint16_t)(f1 << 8 | b);
        }
        v->followed1 = f1;
    }
    v->periodic = m->period != 0 && (size_t)n > m->period;
    v->up = 0;
    v->corner = 0;
    if (v->periodic) {
        uint64_t iu = n - m->period, ic = n - m->period - 1;
        v->up = iu == m->n ? b : m->hist[(size_t)iu & m->hmask];
        v->corner = ic == m->n ? b : m->hist[(size_t)ic & m->hmask];
    }
    /* recall after b, as far as the encoder can tell: a match that b
     * confirms goes on; one it breaks is guessed absent (the index may
     * offer another — a wasted prefetch at most, never a wrong byte) */
    if (m->mlen > 0 && !m->mmiss && m->mbyte == b) {
        v->mlen = m->mlen + 1;
        v->mbyte = m->hist[(size_t)(m->mptr + 1) & m->hmask];
        v->mbyte2 = m->hist[(size_t)(m->mptr + 2) & m->hmask];
    } else {
        v->mlen = 0;
        v->mbyte = 0;
        v->mbyte2 = 0;
    }
}

#endif /* ADAPT_MODEL_STATE_H */
