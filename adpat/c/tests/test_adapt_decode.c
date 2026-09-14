/*
 * test_adapt_decode.c — the C decoder's own tests, needing no Rust
 * toolchain: the library's contract on the committed vectors, every
 * framing error on hand-built bytes, the limits, the stream callbacks, and
 * the messages. The larger vectors pin the decoder to files this
 * implementation wrote across the paths a short input never takes; the
 * battery against the Rust encoder belonged to the crate, and no longer
 * applies since format 13.
 *
 * Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-12.
 *
 *   test_adapt_decode <vectors-dir>
 */
#include "../adapt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                                                 \
    do {                                                                                            \
        checks++;                                                                                   \
        if (!(cond)) {                                                                              \
            failures++;                                                                             \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);                \
        }                                                                                           \
    } while (0)

#define CHECK_STATUS(expr, expected)                                                                \
    do {                                                                                            \
        adapt_status got_ = (expr);                                                                 \
        checks++;                                                                                   \
        if (got_ != (expected)) {                                                                   \
            failures++;                                                                             \
            fprintf(stderr, "%s:%d: %s: expected status %d, got %d\n", __FILE__, __LINE__, #expr, \
                    (int)(expected), (int)got_);                                                    \
        }                                                                                           \
    } while (0)

static const char *vectors_dir;

static uint8_t *read_file(const char *name, size_t *len) {
    char path[512];
    FILE *f;
    uint8_t *buf;
    long size;
    snprintf(path, sizeof(path), "%s/%s", vectors_dir, name);
    f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)size + 1);
    if (buf == NULL || (size > 0 && fread(buf, 1, (size_t)size, f) != (size_t)size)) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(2);
    }
    fclose(f);
    *len = (size_t)size;
    return buf;
}

/* ---- the committed vectors: in memory, and streamed ------------------ */

typedef struct chunked_reader {
    const uint8_t *src;
    size_t len;
    size_t pos;
    size_t chunk; /* bytes per read, to exercise short reads */
    int fail_after; /* reads before the callback fails, or -1 */
    int reads;
} chunked_reader;

static int chunked_read(void *context, uint8_t *buf, size_t len, size_t *got) {
    chunked_reader *r = context;
    size_t n;
    if (r->fail_after >= 0 && r->reads >= r->fail_after) {
        return 5;
    }
    r->reads++;
    n = r->len - r->pos;
    if (n > len) {
        n = len;
    }
    if (n > r->chunk) {
        n = r->chunk;
    }
    memcpy(buf, r->src + r->pos, n);
    r->pos += n;
    *got = n;
    return 0;
}

typedef struct sink {
    uint8_t *out;
    size_t cap;
    size_t len;
    int fail;
} sink;

static int sink_write(void *context, const uint8_t *buf, size_t len) {
    sink *s = context;
    if (s->fail) {
        return -7;
    }
    if (len > s->cap - s->len) {
        return 1;
    }
    memcpy(s->out + s->len, buf, len);
    s->len += len;
    return 0;
}

static void test_vector(const char *name, const char *source) {
    size_t file_len, want_len, out_len;
    uint8_t *file = read_file(name, &file_len);
    uint8_t *want = read_file(source, &want_len);
    uint8_t *out = NULL;
    adapt_error error;
    adapt_stats stats;
    chunked_reader reader;
    sink s;
    size_t chunk;
    uint64_t declared = 999;
    /* in memory */
    CHECK_STATUS(adapt_decode(file, file_len, adapt_default_limits(), &out, &out_len, &error), ADAPT_OK);
    CHECK(error.status == ADAPT_OK);
    CHECK(out != NULL);
    CHECK(out_len == want_len);
    CHECK(out_len == want_len && memcmp(out, want, want_len) == 0);
    adapt_free(out);
    CHECK_STATUS(adapt_declared_len(file, file_len, &declared, &error), ADAPT_OK);
    CHECK(declared == want_len);
    /* streamed, in every chunk size a pipe might deliver */
    s.out = malloc(want_len + 1);
    s.cap = want_len;
    s.fail = 0;
    for (chunk = 1; chunk <= file_len + 1; chunk = chunk < 7 ? chunk + 1 : chunk * 3) {
        reader.src = file;
        reader.len = file_len;
        reader.pos = 0;
        reader.chunk = chunk;
        reader.fail_after = -1;
        reader.reads = 0;
        s.len = 0;
        CHECK_STATUS(adapt_decode_stream(chunked_read, &reader, sink_write, &s, adapt_default_limits(), &stats, &error),
                     ADAPT_OK);
        CHECK(s.len == want_len && memcmp(s.out, want, want_len) == 0);
        CHECK(stats.input_bytes == want_len);
        CHECK(stats.output_bytes == file_len);
        CHECK(stats.crc32 == adapt_crc32(want, want_len));
    }
    /* the trailer's checksum is the CRC-32 of the source */
    CHECK(file_len >= ADAPT_TRAILER_BYTES);
    {
        uint64_t length = 0;
        uint32_t crc = 0;
        CHECK_STATUS(adapt_read_trailer(file + file_len - ADAPT_TRAILER_BYTES, &length, &crc, &error), ADAPT_OK);
        CHECK(length == want_len);
        CHECK(crc == adapt_crc32(want, want_len));
    }
    free(s.out);
    free(file);
    free(want);
}

/* ---- the stream callbacks' failures --------------------------------- */

static void test_stream_failures(void) {
    size_t file_len;
    uint8_t *file = read_file("abc.adpt", &file_len);
    uint8_t buffer[1024];
    adapt_error error;
    chunked_reader reader;
    sink s;
    int after;
    s.out = buffer;
    s.cap = sizeof(buffer);
    s.fail = 0;
    /* a reader that fails at any point is an i/o error carrying its code */
    for (after = 0; after < 6; after++) {
        reader.src = file;
        reader.len = file_len;
        reader.pos = 0;
        reader.chunk = 4;
        reader.fail_after = after;
        reader.reads = 0;
        s.len = 0;
        CHECK_STATUS(adapt_decode_stream(chunked_read, &reader, sink_write, &s, adapt_default_limits(), NULL, &error),
                     ADAPT_ERROR_IO);
        CHECK(error.detail == 5);
    }
    /* a writer that fails is an i/o error with its code made positive */
    reader.src = file;
    reader.len = file_len;
    reader.pos = 0;
    reader.chunk = 1000;
    reader.fail_after = -1;
    reader.reads = 0;
    s.len = 0;
    s.fail = 1;
    CHECK_STATUS(adapt_decode_stream(chunked_read, &reader, sink_write, &s, adapt_default_limits(), NULL, &error),
                 ADAPT_ERROR_IO);
    CHECK(error.detail == 7);
    /* NULL callbacks are refused before anything is read */
    CHECK_STATUS(adapt_decode_stream(NULL, &reader, sink_write, &s, adapt_default_limits(), NULL, &error),
                 ADAPT_ERROR_INVALID_ARGUMENT);
    CHECK_STATUS(adapt_decode_stream(chunked_read, &reader, NULL, &s, adapt_default_limits(), NULL, NULL),
                 ADAPT_ERROR_INVALID_ARGUMENT);
    free(file);
}

