/*
 * adapt_states.h — the bit-history state machine.
 *
 * A section of adapt_model.h, which includes the sections in order into
 * one translation unit: every function stays static inline and the per-bit
 * path is specialized at compile time. Include adapt_model.h, not this file.
 */
#ifndef ADAPT_STATES_H
#define ADAPT_STATES_H

/* ------------------------------------------------------------------------
 * State model — bit-history states
 * --------------------------------------------------------------------- */

/* Count levels a state can hold. */
static const uint8_t LEVELS[17] = {0, 1, 2, 3, 4, 5, 6, 8, 10, 12, 16, 20, 24, 32, 40, 48, 64};

typedef struct state_table {
    /* next[state][bit]; rows past the last reachable state map to themselves. */
    uint8_t next[256][2];
    uint8_t n0[256];
    uint8_t n1[256];
    /* The sign of a state's third arbiter lane under the max profile:
     * 1 when it has seen ones only, -1 zeros only, 0 for state 0 and
     * the mixed states (see lane2_of). */
    int8_t lane2_mode[256];
    /* next again, by bit then state, for a loop over states with the
     * bit fixed. */
    uint8_t next8[2][256];
    unsigned len;
} state_table;

/* A pair is a state when one count is small, or both are moderate. */
static inline int allowed(unsigned a, unsigned b) {
    unsigned lo = a < b ? a : b;
    unsigned hi = a < b ? b : a;
    return lo <= 4 || hi <= 12;
}

static inline unsigned level_up(unsigned v) {
    unsigned i;
    for (i = 0; i < 17; i++) {
        if (LEVELS[i] == v) {
            return i + 1 < 17 ? LEVELS[i + 1] : v;
        }
    }
    return v;
}

/* Largest level not above v. */
static inline unsigned level_floor(unsigned v) {
    unsigned i;
    for (i = 17; i-- > 0;) {
        if (LEVELS[i] <= v) {
            return LEVELS[i];
        }
    }
    return 0;
}

static inline void transition(unsigned a, unsigned b, unsigned bit, unsigned *ra, unsigned *rb) {
    unsigned seen = bit ? b : a;
    unsigned other = bit ? a : b;
    seen = level_up(seen);
    if (other > 2) {
        other = level_floor((other + 2) / 2);
    }
    while (!allowed(seen, other)) {
        other = level_floor((other + 1) / 2);
    }
    if (bit) {
        *ra = other;
        *rb = seen;
    } else {
        *ra = seen;
        *rb = other;
    }
}

static inline unsigned state_index(const state_table *t, unsigned a, unsigned b) {
    unsigned i;
    for (i = 0; i < t->len; i++) {
        if (t->n0[i] == a && t->n1[i] == b) {
            return i;
        }
    }
    /* Unreachable: the transition rule never leaves the state set. */
    abort();
}

static inline void state_table_init(state_table *t) {
    unsigned ia, ib, i;
    t->len = 0;
    for (ia = 0; ia < 17; ia++) {
        for (ib = 0; ib < 17; ib++) {
            if (allowed(LEVELS[ia], LEVELS[ib])) {
                t->n0[t->len] = LEVELS[ia];
                t->n1[t->len] = LEVELS[ib];
                t->len++;
            }
        }
    }
    for (i = 0; i < t->len; i++) {
        unsigned a, b;
        transition(t->n0[i], t->n1[i], 0, &a, &b);
        t->next[i][0] = (uint8_t)state_index(t, a, b);
        transition(t->n0[i], t->n1[i], 1, &a, &b);
        t->next[i][1] = (uint8_t)state_index(t, a, b);
    }
    for (i = t->len; i < 256; i++) {
        t->next[i][0] = (uint8_t)i;
        t->next[i][1] = (uint8_t)i;
    }
    for (i = 0; i < 256; i++) {
        t->lane2_mode[i] = (int8_t)(i == 0 ? 0 : (t->n0[i] == 0 ? 1 : (t->n1[i] == 0 ? -1 : 0)));
        t->next8[0][i] = t->next[i][0];
        t->next8[1][i] = t->next[i][1];
    }
}

