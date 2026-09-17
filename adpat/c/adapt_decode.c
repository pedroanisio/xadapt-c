/*
 * adapt_decode.c — the decoding entry points: the streaming decoder, the
 * in-memory decoder, the header and trailer readers, the memory bounds
 * and the messages. The model and the framing rules they share with the
 * encoder are in adapt_model.h.
 *
 * Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-12.
 */
/* For the tables' mapping (adapt_model.h): mmap, MAP_ANONYMOUS, madvise. */
#if defined(__linux__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1
#endif
#include "adapt.h"
#include "adapt_model.h"
#include "adapt_text.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t adapt_crc32(const uint8_t *data, size_t len) {
    crc32_table t;
    crc32_table_init(&t);
    return ~crc32_update(&t, CRC32_INIT, data, len);
}

/* ------------------------------------------------------------------------
 * container.rs — framing
 * --------------------------------------------------------------------- */

int adapt_bmp_probe(const uint8_t *prefix, size_t length, adapt_sizes *sizes) {
    return bmp_probe(prefix, length, sizes);
}
static int header_has_text(const uint8_t *header) {
    return header[4] >= ADAPT_TEXT_VERSION && header[4] <= ADAPT_TEXT_METADATA_LAST_VERSION;
}

/* The mid profile's four versions: plain, under the text transform, and
 * each with metadata (ADAPT_MID_METADATA_OFFSET on). */
static int header_is_mid(const uint8_t *header) {
    return header[4] >= ADAPT_MID_VERSION && header[4] <= ADAPT_TEXT_MID_METADATA_VERSION;
}

/* The keyed max profile's four versions: the text and binary key
 * policies, each with metadata (ADAPT_KEYS_METADATA_OFFSET on). */
static int header_is_keyed(const uint8_t *header) {
    return header[4] >= ADAPT_MAX_KEYS_VERSION && header[4] <= ADAPT_MAX_KEYS_BINARY_METADATA_VERSION;
}

static int header_has_bmp(const uint8_t *header) {
    return (header[4] >= ADAPT_BMP_VERSION && header[4] <= ADAPT_BMP_METADATA_LAST_VERSION) ||
           (header[4] >= ADAPT_MAX_BMP_VERSION && header[4] <= ADAPT_MAX_BMP_METADATA_LAST_VERSION);
}

int adapt_header_has_metadata(const uint8_t *header) {
    return header != NULL && ((header[4] >= ADAPT_METADATA_VERSION && header[4] <= ADAPT_METADATA_MAX_VERSION) ||
                              header[4] == ADAPT_MAX_COMPACT_METADATA_VERSION || header[4] == ADAPT_MAX_LEAN_METADATA_VERSION ||
                              header[4] == ADAPT_MAX_SIZED_METADATA_VERSION || header[4] == ADAPT_MAX_PRESSURE_METADATA_VERSION ||
                              (header[4] >= ADAPT_TEXT_METADATA_VERSION && header[4] <= ADAPT_TEXT_METADATA_LAST_VERSION) ||
                              header[4] == ADAPT_MID_METADATA_VERSION || header[4] == ADAPT_TEXT_MID_METADATA_VERSION ||
                              header[4] == ADAPT_MAX_KEYS_METADATA_VERSION || header[4] == ADAPT_MAX_KEYS_BINARY_METADATA_VERSION ||
                              (header[4] >= ADAPT_ADAPTIVE_METADATA_VERSION && header[4] <= ADAPT_ADAPTIVE_METADATA_LAST_VERSION) ||
                              (header[4] >= ADAPT_BMP_METADATA_VERSION && header[4] <= ADAPT_BMP_METADATA_LAST_VERSION) ||
                              (header[4] >= ADAPT_MAX_ADAPTIVE_METADATA_VERSION && header[4] <= ADAPT_MAX_ADAPTIVE_METADATA_LAST_VERSION) ||
                              (header[4] >= ADAPT_MAX_BMP_METADATA_VERSION && header[4] <= ADAPT_MAX_BMP_METADATA_LAST_VERSION));
}

/* The sized max profile's versions carry the sizing byte. */
static int header_is_sized(const uint8_t *fixed) {
    return fixed[4] == ADAPT_MAX_SIZED_VERSION || fixed[4] == ADAPT_MAX_SIZED_METADATA_VERSION ||
           fixed[4] == ADAPT_MAX_PRESSURE_VERSION || fixed[4] == ADAPT_MAX_PRESSURE_METADATA_VERSION || header_is_keyed(fixed);
}

size_t adapt_header_len(const uint8_t *fixed) {
    return ADAPT_HEADER_BYTES + ((fixed[7] & ADAPT_PERIOD_FLAG) ? ADAPT_PERIOD_BYTES : 0) +
           (header_is_sized(fixed) ? ADAPT_SIZING_BYTES : 0) +
           (adapt_header_has_metadata(fixed) ? ADAPT_METADATA_HEADER_BYTES : 0) + (header_has_bmp(fixed) ? ADAPT_BMP_BYTES : 0);
}

size_t adapt_header_add_metadata(const uint8_t *header, size_t len, const adapt_metadata *metadata,
                                 uint8_t *out, size_t cap) {
    adapt_sizes sizes;
    uint32_t crc;
    size_t total, i;
    if (header == NULL || out == NULL || adapt_read_header(header, len, &sizes, NULL) != ADAPT_OK ||
        adapt_header_has_metadata(header) || len != adapt_header_len(header)) {
        return 0;
    }
    total = len + ADAPT_METADATA_HEADER_BYTES;
    if (cap < total || !adapt_metadata_write(metadata, out + len)) {
        return 0;
    }
    memcpy(out, header, len);
    out[4] = (uint8_t)(header[4] + (header_has_bmp(header) ? 4 : header_has_text(header) ? ADAPT_TEXT_METADATA_OFFSET
                                    : header_is_mid(header) ? ADAPT_MID_METADATA_OFFSET
                                    : header_is_keyed(header) ? ADAPT_KEYS_METADATA_OFFSET : 3));
    crc = adapt_crc32(out, total - 4);
    for (i = 0; i < 4; i++) {
        out[total - 4 + i] = (uint8_t)(crc >> (8 * i));
    }
    return total;
}

size_t adapt_sizes_header_bytes(adapt_sizes sizes) {
    uint8_t fixed[ADAPT_HEADER_BYTES] = {0};
    fixed[4] = encode_header_version(sizes);
    fixed[7] = sizes.period != 0 ? ADAPT_PERIOD_FLAG : 0;
    return adapt_header_len(fixed);
}

size_t adapt_segment_table_len(uint64_t count) {
    return (size_t)(1 + count * ADAPT_SEGMENT_ENTRY_BYTES + ADAPT_SEGMENT_COUNT_BYTES);
}

int adapt_segment_bytes_valid(uint64_t segment_bytes) {
    return segment_bytes != 0 && segment_bytes % ADAPT_BLOCK_BYTES == 0 && segment_bytes <= UINT32_MAX;
}