/* ---- framing errors on hand-built bytes ------------------------------- */

static const uint8_t EMPTY_FILE[21] = {'A', 'D', 'P', 'T', ADAPT_FORMAT_VERSION, 20, 12, 16, 3, 0, 0,
                                       0,   0,   0,   0,   0,                    0,  0,  0,  0, 0};
/* the same input, segmented: one empty segment in the table */
static const uint8_t EMPTY_SEGMENTED[34] = {'A', 'D', 'P', 'T', ADAPT_FORMAT_VERSION, 20, 12, 16 | 0x40, 5, 0, 0, 0,
                                            0,   0,   0,   0,   0,                    1,  0,  0,        0, 3, 0, 0,
                                            0,   0,   0,   0,   0,                    0,  0,  0,        0, 0};

static adapt_status decode_bytes(const uint8_t *file, size_t len, adapt_limits limits, adapt_error *error) {
    uint8_t *out = NULL;
    size_t out_len = 0;
    adapt_status status = adapt_decode(file, len, limits, &out, &out_len, error);
    if (status == ADAPT_OK) {
        adapt_free(out);
    } else {
        CHECK(out == NULL);
    }
    return status;
}

/* A segmented file on hand-built bytes: the flag, the marker, the table. */
static void test_segmented_framing(void) {
    uint8_t file[64];
    adapt_error error;
    adapt_sizes sizes;
    adapt_limits limits = adapt_default_limits();
    uint64_t length;
    size_t i;
    CHECK(sizeof(EMPTY_SEGMENTED) == ADAPT_HEADER_BYTES + adapt_segment_table_len(1) + ADAPT_TRAILER_BYTES);
    CHECK_STATUS(adapt_read_header(EMPTY_SEGMENTED, sizeof(EMPTY_SEGMENTED), &sizes, &error), ADAPT_OK);
    CHECK(sizes.segmented == 1 && sizes.window_log == 16 && sizes.period == 0);
    CHECK_STATUS(decode_bytes(EMPTY_SEGMENTED, sizeof(EMPTY_SEGMENTED), limits, &error), ADAPT_OK);
    CHECK_STATUS(adapt_declared_len(EMPTY_SEGMENTED, sizeof(EMPTY_SEGMENTED), &length, &error), ADAPT_OK);
    CHECK(length == 0);
    for (i = 0; i < sizeof(EMPTY_SEGMENTED); i++) {
        CHECK_STATUS(decode_bytes(EMPTY_SEGMENTED, i, limits, &error), ADAPT_ERROR_TRUNCATED);
    }
    /* the flag without a table, a table without the flag, a marker first */
    memcpy(file, EMPTY_FILE, sizeof(EMPTY_FILE));
    file[7] |= ADAPT_SEGMENT_FLAG;
    CHECK_STATUS(decode_bytes(file, sizeof(EMPTY_FILE), limits, &error), ADAPT_ERROR_BAD_BLOCK);
    memcpy(file, EMPTY_SEGMENTED, sizeof(EMPTY_SEGMENTED));
    file[7] = 16;
    CHECK_STATUS(decode_bytes(file, sizeof(EMPTY_SEGMENTED), limits, &error), ADAPT_ERROR_BAD_BLOCK);
    memcpy(file, EMPTY_SEGMENTED, sizeof(EMPTY_SEGMENTED));
    memmove(file + 9, file + 8, sizeof(EMPTY_SEGMENTED) - 8);
    file[8] = 4;
    CHECK_STATUS(decode_bytes(file, sizeof(EMPTY_SEGMENTED) + 1, limits, &error), ADAPT_ERROR_BAD_BLOCK);
    /* a count, an entry, that do not match what was read */
    memcpy(file, EMPTY_SEGMENTED, sizeof(EMPTY_SEGMENTED));
    file[17] = 2;
    CHECK_STATUS(decode_bytes(file, sizeof(EMPTY_SEGMENTED), limits, &error), ADAPT_ERROR_BAD_BLOCK);
    memcpy(file, EMPTY_SEGMENTED, sizeof(EMPTY_SEGMENTED));
    file[9] = 1;
    CHECK_STATUS(decode_bytes(file, sizeof(EMPTY_SEGMENTED), limits, &error), ADAPT_ERROR_BAD_BLOCK);
    /* every bit of the window byte is spoken for: the transform flag on an
     * empty file is a valid, idle choice, and a window past the largest is
     * a bad header */
    memcpy(file, EMPTY_FILE, sizeof(EMPTY_FILE));
    file[7] |= ADAPT_X86_FLAG;
    CHECK_STATUS(decode_bytes(file, sizeof(EMPTY_FILE), limits, &error), ADAPT_OK);
    memcpy(file, EMPTY_FILE, sizeof(EMPTY_FILE));
    file[7] = (uint8_t)((file[7] & ~31) | 31);
    CHECK_STATUS(decode_bytes(file, sizeof(EMPTY_FILE), limits, &error), ADAPT_ERROR_BAD_HEADER);
    /* A hand-built file of seventeen one-byte segments, each one stored
     * block: the counter's table of entries grows past its first sixteen,
     * and every way the framing can contradict the table is refused. */
    {
        uint8_t built[8 + 10 + 16 * 11 + 1 + 17 * 8 + 4 + 13];
        uint8_t raw17[17];
        uint8_t *out = NULL;
        size_t out_len = 0, at = 0, k;
        uint32_t crc;
        adapt_limits lim = adapt_default_limits();
        memcpy(built, EMPTY_FILE, 8);
        built[7] |= ADAPT_SEGMENT_FLAG;
        at = 8;
        for (k = 0; k < 17; k++) {
            if (k > 0) {
                built[at++] = 4;
            }
            built[at++] = 2; /* stored, skipped */
            built[at++] = 1;
            built[at++] = 0;
            built[at++] = 0;
            built[at++] = 0;
            built[at++] = 1;
            built[at++] = 0;
            built[at++] = 0;
            built[at++] = 0;
            built[at++] = (uint8_t)('a' + k);
            raw17[k] = (uint8_t)('a' + k);
        }
        built[at++] = 5;
        for (k = 0; k < 17; k++) {
            built[at++] = 1;
            built[at++] = 0;
            built[at++] = 0;
            built[at++] = 0;
            built[at++] = k == 0 ? 10 : 11;
            built[at++] = 0;
            built[at++] = 0;
            built[at++] = 0;
        }
        built[at++] = 17;
        built[at++] = 0;
        built[at++] = 0;
        built[at++] = 0;
        built[at++] = 3;
        built[at++] = 17;
        for (k = 0; k < 7; k++) {
            built[at++] = 0;
        }
        crc = adapt_crc32(raw17, 17);
        built[at++] = (uint8_t)crc;
        built[at++] = (uint8_t)(crc >> 8);
        built[at++] = (uint8_t)(crc >> 16);
        built[at++] = (uint8_t)(crc >> 24);
        CHECK(at == sizeof(built));
        CHECK_STATUS(adapt_decode(built, sizeof(built), lim, &out, &out_len, &error), ADAPT_OK);
        CHECK(out_len == 17 && memcmp(out, raw17, 17) == 0);
        adapt_free(out);
        {
            /* the streaming decoder reads it too */
            chunked_reader src;
            sink s;
            adapt_stats stats;
            uint8_t back[17];
            src.src = built;
            src.len = sizeof(built);
            src.pos = 0;
            src.chunk = 5;
            src.fail_after = -1;
            src.reads = 0;
            s.out = back;
            s.cap = sizeof(back);
            s.len = 0;
            s.fail = 0;
            CHECK_STATUS(adapt_decode_stream(chunked_read, &src, sink_write, &s, lim, &stats, &error), ADAPT_OK);
            CHECK(s.len == 17 && stats.segments == 17 && memcmp(back, raw17, 17) == 0);
        }
        /* a coded length the entries contradict */
        built[8 + 10 + 16 * 11 + 1 + 4] = 9;
        CHECK_STATUS(decode_bytes(built, sizeof(built), lim, &error), ADAPT_ERROR_BAD_BLOCK);
        built[8 + 10 + 16 * 11 + 1 + 4] = 10;
        {
            /* a block after the table, a marker after it, a second table,
             * a marker right before the table: each an empty or misplaced
             * segment */
            uint8_t longer[sizeof(built) + 11];
            size_t table_end = sizeof(built) - 13;
            memcpy(longer, built, table_end);
            memcpy(longer + table_end, built + 8, 10);
            memcpy(longer + table_end + 10, built + table_end, 13);
            CHECK_STATUS(decode_bytes(longer, table_end + 10 + 13, lim, &error), ADAPT_ERROR_BAD_BLOCK);
            memcpy(longer, built, table_end);
            longer[table_end] = 4;
            memcpy(longer + table_end + 1, built + table_end, 13);
            CHECK_STATUS(decode_bytes(longer, table_end + 1 + 13, lim, &error), ADAPT_ERROR_BAD_BLOCK);
            memcpy(longer, built, table_end);
            longer[table_end] = 5;
            memcpy(longer + table_end + 1, built + table_end, 13);
            CHECK_STATUS(decode_bytes(longer, table_end + 1 + 13, lim, &error), ADAPT_ERROR_BAD_BLOCK);
            {
                size_t table_at = 8 + 10 + 16 * 11;
                memcpy(longer, built, table_at);
                longer[table_at] = 4;
                memcpy(longer + table_at + 1, built + table_at, sizeof(built) - table_at);
                CHECK_STATUS(decode_bytes(longer, sizeof(built) + 1, lim, &error), ADAPT_ERROR_BAD_BLOCK);
            }
        }
    }
    CHECK(adapt_segment_bytes_valid(ADAPT_SEGMENT_BYTES) && adapt_segment_bytes_valid(ADAPT_BLOCK_BYTES));
    CHECK(!adapt_segment_bytes_valid(0) && !adapt_segment_bytes_valid(1) && !adapt_segment_bytes_valid(ADAPT_BLOCK_BYTES + 1));
    CHECK(!adapt_segment_bytes_valid((uint64_t)1 << 40));
    /* a header shorter than the fixed part, and a flagged period of 0 */
    CHECK_STATUS(adapt_read_header(EMPTY_FILE, 3, &sizes, &error), ADAPT_ERROR_TRUNCATED);
    {
        uint8_t flagged[10];
        memcpy(flagged, EMPTY_FILE, 8);
        flagged[7] |= ADAPT_PERIOD_FLAG;
        flagged[8] = 0;
        flagged[9] = 0;
        CHECK_STATUS(adapt_read_header(flagged, sizeof(flagged), &sizes, &error), ADAPT_ERROR_BAD_HEADER);
        CHECK_STATUS(adapt_read_header(flagged, 9, &sizes, &error), ADAPT_ERROR_TRUNCATED);
    }
    /* the context names: a set past either menu names nothing, a buffer
     * of no room takes nothing, a short one is cut at a name */
    {
        char names[64];
        adapt_sizes s = sizes;
        s.set_code = ADAPT_CONTEXT_SETS;
        s.fast = 0;
        adapt_dropped_contexts(s, names, sizeof(names));
        CHECK(names[0] == 0);
        s.set_code = ADAPT_FAST_SETS;
        s.fast = 1;
        adapt_fast_contexts(s, names, sizeof(names));
        CHECK(strlen(names) > 0); /* a mask of nothing leaves every name in */
        names[0] = 'x';
        adapt_dropped_contexts(s, names, 0);
        adapt_fast_contexts(s, names, 0);
        CHECK(names[0] == 'x');
        s.set_code = 1;
        s.fast = 0;
        adapt_dropped_contexts(s, names, 4);
        CHECK(strlen(names) < 4);
        s.set_code = 0;
        s.fast = 1;
        adapt_fast_contexts(s, names, 4);
        CHECK(strlen(names) < 4);
    }
    /* the streaming decoder: a flagged header cut inside its extension, a
     * reader that fails inside the table, and trailing bytes past one
     * buffer of them */
    {
        chunked_reader src;
        sink s;
        adapt_stats stats;
        uint8_t back[64];
        uint8_t cut[9];
        uint8_t *padded;
        memcpy(cut, EMPTY_FILE, 8);
        cut[7] |= ADAPT_PERIOD_FLAG;
        cut[8] = 120;
        src.src = cut;
        src.len = 9;
        src.pos = 0;
        src.chunk = 64;
        src.fail_after = -1;
        src.reads = 0;
        s.out = back;
        s.cap = sizeof(back);
        s.len = 0;
        s.fail = 0;
        CHECK_STATUS(adapt_decode_stream(chunked_read, &src, sink_write, &s, adapt_default_limits(), &stats, &error),
                     ADAPT_ERROR_TRUNCATED);
        src.src = cut;
        src.len = 9;
        src.pos = 0;
        src.reads = 0;
        src.fail_after = 1;
        CHECK_STATUS(adapt_decode_stream(chunked_read, &src, sink_write, &s, adapt_default_limits(), &stats, &error),
                     ADAPT_ERROR_IO);
        src.src = EMPTY_SEGMENTED;
        src.len = sizeof(EMPTY_SEGMENTED);
        src.pos = 0;
        src.reads = 0;
        src.fail_after = 2; /* the header, the table's method byte, then no more */
        s.len = 0;
        CHECK_STATUS(adapt_decode_stream(chunked_read, &src, sink_write, &s, adapt_default_limits(), &stats, &error),
                     ADAPT_ERROR_IO);
        padded = malloc(sizeof(EMPTY_FILE) + 5000);
        memcpy(padded, EMPTY_FILE, sizeof(EMPTY_FILE));
        memset(padded + sizeof(EMPTY_FILE), 0, 5000);
        src.src = padded;
        src.len = sizeof(EMPTY_FILE) + 5000;
        src.pos = 0;
        src.reads = 0;
        src.fail_after = -1;
        src.chunk = 8192;
        s.len = 0;
        CHECK_STATUS(adapt_decode_stream(chunked_read, &src, sink_write, &s, adapt_default_limits(), &stats, &error),
                     ADAPT_ERROR_TRAILING_BYTES);
        CHECK(error.detail == 5000);
        src.pos = 0;
        src.reads = 0;
        src.fail_after = 3; /* the header, the trailer's method byte and payload, then no more */
        s.len = 0;
        CHECK_STATUS(adapt_decode_stream(chunked_read, &src, sink_write, &s, adapt_default_limits(), &stats, &error),
                     ADAPT_ERROR_IO);
        free(padded);
    }
    CHECK(adapt_segment_table_len(3) == 1 + 24 + 4);
}

