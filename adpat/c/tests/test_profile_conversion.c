/* Public SDK regression: fractional max sizes must convert to usable fast sizes.
 * Source: OpenAI GPT-6 via Codex, P2 review regression, 2026-09-15. */
#include "../adapt.h"
#include <stdio.h>
#include <string.h>

static const uint8_t plain[] = "A fractional max configuration can become a fast encoder.";

typedef struct input {
    size_t at;
} input;

typedef struct output {
    uint8_t bytes[4096];
    size_t len;
} output;

static int read_bytes(void *context, uint8_t *bytes, size_t len, size_t *got) {
    input *in = context;
    size_t left = sizeof(plain) - 1 - in->at;
    if (len > left) len = left;
    memcpy(bytes, plain + in->at, len);
    in->at += len;
    *got = len;
    return 0;
}

static int write_bytes(void *context, const uint8_t *bytes, size_t len) {
    output *out = context;
    if (len > sizeof(out->bytes) - out->len) return 1;
    memcpy(out->bytes + out->len, bytes, len);
    out->len += len;
    return 0;
}

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "profile conversion failed at line %d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int main(void) {
    adapt_sizes source, sizes, fast, read;
    adapt_error error;
    input in = {0};
    output out = {{0}, 0};
    uint8_t fraction, low_start, *back = NULL;
    size_t back_len = 0;
    uint64_t budget = (uint64_t)160 << 20;
    CHECK(adapt_sizes_for_input_max(1, 100000000, budget, &source, &error) == ADAPT_OK);
    CHECK(adapt_sizes_prioritize_context(1, 100000000, budget, 1, ADAPT_SEGMENT_BYTES, &source, &error) == ADAPT_OK);
    CHECK(source.table_frac != 0);
    for (fraction = 0; fraction <= ADAPT_SIZING_FRAC_MAX; fraction++) {
        for (low_start = 0; low_start <= 1; low_start++) {
            sizes = source;
            sizes.table_frac = fraction;
            sizes.low_start = low_start;
            CHECK(adapt_sizes_valid(sizes));
            fast = adapt_sizes_fast(sizes);
            CHECK(adapt_sizes_valid(fast));
            CHECK(fast.fast && !fast.max);
            CHECK(adapt_encode_memory_bound(fast) <= adapt_encode_memory_bound(sizes));
        }
    }
    fast = adapt_sizes_fast(source);
    CHECK(adapt_encode_stream(read_bytes, &in, write_bytes, &out, fast, budget, NULL, &error) == ADAPT_OK);
    CHECK(adapt_read_header(out.bytes, out.len, &read, &error) == ADAPT_OK);
    CHECK(read.fast && !read.max);
    CHECK(adapt_decode(out.bytes, out.len, adapt_default_limits(), &back, &back_len, &error) == ADAPT_OK);
    CHECK(back_len == sizeof(plain) - 1 && memcmp(back, plain, back_len) == 0);
    adapt_free(back);
    puts("profile conversion: fractional max sizes produce valid fast sizes and round-trip");
    return 0;
}