/* The probability of a one a state suggests before any learning:
 * (n1 + 0.5) / (n0 + n1 + 1), in 22 bits. */
static inline uint32_t prior_p22(const state_table *t, unsigned s) {
    uint32_t n0 = t->n0[s], n1 = t->n1[s];
    uint32_t p = ((n1 * 2 + 1) << 22) / (2 * (n0 + n1) + 2);
    return p < (1u << 22) - 1 ? p : (1u << 22) - 1;
}

/* A learned probability per state: 22-bit probability in the high bits,
 * 10-bit count below. Beside each cell, what the arbiters read of it —
 * the probability in twelve bits, clamped to 1, in the low half, and its
 * stretch in the high half — refreshed whenever the cell learns, so the
 * per-bit path takes a context's inputs from one load with no table
 * lookup behind it. */
/* A context's map cell also carries its state's third-lane mode in the
 * two count bits the count never reaches (MAP_LIMIT is 255): 1 for a
 * state that has seen ones only, 2 zeros only, 0 otherwise. The count
 * arithmetic masks it out and the repack keeps it, so it costs the
 * vector path nothing to read and saves it a gather. */
#define CELL_MODE_SHIFT 8
#define CELL_COUNT_MASK 255u

static inline uint32_t cell_mode_bits(const state_table *t, unsigned s) {
    int8_t mode = t->lane2_mode[s];
    return (uint32_t)(mode > 0 ? 1 : (mode < 0 ? 2 : 0)) << CELL_MODE_SHIFT;
}

static inline void state_map_init(uint32_t *cells, const state_table *t) {
    unsigned s;
    memset(cells, 0, 256 * sizeof(uint32_t));
    for (s = 0; s < 256; s++) {
        cells[s] = (s < t->len ? prior_p22(t, s) << 10 : 0) | cell_mode_bits(t, s);
    }
}

static inline int32_t map_p12(const uint32_t *cells, uint8_t s) {
    int32_t p = (int32_t)(cells[s] >> 20);
    return p > 1 ? p : 1;
}

/* The third lane of a memory context under the max profile, from its
 * probability and the state's precomputed mode (+1 for a state that has
 * seen ones only, -1 zeros only, 0 for state 0 and every mixed state):
 * two masked values in place of the three-way test on the counts, which
 * mispredicted on a fifth of the contexts. */
static inline int32_t lane2_of(int32_t p, int32_t mode) {
    int32_t up = p >> 2;
    int32_t down = -((4096 - p) >> 2);
    return (up & -(int32_t)(mode > 0)) | (down & -(int32_t)(mode < 0));
}

/* A memory context's CTX_LANES(max) inputs from its state and its map:
 * the stretched probability; under the max profile also the probability
 * in the linear domain and the third lane. */
static inline ADAPT_ALWAYS_INLINE void context_lanes(int32_t *lane, const int16_t *stretch, const uint32_t *cells,
                                                     const state_table *t, uint8_t s, const int max) {
    int32_t p = map_p12(cells, s);
    lane[0] = stretch[p];
    if (max) {
        lane[1 + HASHED] = (p - 2048) >> 2;
        lane[2 * (1 + HASHED)] = lane2_of(p, t->lane2_mode[s]);
    }
}

/* Learn that `bit` followed `state`; rate[n] is 65536 / (2n + 3). The
 * count sits in the low eight bits, whatever the two above hold (a
 * context map's mode bits; nothing for the run and recall cells). */
static inline void map_update(uint32_t *cell, uint32_t bit, const int32_t *rate, uint32_t limit) {
    uint32_t c = *cell;
    uint32_t n = c & CELL_COUNT_MASK;
    int32_t p = (int32_t)(c >> 10);
    int32_t target = bit ? (1 << 22) - 1 : 0;
    p = p + ((((target - p) >> 6) * rate[n & (RATE_LEN - 1)]) >> 9);
    n = n + (n < limit);
    *cell = ((uint32_t)p << 10) | (c & (1023u & ~CELL_COUNT_MASK)) | n;
}

#endif /* ADAPT_STATES_H */