static void test_framing(void) {
    uint8_t file[64];
    adapt_error error;
    adapt_sizes sizes;
    adapt_limits limits = adapt_default_limits();
    size_t i;
    uint64_t length;
    /* the empty file decodes to nothing */
    CHECK_STATUS(decode_bytes(EMPTY_FILE, sizeof(EMPTY_FILE), limits, &error), ADAPT_OK);
    CHECK_STATUS(adapt_declared_len(EMPTY_FILE, sizeof(EMPTY_FILE), &length, &error), ADAPT_OK);
    CHECK(length == 0);
    /* every truncation of it */
    for (i = 0; i < sizeof(EMPTY_FILE); i++) {
        CHECK_STATUS(decode_bytes(EMPTY_FILE, i, limits, &error), ADAPT_ERROR_TRUNCATED);
        CHECK_STATUS(adapt_declared_len(EMPTY_FILE, i, &length, &error), ADAPT_ERROR_TRUNCATED);
    }
    /* header fields */
    memcpy(file, EMPTY_FILE, sizeof(EMPTY_FILE));
    file[0] = 'X';
    CHECK_STATUS(decode_bytes(file, sizeof(EMPTY_FILE), limits, &error), ADAPT_ERROR_BAD_MAGIC);
    CHECK_STATUS(adapt_read_header(file, sizeof(file), &sizes, &error), ADAPT_ERROR_BAD_MAGIC);
    memcpy(file, EMPTY_FILE, sizeof(EMPTY_FILE));
    file[4] = 4;
    CHECK_STATUS(decode_bytes(file, sizeof(EMPTY_FILE), limits, &error), ADAPT_ERROR_UNSUPPORTED_VERSION);
    CHECK(error.detail == 4);
    file[4] = ADAPT_BMP_METADATA_LAST_VERSION + 1;
    CHECK_STATUS(decode_bytes(file, sizeof(EMPTY_FILE), limits, &error), ADAPT_ERROR_UNSUPPORTED_VERSION);
    CHECK(error.detail == ADAPT_BMP_METADATA_LAST_VERSION + 1);
    /* 0.8.0's normal profile, version 6, is refused like every version no file was published in */
    file[4] = 6;
    CHECK_STATUS(decode_bytes(file, sizeof(EMPTY_FILE), limits, &error), ADAPT_ERROR_UNSUPPORTED_VERSION);
    CHECK(error.detail == 6);
    /* the fast profile's version: read, with its profile, its context set
     * an index into the fast menu and any other refused */
    file[4] = ADAPT_FAST_VERSION;
    CHECK_STATUS(adapt_read_header(file, sizeof(file), &sizes, &error), ADAPT_OK);
    CHECK(sizes.fast == 1 && sizes.set_code == 0);
    CHECK_STATUS(decode_bytes(file, sizeof(EMPTY_FILE), limits, &error), ADAPT_OK);
    file[6] = (uint8_t)((file[6] & 31) | (ADAPT_FAST_SETS - 1) << 5);
    CHECK_STATUS(adapt_read_header(file, sizeof(file), &sizes, &error), ADAPT_OK);
    CHECK(sizes.fast == 1 && sizes.set_code == ADAPT_FAST_SETS - 1);
    CHECK_STATUS(decode_bytes(file, sizeof(EMPTY_FILE), limits, &error), ADAPT_OK);
    file[6] = (uint8_t)((file[6] & 31) | ADAPT_FAST_SETS << 5);
    CHECK_STATUS(adapt_read_header(file, sizeof(file), &sizes, &error), ADAPT_ERROR_BAD_HEADER);
    CHECK_STATUS(decode_bytes(file, sizeof(EMPTY_FILE), limits, &error), ADAPT_ERROR_BAD_HEADER);
    /* the max profile's version: read, with its profile, its context set
     * an index into the normal menu */
    memcpy(file, EMPTY_FILE, sizeof(EMPTY_FILE));
    file[4] = ADAPT_MAX_VERSION;
    CHECK_STATUS(adapt_read_header(file, sizeof(file), &sizes, &error), ADAPT_OK);
    CHECK(sizes.max == 1 && sizes.fast == 0);
    CHECK_STATUS(decode_bytes(file, sizeof(EMPTY_FILE), limits, &error), ADAPT_OK);
    CHECK(adapt_model_bytes(sizes) > adapt_model_bytes(adapt_sizes_fast(sizes)));
    sizes.fast = 1;
    CHECK(!adapt_sizes_valid(sizes));
    /* 0.7.0's fast profile, 0.8.0's, and 0.9.0's normal profile are
     * versions this reader does not implement */
    memcpy(file, EMPTY_FILE, sizeof(EMPTY_FILE));
    file[4] = 7;
    CHECK_STATUS(decode_bytes(file, sizeof(EMPTY_FILE), limits, &error), ADAPT_ERROR_UNSUPPORTED_VERSION);
    CHECK(error.detail == 7);
    file[4] = 8;
    CHECK_STATUS(decode_bytes(file, sizeof(EMPTY_FILE), limits, &error), ADAPT_ERROR_UNSUPPORTED_VERSION);
    CHECK(error.detail == 8);
    file[4] = 9;
    CHECK_STATUS(decode_bytes(file, sizeof(EMPTY_FILE), limits, &error), ADAPT_ERROR_UNSUPPORTED_VERSION);
    CHECK(error.detail == 9);
    memcpy(file, EMPTY_FILE, sizeof(EMPTY_FILE));
    CHECK_STATUS(adapt_read_header(file, sizeof(file), &sizes, &error), ADAPT_OK);
    CHECK(sizes.fast == 0);
    memcpy(file, EMPTY_FILE, sizeof(EMPTY_FILE));
    file[5] = 9; /* table_log below the minimum */
    CHECK_STATUS(decode_bytes(file, sizeof(EMPTY_FILE), limits, &error), ADAPT_ERROR_BAD_HEADER);
    file[5] = 24; /* the largest table */
    CHECK_STATUS(adapt_read_header(file, sizeof(file), &sizes, &error), ADAPT_OK);
    file[5] = (uint8_t)(20 | 2 << 5 | ADAPT_ARCHIVE_FLAG); /* an archive, class history 21 */
    CHECK_STATUS(adapt_read_header(file, sizeof(file), &sizes, &error), ADAPT_OK);
    CHECK(sizes.archive == 1 && sizes.class_code == 2 && sizes.table_log == 20);
    CHECK_STATUS(decode_bytes(file, sizeof(EMPTY_FILE), limits, &error), ADAPT_OK);
    file[5] = (uint8_t)(20 | 3 << 5 | ADAPT_ARCHIVE_FLAG); /* an unknown class code, flagged */
    CHECK_STATUS(adapt_read_header(file, sizeof(file), &sizes, &error), ADAPT_ERROR_BAD_HEADER);
    file[5] = 26; /* the largest table */
    CHECK_STATUS(adapt_read_header(file, sizeof(file), &sizes, &error), ADAPT_OK);
    file[5] = 27;
    CHECK_STATUS(adapt_read_header(file, sizeof(file), &sizes, &error), ADAPT_ERROR_BAD_HEADER);
    file[5] = 20 | 3 << 5; /* an unknown class code */
    CHECK_STATUS(adapt_read_header(file, sizeof(file), &sizes, &error), ADAPT_ERROR_BAD_HEADER);
    file[5] = 20;
    file[6] = 9;
    CHECK_STATUS(adapt_read_header(file, sizeof(file), &sizes, &error), ADAPT_ERROR_BAD_HEADER);
    file[6] = 25;
    CHECK_STATUS(adapt_read_header(file, sizeof(file), &sizes, &error), ADAPT_ERROR_BAD_HEADER);
    file[6] = 12;
    file[7] = 15;
    CHECK_STATUS(adapt_read_header(file, sizeof(file), &sizes, &error), ADAPT_ERROR_BAD_HEADER);
    file[7] = 31;
    CHECK_STATUS(adapt_read_header(file, sizeof(file), &sizes, &error), ADAPT_ERROR_BAD_HEADER);
    file[7] = 16;
    CHECK_STATUS(adapt_read_header(file, sizeof(file), &sizes, &error), ADAPT_OK);
    CHECK(sizes.table_log == 20 && sizes.match_log == 12 && sizes.window_log == 16 && sizes.class_code == 0);
    CHECK(adapt_class_width(sizes) == 0);
    sizes.class_code = 1;
    CHECK(adapt_class_width(sizes) == 8);
    sizes.class_code = 2;
    CHECK(adapt_class_width(sizes) == 21);
    sizes.class_code = 3;
    CHECK(adapt_class_width(sizes) == 0);
    /* the trailer */
    memcpy(file, EMPTY_FILE, sizeof(EMPTY_FILE));
    file[9] = 1; /* declares one byte, holds none */
    CHECK_STATUS(decode_bytes(file, sizeof(EMPTY_FILE), limits, &error), ADAPT_ERROR_CHECKSUM);
    memcpy(file, EMPTY_FILE, sizeof(EMPTY_FILE));
    file[17] = 1; /* a wrong checksum */
    CHECK_STATUS(decode_bytes(file, sizeof(EMPTY_FILE), limits, &error), ADAPT_ERROR_CHECKSUM);
    memcpy(file, EMPTY_FILE, sizeof(EMPTY_FILE));
    file[21] = 0;
    file[22] = 0;
    CHECK_STATUS(decode_bytes(file, sizeof(EMPTY_FILE) + 2, limits, &error), ADAPT_ERROR_TRAILING_BYTES);
    CHECK(error.detail == 2);
    {
        uint32_t crc;
        /* a trailer must begin with the end method */
        CHECK_STATUS(adapt_read_trailer(file + 7, &length, &crc, &error), ADAPT_ERROR_BAD_BLOCK);
    }
    /* block headers: method, raw length u32, payload length u32 */
    memcpy(file, EMPTY_FILE, 8);
    memset(file + 8, 0, 9);
    memcpy(file + 17, EMPTY_FILE + 8, 13);
    /* a stored block of zero bytes */
    CHECK_STATUS(decode_bytes(file, 30, limits, &error), ADAPT_ERROR_BAD_BLOCK);
    /* a stored block whose payload length differs from its raw length */
    file[9] = 1;
    file[13] = 2;
    CHECK_STATUS(decode_bytes(file, 30, limits, &error), ADAPT_ERROR_BAD_BLOCK);
    /* a stored block with the right lengths but no payload */
    file[13] = 1;
    CHECK_STATUS(decode_bytes(file, 17, limits, &error), ADAPT_ERROR_TRUNCATED);
    /* a modeled block whose payload is not shorter than its raw length */
    file[8] = 1;
    file[9] = 4;
    file[13] = 4;
    CHECK_STATUS(decode_bytes(file, 30, limits, &error), ADAPT_ERROR_BAD_BLOCK);
    /* a modeled block whose payload is shorter than the coder's four bytes */
    file[9] = 5;
    file[13] = 3;
    CHECK_STATUS(decode_bytes(file, 30, limits, &error), ADAPT_ERROR_BAD_BLOCK);
    /* a raw length above a block */
    file[8] = 2;
    file[9] = 1;
    file[11] = 4; /* 2^18 + 1 */
    file[13] = 1;
    file[15] = 4;
    CHECK_STATUS(decode_bytes(file, 30, limits, &error), ADAPT_ERROR_BAD_BLOCK);
    /* an unknown method, with a plausible raw length */
    file[8] = 9;
    file[11] = 0;
    file[15] = 0;
    CHECK_STATUS(decode_bytes(file, 30, limits, &error), ADAPT_ERROR_BAD_METHOD);
    CHECK(error.detail == 9);
    /* the end method where a block header should be, and a block after
     * the trailer cannot be told apart from trailing bytes: both refused */
    file[8] = 3;
    CHECK_STATUS(decode_bytes(file, 30, limits, &error), ADAPT_ERROR_TRAILING_BYTES);
    /* a stored block of one byte, complete, with a matching trailer */
    file[8] = 0;
    file[9] = 1;
    file[13] = 1;
    file[17] = 'x';
    file[18] = 3;
    memset(file + 19, 0, 8);
    file[19] = 1;
    {
        uint32_t crc = adapt_crc32((const uint8_t *)"x", 1);
        file[27] = (uint8_t)crc;
        file[28] = (uint8_t)(crc >> 8);
        file[29] = (uint8_t)(crc >> 16);
        file[30] = (uint8_t)(crc >> 24);
    }
    {
        uint8_t *out = NULL;
        size_t out_len = 0;
        CHECK_STATUS(adapt_decode(file, 31, limits, &out, &out_len, &error), ADAPT_OK);
        CHECK(out_len == 1 && out != NULL && out[0] == 'x');
        adapt_free(out);
        /* the same bytes as a skipped block: the model is never built */
        file[8] = 2;
        CHECK_STATUS(adapt_decode(file, 31, limits, &out, &out_len, &error), ADAPT_OK);
        CHECK(out_len == 1 && out != NULL && out[0] == 'x');
        adapt_free(out);
        /* a modeled block that decodes to the wrong length of stream */
        file[8] = 1;
        file[13] = 0; /* payload_len 0 < 4 */
        CHECK_STATUS(adapt_decode(file, 31, limits, &out, &out_len, &error), ADAPT_ERROR_BAD_BLOCK);
        CHECK(out == NULL);
    }
    /* a corrupted payload of a modeled block: the coder either runs past
     * the stream or recovers bytes the checksum rejects. (The last payload
     * byte is the coder's flush and may carry slack, so it is not the one
     * damaged here; the Rust battery checks every position against the
     * crate.) */
    {
        size_t abc_len;
        uint8_t *abc = read_file("abc.adpt", &abc_len);
        uint8_t *out = NULL;
        size_t out_len = 0;
        adapt_status status;
        CHECK(abc[8] == 1);
        abc[ADAPT_HEADER_BYTES + ADAPT_BLOCK_HEADER_BYTES + 2] ^= 0xFF;
        status = adapt_decode(abc, abc_len, limits, &out, &out_len, &error);
        CHECK(status == ADAPT_ERROR_BAD_BLOCK || status == ADAPT_ERROR_CHECKSUM);
        CHECK(out == NULL);
        free(abc);
    }
}

