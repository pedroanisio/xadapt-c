/*
 * adapt_crc32.h — crc32.
 *
 * A section of adapt_model.h, which includes the sections in order into
 * one translation unit: every function stays static inline and the per-bit
 * path is specialized at compile time. Include adapt_model.h, not this file.
 */
#ifndef ADAPT_CRC32_H
#define ADAPT_CRC32_H

/* ------------------------------------------------------------------------
 * CRC32
 * --------------------------------------------------------------------- */

#define CRC32_INIT 0xFFFFFFFFu

typedef struct crc32_table {
    uint32_t entry[256];
} crc32_table;

static inline void crc32_table_init(crc32_table *t) {
    uint32_t i;
    int k;
    for (i = 0; i < 256; i++) {
        uint32_t c = i;
        for (k = 0; k < 8; k++) {
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        t->entry[i] = c;
    }
}

static inline uint32_t crc32_update(const crc32_table *t, uint32_t c, const uint8_t *data, size_t len) {
    size_t i;
    for (i = 0; i < len; i++) {
        c = t->entry[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    }
    return c;
}

static inline adapt_status fail(adapt_error *error, adapt_status status, uint64_t detail, uint64_t limit) {
    if (error != NULL) {
        error->status = status;
        error->cause = ADAPT_OK;
        error->detail = detail;
        error->limit = limit;
    }
    return status;
}

/* ADAPT_ERROR_VERIFICATION carrying the decoding error `cause` (ADAPT_OK
 * when the decode succeeded with different bytes) and its numbers. */
static inline adapt_status fail_verification(adapt_error *error, const adapt_error *cause) {
    if (error != NULL) {
        error->status = ADAPT_ERROR_VERIFICATION;
        error->cause = cause != NULL ? cause->status : ADAPT_OK;
        error->detail = cause != NULL ? cause->detail : 0;
        error->limit = cause != NULL ? cause->limit : 0;
    }
    return ADAPT_ERROR_VERIFICATION;
}

static inline uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline uint64_t le64(const uint8_t *p) {
    return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;
}

static inline int sizes_valid(adapt_sizes s) {
    return s.table_log >= TABLE_LOG_MIN && s.table_log <= TABLE_LOG_MAX && s.match_log >= MATCH_LOG_MIN &&
           s.match_log <= MATCH_LOG_MAX && s.window_log >= WINDOW_LOG_MIN && s.window_log <= WINDOW_LOG_MAX &&
           s.class_code < 3 && s.set_code < ADAPT_CONTEXT_SETS && s.fast <= 1 &&
           (!s.fast || s.set_code < ADAPT_FAST_SETS) && s.period != 1 && s.period <= ADAPT_PERIOD_MAX &&
           (!s.fast || s.period == 0) && s.segmented <= 1 && s.max <= 1 && !(s.fast && s.max) && s.x86 <= 1 &&
           s.archive <= 1 && s.adaptive <= 3 && (!s.adaptive || !s.fast) &&
           s.bmp <= 1 && (!s.bmp || (!s.fast && !s.x86 && !s.archive)) &&
           s.compact <= 1 && (!s.compact || (s.max && !s.adaptive && !s.bmp)) && s.lean <= 1 &&
           (!s.lean || (s.max && !s.adaptive && !s.bmp)) && s.contexts <= ADAPT_CONTEXTS_LEAN &&
           (s.contexts == ADAPT_CONTEXTS_AUTO || !s.fast) &&
           s.table_frac <= ADAPT_SIZING_FRAC_MAX && (s.table_frac == 0 || (s.max && !s.adaptive && !s.bmp)) &&
           s.text <= ADAPT_TEXT_OFF && (s.text != ADAPT_TEXT_ON || (!s.adaptive && !s.bmp && !s.x86)) &&
           s.low_start <= 1 && (!s.low_start || (s.max && !s.adaptive && !s.bmp)) &&
           s.model2 <= 1 && (!s.model2 || s.max) &&
           s.precise <= 1 && (!s.precise || (s.model2 && !s.adaptive && !s.bmp)) &&
           s.mid <= 1 && (!s.mid || (!s.fast && !s.max && !s.adaptive && !s.bmp && s.set_code == 0)) &&
           s.keys <= ADAPT_KEYS_BINARY &&
           (s.keys == ADAPT_KEYS_CLASSIC || (s.max ? s.model2 != 0 : !s.fast && !s.mid && !s.adaptive && !s.bmp));
}

static inline adapt_status read_trailer_payload(const uint8_t *payload, uint64_t *length, uint32_t *crc32) {
    *length = le64(payload);
    *crc32 = le32(payload + 8);
    return ADAPT_OK;
}

/* The method, raw length and payload length of a block header, checked
 * against the rules of its method. */
static inline adapt_status parse_block_header(const uint8_t *header, uint8_t *method, size_t *raw, size_t *payload_len,
                                       adapt_error *error) {
    uint8_t m = header[0];
    uint32_t r = le32(header + 1);
    uint32_t p = le32(header + 5);
    if (m == METHOD_END || m == METHOD_SEGMENT || m == METHOD_TABLE) {
        return fail(error, ADAPT_ERROR_BAD_BLOCK, 0, 0);
    }
    if (r == 0 || r > ADAPT_BLOCK_BYTES) {
        return fail(error, ADAPT_ERROR_BAD_BLOCK, 0, 0);
    }
    switch (m) {
        case METHOD_MODELED:
        case METHOD_MODELED_PRECISE:
            if (!(p >= 4 && p < r)) {
                return fail(error, ADAPT_ERROR_BAD_BLOCK, 0, 0);
            }
            break;
        case METHOD_STORED_OBSERVED:
        case METHOD_STORED_SKIPPED:
            if (p != r) {
                return fail(error, ADAPT_ERROR_BAD_BLOCK, 0, 0);
            }
            break;
        default:
            return fail(error, ADAPT_ERROR_BAD_METHOD, m, 0);
    }
    *method = m;
    *raw = r;
    *payload_len = p;
    return ADAPT_OK;
}

/* Validate one block header and payload in `src` without allocating; the
 * trailer is returned as METHOD_END with its twelve payload bytes, a
 * segment marker or the table's method byte as their method with no
 * payload (the table's entries follow, and are read by the caller, who
 * knows how many segments it has seen). */
static inline adapt_status read_block(const uint8_t *src, size_t len, size_t *pos, uint8_t *method, size_t *raw,
                               const uint8_t **payload, size_t *payload_len, adapt_error *error) {
    adapt_status status;
    if (len - *pos < 1) {
        return fail(error, ADAPT_ERROR_TRUNCATED, 0, 0);
    }
    if (src[*pos] == METHOD_SEGMENT || src[*pos] == METHOD_TABLE) {
        *method = src[*pos];
        *raw = 0;
        *payload = src + *pos + 1;
        *payload_len = 0;
        *pos += 1;
        return ADAPT_OK;
    }
    if (src[*pos] == METHOD_END) {
        if (len - *pos < ADAPT_TRAILER_BYTES) {
            return fail(error, ADAPT_ERROR_TRUNCATED, 0, 0);
        }
        *method = METHOD_END;
        *raw = 0;
        *payload = src + *pos + 1;
        *payload_len = TRAILER_PAYLOAD;
        *pos += ADAPT_TRAILER_BYTES;
        return ADAPT_OK;
    }
    if (len - *pos < ADAPT_BLOCK_HEADER_BYTES) {
        return fail(error, ADAPT_ERROR_TRUNCATED, 0, 0);
    }
    status = parse_block_header(src + *pos, method, raw, payload_len, error);
    if (status != ADAPT_OK) {
        return status;
    }
    *pos += ADAPT_BLOCK_HEADER_BYTES;
    if (len - *pos < *payload_len) {
        return fail(error, ADAPT_ERROR_TRUNCATED, 0, 0);
    }
    *payload = src + *pos;
    *pos += *payload_len;
    return ADAPT_OK;
}

/* Bytes the run tables, arbiter weights and line buffers occupy under a
 * profile. */
static inline uint64_t side_table_bytes(int fast, int max, int compact, int model2) {
    uint64_t wbytes = model2 ? 2 : 4;
    return (uint64_t)RUNS_OF(max) * RUN_SLOTS * 4 + (uint64_t)MIXER_SETS * ARBITER_STRIDE(fast, max) * wbytes +
           (uint64_t)(MIXER2_SETS + 2 * MIXER34_SETS(compact) + EXTRA_SETS_OF(model2)) * ARBITER_STRIDE(0, max) * wbytes +
           (uint64_t)FINAL_SETS * (FINAL_INPUTS + 1) * 4 + 2 * ROW_MAX;
}

/* Bytes one block's workspace takes: the block, its coded form, the framed
 * copy and the scan table. */
static inline uint64_t block_workspace(void) {
    return 3 * ((uint64_t)ADAPT_BLOCK_BYTES + ADAPT_BLOCK_HEADER_BYTES + 36) + (uint64_t)SCAN_SLOTS * 4;
}

/* A memory bound's arithmetic saturates: a count of workers or threads
 * large enough to wrap the product must read as more memory than any
 * budget, never as less, or check_memory() would admit it. */
static inline uint64_t saturating_mul(uint64_t a, uint64_t b) {
    if (a != 0 && b > UINT64_MAX / a) {
        return UINT64_MAX;
    }
    return a * b;
}

static inline uint64_t saturating_add(uint64_t a, uint64_t b) {
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}

static inline adapt_status check_memory(uint64_t required, uint64_t limit, adapt_error *error) {
    if (required > limit) {
        return fail(error, ADAPT_ERROR_MEMORY_LIMIT, required, limit);
    }
    return ADAPT_OK;
}

/* Read until `buf` is full or the input ends; the number of bytes read. */
static inline adapt_status fill(adapt_read_fn read, void *ctx, uint8_t *buf, size_t len, size_t *got, adapt_error *error) {
    size_t have = 0;
    while (have < len) {
        size_t n = 0;
        int rc = read(ctx, buf + have, len - have, &n);
        if (rc != 0) {
            return fail(error, ADAPT_ERROR_IO, (uint64_t)(rc < 0 ? -(int64_t)rc : rc), 0);
        }
        if (n == 0) {
            break;
        }
        if (n > len - have) {
            /* A callback that claims more than it was asked for is an
             * error in the caller; treat it as an i/o failure. */
            return fail(error, ADAPT_ERROR_IO, 0, 0);
        }
        have += n;
    }
    *got = have;
    return ADAPT_OK;
}

/* What a reader taking a file in order counts of its segments, to hold
 * the table against when it arrives: a marker or a
 * table in a file that is not segmented, an empty segment, a block after
 * the table or a trailer before it are each ADAPT_ERROR_BAD_BLOCK, as is
 * a table that names other lengths than the ones read. */
typedef struct segment_counter {
    int segmented;
    int done;
    uint32_t *entries; /* raw, coded per closed segment */
    size_t count;
    size_t cap;
    uint64_t raw;
    uint64_t coded;
} segment_counter;

static inline void counter_init(segment_counter *c, int segmented) {
    c->segmented = segmented;
    c->done = 0;
    c->entries = NULL;
    c->count = 0;
    c->cap = 0;
    c->raw = 0;
    c->coded = 0;
}

static inline void counter_free(segment_counter *c) {
    free(c->entries);
    c->entries = NULL;
}

/* Record the current segment and start counting the next. */
static inline adapt_status counter_close(segment_counter *c, adapt_error *error) {
    if (c->count == c->cap) {
        size_t cap = c->cap == 0 ? 16 : c->cap * 2;
        uint32_t *grown = realloc(c->entries, cap * 2 * sizeof(uint32_t));
        if (grown == NULL) {
            return fail(error, ADAPT_ERROR_ALLOCATION, 0, 0);
        }
        c->entries = grown;
        c->cap = cap;
    }
    c->entries[2 * c->count] = (uint32_t)c->raw;
    c->entries[2 * c->count + 1] = (uint32_t)c->coded;
    c->count += 1;
    c->raw = 0;
    c->coded = 0;
    return ADAPT_OK;
}

static inline adapt_status counter_block(segment_counter *c, size_t raw, size_t coded, adapt_error *error) {
    if (c->done) {
        return fail(error, ADAPT_ERROR_BAD_BLOCK, 0, 0);
    }
    c->raw += raw;
    c->coded += coded;
    return ADAPT_OK;
}

static inline adapt_status counter_marker(segment_counter *c, adapt_error *error) {
    adapt_status status;
    if (!c->segmented || c->done || c->raw == 0) {
        return fail(error, ADAPT_ERROR_BAD_BLOCK, 0, 0);
    }
    status = counter_close(c, error);
    if (status != ADAPT_OK) {
        return status;
    }
    c->coded += 1;
    return ADAPT_OK;
}

/* Bytes of table after its method byte, when a table may come here. */
static inline adapt_status counter_table_len(const segment_counter *c, size_t *len, adapt_error *error) {
    if (!c->segmented || c->done || (c->count != 0 && c->raw == 0)) {
        return fail(error, ADAPT_ERROR_BAD_BLOCK, 0, 0);
    }
    *len = adapt_segment_table_len((uint64_t)c->count + 1) - 1;
    return ADAPT_OK;
}

/* Check the table's body, counter_table_len bytes after its method byte. */
static inline adapt_status counter_table(segment_counter *c, const uint8_t *body, adapt_error *error) {
    adapt_status status;
    size_t len, i;
    status = counter_table_len(c, &len, error);
    if (status != ADAPT_OK) {
        return status;
    }
    status = counter_close(c, error);
    if (status != ADAPT_OK) {
        return status;
    }
    if (le32(body + len - ADAPT_SEGMENT_COUNT_BYTES) != c->count) {
        return fail(error, ADAPT_ERROR_BAD_BLOCK, 0, 0);
    }
    for (i = 0; i < c->count; i++) {
        if (le32(body + i * ADAPT_SEGMENT_ENTRY_BYTES) != c->entries[2 * i] ||
            le32(body + i * ADAPT_SEGMENT_ENTRY_BYTES + 4) != c->entries[2 * i + 1]) {
            return fail(error, ADAPT_ERROR_BAD_BLOCK, 0, 0);
        }
    }
    c->done = 1;
    return ADAPT_OK;
}

/* The segments a file counted, once the trailer comes. */
static inline adapt_status counter_end(const segment_counter *c, adapt_error *error) {
    if (c->segmented && !c->done) {
        return fail(error, ADAPT_ERROR_BAD_BLOCK, 0, 0);
    }
    return ADAPT_OK;
}

/* Whether a method byte is a block's — stored, modeled, or modeled under
 * the precise coder — rather than a marker's. */
static inline int method_is_block(uint8_t method) {
    return method <= METHOD_STORED_SKIPPED || method == METHOD_MODELED_PRECISE;
}

typedef struct block_decoder {
    model *m;
} block_decoder;

/* Recover the `raw` bytes of one block, already validated by
 * parse_block_header, into `out`. */
static inline adapt_status decode_block(block_decoder *d, uint8_t method, size_t raw, const uint8_t *payload, size_t payload_len,
                                 uint8_t *out, adapt_error *error) {
    switch (method) {
        case METHOD_MODELED_PRECISE:
            /* only a version of the precise coder carries it */
            if (!d->m->precise) {
                return fail(error, ADAPT_ERROR_BAD_METHOD, method, 0);
            }
            /* fall through */
        case METHOD_MODELED: {
            arith a;
            arith_init(&a, payload, payload_len);
            decode_bytes(d->m, &a, raw, out, method == METHOD_MODELED_PRECISE);
            if (!arith_consumed_exactly(&a)) {
                return fail(error, ADAPT_ERROR_BAD_BLOCK, 0, 0);
            }
            break;
        }
        case METHOD_STORED_OBSERVED:
            observe_bytes(d->m, payload, payload_len);
            memcpy(out, payload, payload_len);
            break;
        default:
            skip_bytes(d->m, payload, payload_len);
            memcpy(out, payload, payload_len);
            break;
    }
    return ADAPT_OK;
}


#endif /* ADAPT_CRC32_H */
