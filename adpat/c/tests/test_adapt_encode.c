/*
 * test_adapt_encode.c — the C encoder's own tests, needing no Rust
 * toolchain: the committed vectors written byte for byte, the sizes a
 * length and a budget choose, round trips through the C decoder over every
 * block kind, and the proof's every failure. The battery that proves it
 * against the Rust encoder is `../../tests/c_codec.rs`.
 *
 * Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-12.
 *
 *   test_adapt_encode <vectors-dir>
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

/* Bytes without repeated 8-byte sequences: xorshift64. */
static void noise(uint64_t seed, uint8_t *out, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        out[i] = (uint8_t)(seed >> 24);
    }
}

/* ---- the committed vectors ------------------------------------------- */

static void test_vector(const char *name, const char *source) {
    size_t file_len, want_len, out_len = 1;
    uint8_t *want = read_file(name, &file_len);
    uint8_t *data = read_file(source, &want_len);
    uint8_t *out = (uint8_t *)1;
    adapt_error error;
    CHECK_STATUS(adapt_encode(data, want_len, adapt_default_limits(), &out, &out_len, &error), ADAPT_OK);
    CHECK(error.status == ADAPT_OK);
    CHECK(out_len == file_len);
    CHECK(out != NULL && out_len == file_len && memcmp(out, want, file_len) == 0);
    adapt_free(out);
    CHECK_STATUS(adapt_encode_verified(data, want_len, adapt_default_limits(), &out, &out_len, &error), ADAPT_OK);
    CHECK(out != NULL && out_len == file_len && memcmp(out, want, file_len) == 0);
    adapt_free(out);
    free(want);
    free(data);
}

/* ---- sizes ------------------------------------------------------------- */

static void test_sizes(void) {
    adapt_sizes s;
    adapt_error error;
    uint64_t floor_bound, two;
    /* the vectors' header: a short known length under the default budget */
    CHECK_STATUS(adapt_sizes_for_input(1, 900, ADAPT_DEFAULT_MAX_MEMORY, &s, &error), ADAPT_OK);
    CHECK(s.table_log == 18 && s.match_log == 12 && s.window_log == 16 && s.class_code == 0);
    CHECK(adapt_sizes_valid(s));
    CHECK_STATUS(adapt_sizes_for_input(1, 0, ADAPT_DEFAULT_MAX_MEMORY, &s, &error), ADAPT_OK);
    CHECK(s.table_log == 18 && s.match_log == 12 && s.window_log == 16);
    /* tables grow with the length up to their sweet spot, the window to the length */
    CHECK_STATUS(adapt_sizes_for_input(1, 1u << 20, ADAPT_DEFAULT_MAX_MEMORY, &s, &error), ADAPT_OK);
    CHECK(s.table_log == 20 && s.match_log == 20 && s.window_log == 20);
    CHECK_STATUS(adapt_sizes_for_input(1, (1u << 20) + 1, ADAPT_DEFAULT_MAX_MEMORY, &s, &error), ADAPT_OK);
    CHECK(s.window_log == 21);
    /* a pipe: the largest tables, and a window for two models when two fit */
    CHECK_STATUS(adapt_sizes_for_input(0, 0, ADAPT_DEFAULT_MAX_MEMORY, &s, &error), ADAPT_OK);
    CHECK(s.table_log == 20 && s.match_log == 20);
    CHECK(adapt_verified_memory_bound(s) <= ADAPT_DEFAULT_MAX_MEMORY);
    two = adapt_verified_memory_bound(s);
    CHECK_STATUS(adapt_sizes_for_input(0, 0, (uint64_t)1 << 40, &s, &error), ADAPT_OK);
    CHECK(s.window_log == 30 && adapt_verified_memory_bound(s) > two);
    /* the budget shrinks the tables, largest first, then refuses */
    s.table_log = 10;
    s.match_log = 10;
    s.window_log = 16;
    s.class_code = 0;
    floor_bound = adapt_encode_memory_bound(s);
    CHECK(floor_bound == adapt_decode_memory_bound(s));
    CHECK_STATUS(adapt_sizes_for_input(1, 1u << 30, floor_bound, &s, &error), ADAPT_OK);
    CHECK(s.table_log == 10 && s.match_log == 10 && s.window_log == 16);
    CHECK_STATUS(adapt_sizes_for_input(1, 1u << 30, floor_bound - 1, &s, &error), ADAPT_ERROR_MEMORY_LIMIT);
    CHECK(error.detail == floor_bound && error.limit == floor_bound - 1);
    CHECK_STATUS(adapt_sizes_for_input(0, 0, floor_bound - 1, &s, &error), ADAPT_ERROR_MEMORY_LIMIT);
    CHECK_STATUS(adapt_sizes_for_input(1, 5, floor_bound, NULL, &error), ADAPT_ERROR_INVALID_ARGUMENT);
    CHECK_STATUS(adapt_sizes_for_input(1, 1u << 20, 20u << 20, &s, &error), ADAPT_OK);
    CHECK(adapt_encode_memory_bound(s) <= 20u << 20);
    CHECK(s.table_log < 20 || s.match_log < 20);
    /* every field of the sizes has a range, and each is checked on its own */
    {
        adapt_sizes good, bad;
        CHECK_STATUS(adapt_sizes_for_input(1, 1u << 20, ADAPT_DEFAULT_MAX_MEMORY, &good, &error), ADAPT_OK);
        CHECK(adapt_sizes_valid(good));
        bad = good;
        bad.table_log = 9;
        CHECK(!adapt_sizes_valid(bad));
        bad = good;
        bad.table_log = 24;
        CHECK(adapt_sizes_valid(bad));
        bad.table_log = 25;
        CHECK(!adapt_sizes_valid(bad));
        bad = good;
        bad.match_log = 9;
        CHECK(!adapt_sizes_valid(bad));
        bad = good;
        bad.match_log = 25;
        CHECK(!adapt_sizes_valid(bad));
        bad = good;
        bad.window_log = 15;
        CHECK(!adapt_sizes_valid(bad));
        bad = good;
        bad.window_log = 31;
        CHECK(!adapt_sizes_valid(bad));
        bad = good;
        bad.class_code = 3;
        CHECK(!adapt_sizes_valid(bad));
        bad = good;
        bad.set_code = ADAPT_CONTEXT_SETS;
        CHECK(!adapt_sizes_valid(bad));
        bad = good;
        bad.fast = 2;
        CHECK(!adapt_sizes_valid(bad));
        bad = adapt_sizes_fast(good);
        bad.set_code = ADAPT_FAST_SETS;
        CHECK(!adapt_sizes_valid(bad));
        bad = good;
        bad.period = 1;
        CHECK(!adapt_sizes_valid(bad));
        bad = good;
        bad.period = ADAPT_PERIOD_MAX + 1;
        CHECK(!adapt_sizes_valid(bad));
        bad = adapt_sizes_fast(good);
        bad.period = 120;
        CHECK(!adapt_sizes_valid(bad));
        bad = good;
        bad.segmented = 2;
        CHECK(!adapt_sizes_valid(bad));
        bad = good;
        bad.max = 2;
        CHECK(!adapt_sizes_valid(bad));
        bad = adapt_sizes_max(good);
        bad.fast = 1;
        CHECK(!adapt_sizes_valid(bad));
        bad = good;
        bad.period = 120;
        bad.segmented = 1;
        bad.max = 1;
        CHECK(adapt_sizes_valid(bad));
    }
    /* the table past its sweet spot: a block per eight input bytes from
     * 8 MiB, the largest from 16 MiB, a window at the input's length, a
     * budget that holds it */
    CHECK(adapt_table_log_for(0) == 18 && adapt_table_log_for(1u << 14) == 20);
    CHECK(adapt_table_log_for(4u << 20) == 20 && adapt_table_log_for(8u << 20) == 21);
    CHECK(adapt_table_log_for((16u << 20) - 1) == 21 && adapt_table_log_for(16u << 20) == 22);
    CHECK(adapt_table_log_for((32u << 20) - 1) == 22 && adapt_table_log_for(32u << 20) == 23);
    CHECK(adapt_table_log_for(64u << 20) == 24 && adapt_table_log_for(100000000) == 24);
    CHECK(adapt_table_log_for((uint64_t)1 << 40) == 24);
    CHECK_STATUS(adapt_sizes_for_input(1, 100000000, ADAPT_DEFAULT_MAX_MEMORY, &s, &error), ADAPT_OK);
    CHECK(s.table_log == 20 && s.match_log == 20 && s.window_log == 24);
    CHECK_STATUS(adapt_sizes_for_input(1, 100000000, (uint64_t)256 << 20, &s, &error), ADAPT_OK);
    CHECK(s.table_log == 22 && s.match_log == 20 && s.window_log == 27);
    CHECK(adapt_encode_memory_bound(s) <= (uint64_t)256 << 20);
    s.table_log = 21;
    two = adapt_encode_memory_bound(s);
    CHECK_STATUS(adapt_sizes_for_input(1, 100000000, two, &s, &error), ADAPT_OK);
    CHECK(s.table_log == 21);
    CHECK_STATUS(adapt_sizes_for_input(1, 100000000, two - 1, &s, &error), ADAPT_OK);
    CHECK(s.table_log == 20);
    CHECK_STATUS(adapt_sizes_for_input(1, 40u << 20, (uint64_t)1 << 30, &s, &error), ADAPT_OK);
    CHECK(s.table_log == 23 && s.window_log == 26);
    CHECK_STATUS(adapt_sizes_for_input(1, 16u << 20, (uint64_t)1 << 30, &s, &error), ADAPT_OK);
    CHECK(s.table_log == 22);
    CHECK_STATUS(adapt_sizes_for_input(1, 8u << 20, (uint64_t)1 << 30, &s, &error), ADAPT_OK);
    CHECK(s.table_log == 21);
    CHECK_STATUS(adapt_sizes_for_input(1, 4u << 20, (uint64_t)1 << 30, &s, &error), ADAPT_OK);
    CHECK(s.table_log == 20);
    CHECK_STATUS(adapt_sizes_for_input(0, 0, (uint64_t)1 << 30, &s, &error), ADAPT_OK);
    CHECK(s.table_log == 20);
    CHECK(adapt_sizes_fast(adapt_sizes_for_input(1, 100000000, (uint64_t)256 << 20, &s, &error) == ADAPT_OK ? s : s).table_log ==
          ADAPT_FAST_TABLE_LOG_MAX);
}

