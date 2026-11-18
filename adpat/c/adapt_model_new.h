/*
 * adapt_model_new.h — model_new: the arbiters' starting weights, the sizes' policies, the arena, every table's starting state.
 *
 * A section of adapt_model.h, which includes the sections in order into
 * one translation unit: every function stays static inline and the per-bit
 * path is specialized at compile time. Include adapt_model.h, not this file.
 */
#ifndef ADAPT_MODEL_NEW_H
#define ADAPT_MODEL_NEW_H

/* The starting weight of arbiter lane j under a profile: 1/4 for a
 * context's stretched probability, or 1/32 when `low` (the sized max
 * profile's ADAPT_SIZING_LOW_START), reducing initial trust before
 * input-driven learning. Other lanes and run inputs start at 0. */
static inline int32_t initial_weight(size_t j, int fast, int max, int low) {
    size_t base = (size_t)RUN_BASE(fast, max);
    int32_t trusted = low ? 1 << 11 : 1 << 14;
    if (j < base) {
        /* the first plane, the stretched probabilities, is trusted */
        return j < base / (size_t)CTX_LANES(max) ? trusted : 0;
    }
    return j < base + (size_t)RUNS_OF(max) ? 0 : trusted;
}

static inline model *model_new_policy(adapt_sizes sizes, int legacy_model2) {
    model *m = calloc(1, sizeof(model));
    size_t blocks, i, j, x, pi;
    uint16_t apm_row[APM_BUCKETS];
    if (m == NULL) {
        return NULL;
    }
#if ADAPT_X86_V3
    /* The x86-64-v3 instantiations on a CPU that runs them, unless the
     * environment says ADAPT_ISA=baseline: a diagnostic knob, so the two
     * paths can be run on one input and their bytes compared (the CLI
     * tests do). */
    /* GCC/Clang initialize CPU features before ordinary constructors. */
    m->cpu_v3 = __builtin_cpu_supports("x86-64-v3");
    {
        const char *isa = getenv("ADAPT_ISA");
        if (isa != NULL && strcmp(isa, "baseline") == 0) {
            m->cpu_v3 = 0;
        }
    }
#endif
    if (sizes.adaptive) {
        m->adaptive = malloc(sizeof(*m->adaptive));
        if (m->adaptive == NULL) { model_free(m); return NULL; }
        adaptive_init(m->adaptive, sizes.adaptive);
    }
    state_table_init(&m->states);
    /* stretch: the inverse of squash, by table */
    pi = 0;
    for (x = 0; x <= 4094; x++) {
        int32_t d = (int32_t)x - 2047;
        size_t v = (size_t)squash(d);
        for (i = pi; i <= v && i < 4096; i++) {
            m->stretch[i] = (int16_t)d;
        }
        pi = v + 1;
    }
    for (i = pi; i < 4096; i++) {
        m->stretch[i] = 2047;
    }
    for (i = 0; i < 4095; i++) {
        m->squashed[i] = (int16_t)squash((int32_t)i - 2047);
    }
    for (i = 0; i < 4096; i++) {
        m->stretch32[i] = m->stretch[i];
    }

    for (i = 0; i < RATE_LEN; i++) {
        m->rate[i] = 65536 / (2 * (int32_t)i + 3);
    }
    for (i = 0; i < m->states.len; i++) {
        unsigned sum = (unsigned)m->states.n0[i] + m->states.n1[i];
        m->seen[i] = (uint8_t)(sum < 255 ? sum : 255);
    }
    for (i = 0; i <= MIXER_COUNT_MAX; i++) {
        m->mixer_rate[i] = MIXER_RATE_MIN + ((MIXER_RATE_MAX - MIXER_RATE_MIN) * MIXER_DECAY) / (MIXER_DECAY + (int32_t)i);
    }
    for (i = 0; i < HASHED + 1; i++) {
        state_map_init(m->maps[i], &m->states);
    }
    blocks = (size_t)table_blocks(sizes);
    if (!model_arena(m, sizes)) {
        model_free(m);
        return NULL;
    }
    m->table_mask = blocks - 1;
    m->table_lines = sizes.table_frac != 0 ? blocks / WAYS : 0;
    m->class_bits = 3 * CLASS_WIDTHS[sizes.class_code];
    m->fast = sizes.fast != 0;
    m->mid = sizes.mid != 0;
    m->keys = sizes.keys;
    m->max = sizes.max != 0;
    m->compact = sizes.compact != 0;
    m->low_start = sizes.low_start != 0;
    m->model2 = sizes.model2 != 0;
    m->precise = m->model2 && sizes.precise != 0;
    m->coder_bits = m->precise ? ADAPT_CODER_BITS_PRECISE : ADAPT_CODER_BITS;
    m->slots_on = m->model2 && (legacy_model2 || sizes.table_log >= MODEL2_PRESSURE_LOG);
#ifdef ADAPT_SIZES_OVERRIDE
    {
        /* development builds only (see cadapt.c): ADAPT_MODEL2_SLOTS=1 runs
         * the pressure-gated parts under a small table, so a small input
         * under a named shape meets the model a long input does; such a
         * file decodes only under the same setting */
        const char *o = getenv("ADAPT_MODEL2_SLOTS");
        if (o != NULL && m->model2) m->slots_on = o[0] == '1';
    }
#endif
    m->extra_on = m->slots_on;
    m->mix34_row_mask = MIX34_ROW_MASK(m->compact);
    m->apm2_mask = APM2_CONTEXTS(m->max, m->compact) - 1;
    m->period = sizes.period;
    {
        uint32_t set = m->fast ? FAST_SETS[sizes.set_code] : CONTEXT_SETS[sizes.set_code];
        /* the max profile's kinds run only under it */
        uint32_t dropped = set | (sizes.class_code == 0 ? (1u << H_CLASS) : 0) | (m->max ? 0 : MAX_KINDS) | (m->mid ? MID_DROPPED : 0) |
                           (sizes.lean ? LEAN_DROPPED : 0);
#ifdef ADAPT_SIZES_OVERRIDE
        {
            /* development builds only (see cadapt.c): ADAPT_DROP_MASK leaves
             * kinds out, a bit per kind in H_* order, for ablation under a
             * named pressure; the header still names the set, so such a
             * file decodes only under the same mask */
            const char *o = getenv("ADAPT_DROP_MASK");
            if (o != NULL) dropped |= (uint32_t)strtoul(o, NULL, 0);
        }
#endif
        m->hashed = 0;
        for (i = 0; i < HASHED; i++) {
            m->on[i] = (dropped >> i & 1) == 0;
            if (m->on[i]) {
                m->kind[m->hashed++] = (uint8_t)i;
            }
        }
        m->runs = (set & NO_RUNS) ? 0 : RUNS_OF(m->max);
        m->lane_groups = (int)((1 + m->hashed + 7) / 8);
    }
    m->hmask = ((size_t)1 << sizes.window_log) - 1;
    for (i = 0; i < RUNS * RUN_BUCKETS; i++) {
        m->rsm[i] = CELL_INIT;
    }
    m->mmask = ((size_t)1 << sizes.match_log) - 1;
    for (i = 0; i < MATCH_BUCKETS * 2; i++) {
        m->msm[i] = CELL_INIT;
    }
    /* Weights of one first-layer arbiter at the start: the context inputs'
     * stretched probabilities begin trusted; their other lanes and the run
     * inputs earn it. */
    /* Every set of an arbiter starts from the same row, so the row is
     * built once and copied. First-block candidate trials also construct
     * models, so repeated per-lane initialization would multiply that cost. */
    if (m->model2) {
        /* the int16 arbiters start at the same weights in their own
         * units (1/32768: half the int32 value), the fifth like the rest */
        size_t lanes2 = (size_t)LANES(0, 1);
        size_t sets2 = MIXER2_SETS, sets34 = MIXER34_SETS(m->compact);
        int16_t row16[LANES(0, 1) + 1];
        for (j = 0; j < lanes2; j++) {
            row16[j] = (int16_t)(initial_weight(j, 0, 1, m->low_start) >> 1);
        }
        row16[lanes2] = 0;
        for (i = 0; i < MIXER_SETS + sets2 + 2 * sets34 + EXTRA_SETS; i++) {
            int16_t *w = i < MIXER_SETS                    ? m->w16 + i * (lanes2 + 1)
                         : i < MIXER_SETS + sets2          ? m->w16_2 + (i - MIXER_SETS) * (lanes2 + 1)
                         : i < MIXER_SETS + sets2 + sets34 ? m->w16_3 + (i - MIXER_SETS - sets2) * (lanes2 + 1)
                         : i < MIXER_SETS + sets2 + 2 * sets34
                             ? m->w16_4 + (i - MIXER_SETS - sets2 - sets34) * (lanes2 + 1)
                             : m->w16_x + (i - MIXER_SETS - sets2 - 2 * sets34) * (lanes2 + 1);
            memcpy(w, row16, (lanes2 + 1) * sizeof(int16_t));
        }
    } else {
        size_t lanes = (size_t)LANES(m->fast, m->max), lanes2 = (size_t)LANES(0, m->max);
        int32_t row[LANES(0, 1) + 1], row2[LANES(0, 1) + 1];
        for (j = 0; j < lanes; j++) {
            row[j] = initial_weight(j, m->fast, m->max, m->low_start);
        }
        row[lanes] = 0;
        for (j = 0; j < lanes2; j++) {
            row2[j] = initial_weight(j, 0, m->max, m->low_start);
        }
        row2[lanes2] = 0;
        for (i = 0; i < MIXER_SETS; i++) {
            memcpy(m->weights + i * (lanes + 1), row, (lanes + 1) * sizeof(int32_t));
        }
        for (i = 0; i < MIXER2_SETS + 2 * MIXER34_SETS(m->compact); i++) {
            int32_t *w = i < MIXER2_SETS ? m->weights2 + i * (lanes2 + 1)
                         : i < MIXER2_SETS + MIXER34_SETS(m->compact)
                             ? m->weights3 + (i - MIXER2_SETS) * (lanes2 + 1)
                             : m->weights4 + (i - MIXER2_SETS - MIXER34_SETS(m->compact)) * (lanes2 + 1);
            memcpy(w, row2, (lanes2 + 1) * sizeof(int32_t));
        }
    }
    /* the final arbiter starts as the mean of its verdicts, four or five */
    for (i = 0; i < FINAL_SETS; i++) {
        size_t nf = (size_t)FINAL_INPUTS_ON(m, m->model2);
        for (j = 0; j < nf - 1; j++) {
            m->wfinal[i].w[j] = (1 << 16) / (int32_t)(nf - 1);
        }
        for (j = nf - 1; j < FINAL_INPUTS; j++) {
            m->wfinal[i].w[j] = 0;
        }
        m->wfinal[i].n = 0;
    }
    for (j = 0; j < APM_BUCKETS; j++) {
        apm_row[j] = (uint16_t)(squash(((int32_t)j - 16) * 128) * 16);
    }
    for (i = 0; i < APM_CONTEXTS; i++) {
        memcpy(m->apm[i], apm_row, sizeof(apm_row));
    }
    for (i = 0; i < APM2_CONTEXTS(m->max, m->compact); i++) {
        memcpy(m->apm2[i], apm_row, sizeof(apm_row));
    }
    m->pmix = 2048;
    m->pmix2 = 2048;
    m->pmix3 = 2048;
    m->pmix4 = 2048;
    m->pfinal = 2048;
    m->c0 = 1;
    m->bitpos = 0;
    m->node = 1;
    start_byte(m, m->fast);
    return m;
}


static inline model *model_new(adapt_sizes sizes) {
    return model_new_policy(sizes, 0);
}

#endif /* ADAPT_MODEL_NEW_H */