/* ---- limits ------------------------------------------------------------ */

static void test_limits(void) {
    size_t file_len;
    uint8_t *file = read_file("abc.adpt", &file_len);
    adapt_error error;
    adapt_sizes sizes;
    adapt_limits limits = adapt_default_limits();
    uint64_t bound;
    CHECK(limits.max_output == ((uint64_t)1 << 30));
    CHECK(limits.max_memory == ADAPT_DEFAULT_MAX_MEMORY && limits.max_memory == ((uint64_t)674 << 20));
    CHECK_STATUS(adapt_read_header(file, sizeof(file), &sizes, &error), ADAPT_OK);
    bound = adapt_decode_memory_bound(sizes);
    CHECK(bound > adapt_model_bytes(sizes));
    CHECK(adapt_model_bytes(sizes) > ((uint64_t)1 << sizes.table_log) * 16 + ((uint64_t)1 << sizes.window_log));
    /* the output limit: the declared length is refused before allocating */
    limits.max_output = 899;
    CHECK_STATUS(decode_bytes(file, file_len, limits, &error), ADAPT_ERROR_TOO_LARGE);
    CHECK(error.detail == 900 && error.limit == 899);
    limits.max_output = 900;
    CHECK_STATUS(decode_bytes(file, file_len, limits, &error), ADAPT_OK);
    /* the memory budget: the bound plus the result, in memory */
    limits.max_memory = bound + 900;
    CHECK_STATUS(decode_bytes(file, file_len, limits, &error), ADAPT_OK);
    limits.max_memory = bound + 899;
    CHECK_STATUS(decode_bytes(file, file_len, limits, &error), ADAPT_ERROR_MEMORY_LIMIT);
    CHECK(error.detail == bound + 900 && error.limit == bound + 899);
    /* streamed, the bound alone */
    {
        chunked_reader reader;
        uint8_t buffer[1024];
        sink s;
        s.out = buffer;
        s.cap = sizeof(buffer);
        s.len = 0;
        s.fail = 0;
        reader.src = file;
        reader.len = file_len;
        reader.pos = 0;
        reader.chunk = 1000;
        reader.fail_after = -1;
        reader.reads = 0;
        limits.max_memory = bound;
        CHECK_STATUS(adapt_decode_stream(chunked_read, &reader, sink_write, &s, limits, NULL, &error), ADAPT_OK);
        reader.pos = 0;
        s.len = 0;
        limits.max_memory = bound - 1;
        CHECK_STATUS(adapt_decode_stream(chunked_read, &reader, sink_write, &s, limits, NULL, &error),
                     ADAPT_ERROR_MEMORY_LIMIT);
        CHECK(error.detail == bound && error.limit == bound - 1);
        /* the output limit as the blocks accumulate */
        reader.pos = 0;
        s.len = 0;
        limits.max_memory = bound;
        limits.max_output = 899;
        CHECK_STATUS(adapt_decode_stream(chunked_read, &reader, sink_write, &s, limits, NULL, &error),
                     ADAPT_ERROR_TOO_LARGE);
        CHECK(error.detail == 900 && error.limit == 899);
    }
    /* skipped blocks still allocate the model alongside the result */
    {
        uint8_t skipped[31];
        uint32_t crc = adapt_crc32((const uint8_t *)"x", 1);
        memcpy(skipped, EMPTY_FILE, 8);
        memset(skipped + 8, 0, 23);
        skipped[8] = 2;
        skipped[9] = 1;
        skipped[13] = 1;
        skipped[17] = 'x';
        skipped[18] = 3;
        skipped[19] = 1;
        skipped[27] = (uint8_t)crc;
        skipped[28] = (uint8_t)(crc >> 8);
        skipped[29] = (uint8_t)(crc >> 16);
        skipped[30] = (uint8_t)(crc >> 24);
        limits = adapt_default_limits();
        CHECK_STATUS(adapt_read_header(skipped, sizeof(skipped), &sizes, &error), ADAPT_OK);
        limits.max_memory = adapt_decode_memory_bound(sizes);
        CHECK_STATUS(decode_bytes(skipped, sizeof(skipped), limits, &error), ADAPT_ERROR_MEMORY_LIMIT);
        CHECK(error.detail == adapt_decode_memory_bound(sizes) + 1);
        limits.max_memory++;
        CHECK_STATUS(decode_bytes(skipped, sizeof(skipped), limits, &error), ADAPT_OK);
    }
    free(file);
}