adapt_status adapt_read_header(const uint8_t *header, size_t len, adapt_sizes *sizes, adapt_error *error) {
    adapt_sizes s = {0};
    uint8_t version;
    if (header == NULL || sizes == NULL) {
        return fail(error, ADAPT_ERROR_INVALID_ARGUMENT, 0, 0);
    }
    if (len < ADAPT_HEADER_BYTES) {
        return fail(error, ADAPT_ERROR_TRUNCATED, 0, 0);
    }
    if (memcmp(header, "ADPT", 4) != 0) {
        return fail(error, ADAPT_ERROR_BAD_MAGIC, 0, 0);
    }
    version = header[4];
    if (adapt_header_has_metadata(header)) {
        size_t n = adapt_header_len(header), i;
        uint32_t crc = 0;
        adapt_metadata metadata;
        if (len < n) {
            return fail(error, ADAPT_ERROR_TRUNCATED, 0, 0);
        }
        for (i = 0; i < 4; i++) {
            crc |= (uint32_t)header[n - 4 + i] << (8 * i);
        }
        if (crc != adapt_crc32(header, n - 4)) {
            return fail(error, ADAPT_ERROR_CHECKSUM, 0, 0);
        }
        if (!adapt_metadata_read(header + n - ADAPT_METADATA_HEADER_BYTES, &metadata)) {
            return fail(error, ADAPT_ERROR_BAD_HEADER, 0, 0);
        }
        version = (uint8_t)(version - (header_has_bmp(header) ? 4 : header_has_text(header) ? ADAPT_TEXT_METADATA_OFFSET
                                       : header_is_mid(header) ? ADAPT_MID_METADATA_OFFSET
                                       : header_is_keyed(header) ? ADAPT_KEYS_METADATA_OFFSET : 3));
    }
    s.max = 0;
    s.text = 0;
    s.mid = 0;
    s.keys = ADAPT_KEYS_CLASSIC;
    if (version == ADAPT_MAX_KEYS_VERSION || version == ADAPT_MAX_KEYS_BINARY_VERSION) {
        /* the keyed max profile: version 72's model and sizing byte with
         * the arbiters' key policy the version names */
        s.keys = (uint8_t)(version == ADAPT_MAX_KEYS_VERSION ? ADAPT_KEYS_TEXT : ADAPT_KEYS_BINARY);
        version = ADAPT_MAX_PRESSURE_VERSION;
    }
    if (version == ADAPT_TEXT_MID_VERSION) {
        s.text = ADAPT_TEXT_ON;
        version = ADAPT_MID_VERSION;
    }
    if (version >= ADAPT_TEXT_VERSION && version <= ADAPT_TEXT_LAST_VERSION) {
        /* the text versions front the models in the order of their own */
        static const uint8_t fronted[] = {ADAPT_FORMAT_VERSION, ADAPT_FAST_VERSION, ADAPT_MAX_VERSION,
                                          ADAPT_MAX_COMPACT_VERSION, ADAPT_MAX_LEAN_VERSION};
        s.text = ADAPT_TEXT_ON;
        version = fronted[version - ADAPT_TEXT_VERSION];
    }
    if (version >= ADAPT_MAX_ADAPTIVE_VERSION && version <= ADAPT_MAX_ADAPTIVE_LAST_VERSION) {
        s.max = 1;
        version = (uint8_t)(version - ADAPT_MAX_ADAPTIVE_VERSION + ADAPT_ADAPTIVE_VERSION);
    } else if (version >= ADAPT_MAX_BMP_VERSION && version <= ADAPT_MAX_BMP_LAST_VERSION) {
        s.max = 1;
        version = (uint8_t)(version - ADAPT_MAX_BMP_VERSION + ADAPT_BMP_VERSION);
    }
    s.adaptive = 0;
    s.compact = 0;
    s.lean = 0;
    if (version == ADAPT_FORMAT_VERSION) {
        s.fast = 0;
    } else if (version == ADAPT_MID_VERSION) {
        s.fast = 0;
        s.mid = 1;
    } else if (version == ADAPT_FAST_VERSION) {
        s.fast = 1;
    } else if (version == ADAPT_MAX_VERSION) {
        s.fast = 0;
        s.max = 1;
    } else if (version == ADAPT_MAX_COMPACT_VERSION || version == ADAPT_MAX_LEAN_VERSION) {
        s.fast = 0;
        s.max = 1;
        s.compact = 1;
        s.lean = (uint8_t)(version == ADAPT_MAX_LEAN_VERSION);
    } else if (version == ADAPT_MAX_SIZED_VERSION || version == ADAPT_MAX_PRESSURE_VERSION) {
        /* the sized max profile: its tables, context set and fraction are
         * the sizing byte's, read below */
        s.fast = 0;
        s.max = 1;
    } else if (version >= ADAPT_ADAPTIVE_VERSION && version <= ADAPT_ADAPTIVE_LAST_VERSION) {
        s.fast = 0;
        s.adaptive = (uint8_t)(version - ADAPT_ADAPTIVE_VERSION + 1);
    } else if (version >= ADAPT_BMP_VERSION && version <= ADAPT_BMP_LAST_VERSION) {
        s.bmp = 1;
        s.adaptive = (uint8_t)(version - ADAPT_BMP_VERSION);
    } else {
        return fail(error, ADAPT_ERROR_UNSUPPORTED_VERSION, header[4], 0);
    }
    s.period = 0;
    if (header[7] & ADAPT_PERIOD_FLAG) {
        if (len < ADAPT_HEADER_BYTES + ADAPT_PERIOD_BYTES) {
            return fail(error, ADAPT_ERROR_TRUNCATED, 0, 0);
        }
        s.period = (uint16_t)(header[8] | (uint16_t)header[9] << 8);
        if (s.period == 0) {
            return fail(error, ADAPT_ERROR_BAD_HEADER, 0, 0);
        }
    }
    s.table_frac = 0;
    if (version == ADAPT_MAX_SIZED_VERSION || version == ADAPT_MAX_PRESSURE_VERSION) {
        size_t at = ADAPT_HEADER_BYTES + (s.period ? ADAPT_PERIOD_BYTES : 0);
        uint8_t sizing;
        if (len < at + ADAPT_SIZING_BYTES) {
            return fail(error, ADAPT_ERROR_TRUNCATED, 0, 0);
        }
        sizing = header[at];
        if (sizing & ~(ADAPT_SIZING_COMPACT | ADAPT_SIZING_LEAN | ADAPT_SIZING_TEXT | ADAPT_SIZING_LOW_START |
                       ADAPT_SIZING_MODEL2 | (ADAPT_SIZING_FRAC_MAX << ADAPT_SIZING_FRAC_SHIFT))) {
            return fail(error, ADAPT_ERROR_BAD_HEADER, 0, 0);
        }
        s.compact = (sizing & ADAPT_SIZING_COMPACT) != 0;
        s.lean = (sizing & ADAPT_SIZING_LEAN) != 0;
        if (sizing & ADAPT_SIZING_TEXT) s.text = ADAPT_TEXT_ON;
        s.low_start = (sizing & ADAPT_SIZING_LOW_START) != 0;
        s.model2 = (sizing & ADAPT_SIZING_MODEL2) != 0;
        if (version == ADAPT_MAX_PRESSURE_VERSION && !s.model2) {
            return fail(error, ADAPT_ERROR_BAD_HEADER, 0, 0);
        }
        /* the lean set ran only with the compact side tables until
         * ADAPT_CONTEXTS_LEAN, which takes it with the full ones */
        s.table_frac = (uint8_t)((sizing >> ADAPT_SIZING_FRAC_SHIFT) & ADAPT_SIZING_FRAC_MAX);
    }
    if (s.bmp) {
        size_t at = ADAPT_HEADER_BYTES + (s.period ? ADAPT_PERIOD_BYTES : 0);
        if (len < at + ADAPT_BMP_BYTES) return fail(error, ADAPT_ERROR_TRUNCATED, 0, 0);
        s.bmp_offset = bmp_load32(header + at);
        s.bmp_width = bmp_load32(header + at + 4);
        s.bmp_rows = bmp_load32(header + at + 8);
        if (!bmp_descriptor_valid(s)) return fail(error, ADAPT_ERROR_BAD_HEADER, 0, 0);
    }
    s.table_log = header[5] & 31;
    s.match_log = header[6] & 31;
    s.window_log = (uint8_t)(header[7] & ~(ADAPT_PERIOD_FLAG | ADAPT_SEGMENT_FLAG | ADAPT_X86_FLAG));
    s.class_code = (header[5] >> 5) & 3;
    s.archive = (header[5] & ADAPT_ARCHIVE_FLAG) != 0;
    s.set_code = header[6] >> 5;
    s.segmented = (header[7] & ADAPT_SEGMENT_FLAG) != 0;
    s.x86 = (header[7] & ADAPT_X86_FLAG) != 0;
    if (!sizes_valid(s)) {
        return fail(error, ADAPT_ERROR_BAD_HEADER, 0, 0);
    }
    *sizes = s;
    return fail(error, ADAPT_OK, 0, 0);
}