/* ---- round trips ------------------------------------------------------- */

static void roundtrip(const uint8_t *data, size_t n, uint8_t expect_method, int verified) {
    uint8_t *file = NULL, *back = NULL;
    size_t file_len = 0, back_len = 0;
    adapt_error error;
    adapt_status status = verified ? adapt_encode_verified(data, n, adapt_default_limits(), &file, &file_len, &error)
                                   : adapt_encode(data, n, adapt_default_limits(), &file, &file_len, &error);
    CHECK_STATUS(status, ADAPT_OK);
    CHECK(file != NULL && file_len >= ADAPT_HEADER_BYTES + ADAPT_TRAILER_BYTES);
    if (file != NULL && n > 0) {
        CHECK(file[ADAPT_HEADER_BYTES] == expect_method);
    }
    CHECK(file_len <= n + ADAPT_HEADER_BYTES + ADAPT_TRAILER_BYTES + ADAPT_BLOCK_HEADER_BYTES * (n / ADAPT_BLOCK_BYTES + 1));
    CHECK_STATUS(adapt_decode(file, file_len, adapt_default_limits(), &back, &back_len, &error), ADAPT_OK);
    CHECK(back_len == n && (n == 0 || memcmp(back, data, n) == 0));
    adapt_free(file);
    adapt_free(back);
}

static void test_roundtrips(void) {
    size_t n = 3 * ADAPT_BLOCK_BYTES + 777;
    uint8_t *buf = malloc(n);
    size_t i;
    /* nothing, one byte, a short text: stored, observed */
    roundtrip(NULL, 0, 0, 1);
    buf[0] = 0x80;
    roundtrip(buf, 1, 0, 1);
    memcpy(buf, "hello, world\n", 13);
    roundtrip(buf, 13, 0, 0);
    /* a repeated phrase: modeled, several blocks */
    for (i = 0; i < n; i++) {
        buf[i] = "the rain in spain stays mainly in the plain "[i % 44];
    }
    roundtrip(buf, n, 1, 1);
    roundtrip(buf, n, 1, 0);
    /* noise: stored, skipped, and the trailer still right */
    noise(7, buf, n);
    roundtrip(buf, n, 2, 1);
    /* sparse repetition does not justify training an otherwise random block */
    memcpy(buf + 128, buf, 64);
    roundtrip(buf, ADAPT_BLOCK_BYTES, 2, 1);
    noise(7, buf, n);
    /* noise, then a repeat of it: recalled across the skipped block */
    memcpy(buf + ADAPT_BLOCK_BYTES, buf, ADAPT_BLOCK_BYTES);
    roundtrip(buf, 2 * ADAPT_BLOCK_BYTES, 2, 1);
    /* a table the widest class history reads, and letters that decline it */
    {
        uint64_t x = 0x2545F4914F6CDD1Dull;
        size_t k = 0;
        uint8_t *file = NULL;
        size_t file_len = 0;
        adapt_error error;
        while (k + 21 <= 200000) {
            for (i = 0; i < 16; i++) {
                x ^= x << 13;
                x ^= x >> 7;
                x ^= x << 17;
                buf[k++] = (uint8_t)('a' + x % 26);
            }
            for (i = 0; i < 4; i++) {
                x ^= x << 13;
                x ^= x >> 7;
                x ^= x << 17;
                buf[k++] = (uint8_t)('0' + x % 10);
            }
            buf[k++] = '\n';
        }
        roundtrip(buf, k, 1, 1);
        CHECK_STATUS(adapt_encode(buf, k, adapt_default_limits(), &file, &file_len, &error), ADAPT_OK);
        CHECK(file != NULL && (file[5] >> 5) == 2);
        adapt_free(file);
        for (i = 0; i < 65536; i++) {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            buf[i] = (uint8_t)('a' + (x >> 24) % 26);
        }
        roundtrip(buf, 65536, 1, 1);
        CHECK_STATUS(adapt_encode(buf, 65536, adapt_default_limits(), &file, &file_len, &error), ADAPT_OK);
        CHECK(file != NULL && (file[5] >> 5) == 0);
        adapt_free(file);
    }
    free(buf);
}

/* ---- streams and the proof --------------------------------------------- */

typedef struct reader {
    const uint8_t *src;
    size_t len;
    size_t pos;
    size_t chunk;
    int fail_after;
    int reads;
} reader;