/* ---- arguments and messages ------------------------------------------- */

static void test_arguments_and_messages(void) {
    uint8_t *out = (uint8_t *)1;
    size_t out_len = 1;
    uint64_t length;
    uint32_t crc;
    adapt_sizes sizes;
    adapt_error error;
    char message[160];
    struct {
        adapt_status status;
        uint64_t detail;
        uint64_t limit;
        const char *text;
    } cases[] = {
        {ADAPT_OK, 0, 0, "no error"},
        {ADAPT_ERROR_MEMORY_LIMIT, 2, 1, "memory budget exceeded: need 2 bytes, limit 1"},
        {ADAPT_ERROR_ALLOCATION, 0, 0, "memory allocation failed"},
        {ADAPT_ERROR_BAD_MAGIC, 0, 0, "not an adapt file (bad signature)"},
        {ADAPT_ERROR_UNSUPPORTED_VERSION, 9, 0, "unsupported format version 9"},
        {ADAPT_ERROR_BAD_HEADER, 0, 0, "invalid header field"},
        {ADAPT_ERROR_TOO_LARGE, 5, 4, "declared size 5 exceeds the limit 4"},
        {ADAPT_ERROR_TRUNCATED, 0, 0, "file is truncated"},
        {ADAPT_ERROR_BAD_BLOCK, 0, 0, "invalid block"},
        {ADAPT_ERROR_BAD_METHOD, 7, 0, "unknown block method 7"},
        {ADAPT_ERROR_TRAILING_BYTES, 2, 0, "2 unexpected trailing byte(s)"},
        {ADAPT_ERROR_CHECKSUM, 0, 0, "checksum mismatch: the recovered bytes are damaged"},
        {ADAPT_ERROR_IO, 5, 0, "i/o error: callback status 5"},
        {ADAPT_ERROR_INVALID_ARGUMENT, 0, 0, "invalid argument"},
        {ADAPT_ERROR_BAD_ARCHIVE, 42, 0, "invalid archive at byte 42"},
    };
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        error.status = cases[i].status;
        error.detail = cases[i].detail;
        error.limit = cases[i].limit;
        adapt_error_message(&error, message, sizeof(message));
        CHECK(strcmp(message, cases[i].text) == 0);
        /* a truncated buffer is still terminated, and the length is reported */
        CHECK(adapt_error_message(&error, message, 4) == (int)strlen(cases[i].text));
        CHECK(strlen(message) == 3);
    }
    error.status = (adapt_status)99;
    adapt_error_message(&error, message, sizeof(message));
    CHECK(strcmp(message, "unknown error 99") == 0);
    adapt_error_message(NULL, message, sizeof(message));
    CHECK(strcmp(message, "no error") == 0);
    /* NULL pointers are refused, and an error pointer may be NULL */
    CHECK_STATUS(adapt_decode(NULL, 5, adapt_default_limits(), &out, &out_len, &error), ADAPT_ERROR_INVALID_ARGUMENT);
    CHECK_STATUS(adapt_decode(EMPTY_FILE, 21, adapt_default_limits(), NULL, &out_len, NULL), ADAPT_ERROR_INVALID_ARGUMENT);
    CHECK_STATUS(adapt_decode(EMPTY_FILE, 21, adapt_default_limits(), &out, NULL, NULL), ADAPT_ERROR_INVALID_ARGUMENT);
    CHECK_STATUS(adapt_decode(NULL, 0, adapt_default_limits(), &out, &out_len, NULL), ADAPT_ERROR_TRUNCATED);
    CHECK(out == NULL && out_len == 0);
    CHECK_STATUS(adapt_declared_len(NULL, 5, &length, NULL), ADAPT_ERROR_INVALID_ARGUMENT);
    CHECK_STATUS(adapt_declared_len(EMPTY_FILE, 21, NULL, NULL), ADAPT_ERROR_INVALID_ARGUMENT);
    CHECK_STATUS(adapt_declared_len(NULL, 0, &length, NULL), ADAPT_ERROR_TRUNCATED);
    CHECK_STATUS(adapt_read_header(NULL, 0, &sizes, NULL), ADAPT_ERROR_INVALID_ARGUMENT);
    CHECK_STATUS(adapt_read_header(EMPTY_FILE, sizeof(EMPTY_FILE), NULL, NULL), ADAPT_ERROR_INVALID_ARGUMENT);
    CHECK_STATUS(adapt_read_trailer(NULL, &length, &crc, NULL), ADAPT_ERROR_INVALID_ARGUMENT);
    CHECK_STATUS(adapt_read_trailer(EMPTY_FILE + 8, NULL, &crc, NULL), ADAPT_ERROR_INVALID_ARGUMENT);
    CHECK_STATUS(adapt_read_trailer(EMPTY_FILE + 8, &length, NULL, NULL), ADAPT_ERROR_INVALID_ARGUMENT);
    CHECK_STATUS(adapt_read_trailer(EMPTY_FILE + 8, &length, &crc, NULL), ADAPT_OK);
    CHECK(length == 0 && crc == 0);
    adapt_free(NULL);
}