adapt_status adapt_read_metadata(const uint8_t *header, size_t len, adapt_metadata *metadata, adapt_error *error) {
    adapt_sizes sizes;
    adapt_status status;
    if (metadata == NULL) {
        return fail(error, ADAPT_ERROR_INVALID_ARGUMENT, 0, 0);
    }
    status = adapt_read_header(header, len, &sizes, error);
    if (status != ADAPT_OK) {
        return status;
    }
    if (!adapt_header_has_metadata(header)) {
        return fail(error, ADAPT_ERROR_BAD_HEADER, 0, 0);
    }
    (void)adapt_metadata_read(header + adapt_header_len(header) - ADAPT_METADATA_HEADER_BYTES, metadata);
    return ADAPT_OK;
}

adapt_status adapt_read_trailer(const uint8_t *trailer, uint64_t *length, uint32_t *crc32, adapt_error *error) {
    if (trailer == NULL || length == NULL || crc32 == NULL) {
        return fail(error, ADAPT_ERROR_INVALID_ARGUMENT, 0, 0);
    }
    if (trailer[0] != METHOD_END) {
        return fail(error, ADAPT_ERROR_BAD_BLOCK, 0, 0);
    }
    read_trailer_payload(trailer + 1, length, crc32);
    return fail(error, ADAPT_OK, 0, 0);
}

uint32_t adapt_class_width(adapt_sizes sizes) {
    return sizes.class_code < 3 ? CLASS_WIDTHS[sizes.class_code] : 0;
}

void adapt_dropped_contexts(adapt_sizes sizes, char *buf, size_t len) {
    uint32_t mask = (sizes.set_code < ADAPT_CONTEXT_SETS ? CONTEXT_SETS[sizes.set_code] : 0) | (sizes.lean ? LEAN_DROPPED : 0) |
                    (sizes.mid ? MID_DROPPED : 0);
    /* the max profile's kinds are not the normal profile's to drop */
    size_t kinds = sizes.max ? HASHED : BASE_HASHED;
    size_t i, at = 0;
    if (len == 0) {
        return;
    }
    buf[0] = 0;
    for (i = 0; i <= kinds; i++) {
        const char *name = i < kinds ? CONTEXT_NAMES[i] : "runs";
        int dropped = i < kinds ? (mask >> i & 1) != 0 : (mask & NO_RUNS) != 0;
        if (dropped) {
            int w = snprintf(buf + at, len - at, "%s%s", at == 0 ? "" : ", ", name);
            if (w < 0 || (size_t)w >= len - at) {
                return;
            }
            at += (size_t)w;
        }
    }
}

uint64_t adapt_table_blocks(adapt_sizes sizes) {
    return table_blocks(sizes);
}

uint64_t adapt_model_bytes(adapt_sizes sizes) {
    return table_blocks(sizes) * 16 + (sizes.fast ? O1_BYTES : 0) + ((uint64_t)1 << sizes.match_log) * 4 +
           (uint64_t)(APM_CONTEXTS + APM2_CONTEXTS(sizes.max, sizes.compact)) * APM_BUCKETS * 2 +
           side_table_bytes(sizes.fast, sizes.max, sizes.compact, sizes.model2) +
           ((uint64_t)1 << sizes.window_log) + (sizes.adaptive ? sizeof(adaptive_model) : 0);
}

void adapt_fast_contexts(adapt_sizes sizes, char *buf, size_t len) {
    uint32_t mask = sizes.set_code < ADAPT_FAST_SETS ? FAST_SETS[sizes.set_code] : 0;
    size_t i, at = 0;
    if (len == 0) {
        return;
    }
    buf[0] = 0;
    for (i = 0; i < HASHED; i++) {
        if ((mask >> i & 1) == 0) {
            int w = snprintf(buf + at, len - at, "%s%s", at == 0 ? "" : ", ", CONTEXT_NAMES[i]);
            if (w < 0 || (size_t)w >= len - at) {
                return;
            }
            at += (size_t)w;
        }
    }
}

uint64_t adapt_decode_memory_bound(adapt_sizes sizes) {
    return adapt_model_bytes(sizes) + block_workspace() + HEADROOM;
}

adapt_limits adapt_default_limits(void) {
    adapt_limits l;
    l.max_output = ADAPT_DEFAULT_MAX_OUTPUT;
    l.max_memory = ADAPT_DEFAULT_MAX_MEMORY;
    return l;
}

/* ------------------------------------------------------------------------
 * The text transform as a writer: what the coder recovers is undone on
 * its way to the caller's writer, and the original bytes' count and
 * checksum, the ones the trailer holds, are taken here.
 * --------------------------------------------------------------------- */

#define TEXT_SINK_CHUNK ((size_t)4096)

typedef struct text_sink {
    adapt_write_fn write;
    void *write_context;
    text_inverse inv;
    uint8_t *out; /* TEXT_INVERSE_SLACK * TEXT_SINK_CHUNK */
    const crc32_table *crc_table;
    uint32_t crc;
    uint64_t written, max_output;
    adapt_status status; /* ADAPT_OK, or why the stream was refused */
    uint64_t detail;
    int rc;              /* the caller's writer's failure, when it failed */
} text_sink;

static int text_sink_init(text_sink *s, adapt_write_fn write, void *write_context, const crc32_table *t,
                          uint64_t max_output) {
    memset(s, 0, sizeof(*s));
    s->write = write;
    s->write_context = write_context;
    s->crc_table = t;
    s->crc = CRC32_INIT;
    s->max_output = max_output;
    s->out = malloc(TEXT_INVERSE_SLACK * TEXT_SINK_CHUNK);
    if (s->out == NULL || !text_inverse_init(&s->inv)) {
        free(s->out);
        s->out = NULL;
        return 0;
    }
    return 1;
}

static void text_sink_free(text_sink *s) {
    text_inverse_free(&s->inv);
    free(s->out);
    s->out = NULL;
}

static int text_sink_write(void *context, const uint8_t *buf, size_t len) {
    text_sink *s = context;
    size_t i;
    if (s->status != ADAPT_OK) return 1;
    for (i = 0; i < len; i += TEXT_SINK_CHUNK) {
        size_t take = len - i < TEXT_SINK_CHUNK ? len - i : TEXT_SINK_CHUNK;
        size_t w = text_inverse_feed(&s->inv, buf + i, take, s->out);
        int rc;
        if (w == (size_t)-1) {
            s->status = ADAPT_ERROR_BAD_TEXT;
            return 1;
        }
        if (w > s->max_output - s->written) {
            s->status = ADAPT_ERROR_TOO_LARGE;
            s->detail = s->written + w;
            return 1;
        }
        s->crc = crc32_update(s->crc_table, s->crc, s->out, w);
        s->written += w;
        rc = s->write(s->write_context, s->out, w);
        if (rc != 0) {
            s->status = ADAPT_ERROR_IO;
            s->rc = rc;
            return rc;
        }
    }
    return 0;
}

