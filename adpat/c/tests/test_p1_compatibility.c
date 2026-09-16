/* Immutable files from both historical model2 predictors must decode.
 * legacy-max2-always.adpt is the text-max2 vector at 64ec3f9;
 * legacy-max2-pressure.adpt is that vector at 0170f51. The matching
 * legacy-text files were encoded by those revisions with --max
 * --transform text from text.bin, retaining their metadata headers.
 * Never regenerate these legacy fixtures with the current encoder.
 * Source: OpenAI GPT-6 via Codex, 2026-09-15. */
#include "../adapt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

typedef struct source { const uint8_t *data; size_t len, at, chunk, fail_at; } source;
typedef struct sink { const uint8_t *want; size_t len, at; int fail; } sink;
static int read_bytes(void *context, uint8_t *buf, size_t len, size_t *got) {
    source *r = context;
    if (r->at >= r->fail_at) return 41;
    *got = r->len - r->at;
    if (*got > len) *got = len;
    if (*got > r->chunk) *got = r->chunk;
    memcpy(buf, r->data + r->at, *got);
    r->at += *got;
    return 0;
}
static int write_bytes(void *context, const uint8_t *buf, size_t len) {
    sink *w = context;
    if (w->fail) return -42;
    CHECK(len <= w->len - w->at);
    CHECK(memcmp(buf, w->want + w->at, len) == 0);
    w->at += len;
    return 0;
}
static uint8_t *load(const char *dir, const char *name, size_t *len) {
    char path[4096]; FILE *f; long n; uint8_t *p;
    CHECK(snprintf(path, sizeof(path), "%s/%s", dir, name) > 0);
    f = fopen(path, "rb"); CHECK(f != NULL);
    CHECK(fseek(f, 0, SEEK_END) == 0); n = ftell(f); CHECK(n > 0); rewind(f);
    *len = (size_t)n; p = malloc(*len); CHECK(p != NULL);
    CHECK(fread(p, 1, *len, f) == *len); CHECK(fclose(f) == 0); return p;
}
static void exercise(const uint8_t *file, size_t len, const uint8_t *raw, size_t raw_len) {
    adapt_sizes sizes; adapt_error error; adapt_stats stats;
    adapt_limits limits = adapt_default_limits();
    uint8_t *out = NULL, *broken; size_t out_len = 0, chunk;
    uint64_t bound;
    CHECK(adapt_read_header(file, len, &sizes, &error) == ADAPT_OK);
    bound = adapt_decode_memory_bound(sizes);
    limits.max_memory = bound + raw_len;
    CHECK(adapt_decode(file, len, limits, &out, &out_len, NULL) == ADAPT_OK);
    CHECK(out_len == raw_len && memcmp(out, raw, raw_len) == 0); adapt_free(out);
    for (chunk = 1; chunk <= 257; chunk += 256) {
        source r = {file, len, 0, chunk, (size_t)-1}; sink w = {raw, raw_len, 0, 0};
        limits.max_memory = bound;
        CHECK(adapt_decode_stream(read_bytes, &r, write_bytes, &w, limits, &stats, NULL) == ADAPT_OK);
        CHECK(w.at == raw_len && stats.input_bytes == raw_len && stats.output_bytes == len);
        r.at = 0; w.at = 0; limits.max_memory = bound - 1;
        CHECK(adapt_decode_stream(read_bytes, &r, write_bytes, &w, limits, NULL, &error) == ADAPT_ERROR_MEMORY_LIMIT);
        CHECK(w.at == 0 && error.detail == bound);
    }
    {
        source r = {file, len, 0, 97, (size_t)-1}; sink w = {raw, raw_len, 0, 0};
        limits.max_memory = bound + len;
        CHECK(adapt_decode_threads(file, len, limits, 2, write_bytes, &w, &stats, NULL) == ADAPT_OK);
        CHECK(w.at == raw_len && stats.workers == 1);
        w.at = 0; limits.max_memory--;
        CHECK(adapt_decode_threads(file, len, limits, 2, write_bytes, &w, NULL, &error) == ADAPT_ERROR_MEMORY_LIMIT);
        CHECK(w.at == 0);
        limits.max_memory = bound;
        r.fail_at = adapt_header_len(file) + 1;
        CHECK(adapt_decode_stream(read_bytes, &r, write_bytes, &w, limits, NULL, &error) == ADAPT_ERROR_IO);
        CHECK(error.detail == 41 && w.at == 0);
        r.at = 0; r.fail_at = (size_t)-1; w.fail = 1;
        CHECK(adapt_decode_stream(read_bytes, &r, write_bytes, &w, limits, NULL, &error) == ADAPT_ERROR_IO);
        CHECK(error.detail == 42 && w.at == 0);
        r.at = 0; w.fail = 0; limits.max_output = raw_len - 1;
        CHECK(adapt_decode_stream(read_bytes, &r, write_bytes, &w, limits, NULL, NULL) != ADAPT_OK);
        CHECK(w.at == 0);
    }
    broken = malloc(len); CHECK(broken != NULL); memcpy(broken, file, len); broken[len - 1] ^= 1;
    {
        source r = {broken, len, 0, 97, (size_t)-1}; sink w = {raw, raw_len, 0, 0};
        limits.max_output = raw_len;
        CHECK(adapt_decode_stream(read_bytes, &r, write_bytes, &w, limits, NULL, NULL) != ADAPT_OK);
        CHECK(w.at == 0); /* failed trial output must never reach the caller */
    }
    free(broken);
}
static void put32(uint8_t *p, uint32_t n) { size_t i; for (i = 0; i < 4; i++) p[i] = (uint8_t)(n >> (8 * i)); }
static void segmented(const uint8_t *file, size_t len, const uint8_t *raw, size_t raw_len) {
    size_t h = adapt_header_len(file), body = len - h - ADAPT_TRAILER_BYTES;
    size_t n = h + body * 2 + 1 + adapt_segment_table_len(2) + ADAPT_TRAILER_BYTES, at;
    uint8_t *f = calloc(n, 1), *twice = malloc(raw_len * 2);
    CHECK(f != NULL && twice != NULL && !adapt_header_has_metadata(file));
    memcpy(twice, raw, raw_len); memcpy(twice + raw_len, raw, raw_len);
    memcpy(f, file, h); f[7] |= ADAPT_SEGMENT_FLAG; at = h;
    memcpy(f + at, file + h, body); at += body; f[at++] = 4;
    memcpy(f + at, file + h, body); at += body; f[at++] = 5;
    put32(f + at, (uint32_t)raw_len); put32(f + at + 4, (uint32_t)body); at += 8;
    put32(f + at, (uint32_t)raw_len); put32(f + at + 4, (uint32_t)body + 1); at += 8;
    put32(f + at, 2); at += 4; f[at++] = 3;
    put32(f + at, (uint32_t)(raw_len * 2)); at += 8;
    put32(f + at, adapt_crc32(twice, raw_len * 2)); at += 4; CHECK(at == n);
    exercise(f, n, twice, raw_len * 2); free(twice); free(f);
}
int main(int argc, char **argv) {
    static const char *const names[] = {"legacy-max2-always.adpt", "legacy-max2-pressure.adpt",
        "legacy-text-max2-always.adpt", "legacy-text-max2-pressure.adpt"};
    uint8_t *raw; size_t raw_len, i;
    CHECK(argc == 2); raw = load(argv[1], "text.bin", &raw_len);
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        size_t len; uint8_t *file = load(argv[1], names[i], &len);
        exercise(file, len, raw, raw_len);
        if (i < 2) segmented(file, len, raw, raw_len);
        free(file);
    }
    {
        size_t len, historical_len, header_len;
        uint8_t *current = load(argv[1], "text-max2.adpt", &len);
        uint8_t *historical = load(argv[1], "legacy-max2-pressure.adpt", &historical_len);
        uint8_t header[ADAPT_HEADER_MAX_BYTES];
        adapt_sizes sizes;
        adapt_metadata metadata = {0};
        CHECK(current[4] == ADAPT_MAX_PRESSURE_VERSION && len == historical_len);
        CHECK(memcmp(current, historical, 4) == 0 && memcmp(current + 5, historical + 5, len - 5) == 0);
        header_len = adapt_header_len(current);
        CHECK(adapt_header_add_metadata(current, header_len, &metadata, header, sizeof(header)) ==
              header_len + ADAPT_METADATA_HEADER_BYTES);
        CHECK(header[4] == ADAPT_MAX_PRESSURE_METADATA_VERSION && adapt_header_has_metadata(header));
        CHECK(adapt_read_header(header, adapt_header_len(header), &sizes, NULL) == ADAPT_OK);
        current[8] &= (uint8_t)~ADAPT_SIZING_MODEL2;
        CHECK(adapt_read_header(current, len, &sizes, NULL) == ADAPT_ERROR_BAD_HEADER);
        free(current); free(historical);
    }
    free(raw); puts("historical model2 compatibility: both predictors, metadata, text, segments and failure isolation passed");
    return 0;
}