/* ---- the checksum ------------------------------------------------------ */

/* The x86 flag on hand-built bytes: a stored block holds the transformed
 * bytes, and both decoders return the original — a small call rewritten to
 * a block-relative address, a far call and a trailing opcode left alone —
 * with the checksum over the original. */
static void test_x86_flag(void) {
    static const uint8_t original[13] = {0x90, 0xE8, 0x01, 0x00, 0x00, 0x00, 0x90, 0xE8, 0x12, 0x34, 0x56, 0x78, 0xE9};
    static const uint8_t stored[13] = {0x90, 0xE8, 0x07, 0x00, 0x00, 0x00, 0x90, 0xE8, 0x12, 0x34, 0x56, 0x78, 0xE9};
    uint8_t file[8 + 9 + 13 + 13];
    uint8_t *out = NULL;
    size_t out_len = 0, i;
    uint32_t crc = adapt_crc32(original, sizeof(original));
    adapt_sizes sizes, plain;
    adapt_error error;
    adapt_limits limits = adapt_default_limits();
    memcpy(file, EMPTY_FILE, 8);
    file[7] |= ADAPT_X86_FLAG;
    memset(file + 8, 0, sizeof(file) - 8);
    file[8] = 2;
    file[9] = 13;
    file[13] = 13;
    memcpy(file + 17, stored, 13);
    file[30] = 3;
    file[31] = 13;
    for (i = 0; i < 4; i++) {
        file[39 + i] = (uint8_t)(crc >> (8 * i));
    }
    CHECK_STATUS(adapt_read_header(file, sizeof(file), &sizes, &error), ADAPT_OK);
    CHECK(sizes.x86 == 1 && sizes.window_log == 16 && adapt_sizes_valid(sizes));
    plain = sizes;
    plain.x86 = 0;
    CHECK(adapt_decode_memory_bound(sizes) == adapt_decode_memory_bound(plain));
    CHECK_STATUS(adapt_decode(file, sizeof(file), limits, &out, &out_len, &error), ADAPT_OK);
    CHECK(out_len == 13 && memcmp(out, original, 13) == 0);
    adapt_free(out);
    {
        chunked_reader src;
        sink s;
        uint8_t back[64];
        adapt_stats stats;
        src.src = file;
        src.len = sizeof(file);
        src.pos = 0;
        src.reads = 0;
        src.fail_after = -1;
        src.chunk = 5;
        s.out = back;
        s.cap = sizeof(back);
        s.len = 0;
        s.fail = 0;
        CHECK_STATUS(adapt_decode_stream(chunked_read, &src, sink_write, &s, limits, &stats, &error), ADAPT_OK);
        CHECK(s.len == 13 && memcmp(back, original, 13) == 0 && stats.crc32 == crc);
    }
    /* without the flag the stored bytes are the output, and the checksum
     * of the original no longer matches them */
    file[7] = (uint8_t)(file[7] & ~ADAPT_X86_FLAG);
    CHECK_STATUS(decode_bytes(file, sizeof(file), limits, &error), ADAPT_ERROR_CHECKSUM);
}