/* The error a failed sink stands for, in place of the writer's. */
static adapt_status text_sink_fail(const text_sink *s, uint64_t max_output, adapt_error *error) {
    if (s->status == ADAPT_ERROR_TOO_LARGE) return fail(error, ADAPT_ERROR_TOO_LARGE, s->detail, max_output);
    if (s->status == ADAPT_ERROR_IO) return fail(error, ADAPT_ERROR_IO, (uint64_t)(s->rc < 0 ? -(int64_t)s->rc : s->rc), 0);
    return fail(error, ADAPT_ERROR_BAD_TEXT, 0, 0);
}

/* ------------------------------------------------------------------------
 * container.rs — the streaming decoder
 * --------------------------------------------------------------------- */

/* Both historical model2 variants used 58/61. At small table sizes their
 * bytes cannot be distinguished by a header. Keep only one model alive,
 * spool its tentative output, and release bytes only after verification. */
static int legacy_ambiguous(const uint8_t *header, adapt_sizes sizes) {
    return (header[4] == ADAPT_MAX_SIZED_VERSION || header[4] == ADAPT_MAX_SIZED_METADATA_VERSION) &&
           sizes.model2 && sizes.table_log < MODEL2_PRESSURE_LOG;
}

static adapt_status decode_legacy(const uint8_t *header, size_t header_len,
                                  adapt_read_fn read, void *read_context, adapt_write_fn write, void *write_context,
                                  adapt_limits limits, adapt_stats *stats, adapt_error *error);

static adapt_status decode_stream_mode(adapt_read_fn read, void *read_context, adapt_write_fn write, void *write_context,
                                       adapt_limits limits, adapt_stats *stats, adapt_error *error, int legacy) {
    uint8_t header[ADAPT_HEADER_MAX_BYTES];
    uint8_t block_header[ADAPT_BLOCK_HEADER_BYTES];
    adapt_sizes sizes;
    adapt_status status;
    size_t header_len;
    block_decoder decoder = {NULL};
    uint8_t *payload = NULL;
    uint8_t *scratch = NULL;
    uint8_t *table = NULL;
    segment_counter segments;
    crc32_table crc_table;
    uint64_t written = 0;
    uint32_t crc = CRC32_INIT;
    uint64_t output_bytes = ADAPT_HEADER_BYTES;
    size_t got;
    text_sink text;
    int has_text = 0;
    if (read == NULL || write == NULL) {
        return fail(error, ADAPT_ERROR_INVALID_ARGUMENT, 0, 0);
    }
    fail(error, ADAPT_OK, 0, 0);
    counter_init(&segments, 0);
    status = fill(read, read_context, header, ADAPT_HEADER_BYTES, &got, error);
    if (status != ADAPT_OK) {
        return status;
    }
    if (got != ADAPT_HEADER_BYTES) {
        return fail(error, ADAPT_ERROR_TRUNCATED, 0, 0);
    }
    header_len = adapt_header_len(header);
    if (header_len > ADAPT_HEADER_BYTES) {
        status = fill(read, read_context, header + ADAPT_HEADER_BYTES, header_len - ADAPT_HEADER_BYTES, &got, error);
        if (status != ADAPT_OK) {
            return status;
        }
        if (got != header_len - ADAPT_HEADER_BYTES) {
            return fail(error, ADAPT_ERROR_TRUNCATED, 0, 0);
        }
    }
    status = adapt_read_header(header, header_len, &sizes, error);
    if (status != ADAPT_OK) {
        return status;
    }
    output_bytes = header_len;
    status = check_memory(adapt_decode_memory_bound(sizes), limits.max_memory, error);
    if (status != ADAPT_OK) {
        return status;
    }
    if (legacy < 0 && legacy_ambiguous(header, sizes)) {
        return decode_legacy(header, header_len, read, read_context, write, write_context, limits, stats, error);
    }
    segments.segmented = sizes.segmented;
    payload = malloc(ADAPT_BLOCK_BYTES);
    scratch = malloc(ADAPT_BLOCK_BYTES);
    if (payload == NULL || scratch == NULL) {
        status = fail(error, ADAPT_ERROR_ALLOCATION, 0, 0);
        goto done;
    }
    crc32_table_init(&crc_table);
    if (sizes.text == ADAPT_TEXT_ON) {
        /* the recovered bytes are the transform's: undone on their way out */
        if (!text_sink_init(&text, write, write_context, &crc_table, limits.max_output)) {
            status = fail(error, ADAPT_ERROR_ALLOCATION, 0, 0);
            goto done;
        }
        has_text = 1;
        write = text_sink_write;
        write_context = &text;
    }
    for (;;) {
        uint8_t method;
        size_t raw, payload_len;
        status = fill(read, read_context, block_header, 1, &got, error);
        if (status != ADAPT_OK) {
            goto done;
        }
        if (got != 1) {
            status = fail(error, ADAPT_ERROR_TRUNCATED, 0, 0);
            goto done;
        }
        if (block_header[0] == METHOD_SEGMENT) {
            /* A fresh model for the next segment; the old one goes first,
             * so that one model's memory is ever held. */
            status = counter_marker(&segments, error);
            if (status != ADAPT_OK) {
                goto done;
            }
            output_bytes += 1;
            model_free(decoder.m);
            decoder.m = NULL;
            continue;
        }
        if (block_header[0] == METHOD_TABLE) {
            size_t len;
            status = counter_table_len(&segments, &len, error);
            if (status != ADAPT_OK) {
                goto done;
            }
            free(table);
            table = malloc(len);
            if (table == NULL) {
                status = fail(error, ADAPT_ERROR_ALLOCATION, 0, 0);
                goto done;
            }
            status = fill(read, read_context, table, len, &got, error);
            if (status != ADAPT_OK) {
                goto done;
            }
            if (got != len) {
                status = fail(error, ADAPT_ERROR_TRUNCATED, 0, 0);
                goto done;
            }
            status = counter_table(&segments, table, error);
            if (status != ADAPT_OK) {
                goto done;
            }
            output_bytes += 1 + len;
            continue;
        }
        if (block_header[0] == METHOD_END) {
            uint8_t trailer[TRAILER_PAYLOAD];
            uint64_t len;
            uint32_t sum;
            uint8_t rest[4096];
            status = counter_end(&segments, error);
            if (status != ADAPT_OK) {
                goto done;
            }
            status = fill(read, read_context, trailer, TRAILER_PAYLOAD, &got, error);
            if (status != ADAPT_OK) {
                goto done;
            }
            if (got != TRAILER_PAYLOAD) {
                status = fail(error, ADAPT_ERROR_TRUNCATED, 0, 0);
                goto done;
            }
            output_bytes += ADAPT_TRAILER_BYTES;
            read_trailer_payload(trailer, &len, &sum);
            if (has_text) {
                /* the trailer names the original bytes, which the sink wrote */
                if (!text_inverse_complete(&text.inv)) {
                    status = fail(error, ADAPT_ERROR_BAD_TEXT, 0, 0);
                    goto done;
                }
                if (stats != NULL) {
                    stats->text_bytes = written;
                    stats->text_words = text.inv.book.words;
                }
                written = text.written;
                crc = text.crc;
            } else if (stats != NULL) {
                stats->text_bytes = 0;
                stats->text_words = 0;
            }
            if (len != written || sum != ~crc) {
                status = fail(error, ADAPT_ERROR_CHECKSUM, 0, 0);
                goto done;
            }
            status = fill(read, read_context, rest, 1, &got, error);
            if (status != ADAPT_OK) {
                goto done;
            }
            if (got != 0) {
                uint64_t extra = 1;
                for (;;) {
                    status = fill(read, read_context, rest, sizeof(rest), &got, error);
                    if (status != ADAPT_OK) {
                        goto done;
                    }
                    extra += got;
                    if (got < sizeof(rest)) {
                        break;
                    }
                }
                status = fail(error, ADAPT_ERROR_TRAILING_BYTES, extra, 0);
                goto done;
            }
            if (stats != NULL) {
                stats->input_bytes = written;
                stats->output_bytes = output_bytes;
                stats->crc32 = sum;
                stats->segments = segments.segmented ? (uint32_t)segments.count : 1;
                stats->workers = 1;
            }
            status = ADAPT_OK;
            goto done;
        }
        status = fill(read, read_context, block_header + 1, ADAPT_BLOCK_HEADER_BYTES - 1, &got, error);
        if (status != ADAPT_OK) {
            goto done;
        }
        if (got != ADAPT_BLOCK_HEADER_BYTES - 1) {
            status = fail(error, ADAPT_ERROR_TRUNCATED, 0, 0);
            goto done;
        }
        status = parse_block_header(block_header, &method, &raw, &payload_len, error);
        if (status != ADAPT_OK) {
            goto done;
        }
        status = counter_block(&segments, raw, ADAPT_BLOCK_HEADER_BYTES + payload_len, error);
        if (status != ADAPT_OK) {
            goto done;
        }
        status = fill(read, read_context, payload, payload_len, &got, error);
        if (status != ADAPT_OK) {
            goto done;
        }
        if (got != payload_len) {
            status = fail(error, ADAPT_ERROR_TRUNCATED, 0, 0);
            goto done;
        }
        output_bytes += ADAPT_BLOCK_HEADER_BYTES + payload_len;
        if (!has_text && written + raw > limits.max_output) {
            status = fail(error, ADAPT_ERROR_TOO_LARGE, written + raw, limits.max_output);
            goto done;
        }
        if (decoder.m == NULL) {
            decoder.m = model_new_policy(sizes, legacy == 1);
            if (decoder.m == NULL) {
                status = fail(error, ADAPT_ERROR_ALLOCATION, 0, 0);
                goto done;
            }
        }
        status = decode_block(&decoder, method, raw, payload, payload_len, scratch, error);
        if (status != ADAPT_OK) {
            goto done;
        }
        if (sizes.x86) {
            x86_inverse(scratch, raw);
        }
        if (sizes.bmp) bmp_transform(scratch, raw, written, sizes, 1);
        crc = crc32_update(&crc_table, crc, scratch, raw);
        written += raw;
        {
            int rc = write(write_context, scratch, raw);
            if (rc != 0) {
                status = has_text ? text_sink_fail(&text, limits.max_output, error)
                                  : fail(error, ADAPT_ERROR_IO, (uint64_t)(rc < 0 ? -(int64_t)rc : rc), 0);
                goto done;
            }
        }
    }
done:
    if (has_text) text_sink_free(&text);
    model_free(decoder.m);
    counter_free(&segments);
    free(table);
    free(payload);
    free(scratch);
    return status;
}