static int reader_read(void *context, uint8_t *buf, size_t len, size_t *got) {
    reader *r = context;
    size_t n;
    if (r->fail_after >= 0 && r->reads >= r->fail_after) {
        return 9;
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

typedef struct writer {
    uint8_t *out;
    size_t cap;
    size_t len;
    int fail_after;
    int writes;
} writer;

static int writer_write(void *context, const uint8_t *buf, size_t len) {
    writer *w = context;
    if (w->fail_after >= 0 && w->writes >= w->fail_after) {
        return -4;
    }
    w->writes++;
    if (len > w->cap - w->len) {
        return 1;
    }
    memcpy(w->out + w->len, buf, len);
    w->len += len;
    return 0;
}

static void reset(reader *r, const uint8_t *src, size_t len, size_t chunk, writer *w, uint8_t *out, size_t cap) {
    r->src = src;
    r->len = len;
    r->pos = 0;
    r->chunk = chunk;
    r->fail_after = -1;
    r->reads = 0;
    w->out = out;
    w->cap = cap;
    w->len = 0;
    w->fail_after = -1;
    w->writes = 0;
}

static void test_streams(void) {
    size_t n = 40000, chunk;
    uint8_t *data = malloc(n);
    uint8_t *file = malloc(n + 64);
    uint8_t *reference = NULL;
    size_t reference_len = 0, i;
    reader r, source;
    writer w;
    adapt_sizes sizes;
    adapt_stats stats;
    adapt_error error;
    for (i = 0; i < n; i++) {
        data[i] = "streams are the same code over slices; "[i % 39];
    }
    CHECK_STATUS(adapt_encode(data, n, adapt_default_limits(), &reference, &reference_len, &error), ADAPT_OK);
    CHECK_STATUS(adapt_sizes_for_input(1, n, ADAPT_DEFAULT_MAX_MEMORY, &sizes, &error), ADAPT_OK);
    /* every chunk size a pipe might deliver, proven and not */
    for (chunk = 1; chunk <= n + 1; chunk = chunk < 7 ? chunk + 1 : chunk * 5) {
        reset(&r, data, n, chunk, &w, file, n + 64);
        CHECK_STATUS(adapt_encode_stream(reader_read, &r, writer_write, &w, sizes, ADAPT_DEFAULT_MAX_MEMORY, &stats, &error),
                     ADAPT_OK);
        CHECK(w.len == reference_len && memcmp(file, reference, reference_len) == 0);
        CHECK(stats.input_bytes == n && stats.output_bytes == reference_len);
        CHECK(stats.crc32 == adapt_crc32(data, n));
        reset(&r, data, n, chunk, &w, file, n + 64);
        CHECK_STATUS(adapt_encode_stream_verified(reader_read, &r, writer_write, &w, sizes, ADAPT_DEFAULT_MAX_MEMORY, &stats,
                                                  &error),
                     ADAPT_OK);
        CHECK(w.len == reference_len && memcmp(file, reference, reference_len) == 0);
        CHECK(stats.input_bytes == n);
    }
    /* the proof after the fact: the source read again beside the file */
    reset(&source, data, n, 1000, &w, file, n + 64);
    reset(&r, reference, reference_len, 7, &w, file, n + 64);
    CHECK_STATUS(adapt_verify_stream(reader_read, &source, reader_read, &r, ADAPT_DEFAULT_MAX_MEMORY, &stats, &error),
                 ADAPT_OK);
    CHECK(stats.input_bytes == n && stats.output_bytes == reference_len);
    /* a source that differs, ends early, or runs long: no cause */
    data[n / 2] ^= 1;
    reset(&source, data, n, 1000, &w, file, n + 64);
    reset(&r, reference, reference_len, 1000, &w, file, n + 64);
    CHECK_STATUS(adapt_verify_stream(reader_read, &source, reader_read, &r, ADAPT_DEFAULT_MAX_MEMORY, NULL, &error),
                 ADAPT_ERROR_VERIFICATION);
    CHECK(error.cause == ADAPT_OK);
    data[n / 2] ^= 1;
    reset(&source, data, n - 1, 1000, &w, file, n + 64);
    reset(&r, reference, reference_len, 1000, &w, file, n + 64);
    CHECK_STATUS(adapt_verify_stream(reader_read, &source, reader_read, &r, ADAPT_DEFAULT_MAX_MEMORY, NULL, &error),
                 ADAPT_ERROR_VERIFICATION);
    CHECK(error.cause == ADAPT_OK);
    memcpy(file, data, n);
    file[n] = '!';
    reset(&source, file, n + 1, 1000, &w, file, 0);
    reset(&r, reference, reference_len, 1000, &w, file, 0);
    CHECK_STATUS(adapt_verify_stream(reader_read, &source, reader_read, &r, ADAPT_DEFAULT_MAX_MEMORY, NULL, &error),
                 ADAPT_ERROR_VERIFICATION);
    CHECK(error.cause == ADAPT_OK);
    /* a damaged file: the decoding error is the cause */
    memcpy(file, reference, reference_len);
    file[reference_len - 1] ^= 0xFF;
    reset(&source, data, n, 1000, &w, file, 0);
    reset(&r, file, reference_len, 1000, &w, file, 0);
    CHECK_STATUS(adapt_verify_stream(reader_read, &source, reader_read, &r, ADAPT_DEFAULT_MAX_MEMORY, NULL, &error),
                 ADAPT_ERROR_VERIFICATION);
    CHECK(error.cause == ADAPT_ERROR_CHECKSUM);
    reset(&source, data, n, 1000, &w, file, 0);
    reset(&r, reference, reference_len - 1, 1000, &w, file, 0);
    CHECK_STATUS(adapt_verify_stream(reader_read, &source, reader_read, &r, ADAPT_DEFAULT_MAX_MEMORY, NULL, &error),
                 ADAPT_ERROR_VERIFICATION);
    CHECK(error.cause == ADAPT_ERROR_TRUNCATED);
    /* a source that cannot be read: its failure is the cause */
    reset(&source, data, n, 1000, &w, file, 0);
    source.fail_after = 0;
    reset(&r, reference, reference_len, 1000, &w, file, 0);
    CHECK_STATUS(adapt_verify_stream(reader_read, &source, reader_read, &r, ADAPT_DEFAULT_MAX_MEMORY, NULL, &error),
                 ADAPT_ERROR_VERIFICATION);
    CHECK(error.cause == ADAPT_ERROR_IO && error.detail == 9);
    /* a budget below the model: refused, as a cause */
    reset(&source, data, n, 1000, &w, file, 0);
    reset(&r, reference, reference_len, 1000, &w, file, 0);
    CHECK_STATUS(adapt_verify_stream(reader_read, &source, reader_read, &r, 1, NULL, &error), ADAPT_ERROR_VERIFICATION);
    CHECK(error.cause == ADAPT_ERROR_MEMORY_LIMIT && error.limit == 1);
    /* the proof alongside checks the bytes the encoder built, not what the
     * writer kept — a writer that corrupts its store is the after-the-fact
     * proof's to catch, above — and needs two models in the budget */
    reset(&r, data, n, 1000, &w, file, n + 64);
    CHECK_STATUS(adapt_encode_stream_verified(reader_read, &r, writer_write, &w, sizes,
                                              adapt_verified_memory_bound(sizes) - 1, NULL, &error),
                 ADAPT_ERROR_MEMORY_LIMIT);
    CHECK(error.detail == adapt_verified_memory_bound(sizes));
    /* while the plain encoder needs one, and refuses invalid sizes */
    reset(&r, data, n, 1000, &w, file, n + 64);
    CHECK_STATUS(adapt_encode_stream(reader_read, &r, writer_write, &w, sizes, adapt_encode_memory_bound(sizes) - 1, NULL,
                                     &error),
                 ADAPT_ERROR_MEMORY_LIMIT);
    sizes.class_code = 3;
    CHECK(!adapt_sizes_valid(sizes));
    CHECK_STATUS(adapt_encode_stream(reader_read, &r, writer_write, &w, sizes, ADAPT_DEFAULT_MAX_MEMORY, NULL, &error),
                 ADAPT_ERROR_BAD_HEADER);
    CHECK_STATUS(adapt_encode_stream_verified(reader_read, &r, writer_write, &w, sizes, ADAPT_DEFAULT_MAX_MEMORY, NULL,
                                              &error),
                 ADAPT_ERROR_BAD_HEADER);
    sizes.class_code = 0;
    /* callbacks that fail carry their code */
    reset(&r, data, n, 1000, &w, file, n + 64);
    r.fail_after = 1;
    CHECK_STATUS(adapt_encode_stream(reader_read, &r, writer_write, &w, sizes, ADAPT_DEFAULT_MAX_MEMORY, NULL, &error),
                 ADAPT_ERROR_IO);
    CHECK(error.detail == 9);
    for (i = 0; i < 3; i++) {
        reset(&r, data, n, 1000, &w, file, n + 64);
        w.fail_after = (int)i;
        CHECK_STATUS(adapt_encode_stream(reader_read, &r, writer_write, &w, sizes, ADAPT_DEFAULT_MAX_MEMORY, NULL, &error),
                     ADAPT_ERROR_IO);
        CHECK(error.detail == 4);
    }
    CHECK_STATUS(adapt_encode_stream(NULL, &r, writer_write, &w, sizes, ADAPT_DEFAULT_MAX_MEMORY, NULL, &error),
                 ADAPT_ERROR_INVALID_ARGUMENT);
    CHECK_STATUS(adapt_encode_stream_verified(reader_read, &r, NULL, &w, sizes, ADAPT_DEFAULT_MAX_MEMORY, NULL, NULL),
                 ADAPT_ERROR_INVALID_ARGUMENT);
    CHECK_STATUS(adapt_verify_stream(NULL, &r, reader_read, &r, ADAPT_DEFAULT_MAX_MEMORY, NULL, NULL),
                 ADAPT_ERROR_INVALID_ARGUMENT);
    adapt_free(reference);
    free(data);
    free(file);
}

/* ---- the proof alongside, over several blocks --------------------------
 * Five blocks through the proof that runs beside the encoder: the ring of
 * blocks it holds fills, wraps and drains, and the file is the one the
 * plain encoder writes; then a writer that fails while blocks are still
 * queued for proof, so the encoder returns while the proof has work. */

static void test_verified_blocks(void) {
    size_t n = 4 * ADAPT_BLOCK_BYTES + 12345, i, k;
    uint8_t *data = malloc(n);
    uint8_t *file = malloc(n + 64);
    uint8_t *reference = malloc(n + 64);
    reader r;
    writer w;
    adapt_sizes sizes;
    adapt_stats stats;
    adapt_error error;
    for (i = 0; i < n; i++) {
        data[i] = "blocks proven behind the coder, in order, on another thread; "[i % 61];
    }
    uint64_t budget;
    CHECK_STATUS(adapt_sizes_for_input(1, n, ADAPT_DEFAULT_MAX_MEMORY, &sizes, &error), ADAPT_OK);
    /* an input this long needs more than the default budget for two models */
    budget = adapt_verified_memory_bound(sizes);
    reset(&r, data, n, 65536, &w, reference, n + 64);
    CHECK_STATUS(adapt_encode_stream(reader_read, &r, writer_write, &w, sizes, budget, &stats, &error), ADAPT_OK);
    k = w.len;
    reset(&r, data, n, 65536, &w, file, n + 64);
    CHECK_STATUS(adapt_encode_stream_verified(reader_read, &r, writer_write, &w, sizes, budget, &stats, &error), ADAPT_OK);
    CHECK(w.len == k && memcmp(file, reference, k) == 0);
    CHECK(stats.input_bytes == n && stats.output_bytes == k);
    /* the writer fails on the fourth block: two proofs may be pending */
    for (i = 2; i <= 4; i++) {
        reset(&r, data, n, 65536, &w, file, n + 64);
        w.fail_after = (int)i;
        CHECK_STATUS(adapt_encode_stream_verified(reader_read, &r, writer_write, &w, sizes, budget, NULL, &error),
                     ADAPT_ERROR_IO);
        CHECK(error.detail == 4);
    }
    free(data);
    free(file);
    free(reference);
}

/* ---- the fast profile ---------------------------------------------------
 * Format version 8 through the stream encoder: the committed vector the
 * crate wrote with --fast, then prose under both profiles — the fast file
 * names a set of the fast menu, differs from the normal one past the
 * header, and both decode to the source; the version byte alone tells
 * them apart. adapt_sizes_fast caps the table as the command line does. */

/* ---- the record period ---------------------------------------------------
 * The detector on a raster the crate's test draws (a pixel stride of
 * three, a scanline of 120 bytes) and on prose; the period after the
 * header, read back, and refused on the fast profile. */

static void test_record_period(void) {
    size_t width = 40, rows = 300, n = width * rows * 3, x, y, c, out_len, i;
    uint8_t *image = malloc(n);
    uint8_t *prose = malloc(40000);
    uint8_t *coded = malloc(n + 64);
    uint8_t *out = NULL;
    reader r;
    writer w;
    adapt_sizes sizes, read;
    adapt_error error;
    for (y = 0; y < rows; y++) {
        for (x = 0; x < width; x++) {
            uint32_t h = ((uint32_t)(x / 5) * 31 + (uint32_t)(y / 4) * 17) & 0xFF;
            for (c = 0; c < 3; c++) {
                image[(y * width + x) * 3 + c] = (uint8_t)((h + c * 85) & 0xFF);
            }
        }
    }
    for (i = 0; i < 40000; i++) {
        prose[i] = "the rain in spain stays mainly in the plain; "[i % 45];
    }
    CHECK(adapt_detect_period(image, n) == 120);
    CHECK(adapt_detect_period(prose, 40000) == 0);
    CHECK(adapt_detect_period(image, 4000) == 0);
    CHECK_STATUS(adapt_sizes_for_input(1, n, ADAPT_DEFAULT_MAX_MEMORY, &sizes, &error), ADAPT_OK);
    reset(&r, image, n, 4096, &w, coded, n + 64);
    CHECK_STATUS(adapt_encode_stream(reader_read, &r, writer_write, &w, sizes, ADAPT_DEFAULT_MAX_MEMORY, NULL, &error),
                 ADAPT_OK);
    CHECK(coded[4] == ADAPT_FORMAT_VERSION && (coded[7] & ADAPT_PERIOD_FLAG) != 0);
    CHECK(coded[8] == 120 && coded[9] == 0);
    CHECK(adapt_header_len(coded) == ADAPT_HEADER_BYTES + ADAPT_PERIOD_BYTES);
    CHECK_STATUS(adapt_read_header(coded, w.len, &read, &error), ADAPT_OK);
    CHECK(read.period == 120 && read.window_log == sizes.window_log);
    CHECK(adapt_sizes_header_bytes(read) == ADAPT_HEADER_BYTES + ADAPT_PERIOD_BYTES);
    CHECK_STATUS(adapt_read_header(coded, ADAPT_HEADER_BYTES + 1, &read, &error), ADAPT_ERROR_TRUNCATED);
    CHECK_STATUS(adapt_decode(coded, w.len, adapt_default_limits(), &out, &out_len, &error), ADAPT_OK);
    CHECK(out_len == n && memcmp(out, image, n) == 0);
    adapt_free(out);
    coded[8] = 1;
    coded[9] = 0;
    CHECK_STATUS(adapt_read_header(coded, w.len, &read, &error), ADAPT_ERROR_BAD_HEADER);
    sizes = adapt_sizes_fast(sizes);
    sizes.period = 120;
    CHECK(!adapt_sizes_valid(sizes));
    free(image);
    free(prose);
    free(coded);
}

/* Segments: every segment starts a fresh model, the table names them, both
 * decoders read them, and the crate's thread sizes are computed here. */
static void test_segments(void) {
    size_t n = 4 * ADAPT_BLOCK_BYTES + 123, i, out_len, first_coded, k;
    uint8_t *data = malloc(n);
    uint8_t *coded = malloc(n + 256);
    uint8_t *again = malloc(n + 256);
    uint8_t *out = NULL;
    reader r;
    writer w;
    adapt_sizes sizes, read, threads;
    adapt_stats stats;
    adapt_error error;
    size_t table_at, table_len = adapt_segment_table_len(3);
    reset(&r, data, n, 4096, &w, coded, n + 256);
    for (i = 0; i < n; i++) {
        data[i] = "seed 6 line: the rain in spain stays mainly in the plain; "[(i * 7 + i / 977) % 58];
    }
    CHECK_STATUS(adapt_sizes_for_input(1, n, ADAPT_DEFAULT_MAX_MEMORY, &sizes, &error), ADAPT_OK);
    CHECK(sizes.segmented == 0);
    sizes.segmented = 1;
    /* a segment length must be whole blocks */
    reset(&r, data, n, 4096, &w, coded, n + 256);
    CHECK_STATUS(adapt_encode_stream_segments(reader_read, &r, writer_write, &w, sizes, ADAPT_DEFAULT_MAX_MEMORY, 100,
                                              &stats, &error),
                 ADAPT_ERROR_BAD_HEADER);
    reset(&r, data, n, 4096, &w, coded, n + 256);
    CHECK_STATUS(adapt_encode_stream_segments(reader_read, &r, writer_write, &w, sizes, ADAPT_DEFAULT_MAX_MEMORY,
                                              2 * ADAPT_BLOCK_BYTES, &stats, &error),
                 ADAPT_OK);
    CHECK(stats.segments == 3 && stats.output_bytes == w.len && stats.input_bytes == n);
    CHECK(coded[4] == ADAPT_FORMAT_VERSION && (coded[7] & ADAPT_SEGMENT_FLAG) != 0);
    CHECK_STATUS(adapt_read_header(coded, w.len, &read, &error), ADAPT_OK);
    CHECK(read.segmented == 1 && read.window_log == sizes.window_log);
    /* the table before the trailer: three segments of the lengths cut */
    table_at = w.len - ADAPT_TRAILER_BYTES - table_len;
    CHECK(coded[table_at] == 5);
    CHECK(coded[table_at + 1] == 0 && coded[table_at + 2] == 0 && coded[table_at + 3] == 8 && coded[table_at + 4] == 0);
    CHECK(coded[table_at + 17] == 123 && coded[table_at + 18] == 0);
    CHECK(coded[table_at + 25] == 3 && coded[table_at + 26] == 0);
    first_coded = (size_t)coded[table_at + 5] | (size_t)coded[table_at + 6] << 8 | (size_t)coded[table_at + 7] << 16;
    CHECK(coded[adapt_sizes_header_bytes(read) + first_coded] == 4);
    /* both decoders read it */
    CHECK_STATUS(adapt_decode(coded, w.len, adapt_default_limits(), &out, &out_len, &error), ADAPT_OK);
    CHECK(out_len == n && memcmp(out, data, n) == 0);
    adapt_free(out);
    {
        reader cr;
        writer cw;
        reset(&cr, coded, w.len, 777, &cw, again, n + 256);
        CHECK_STATUS(adapt_decode_stream(reader_read, &cr, writer_write, &cw, adapt_default_limits(), &stats, &error),
                     ADAPT_OK);
        CHECK(cw.len == n && memcmp(again, data, n) == 0);
        CHECK(stats.segments == 3 && stats.output_bytes == w.len);
    }
    /* the proof alongside writes the same bytes */
    k = w.len;
    memcpy(again, coded, k);
    reset(&r, data, n, 4096, &w, coded, n + 256);
    /* under a budget that holds two models */
    CHECK_STATUS(adapt_encode_stream_verified_segments(reader_read, &r, writer_write, &w, sizes, (uint64_t)256 << 20,
                                                       2 * ADAPT_BLOCK_BYTES, &stats, &error),
                 ADAPT_OK);
    CHECK(w.len == k && memcmp(coded, again, k) == 0 && stats.segments == 3);
    /* a table that does not add up, and a marker where none may be */
    coded[w.len - ADAPT_TRAILER_BYTES - 1] ^= 1;
    CHECK_STATUS(adapt_decode(coded, w.len, adapt_default_limits(), &out, &out_len, &error), ADAPT_ERROR_BAD_BLOCK);
    coded[w.len - ADAPT_TRAILER_BYTES - 1] ^= 1;
    coded[table_at + 1] ^= 1;
    CHECK_STATUS(adapt_decode(coded, w.len, adapt_default_limits(), &out, &out_len, &error), ADAPT_ERROR_BAD_BLOCK);
    coded[table_at + 1] ^= 1;
    memmove(coded + adapt_sizes_header_bytes(read) + 1, coded + adapt_sizes_header_bytes(read), w.len - adapt_sizes_header_bytes(read));
    coded[adapt_sizes_header_bytes(read)] = 4;
    CHECK_STATUS(adapt_decode(coded, w.len + 1, adapt_default_limits(), &out, &out_len, &error), ADAPT_ERROR_BAD_BLOCK);
    /* the crate's sizes for its threads: a segment's model, one thread's
     * share of the budget, and a refusal below the floor */
    CHECK_STATUS(adapt_sizes_for_threads((uint64_t)1 << 30, (uint64_t)1 << 30, 4, ADAPT_SEGMENT_BYTES, &threads, &error),
                 ADAPT_OK);
    CHECK(threads.segmented == 1 && threads.window_log == 24 && threads.table_log == 22);
    CHECK(adapt_threads_encode_memory_bound(threads, 4, ADAPT_SEGMENT_BYTES) <= ((uint64_t)1 << 30));
    CHECK_STATUS(adapt_sizes_for_threads((uint64_t)1 << 30, (uint64_t)32 << 20, 4, ADAPT_SEGMENT_BYTES, &threads, &error),
                 ADAPT_ERROR_MEMORY_LIMIT);
    CHECK_STATUS(adapt_sizes_for_threads(10, (uint64_t)1 << 30, 0, ADAPT_SEGMENT_BYTES, &threads, &error),
                 ADAPT_ERROR_BAD_HEADER);
    CHECK_STATUS(adapt_sizes_for_threads(10, (uint64_t)1 << 30, 2, 7, &threads, &error), ADAPT_ERROR_BAD_HEADER);
    CHECK(adapt_segment_out_bound(ADAPT_SEGMENT_BYTES) == 1 + ADAPT_SEGMENT_BYTES + 64 * ADAPT_BLOCK_HEADER_BYTES);
    free(data);
    free(coded);
    free(again);
}

/* The max profile: the vector, a round trip, and its sizes. */
static void test_max_profile(void) {
    size_t want_len, src_len, n = 60000, i, out_len;
    uint8_t *want = read_file("abc-max.adpt", &want_len);
    uint8_t *src = read_file("abc.bin", &src_len);
    uint8_t *data = malloc(n);
    uint8_t *normal = malloc(n + 64);
    uint8_t *coded = malloc(n + 64);
    uint8_t *out = NULL;
    reader r;
    writer w;
    adapt_sizes sizes, plain;
    adapt_error error;
    CHECK_STATUS(adapt_sizes_for_input_max(1, src_len, ADAPT_DEFAULT_MAX_MEMORY, &sizes, &error), ADAPT_OK);
    CHECK(sizes.max == 1 && sizes.fast == 0);
    sizes = adapt_sizes_max(sizes);
    CHECK(sizes.max == 1 && sizes.fast == 0 && adapt_sizes_valid(sizes));
    reset(&r, src, src_len, 1000, &w, coded, n + 64);
    CHECK_STATUS(adapt_encode_stream(reader_read, &r, writer_write, &w, sizes, ADAPT_DEFAULT_MAX_MEMORY, NULL, &error),
                 ADAPT_OK);
    CHECK(w.len == want_len && memcmp(coded, want, want_len) == 0);
    CHECK(coded[4] == ADAPT_MAX_VERSION);
    reset(&r, src, src_len, 1000, &w, coded, n + 64);
    CHECK_STATUS(adapt_encode_stream_verified(reader_read, &r, writer_write, &w, sizes, ADAPT_DEFAULT_MAX_MEMORY, NULL,
                                              &error),
                 ADAPT_OK);
    CHECK(w.len == want_len && memcmp(coded, want, want_len) == 0);
    for (i = 0; i < n; i++) {
        data[i] = "the max profile keys its first arbiter by the whole previous byte and estimates twice; "[i % 87];
    }
    CHECK_STATUS(adapt_sizes_for_input(1, n, ADAPT_DEFAULT_MAX_MEMORY, &plain, &error), ADAPT_OK);
    reset(&r, data, n, 4096, &w, normal, n + 64);
    CHECK_STATUS(adapt_encode_stream(reader_read, &r, writer_write, &w, plain, ADAPT_DEFAULT_MAX_MEMORY, NULL, &error),
                 ADAPT_OK);
    CHECK(normal[4] == ADAPT_FORMAT_VERSION);
    sizes = adapt_sizes_max(plain);
    reset(&r, data, n, 4096, &w, coded, n + 64);
    CHECK_STATUS(adapt_encode_stream(reader_read, &r, writer_write, &w, sizes, ADAPT_DEFAULT_MAX_MEMORY, NULL, &error),
                 ADAPT_OK);
    CHECK(coded[4] == ADAPT_MAX_VERSION && w.len < n / 4);
    CHECK(memcmp(coded + ADAPT_HEADER_BYTES, normal + ADAPT_HEADER_BYTES, 32) != 0);
    CHECK_STATUS(adapt_decode(coded, w.len, adapt_default_limits(), &out, &out_len, &error), ADAPT_OK);
    CHECK(out_len == n && memcmp(out, data, n) == 0);
    adapt_free(out);
    /* the max model is larger — its arbiters' lanes and its four more run
     * memories, under 4 MiB — and a budget fitted for it counts that */
    CHECK(adapt_model_bytes(sizes) > adapt_model_bytes(plain));
    CHECK(adapt_model_bytes(sizes) < adapt_model_bytes(plain) + ((uint64_t)4 << 20));
    {
        adapt_sizes big;
        CHECK_STATUS(adapt_sizes_for_input(1, (uint64_t)1 << 30, (uint64_t)1 << 30, &big, &error), ADAPT_OK);
        CHECK_STATUS(adapt_sizes_for_input_max(1, (uint64_t)1 << 30, adapt_encode_memory_bound(big), &sizes, &error), ADAPT_OK);
        CHECK(sizes.max == 1 && adapt_encode_memory_bound(sizes) <= adapt_encode_memory_bound(big));
        CHECK(sizes.table_log < big.table_log || sizes.window_log < big.window_log);
    }
    CHECK_STATUS(adapt_sizes_for_threads_max((uint64_t)1 << 30, (uint64_t)1 << 30, 4, ADAPT_SEGMENT_BYTES, &sizes, &error),
                 ADAPT_OK);
    CHECK(sizes.max == 1 && sizes.segmented == 1);
    /* never both profiles */
    sizes = adapt_sizes_fast(plain);
    sizes.max = 1;
    CHECK(!adapt_sizes_valid(sizes));
    reset(&r, data, n, 4096, &w, coded, n + 64);
    CHECK_STATUS(adapt_encode_stream(reader_read, &r, writer_write, &w, sizes, ADAPT_DEFAULT_MAX_MEMORY, NULL, &error),
                 ADAPT_ERROR_BAD_HEADER);
    free(want);
    free(src);
    free(data);
    free(normal);
    free(coded);
}

/* Branches the round trips do not reach: the detector on noise and on a
 * bare stride, a block of noise that repeats, the lean-set proof on a
 * deep block, write failures at a marker and at the table, and the
 * argument checks. */
static void test_edges(void) {
    size_t n = 4 * ADAPT_BLOCK_BYTES, i, k, out_len;
    uint8_t *noise = malloc(n);
    uint8_t *text = malloc(n);
    uint8_t *coded = malloc(n + 4096);
    uint8_t *out = NULL;
    uint64_t x = 0x2545F4914F6CDD1Dull;
    reader r;
    writer w;
    adapt_sizes sizes;
    adapt_stats stats;
    adapt_error error;
    for (i = 0; i < n; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        noise[i] = (uint8_t)(x >> 24);
    }
    /* noise has no stride; alternating bytes have a stride that every
     * multiple repeats alike, which is no period */
    CHECK(adapt_detect_period(noise, 40000) == 0);
    for (i = 0; i < 40000; i++) {
        text[i] = (uint8_t)(i & 1);
    }
    CHECK(adapt_detect_period(text, 40000) == 0);
    /* the fast profile's table cap leaves a table under it alone: the
     * smallest model, under the budget that holds exactly it */
    sizes.table_log = 10;
    sizes.match_log = 10;
    sizes.window_log = 16;
    sizes.class_code = 0;
    sizes.set_code = 0;
    sizes.fast = 0;
    sizes.period = 0;
    sizes.segmented = 0;
    sizes.max = 0;
    CHECK_STATUS(adapt_sizes_for_input(1, 900, adapt_encode_memory_bound(sizes), &sizes, &error), ADAPT_OK);
    CHECK(sizes.table_log < ADAPT_FAST_TABLE_LOG_MAX && adapt_sizes_fast(sizes).table_log == sizes.table_log);
    /* a first block of noise with a 64-byte motif copied through it: high
     * entropy, but repeats enough to be modeled, and modeled it does not
     * pay, so it is stored observed */
    for (k = 1; k < 40; k++) {
        memcpy(noise + k * 4096, noise, 64);
    }
    CHECK_STATUS(adapt_sizes_for_input(1, ADAPT_BLOCK_BYTES, ADAPT_DEFAULT_MAX_MEMORY, &sizes, &error), ADAPT_OK);
    reset(&r, noise, ADAPT_BLOCK_BYTES, 4096, &w, coded, n + 4096);
    CHECK_STATUS(adapt_encode_stream(reader_read, &r, writer_write, &w, sizes, ADAPT_DEFAULT_MAX_MEMORY, &stats, &error),
                 ADAPT_OK);
    /* modeled — the repeats pass the scan — and then stored when the
     * coded form is no smaller, or kept when it just is */
    CHECK((coded[adapt_sizes_header_bytes(sizes)] == 0 || coded[adapt_sizes_header_bytes(sizes)] == 1) &&
          stats.input_bytes == ADAPT_BLOCK_BYTES);
    CHECK_STATUS(adapt_decode(coded, w.len, adapt_default_limits(), &out, &out_len, &error), ADAPT_OK);
    CHECK(out_len == ADAPT_BLOCK_BYTES && memcmp(out, noise, out_len) == 0);
    adapt_free(out);
    /* a deep first block of prose: the lean set the sample chooses is
     * proved on the block, under both profiles */
    for (i = 0; i < n; i++) {
        text[i] = "It was the best of times, it was the worst of times, it was the age of wisdom, "[(i * 3 + i / 1013) % 80];
    }
    CHECK_STATUS(adapt_sizes_for_input(1, n, ADAPT_DEFAULT_MAX_MEMORY, &sizes, &error), ADAPT_OK);
    reset(&r, text, n, 65536, &w, coded, n + 4096);
    CHECK_STATUS(adapt_encode_stream(reader_read, &r, writer_write, &w, sizes, ADAPT_DEFAULT_MAX_MEMORY, &stats, &error),
                 ADAPT_OK);
    CHECK(w.len < n / 8);
    CHECK_STATUS(adapt_decode(coded, w.len, adapt_default_limits(), &out, &out_len, &error), ADAPT_OK);
    CHECK(out_len == n && memcmp(out, text, n) == 0);
    adapt_free(out);
    sizes = adapt_sizes_fast(sizes);
    reset(&r, text, n, 65536, &w, coded, n + 4096);
    CHECK_STATUS(adapt_encode_stream(reader_read, &r, writer_write, &w, sizes, ADAPT_DEFAULT_MAX_MEMORY, &stats, &error),
                 ADAPT_OK);
    CHECK(coded[4] == ADAPT_FAST_VERSION && w.len < n / 8);
    /* a write that fails at the marker, and one at the table */
    CHECK_STATUS(adapt_sizes_for_input(1, n, ADAPT_DEFAULT_MAX_MEMORY, &sizes, &error), ADAPT_OK);
    sizes.segmented = 1;
    reset(&r, text, n, 65536, &w, coded, n + 4096);
    w.fail_after = 2; /* the header, the first block, then the marker */
    CHECK_STATUS(adapt_encode_stream_segments(reader_read, &r, writer_write, &w, sizes, ADAPT_DEFAULT_MAX_MEMORY,
                                              ADAPT_BLOCK_BYTES, &stats, &error),
                 ADAPT_ERROR_IO);
    reset(&r, text, n, 65536, &w, coded, n + 4096);
    w.fail_after = 8; /* the header, four blocks and three markers, then the table */
    CHECK_STATUS(adapt_encode_stream_segments(reader_read, &r, writer_write, &w, sizes, ADAPT_DEFAULT_MAX_MEMORY,
                                              ADAPT_BLOCK_BYTES, &stats, &error),
                 ADAPT_ERROR_IO);
    /* the argument checks of the entry points */
    CHECK_STATUS(adapt_encode_stream_segments(NULL, &r, writer_write, &w, sizes, ADAPT_DEFAULT_MAX_MEMORY, ADAPT_BLOCK_BYTES,
                                              &stats, &error),
                 ADAPT_ERROR_INVALID_ARGUMENT);
    CHECK_STATUS(adapt_encode_stream_verified_segments(reader_read, &r, NULL, &w, sizes, ADAPT_DEFAULT_MAX_MEMORY,
                                                       ADAPT_BLOCK_BYTES, &stats, &error),
                 ADAPT_ERROR_INVALID_ARGUMENT);
    CHECK_STATUS(adapt_verify_stream(NULL, &r, reader_read, &r, ADAPT_DEFAULT_MAX_MEMORY, NULL, &error),
                 ADAPT_ERROR_INVALID_ARGUMENT);
    CHECK_STATUS(adapt_sizes_for_threads(10, ADAPT_DEFAULT_MAX_MEMORY, 1, ADAPT_BLOCK_BYTES, NULL, &error),
                 ADAPT_ERROR_INVALID_ARGUMENT);
    free(noise);
    free(text);
    free(coded);
}

static void test_fast_profile(void) {
    size_t want_len, src_len, n = 60000, i, out_len;
    uint8_t *want = read_file("abc-fast.adpt", &want_len);
    uint8_t *src = read_file("abc.bin", &src_len);
    uint8_t *data = malloc(n);
    uint8_t *normal = malloc(n + 64);
    uint8_t *fast = malloc(n + 64);
    uint8_t *out = NULL;
    reader r;
    writer w;
    adapt_sizes sizes;
    adapt_error error;
    CHECK_STATUS(adapt_sizes_for_input(1, src_len, ADAPT_DEFAULT_MAX_MEMORY, &sizes, &error), ADAPT_OK);
    CHECK(sizes.fast == 0);
    sizes = adapt_sizes_fast(sizes);
    CHECK(sizes.fast == 1 && sizes.table_log <= ADAPT_FAST_TABLE_LOG_MAX && sizes.set_code == 0);
    reset(&r, src, src_len, 1000, &w, fast, n + 64);
    CHECK_STATUS(adapt_encode_stream(reader_read, &r, writer_write, &w, sizes, ADAPT_DEFAULT_MAX_MEMORY, NULL, &error),
                 ADAPT_OK);
    CHECK(w.len == want_len && memcmp(fast, want, want_len) == 0);
    reset(&r, src, src_len, 1000, &w, fast, n + 64);
    CHECK_STATUS(adapt_encode_stream_verified(reader_read, &r, writer_write, &w, sizes, ADAPT_DEFAULT_MAX_MEMORY, NULL,
                                              &error),
                 ADAPT_OK);
    CHECK(w.len == want_len && memcmp(fast, want, want_len) == 0);
    for (i = 0; i < n; i++) {
        data[i] = "the fast profile keeps two contexts, the word run, recall and one arbiter; "[i % 75];
    }
    CHECK_STATUS(adapt_sizes_for_input(1, n, ADAPT_DEFAULT_MAX_MEMORY, &sizes, &error), ADAPT_OK);
    reset(&r, data, n, 4096, &w, normal, n + 64);
    CHECK_STATUS(adapt_encode_stream(reader_read, &r, writer_write, &w, sizes, ADAPT_DEFAULT_MAX_MEMORY, NULL, &error),
                 ADAPT_OK);
    CHECK(normal[4] == ADAPT_FORMAT_VERSION);
    sizes = adapt_sizes_fast(sizes);
    reset(&r, data, n, 4096, &w, fast, n + 64);
    CHECK_STATUS(adapt_encode_stream(reader_read, &r, writer_write, &w, sizes, ADAPT_DEFAULT_MAX_MEMORY, NULL, &error),
                 ADAPT_OK);
    CHECK(fast[4] == ADAPT_FAST_VERSION && (fast[6] >> 5) < ADAPT_FAST_SETS);
    CHECK(w.len < n / 4);
    CHECK(memcmp(fast + ADAPT_HEADER_BYTES, normal + ADAPT_HEADER_BYTES, 32) != 0);
    CHECK_STATUS(adapt_decode(fast, w.len, adapt_default_limits(), &out, &out_len, &error), ADAPT_OK);
    CHECK(out_len == n && memcmp(out, data, n) == 0);
    adapt_free(out);
    /* under the other profile's version byte the same stream is another
     * file: refused, or other bytes */
    fast[4] = ADAPT_FORMAT_VERSION;
    if (adapt_decode(fast, w.len, adapt_default_limits(), &out, &out_len, &error) == ADAPT_OK) {
        CHECK(out_len != n || memcmp(out, data, n) != 0);
        adapt_free(out);
    }
    fast[4] = ADAPT_FAST_VERSION;
    fast[6] = (uint8_t)((fast[6] & 31) | ADAPT_FAST_SETS << 5);
    CHECK_STATUS(adapt_decode(fast, w.len, adapt_default_limits(), &out, &out_len, &error), ADAPT_ERROR_BAD_HEADER);
    sizes.fast = 2;
    CHECK(!adapt_sizes_valid(sizes));
    free(want);
    free(src);
    free(data);
    free(normal);
    free(fast);
}

/* ---- in memory: arguments, budgets, the proof after the fact ----------- */

/* The x86 branch transform: chosen by the first block's signature, flagged
 * in the header, undone by every decoder and by the proof alongside; each
 * operand shape the transform decides on is in the input. */
static void test_x86_transform(void) {
    size_t n = 2 * ADAPT_BLOCK_BYTES + 4321, i, out_len, at;
    uint8_t *data = malloc(n);
    uint8_t *coded = malloc(n + 256);
    uint8_t *out = NULL;
    reader r;
    writer w;
    adapt_sizes sizes, read;
    adapt_stats stats;
    adapt_error error;
    static const uint8_t elf[4] = {0x7F, 'E', 'L', 'F'};
    static const uint8_t macho_le[4] = {0xCF, 0xFA, 0xED, 0xFE};
    static const uint8_t macho_be[4] = {0xFE, 0xED, 0xFA, 0xCE};
    static const uint8_t fat[4] = {0xCA, 0xFE, 0xBA, 0xBE};
    /* "code": a call to one of a few targets every dozen bytes, relative to
     * the instruction, so the operands differ everywhere the targets agree;
     * a far call (a large displacement, left alone), a call whose operand
     * holds an E8 (skipped, not an opcode), and one cut by the block's end */
    for (i = 0; i < n; i++) {
        data[i] = (uint8_t)(0x40 + (i * 7) % 60);
    }
    for (at = 4; at + 5 <= n; at += 12) {
        uint32_t target = (uint32_t)(3000 + 500 * (at % 5));
        uint32_t rel = target - (uint32_t)(at % ADAPT_BLOCK_BYTES + 5);
        data[at] = (at % 24 == 4) ? 0xE9 : 0xE8;
        data[at + 1] = (uint8_t)rel;
        data[at + 2] = (uint8_t)(rel >> 8);
        data[at + 3] = (uint8_t)(rel >> 16);
        data[at + 4] = (uint8_t)(rel >> 24);
    }
    data[100] = 0xE8;
    data[101] = 0x12;
    data[102] = 0x34;
    data[103] = 0x56;
    data[104] = 0x78; /* far: the top byte is neither 0 nor 0xFF */
    data[200] = 0xE8;
    data[201] = 0xE8;
    data[202] = 0xE8;
    data[203] = 0x00;
    data[204] = 0x00; /* an E8 inside an operand */
    data[ADAPT_BLOCK_BYTES - 3] = 0xE8; /* an opcode without room for its operand */
    data[n - 2] = 0xE9;
    memcpy(data, elf, 4);
    /* in memory, both ways, with the flag in the header */
    CHECK_STATUS(adapt_encode(data, n, adapt_default_limits(), &out, &out_len, &error), ADAPT_OK);
    CHECK((out[7] & ADAPT_X86_FLAG) != 0);
    CHECK_STATUS(adapt_read_header(out, out_len, &read, &error), ADAPT_OK);
    CHECK(read.x86 == 1 && adapt_sizes_valid(read));
    memcpy(coded, out, out_len);
    at = out_len;
    adapt_free(out);
    CHECK_STATUS(adapt_decode(coded, at, adapt_default_limits(), &out, &out_len, &error), ADAPT_OK);
    CHECK(out_len == n && memcmp(out, data, n) == 0);
    adapt_free(out);
    /* the same bytes proven alongside, and streamed in segments */
    CHECK_STATUS(adapt_encode_verified(data, n, adapt_default_limits(), &out, &out_len, &error), ADAPT_OK);
    CHECK(out_len == at && memcmp(out, coded, at) == 0);
    adapt_free(out);
    CHECK_STATUS(adapt_sizes_for_input(1, n, ADAPT_DEFAULT_MAX_MEMORY, &sizes, &error), ADAPT_OK);
    sizes.segmented = 1;
    reset(&r, data, n, 5000, &w, coded, n + 256);
    CHECK_STATUS(adapt_encode_stream_segments(reader_read, &r, writer_write, &w, sizes, ADAPT_DEFAULT_MAX_MEMORY,
                                              ADAPT_BLOCK_BYTES, &stats, &error),
                 ADAPT_OK);
    CHECK(stats.segments == 3 && (coded[7] & ADAPT_X86_FLAG) != 0 && (coded[7] & ADAPT_SEGMENT_FLAG) != 0);
    CHECK_STATUS(adapt_decode(coded, w.len, adapt_default_limits(), &out, &out_len, &error), ADAPT_OK);
    CHECK(out_len == n && memcmp(out, data, n) == 0);
    adapt_free(out);
    /* the fast and max profiles carry it too */
    sizes.segmented = 0;
    reset(&r, data, n, 5000, &w, coded, n + 256);
    CHECK_STATUS(adapt_encode_stream(reader_read, &r, writer_write, &w, adapt_sizes_fast(sizes), ADAPT_DEFAULT_MAX_MEMORY,
                                     &stats, &error),
                 ADAPT_OK);
    CHECK(coded[4] == ADAPT_FAST_VERSION && (coded[7] & ADAPT_X86_FLAG) != 0);
    CHECK_STATUS(adapt_decode(coded, w.len, adapt_default_limits(), &out, &out_len, &error), ADAPT_OK);
    CHECK(out_len == n && memcmp(out, data, n) == 0);
    adapt_free(out);
    reset(&r, data, n, 5000, &w, coded, n + 256);
    CHECK_STATUS(adapt_encode_stream(reader_read, &r, writer_write, &w, adapt_sizes_max(sizes), ADAPT_DEFAULT_MAX_MEMORY,
                                     &stats, &error),
                 ADAPT_OK);
    CHECK(coded[4] == ADAPT_MAX_VERSION && (coded[7] & ADAPT_X86_FLAG) != 0);
    CHECK_STATUS(adapt_decode(coded, w.len, adapt_default_limits(), &out, &out_len, &error), ADAPT_OK);
    CHECK(out_len == n && memcmp(out, data, n) == 0);
    adapt_free(out);
    /* the transform pays: the same bytes without the signature are coded
     * without it, and larger */
    data[0] = 'x';
    reset(&r, data, n, 5000, &w, coded, n + 256);
    CHECK_STATUS(adapt_encode_stream(reader_read, &r, writer_write, &w, sizes, ADAPT_DEFAULT_MAX_MEMORY, &stats, &error),
                 ADAPT_OK);
    CHECK((coded[7] & ADAPT_X86_FLAG) == 0 && w.len > at);
    CHECK_STATUS(adapt_decode(coded, w.len, adapt_default_limits(), &out, &out_len, &error), ADAPT_OK);
    CHECK(out_len == n && memcmp(out, data, n) == 0);
    adapt_free(out);
    /* a budget that holds the model but not the untransformed block
     * declines the transform rather than the input */
    memcpy(data, elf, 4);
    reset(&r, data, n, 5000, &w, coded, n + 256);
    CHECK_STATUS(adapt_encode_stream(reader_read, &r, writer_write, &w, sizes, adapt_encode_memory_bound(sizes), &stats,
                                     &error),
                 ADAPT_OK);
    CHECK((coded[7] & ADAPT_X86_FLAG) == 0);
    CHECK_STATUS(adapt_decode(coded, w.len, adapt_default_limits(), &out, &out_len, &error), ADAPT_OK);
    CHECK(out_len == n && memcmp(out, data, n) == 0);
    adapt_free(out);
    {
        adapt_sizes flagged = sizes;
        flagged.x86 = 1;
        CHECK(adapt_encode_memory_bound(flagged) == adapt_encode_memory_bound(sizes) + ADAPT_BLOCK_BYTES);
        flagged.x86 = 2;
        CHECK(!adapt_sizes_valid(flagged));
    }
    /* every signature the encoder knows, on a short input */
    memcpy(data, macho_le, 4);
    CHECK_STATUS(adapt_encode(data, 64, adapt_default_limits(), &out, &out_len, &error), ADAPT_OK);
    CHECK((out[7] & ADAPT_X86_FLAG) != 0);
    adapt_free(out);
    memcpy(data, macho_be, 4);
    CHECK_STATUS(adapt_encode(data, 64, adapt_default_limits(), &out, &out_len, &error), ADAPT_OK);
    CHECK((out[7] & ADAPT_X86_FLAG) != 0);
    adapt_free(out);
    memcpy(data, fat, 4);
    CHECK_STATUS(adapt_encode(data, 64, adapt_default_limits(), &out, &out_len, &error), ADAPT_OK);
    CHECK((out[7] & ADAPT_X86_FLAG) != 0);
    adapt_free(out);
    data[0] = 'M';
    data[1] = 'Z';
    CHECK_STATUS(adapt_encode(data, 64, adapt_default_limits(), &out, &out_len, &error), ADAPT_OK);
    CHECK((out[7] & ADAPT_X86_FLAG) != 0);
    CHECK_STATUS(adapt_decode(out, out_len, adapt_default_limits(), &coded, &at, &error), ADAPT_OK);
    CHECK(at == 64 && memcmp(coded, data, 64) == 0);
    adapt_free(coded);
    adapt_free(out);
    /* one byte of signature, or one short of it, is none */
    CHECK_STATUS(adapt_encode(data, 1, adapt_default_limits(), &out, &out_len, &error), ADAPT_OK);
    CHECK((out[7] & ADAPT_X86_FLAG) == 0);
    adapt_free(out);
    memcpy(data, elf, 4);
    CHECK_STATUS(adapt_encode(data, 3, adapt_default_limits(), &out, &out_len, &error), ADAPT_OK);
    CHECK((out[7] & ADAPT_X86_FLAG) == 0);
    adapt_free(out);
    free(data);
}

/* The archive flag: carried in the table byte beside the class code,
 * read back with it, and nothing to the model — the same bytes follow. */
static void test_archive_flag(void) {
    size_t n = 5000, i, out_len;
    uint8_t *data = malloc(n);
    uint8_t *plain = malloc(n + 256);
    uint8_t *flagged = malloc(n + 256);
    uint8_t *out = NULL;
    reader r;
    writer w;
    adapt_sizes sizes, read;
    adapt_error error;
    size_t plain_len;
    for (i = 0; i < n; i++) {
        data[i] = "an archive is bytes like any other to the model. "[i % 49];
    }
    CHECK_STATUS(adapt_sizes_for_input(1, n, ADAPT_DEFAULT_MAX_MEMORY, &sizes, &error), ADAPT_OK);
    CHECK(sizes.archive == 0);
    reset(&r, data, n, 700, &w, plain, n + 256);
    CHECK_STATUS(adapt_encode_stream(reader_read, &r, writer_write, &w, sizes, ADAPT_DEFAULT_MAX_MEMORY, NULL, &error),
                 ADAPT_OK);
    plain_len = w.len;
    sizes.archive = 1;
    CHECK(adapt_sizes_valid(sizes));
    reset(&r, data, n, 700, &w, flagged, n + 256);
    CHECK_STATUS(adapt_encode_stream(reader_read, &r, writer_write, &w, sizes, ADAPT_DEFAULT_MAX_MEMORY, NULL, &error),
                 ADAPT_OK);
    CHECK(w.len == plain_len && (flagged[5] & ADAPT_ARCHIVE_FLAG) != 0 && (plain[5] & ADAPT_ARCHIVE_FLAG) == 0);
    CHECK((flagged[5] & 0x7F) == plain[5] && memcmp(flagged + 6, plain + 6, plain_len - 6) == 0);
    CHECK_STATUS(adapt_read_header(flagged, w.len, &read, &error), ADAPT_OK);
    CHECK(read.archive == 1 && read.class_code == sizes.class_code && read.table_log == sizes.table_log);
    CHECK_STATUS(adapt_read_header(plain, plain_len, &read, &error), ADAPT_OK);
    CHECK(read.archive == 0);
    CHECK_STATUS(adapt_decode(flagged, w.len, adapt_default_limits(), &out, &out_len, &error), ADAPT_OK);
    CHECK(out_len == n && memcmp(out, data, n) == 0);
    adapt_free(out);
    sizes.archive = 2;
    CHECK(!adapt_sizes_valid(sizes));
    free(data);
    free(plain);
    free(flagged);
}

static void test_in_memory(void) {
    uint8_t *out = (uint8_t *)1;
    size_t out_len = 1, n = 30000, i;
    uint8_t *data = malloc(n);
    adapt_limits limits = adapt_default_limits();
    adapt_sizes sizes;
    adapt_error error;
    char message[160];
    {
        size_t large_len = 17u << 20;
        uint8_t *large = malloc(large_len);
        noise(9, large, large_len);
        roundtrip(large, large_len, 2, 0);
        roundtrip(large, large_len, 2, 1);
        free(large);
    }
    for (i = 0; i < n; i++) {
        data[i] = "in memory, the proof runs after when one model fits; "[i % 53];
    }
    CHECK_STATUS(adapt_encode(NULL, 5, limits, &out, &out_len, &error), ADAPT_ERROR_INVALID_ARGUMENT);
    CHECK_STATUS(adapt_encode(data, n, limits, NULL, &out_len, NULL), ADAPT_ERROR_INVALID_ARGUMENT);
    CHECK_STATUS(adapt_encode(data, n, limits, &out, NULL, NULL), ADAPT_ERROR_INVALID_ARGUMENT);
    limits.max_memory = 1;
    CHECK_STATUS(adapt_encode(data, n, limits, &out, &out_len, &error), ADAPT_ERROR_MEMORY_LIMIT);
    CHECK(out == NULL && out_len == 0);
    CHECK_STATUS(adapt_encode_verified(data, n, limits, &out, &out_len, &error), ADAPT_ERROR_MEMORY_LIMIT);
    /* a budget for one model: the proof runs after, to the same bytes */
    CHECK_STATUS(adapt_sizes_for_input(1, n, ADAPT_DEFAULT_MAX_MEMORY, &sizes, &error), ADAPT_OK);
    limits.max_memory = adapt_encode_memory_bound(sizes);
    CHECK(adapt_verified_memory_bound(sizes) > limits.max_memory);
    CHECK_STATUS(adapt_encode_verified(data, n, limits, &out, &out_len, &error), ADAPT_OK);
    CHECK(out != NULL && out_len > ADAPT_HEADER_BYTES);
    {
        uint8_t *plain = NULL;
        size_t plain_len = 0;
        CHECK_STATUS(adapt_encode(data, n, limits, &plain, &plain_len, &error), ADAPT_OK);
        CHECK(plain_len == out_len && memcmp(plain, out, out_len) == 0);
        adapt_free(plain);
    }
    adapt_free(out);
    /* the verification messages */
    error.status = ADAPT_ERROR_VERIFICATION;
    error.cause = ADAPT_OK;
    adapt_error_message(&error, message, sizeof(message));
    CHECK(strcmp(message, "verification failed: decoded bytes differ from the input") == 0);
    error.cause = ADAPT_ERROR_CHECKSUM;
    adapt_error_message(&error, message, sizeof(message));
    CHECK(strcmp(message, "verification failed: checksum mismatch: the recovered bytes are damaged") == 0);
    error.cause = ADAPT_ERROR_TRAILING_BYTES;
    error.detail = 3;
    adapt_error_message(&error, message, sizeof(message));
    CHECK(strcmp(message, "verification failed: 3 unexpected trailing byte(s)") == 0);
    free(data);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: test_adapt_encode <vectors-dir>\n");
        return 2;
    }
    vectors_dir = argv[1];
    test_vector("empty.adpt", "empty.bin");
    test_vector("hello.adpt", "hello.bin");
    test_vector("abc.adpt", "abc.bin");
    test_sizes();
    test_roundtrips();
    test_streams();
    test_verified_blocks();
    test_segments();
    test_fast_profile();
    test_max_profile();
    test_edges();
    test_record_period();
    test_x86_transform();
    test_archive_flag();
    test_in_memory();
    if (failures != 0) {
        fprintf(stderr, "%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("%d checks passed\n", checks);
    return 0;
}