static void test_crc32(void) {
    CHECK(adapt_crc32((const uint8_t *)"", 0) == 0);
    CHECK(adapt_crc32((const uint8_t *)"a", 1) == 0xE8B7BE43u);
    CHECK(adapt_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u);
    CHECK(adapt_crc32((const uint8_t *)"The quick brown fox jumps over the lazy dog", 43) == 0x414FA339u);
}

static void metadata_checksum(uint8_t *header, size_t len) {
    uint32_t crc = adapt_crc32(header, len - 4);
    size_t i;
    for (i = 0; i < 4; i++) {
        header[len - 4 + i] = (uint8_t)(crc >> (8 * i));
    }
}

static void test_metadata_headers(void) {
    adapt_metadata m = {0751, -123, 456, 789, 123456789}, back;
    uint8_t header[ADAPT_HEADER_MAX_BYTES], damaged[ADAPT_HEADER_MAX_BYTES];
    uint8_t file[ADAPT_HEADER_MAX_BYTES + sizeof(EMPTY_FILE)];
    adapt_sizes sizes;
    adapt_error error;
    size_t n, i;
    uint64_t length;
    CHECK(!adapt_header_has_metadata(NULL));
    n = adapt_header_add_metadata(EMPTY_FILE, ADAPT_HEADER_BYTES, &m, header, sizeof(header));
    CHECK(n == ADAPT_HEADER_BYTES + ADAPT_METADATA_HEADER_BYTES);
    CHECK(header[4] == ADAPT_METADATA_VERSION && adapt_header_has_metadata(header));
    CHECK(adapt_header_len(header) == n);
    CHECK_STATUS(adapt_read_header(header, n, &sizes, &error), ADAPT_OK);
    CHECK_STATUS(adapt_read_metadata(header, n, &back, &error), ADAPT_OK);
    CHECK(back.mode == m.mode && back.atime_seconds == m.atime_seconds && back.atime_nanoseconds == 456);
    CHECK(back.mtime_seconds == m.mtime_seconds && back.mtime_nanoseconds == m.mtime_nanoseconds);
    CHECK_STATUS(adapt_read_metadata(header, n, NULL, &error), ADAPT_ERROR_INVALID_ARGUMENT);
    CHECK_STATUS(adapt_read_metadata(NULL, 0, &back, &error), ADAPT_ERROR_INVALID_ARGUMENT);
    CHECK_STATUS(adapt_read_metadata(EMPTY_FILE, sizeof(EMPTY_FILE), &back, &error), ADAPT_ERROR_BAD_HEADER);
    CHECK(adapt_header_add_metadata(NULL, 0, &m, header, sizeof(header)) == 0);
    CHECK(adapt_header_add_metadata(EMPTY_FILE, ADAPT_HEADER_BYTES, &m, NULL, 0) == 0);
    CHECK(adapt_header_add_metadata(EMPTY_FILE, ADAPT_HEADER_BYTES, &m, damaged, n - 1) == 0);
    CHECK(adapt_header_add_metadata(EMPTY_FILE, sizeof(EMPTY_FILE), &m, damaged, sizeof(damaged)) == 0);
    CHECK(adapt_header_add_metadata(header, n, &m, damaged, sizeof(damaged)) == 0);
    CHECK(adapt_header_add_metadata(EMPTY_FILE, ADAPT_HEADER_BYTES, NULL, damaged, sizeof(damaged)) == 0);
    for (i = 0; i < n; i++) {
        CHECK_STATUS(adapt_read_metadata(header, i, &back, &error), ADAPT_ERROR_TRUNCATED);
        memcpy(damaged, header, n);
        damaged[i] ^= 1;
        CHECK(adapt_read_header(damaged, n, &sizes, &error) != ADAPT_OK);
    }
    for (i = ADAPT_HEADER_BYTES + 3; i < n - 4; i += 12) {
        memcpy(damaged, header, n);
        damaged[i] = 255;
        metadata_checksum(damaged, n);
        CHECK_STATUS(adapt_read_header(damaged, n, &sizes, &error), ADAPT_ERROR_BAD_HEADER);
    }
    memcpy(file, header, n);
    memcpy(file + n, EMPTY_FILE + ADAPT_HEADER_BYTES, sizeof(EMPTY_FILE) - ADAPT_HEADER_BYTES);
    CHECK_STATUS(decode_bytes(file, n + sizeof(EMPTY_FILE) - ADAPT_HEADER_BYTES, adapt_default_limits(), &error), ADAPT_OK);
    CHECK_STATUS(adapt_declared_len(file, n + sizeof(EMPTY_FILE) - ADAPT_HEADER_BYTES, &length, &error), ADAPT_OK);
    CHECK(length == 0);
    CHECK_STATUS(adapt_declared_len(file, n + ADAPT_TRAILER_BYTES - 1, &length, &error), ADAPT_ERROR_TRUNCATED);
    /* Exercise all predictors, optional period, and segment accounting with
     * an extended header; the underlying golden payloads stay unchanged. */
    {
        static const char *names[] = {"hello.adpt", "abc-fast.adpt", "abc-max.adpt", "records.adpt", "segmented.adpt"};
        size_t k;
        for (k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
            size_t old_len, old_header, out_len, expected_len;
            uint8_t *old = read_file(names[k], &old_len), *encoded, *out = NULL, *expected = NULL;
            old_header = adapt_header_len(old);
            n = adapt_header_add_metadata(old, old_header, &m, header, sizeof(header));
            CHECK(n == old_header + ADAPT_METADATA_HEADER_BYTES);
            encoded = malloc(old_len + ADAPT_METADATA_HEADER_BYTES);
            CHECK(encoded != NULL);
            memcpy(encoded, header, n);
            memcpy(encoded + n, old + old_header, old_len - old_header);
            CHECK_STATUS(adapt_decode(old, old_len, adapt_default_limits(), &expected, &expected_len, &error), ADAPT_OK);
            CHECK_STATUS(adapt_decode(encoded, old_len + ADAPT_METADATA_HEADER_BYTES, adapt_default_limits(), &out, &out_len, &error), ADAPT_OK);
            CHECK(out_len == expected_len && memcmp(out, expected, expected_len) == 0);
            CHECK_STATUS(adapt_declared_len(encoded, old_len + ADAPT_METADATA_HEADER_BYTES, &length, &error), ADAPT_OK);
            CHECK(length == expected_len);
            adapt_free(expected);
            adapt_free(out);
            free(encoded);
            free(old);
        }
    }
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: test_adapt_decode <vectors-dir>\n");
        return 2;
    }
    vectors_dir = argv[1];
    test_crc32();
    test_metadata_headers();
    test_vector("empty.adpt", "empty.bin");
    test_vector("hello.adpt", "hello.bin");
    test_vector("abc.adpt", "abc.bin");
    test_vector("abc-fast.adpt", "abc.bin");
    test_vector("abc-max.adpt", "abc.bin");
    test_vector("text.adpt", "text.bin");
    test_vector("text-fast.adpt", "text.bin");
    test_vector("text-max.adpt", "text.bin");
    test_vector("records.adpt", "records.bin");
    test_vector("segmented.adpt", "segmented.bin");
    test_vector("cyrillic.adpt", "cyrillic.bin");
    test_vector("cjk.adpt", "cjk.bin");
    test_stream_failures();
    test_framing();
    test_segmented_framing();
    test_limits();
    test_x86_flag();
    test_arguments_and_messages();
    if (failures != 0) {
        fprintf(stderr, "%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("%d checks passed\n", checks);
    return 0;
}