typedef struct legacy_reader {
    FILE *cache;
    const uint8_t *header;
    size_t header_len, header_at;
    adapt_read_fn read;
    void *context;
    int replay;
} legacy_reader;

/* Cache only bytes the first trial consumes. On retry, replay that prefix
 * and then continue the original reader; a pipe need not be seekable. */
static int legacy_read(void *context, uint8_t *buf, size_t len, size_t *got) {
    legacy_reader *r = context;
    int rc;
    if (r->replay) {
        *got = fread(buf, 1, len, r->cache);
        if (ferror(r->cache)) return EIO;
        if (*got != 0) return 0;
        return r->read(r->context, buf, len, got);
    }
    if (r->header_at < r->header_len) {
        *got = r->header_len - r->header_at;
        if (*got > len) *got = len;
        memcpy(buf, r->header + r->header_at, *got);
        r->header_at += *got;
        return 0;
    }
    rc = r->read(r->context, buf, len, got);
    if (rc != 0) return rc;
    if (*got > len) return EIO;
    return fwrite(buf, 1, *got, r->cache) == *got ? 0 : EIO;
}

static int legacy_write(void *context, const uint8_t *buf, size_t len) {
    return fwrite(buf, 1, len, context) == len ? 0 : EIO;
}

static adapt_status decode_legacy(const uint8_t *header, size_t header_len,
                                  adapt_read_fn read, void *read_context, adapt_write_fn write, void *write_context,
                                  adapt_limits limits, adapt_stats *stats, adapt_error *error) {
    legacy_reader reader;
    FILE *output = NULL;
    adapt_status status;
    adapt_error trial_error;
    adapt_stats trial_stats;
    uint8_t buffer[4096];
    size_t got;
    int attempt;
    memset(&reader, 0, sizeof(reader));
    reader.read = read;
    reader.context = read_context;
    reader.header = header;
    reader.header_len = header_len;
    reader.cache = tmpfile();
    if (reader.cache == NULL) return fail(error, ADAPT_ERROR_IO, EIO, 0);
    status = fail(&trial_error, ADAPT_ERROR_IO, EIO, 0);
    if (fwrite(header, 1, header_len, reader.cache) != header_len) goto done;
    for (attempt = 0; attempt < 2; attempt++) {
        output = tmpfile();
        if (output == NULL) { status = fail(&trial_error, ADAPT_ERROR_IO, EIO, 0); break; }
        status = decode_stream_mode(legacy_read, &reader, legacy_write, output, limits, &trial_stats, &trial_error, attempt);
        if (status == ADAPT_OK || attempt != 0 ||
            (status != ADAPT_ERROR_BAD_BLOCK && status != ADAPT_ERROR_CHECKSUM &&
             status != ADAPT_ERROR_BAD_TEXT && status != ADAPT_ERROR_TOO_LARGE)) break;
        if (fclose(output) != 0) { output = NULL; status = fail(&trial_error, ADAPT_ERROR_IO, EIO, 0); break; }
        output = NULL;
        if (fflush(reader.cache) != 0 || fseek(reader.cache, 0, SEEK_SET) != 0) {
            status = fail(&trial_error, ADAPT_ERROR_IO, EIO, 0); break;
        }
        reader.replay = 1;
    }
    if (status == ADAPT_OK && (fflush(output) != 0 || fseek(output, 0, SEEK_SET) != 0)) {
        status = fail(&trial_error, ADAPT_ERROR_IO, EIO, 0);
    }
    while (status == ADAPT_OK && (got = fread(buffer, 1, sizeof(buffer), output)) != 0) {
        int rc = write(write_context, buffer, got);
        if (rc != 0) status = fail(&trial_error, ADAPT_ERROR_IO, (uint64_t)(rc < 0 ? -(int64_t)rc : rc), 0);
    }
    if (status == ADAPT_OK && ferror(output)) status = fail(&trial_error, ADAPT_ERROR_IO, EIO, 0);
done:
    if (output != NULL && fclose(output) != 0 && status == ADAPT_OK) status = fail(&trial_error, ADAPT_ERROR_IO, EIO, 0);
    if (fclose(reader.cache) != 0 && status == ADAPT_OK) status = fail(&trial_error, ADAPT_ERROR_IO, EIO, 0);
    if (status == ADAPT_OK && stats != NULL) *stats = trial_stats;
    if (error != NULL) *error = trial_error;
    return status;
}

adapt_status adapt_decode_stream(adapt_read_fn read, void *read_context, adapt_write_fn write, void *write_context,
                                 adapt_limits limits, adapt_stats *stats, adapt_error *error) {
    return decode_stream_mode(read, read_context, write, write_context, limits, stats, error, -1);
}

/* ------------------------------------------------------------------------
 * container.rs — the in-memory decoder
 * --------------------------------------------------------------------- */

typedef struct memory_reader {
    const uint8_t *src;
    size_t len;
    size_t pos;
} memory_reader;

static int memory_read(void *context, uint8_t *buf, size_t len, size_t *got) {
    memory_reader *r = context;
    size_t n = r->len - r->pos < len ? r->len - r->pos : len;
    /* An empty source may be NULL, which may not be offset or handed to
     * memcpy even for zero bytes. */
    if (n != 0) {
        memcpy(buf, r->src + r->pos, n);
    }
    r->pos += n;
    *got = n;
    return 0;
}

typedef struct memory_writer {
    uint8_t *out;
    size_t len;
    size_t pos;
    int overflow; /* more bytes came than the trailer declared */
} memory_writer;

static int memory_write(void *context, const uint8_t *buf, size_t len) {
    memory_writer *w = context;
    if (len > w->len - w->pos) {
        w->overflow = 1;
        return 1;
    }
    memcpy(w->out + w->pos, buf, len);
    w->pos += len;
    return 0;
}

adapt_status adapt_decode(const uint8_t *file, size_t file_len, adapt_limits limits, uint8_t **out, size_t *out_len,
                          adapt_error *error) {
    adapt_sizes sizes;
    adapt_status status;
    size_t pos = ADAPT_HEADER_BYTES;
    uint64_t total = 0, len = 0, required;
    uint32_t sum = 0;
    memory_reader reader;
    memory_writer writer;
    adapt_stats stats;
    segment_counter segments;
    if ((file == NULL && file_len != 0) || out == NULL || out_len == NULL) {
        return fail(error, ADAPT_ERROR_INVALID_ARGUMENT, 0, 0);
    }
    *out = NULL;
    *out_len = 0;
    if (file_len < ADAPT_HEADER_BYTES) {
        return fail(error, ADAPT_ERROR_TRUNCATED, 0, 0);
    }
    status = adapt_read_header(file, file_len, &sizes, error);
    if (status != ADAPT_OK) {
        return status;
    }
    pos = adapt_header_len(file);
    counter_init(&segments, sizes.segmented);
    for (;;) {
        uint8_t method;
        size_t raw, payload_len, start = pos;
        const uint8_t *payload;
        status = read_block(file, file_len, &pos, &method, &raw, &payload, &payload_len, error);
        if (status != ADAPT_OK) {
            counter_free(&segments);
            return status;
        }
        if (method == METHOD_END) {
            status = counter_end(&segments, error);
            if (status != ADAPT_OK) {
                counter_free(&segments);
                return status;
            }
            read_trailer_payload(payload, &len, &sum);
            break;
        } else if (method == METHOD_SEGMENT) {
            status = counter_marker(&segments, error);
        } else if (method == METHOD_TABLE) {
            size_t table_len;
            status = counter_table_len(&segments, &table_len, error);
            if (status == ADAPT_OK) {
                if (file_len - pos < table_len) {
                    status = fail(error, ADAPT_ERROR_TRUNCATED, 0, 0);
                } else {
                    status = counter_table(&segments, file + pos, error);
                    pos += table_len;
                }
            }
        } else {
            status = counter_block(&segments, raw, pos - start, error);
            total += raw;
        }
        if (status != ADAPT_OK) {
            counter_free(&segments);
            return status;
        }
    }
    counter_free(&segments);
    if (pos != file_len) {
        return fail(error, ADAPT_ERROR_TRAILING_BYTES, file_len - pos, 0);
    }
    if (len != total && sizes.text != ADAPT_TEXT_ON) {
        return fail(error, ADAPT_ERROR_CHECKSUM, 0, 0);
    }
    if (len > limits.max_output) {
        return fail(error, ADAPT_ERROR_TOO_LARGE, len, limits.max_output);
    }
    required = adapt_decode_memory_bound(sizes) + len;
    status = check_memory(required, limits.max_memory, error);
    if (status != ADAPT_OK) {
        return status;
    }
    if (len > (uint64_t)(size_t)-1) {
        return fail(error, ADAPT_ERROR_ALLOCATION, 0, 0);
    }
    writer.out = malloc(len > 0 ? (size_t)len : 1);
    if (writer.out == NULL) {
        return fail(error, ADAPT_ERROR_ALLOCATION, 0, 0);
    }
    writer.len = (size_t)len;
    writer.pos = 0;
    writer.overflow = 0;
    reader.src = file;
    reader.len = file_len;
    reader.pos = 0;
    status = adapt_decode_stream(memory_read, &reader, memory_write, &writer, limits, &stats, error);
    if (status != ADAPT_OK) {
        free(writer.out);
        /* under the text transform the blocks' bytes do not say how many
         * come out; more than the trailer declared is the trailer's lie */
        return status == ADAPT_ERROR_IO && writer.overflow ? fail(error, ADAPT_ERROR_CHECKSUM, 0, 0) : status;
    }
    *out = writer.out;
    *out_len = writer.pos;
    return ADAPT_OK;
}

void adapt_free(uint8_t *bytes) {
    free(bytes);
}

/* ------------------------------------------------------------------------
 * Segments on threads
 * --------------------------------------------------------------------- */

/* Bytes the parallel decoder needs: per worker a model, the longest
 * segment's bytes and a block's workspace; the file it reads from, which
 * the caller holds; the headroom. */
uint64_t adapt_parallel_decode_memory_bound(adapt_sizes sizes, uint64_t workers, uint64_t segment_raw_max,
                                            uint64_t file_len) {
    return (adapt_model_bytes(sizes) + segment_raw_max + block_workspace()) * workers + file_len + HEADROOM;
}

uint64_t adapt_decode_workers(adapt_sizes sizes, uint64_t max_memory, uint64_t segment_raw_max, uint64_t file_len,
                              uint64_t threads) {
#ifdef ADAPT_THREADS
    uint64_t w = threads > ADAPT_WORKERS_MAX ? ADAPT_WORKERS_MAX : threads;
    if (!sizes.segmented) {
        return 1;
    }
    while (w >= 2 && adapt_parallel_decode_memory_bound(sizes, w, segment_raw_max, file_len) > max_memory) {
        w--;
    }
    return w < 1 ? 1 : w;
#else
    (void)sizes;
    (void)max_memory;
    (void)segment_raw_max;
    (void)file_len;
    (void)threads;
    return 1;
#endif
}

/* The segment table of a complete segmented file, read from its end: the
 * count before the trailer, the entries before the count. Each entry is
 * a segment's original and coded length, the coded length counting the
 * marker byte before every segment after the first. Checks that the
 * entries account for exactly the bytes between the header and the
 * table, and for the length the trailer declares. */
static adapt_status read_segment_table(const uint8_t *file, size_t file_len, size_t header_len, uint64_t declared,
                                       int text, const uint8_t **entries, uint32_t *count_out, uint64_t *raw_max,
                                       adapt_error *error) {
    uint32_t count, i;
    size_t table_len, table_at;
    uint64_t raw_total = 0, coded_total = 0, longest = 0;
    if (file_len < header_len + ADAPT_TRAILER_BYTES + ADAPT_SEGMENT_COUNT_BYTES + 1) {
        return fail(error, ADAPT_ERROR_TRUNCATED, 0, 0);
    }
    count = le32(file + file_len - ADAPT_TRAILER_BYTES - ADAPT_SEGMENT_COUNT_BYTES);
    table_len = adapt_segment_table_len(count);
    if (count == 0 || file_len - ADAPT_TRAILER_BYTES < header_len + table_len) {
        return fail(error, ADAPT_ERROR_BAD_BLOCK, 0, 0);
    }
    table_at = file_len - ADAPT_TRAILER_BYTES - table_len;
    if (file[table_at] != METHOD_TABLE) {
        return fail(error, ADAPT_ERROR_BAD_BLOCK, 0, 0);
    }
    for (i = 0; i < count; i++) {
        uint32_t raw = le32(file + table_at + 1 + (size_t)i * ADAPT_SEGMENT_ENTRY_BYTES);
        uint32_t coded = le32(file + table_at + 1 + (size_t)i * ADAPT_SEGMENT_ENTRY_BYTES + 4);
        /* An empty segmented container has one entry, both lengths zero,
         * and no body. It falls back to streaming below, including CRC
         * validation and the retained-input memory budget. */
        if ((raw == 0 || coded == 0) && !(count == 1 && raw == 0 && coded == 0 && declared == 0)) {
            return fail(error, ADAPT_ERROR_BAD_BLOCK, 0, 0);
        }
        raw_total += raw;
        coded_total += coded;
        if (raw > longest) {
            longest = raw;
        }
    }
    /* the segments' raw bytes are the transform's under it, not the declared original's */
    if (coded_total != table_at - header_len || (!text && raw_total != declared)) {
        return fail(error, ADAPT_ERROR_BAD_BLOCK, 0, 0);
    }
    *entries = file + table_at + 1;
    *count_out = count;
    *raw_max = longest;
    return ADAPT_OK;
}

#ifdef ADAPT_THREADS
#include <pthread.h>

/* One segment's decoding, on a worker: its coded bytes — the marker
 * before every segment after the first, then its blocks — decoded under a
 * fresh model into `output`, which must come to exactly `raw` bytes. */
typedef struct decode_job {
    adapt_sizes sizes;
    uint64_t index;
    uint64_t position;
    const uint8_t *coded;
    size_t coded_len;
    uint8_t *output;
    size_t raw;
    size_t produced;
    adapt_status status;
    adapt_error error;
    pthread_t thread;
    int busy;
} decode_job;

static void *decode_segment(void *arg) {
    decode_job *j = arg;
    block_decoder d;
    size_t pos = 0;
    j->produced = 0;
    j->status = ADAPT_OK;
    if (j->index > 0) {
        if (j->coded_len < 1 || j->coded[0] != METHOD_SEGMENT) {
            j->status = fail(&j->error, ADAPT_ERROR_BAD_BLOCK, 0, 0);
            return NULL;
        }
        pos = 1;
    }
    d.m = model_new(j->sizes);
    if (d.m == NULL) {
        j->status = fail(&j->error, ADAPT_ERROR_ALLOCATION, 0, 0);
        return NULL;
    }
    while (pos < j->coded_len) {
        uint8_t method;
        size_t raw, payload_len;
        const uint8_t *payload;
        /* a block the segment's bytes cut short is the table's fault: the
         * file itself was whole */
        if (read_block(j->coded, j->coded_len, &pos, &method, &raw, &payload, &payload_len, &j->error) != ADAPT_OK ||
            method > METHOD_STORED_SKIPPED || raw > j->raw - j->produced) {
            j->status = fail(&j->error, ADAPT_ERROR_BAD_BLOCK, 0, 0);
            break;
        }
        j->status = decode_block(&d, method, raw, payload, payload_len, j->output + j->produced, &j->error);
        if (j->status != ADAPT_OK) {
            break;
        }
        if (j->sizes.x86) {
            x86_inverse(j->output + j->produced, raw);
        }
        if (j->sizes.bmp) {
            bmp_transform(j->output + j->produced, raw, j->position + j->produced, j->sizes, 1);
        }
        j->produced += raw;
    }
    if (j->status == ADAPT_OK && j->produced != j->raw) {
        j->status = fail(&j->error, ADAPT_ERROR_BAD_BLOCK, 0, 0);
    }
    model_free(d.m);
    return NULL;
}

static void decode_job_start(decode_job *j) {
    j->busy = 1;
    if (pthread_create(&j->thread, NULL, decode_segment, j) != 0) {
        decode_segment(j);
        j->busy = 2;
    }
}

static void decode_job_wait(decode_job *j) {
    if (j->busy == 1) {
        pthread_join(j->thread, NULL);
    }
    j->busy = 0;
}

/* The finished segment j, in order: its bytes to the writer and into the
 * checksum. */
static adapt_status decode_job_emit(decode_job *j, adapt_write_fn write, void *write_context, const crc32_table *t,
                                    uint32_t *crc, uint64_t *written, adapt_error *error) {
    int rc;
    if (j->status != ADAPT_OK) {
        if (error != NULL) {
            *error = j->error;
        }
        return j->status;
    }
    *crc = crc32_update(t, *crc, j->output, j->produced);
    rc = write(write_context, j->output, j->produced);
    if (rc != 0) {
        return fail(error, ADAPT_ERROR_IO, (uint64_t)(rc < 0 ? -(int64_t)rc : rc), 0);
    }
    *written += j->produced;
    return ADAPT_OK;
}

static adapt_status decode_segments_parallel(const uint8_t *file, size_t header_len, adapt_sizes sizes,
                                             const uint8_t *entries, uint32_t count, uint64_t raw_max, uint64_t workers,
                                             adapt_write_fn write, void *write_context, uint32_t *crc_out,
                                             uint64_t *written_out, adapt_error *error) {
    decode_job *jobs;
    size_t slots = (size_t)workers, i;
    size_t coded_at = header_len;
    uint64_t position = 0, written = 0;
    uint32_t crc = CRC32_INIT, k;
    crc32_table t;
    adapt_status status = ADAPT_OK;
    crc32_table_init(&t);
    jobs = calloc(slots, sizeof(*jobs));
    if (jobs == NULL) {
        return fail(error, ADAPT_ERROR_ALLOCATION, 0, 0);
    }
    for (i = 0; i < slots; i++) {
        jobs[i].sizes = sizes;
        jobs[i].output = malloc((size_t)raw_max);
        if (jobs[i].output == NULL) {
            status = fail(error, ADAPT_ERROR_ALLOCATION, 0, 0);
        }
    }
    for (k = 0; k < count && status == ADAPT_OK; k++) {
        decode_job *j = &jobs[k % slots];
        uint32_t raw = le32(entries + (size_t)k * ADAPT_SEGMENT_ENTRY_BYTES);
        uint32_t coded = le32(entries + (size_t)k * ADAPT_SEGMENT_ENTRY_BYTES + 4);
        if (j->busy) {
            decode_job_wait(j);
            status = decode_job_emit(j, write, write_context, &t, &crc, &written, error);
            if (status != ADAPT_OK) {
                break;
            }
        }
        j->index = k;
        j->position = position;
        j->coded = file + coded_at;
        j->coded_len = coded;
        j->raw = raw;
        coded_at += coded;
        position += raw;
        decode_job_start(j);
    }
    for (i = 0; i < slots; i++) {
        decode_job *j = &jobs[(k + i) % slots];
        if (!j->busy) {
            continue;
        }
        decode_job_wait(j);
        if (status == ADAPT_OK) {
            status = decode_job_emit(j, write, write_context, &t, &crc, &written, error);
        }
    }
    for (i = 0; i < slots; i++) {
        free(jobs[i].output);
    }
    free(jobs);
    *crc_out = crc;
    *written_out = written;
    return status;
}
#endif

adapt_status adapt_decode_threads(const uint8_t *file, size_t file_len, adapt_limits limits, uint64_t threads,
                                  adapt_write_fn write, void *write_context, adapt_stats *stats, adapt_error *error) {
    adapt_sizes sizes;
    adapt_status status;
    size_t header_len;
    uint64_t declared, workers = 1, raw_max = 0;
    uint32_t crc, count = 0;
    const uint8_t *entries = NULL;
    if ((file == NULL && file_len != 0) || write == NULL || threads == 0) {
        return fail(error, ADAPT_ERROR_INVALID_ARGUMENT, 0, 0);
    }
    if (file_len < ADAPT_HEADER_BYTES) {
        return fail(error, ADAPT_ERROR_TRUNCATED, 0, 0);
    }
    status = adapt_read_header(file, file_len, &sizes, error);
    if (status != ADAPT_OK) {
        return status;
    }
    header_len = adapt_header_len(file);
    if (sizes.segmented && threads >= 2 && !legacy_ambiguous(file, sizes)) {
        if (file_len < header_len + ADAPT_TRAILER_BYTES) {
            return fail(error, ADAPT_ERROR_TRUNCATED, 0, 0);
        }
        status = adapt_read_trailer(file + file_len - ADAPT_TRAILER_BYTES, &declared, &crc, error);
        if (status != ADAPT_OK) {
            return status;
        }
        status = read_segment_table(file, file_len, header_len, declared, sizes.text == ADAPT_TEXT_ON, &entries, &count,
                                    &raw_max, error);
        if (status != ADAPT_OK) {
            return status;
        }
        if (declared > limits.max_output) {
            return fail(error, ADAPT_ERROR_TOO_LARGE, declared, limits.max_output);
        }
        workers = adapt_decode_workers(sizes, limits.max_memory, raw_max, file_len, threads);
        if (workers > count) {
            workers = count;
        }
    }
#ifdef ADAPT_THREADS
    if (workers >= 2) {
        uint32_t sum;
        uint64_t written, text_bytes = 0;
        uint32_t text_words = 0;
        crc32_table t;
        text_sink text;
        status = check_memory(adapt_parallel_decode_memory_bound(sizes, workers, raw_max, file_len), limits.max_memory,
                              error);
        if (status != ADAPT_OK) {
            return status;
        }
        if (sizes.text == ADAPT_TEXT_ON) {
            /* the segments come out in order: undone on their way to the writer */
            crc32_table_init(&t);
            if (!text_sink_init(&text, write, write_context, &t, limits.max_output)) {
                return fail(error, ADAPT_ERROR_ALLOCATION, 0, 0);
            }
            write = text_sink_write;
            write_context = &text;
        }
        status = decode_segments_parallel(file, header_len, sizes, entries, count, raw_max, workers, write, write_context,
                                          &sum, &written, error);
        if (sizes.text == ADAPT_TEXT_ON) {
            if (status == ADAPT_ERROR_IO && text.status != ADAPT_OK) status = text_sink_fail(&text, limits.max_output, error);
            if (status == ADAPT_OK && !text_inverse_complete(&text.inv)) status = fail(error, ADAPT_ERROR_BAD_TEXT, 0, 0);
            text_bytes = written;
            text_words = text.inv.book.words;
            written = text.written;
            sum = text.crc;
            text_sink_free(&text);
        }
        if (status != ADAPT_OK) {
            return status;
        }
        if (written != declared || ~sum != crc) {
            return fail(error, ADAPT_ERROR_CHECKSUM, 0, 0);
        }
        if (stats != NULL) {
            stats->input_bytes = written;
            stats->output_bytes = file_len;
            stats->crc32 = crc;
            stats->segments = count;
            stats->workers = (uint32_t)workers;
            stats->text_bytes = text_bytes;
            stats->text_words = text_words;
        }
        return fail(error, ADAPT_OK, 0, 0);
    }
#endif
    {
        memory_reader reader;
        uint64_t bound = adapt_decode_memory_bound(sizes);
        uint64_t required = file_len > UINT64_MAX - bound ? UINT64_MAX : bound + file_len;
        /* The input remains resident when only one worker fits, including
         * unsegmented files and builds without thread support. */
        status = check_memory(required, limits.max_memory, error);
        if (status != ADAPT_OK) return status;
        limits.max_memory -= file_len;
        reader.src = file;
        reader.len = file_len;
        reader.pos = 0;
        return adapt_decode_stream(memory_read, &reader, write, write_context, limits, stats, error);
    }
}

adapt_status adapt_declared_len(const uint8_t *file, size_t file_len, uint64_t *length, adapt_error *error) {
    adapt_sizes sizes;
    adapt_status status;
    uint32_t crc;
    if ((file == NULL && file_len != 0) || length == NULL) {
        return fail(error, ADAPT_ERROR_INVALID_ARGUMENT, 0, 0);
    }
    if (file_len < ADAPT_HEADER_BYTES) {
        return fail(error, ADAPT_ERROR_TRUNCATED, 0, 0);
    }
    status = adapt_read_header(file, file_len, &sizes, error);
    if (status != ADAPT_OK) {
        return status;
    }
    if (file_len < adapt_header_len(file) + ADAPT_TRAILER_BYTES) {
        return fail(error, ADAPT_ERROR_TRUNCATED, 0, 0);
    }
    return adapt_read_trailer(file + file_len - ADAPT_TRAILER_BYTES, length, &crc, error);
}

/* ------------------------------------------------------------------------
 * lib.rs — the messages
 * --------------------------------------------------------------------- */

int adapt_error_message(const adapt_error *error, char *buf, size_t len) {
    if (error == NULL) {
        return snprintf(buf, len, "no error");
    }
    switch (error->status) {
        case ADAPT_OK:
            return snprintf(buf, len, "no error");
        case ADAPT_ERROR_MEMORY_LIMIT:
            return snprintf(buf, len, "memory budget exceeded: need %llu bytes, limit %llu", (unsigned long long)error->detail,
                            (unsigned long long)error->limit);
        case ADAPT_ERROR_ALLOCATION:
            return snprintf(buf, len, "memory allocation failed");
        case ADAPT_ERROR_BAD_MAGIC:
            return snprintf(buf, len, "not an adapt file (bad signature)");
        case ADAPT_ERROR_UNSUPPORTED_VERSION:
            return snprintf(buf, len, "unsupported format version %llu", (unsigned long long)error->detail);
        case ADAPT_ERROR_BAD_HEADER:
            return snprintf(buf, len, "invalid header field");
        case ADAPT_ERROR_TOO_LARGE:
            return snprintf(buf, len, "declared size %llu exceeds the limit %llu", (unsigned long long)error->detail,
                            (unsigned long long)error->limit);
        case ADAPT_ERROR_TRUNCATED:
            return snprintf(buf, len, "file is truncated");
        case ADAPT_ERROR_BAD_BLOCK:
            return snprintf(buf, len, "invalid block");
        case ADAPT_ERROR_BAD_METHOD:
            return snprintf(buf, len, "unknown block method %llu", (unsigned long long)error->detail);
        case ADAPT_ERROR_TRAILING_BYTES:
            return snprintf(buf, len, "%llu unexpected trailing byte(s)", (unsigned long long)error->detail);
        case ADAPT_ERROR_CHECKSUM:
            return snprintf(buf, len, "checksum mismatch: the recovered bytes are damaged");
        case ADAPT_ERROR_VERIFICATION:
            if (error->cause == ADAPT_OK) {
                return snprintf(buf, len, "verification failed: decoded bytes differ from the input");
            } else {
                adapt_error cause;
                char text[128];
                cause.status = error->cause;
                cause.cause = ADAPT_OK;
                cause.detail = error->detail;
                cause.limit = error->limit;
                adapt_error_message(&cause, text, sizeof(text));
                return snprintf(buf, len, "verification failed: %s", text);
            }
        case ADAPT_ERROR_IO:
            return snprintf(buf, len, "i/o error: callback status %llu", (unsigned long long)error->detail);
        case ADAPT_ERROR_INVALID_ARGUMENT:
            return snprintf(buf, len, "invalid argument");
        case ADAPT_ERROR_BAD_ARCHIVE:
            return snprintf(buf, len, "invalid archive at byte %llu", (unsigned long long)error->detail);
        case ADAPT_ERROR_BAD_TEXT:
            return snprintf(buf, len, "invalid text transform stream");
    }
    return snprintf(buf, len, "unknown error %d", (int)error->status);
}
