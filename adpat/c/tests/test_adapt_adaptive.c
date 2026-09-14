/* Causal adaptive experts: bounded probabilities, normalized weights,
 * learning, deterministic replay, and retained-state replacement. */
#include "../adapt_adaptive.h"
#include "../adapt.h"
#include "../adapt_bmp.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct buffer { uint8_t *data; size_t pos, len, cap; } buffer;
static int read_buffer(void *ctx, uint8_t *out, size_t cap, size_t *n) {
    buffer *b = ctx;
    *n = b->len - b->pos < cap ? b->len - b->pos : cap;
    memcpy(out, b->data + b->pos, *n); b->pos += *n; return 0;
}
static int write_buffer(void *ctx, const uint8_t *in, size_t n) {
    buffer *b = ctx;
    if (n > b->cap - b->len) return 1;
    memcpy(b->data + b->len, in, n); b->len += n; return 0;
}
int main(void) {
    adaptive_model *a = calloc(1, sizeof(*a)), *b = calloc(1, sizeof(*b));
    int16_t squash_table[4095];
    uint64_t history = 0;
    unsigned mode, t, i, bit, max_profile, c0 = 1;
    adapt_sizes sizes, normal, parsed;
    adapt_error error;
    uint8_t input[8192], *coded, *decoded;
    size_t coded_len, decoded_len;
    buffer source, destination;
    adapt_limits limits = adapt_default_limits();
    assert(a != NULL && b != NULL);
    assert(adapt_sizes_for_input(1, sizeof(input), 64u << 20, &normal, &error) == ADAPT_OK);
    for (i = 0; i < sizeof(input); i++) input[i] = (uint8_t)((i / 7 + i * 13) % 251);
    for (max_profile = 0; max_profile <= 1; max_profile++) {
    assert((max_profile ? adapt_sizes_for_input_max : adapt_sizes_for_input)(1, sizeof(input), 128u << 20, &normal, &error) == ADAPT_OK);
    for (mode = 1; mode <= 3; mode++) {
        assert((max_profile ? adapt_sizes_for_input_max_adaptive : adapt_sizes_for_input_adaptive)(1, sizeof(input), 128u << 20, mode, &sizes, &error) == ADAPT_OK);
        assert(sizes.adaptive == mode);
        parsed = normal;
        parsed.adaptive = (uint8_t)mode;
        assert(adapt_model_bytes(parsed) - adapt_model_bytes(normal) == sizeof(adaptive_model));
        coded = malloc(10000); assert(coded != NULL);
        source = (buffer){input, 0, sizeof(input), sizeof(input)};
        destination = (buffer){coded, 0, 0, 10000};
        assert(adapt_encode_stream(read_buffer, &source, write_buffer, &destination, sizes, 128u << 20, NULL, &error) == ADAPT_OK);
        coded_len = destination.len;
        assert(coded[4] == (max_profile ? ADAPT_MAX_ADAPTIVE_VERSION : ADAPT_ADAPTIVE_VERSION) + mode - 1);
        assert(adapt_read_header(coded, coded_len, &parsed, &error) == ADAPT_OK);
        assert(parsed.adaptive == mode && parsed.max == max_profile);
        assert(adapt_decode(coded, coded_len, limits, &decoded, &decoded_len, &error) == ADAPT_OK);
        assert(decoded_len == sizeof(input) && memcmp(decoded, input, sizeof(input)) == 0);
        free(decoded); free(coded);
    }
    }
    normal.max = 0;
    assert(adapt_sizes_for_input_adaptive(1, 0, 1u << 20, 1, &sizes, &error) == ADAPT_ERROR_MEMORY_LIMIT);
    assert(adapt_sizes_for_input_adaptive(1, 0, 64u << 20, 4, &sizes, &error) == ADAPT_ERROR_INVALID_ARGUMENT);
    assert(adapt_sizes_for_input_max_adaptive(1, 0, 1u << 20, 1, &sizes, &error) == ADAPT_ERROR_MEMORY_LIMIT);
    assert(adapt_sizes_for_input_max_adaptive(1, 0, 128u << 20, 4, &sizes, &error) == ADAPT_ERROR_INVALID_ARGUMENT);
    assert(adapt_sizes_for_threads_max_adaptive(0, 128u << 20, 1, ADAPT_SEGMENT_BYTES, 4, &sizes, &error) == ADAPT_ERROR_INVALID_ARGUMENT);
    /* Every short block boundary, arbitrary color bytes and row padding. */
    sizes = normal;
    sizes.bmp = 1; sizes.bmp_offset = 54; sizes.bmp_width = 17; sizes.bmp_rows = 13;
    for (mode = 1; mode <= 71; mode++) {
        uint8_t transformed[8192];
        size_t at;
        memcpy(transformed, input, sizeof(input));
        for (at = 0; at < sizeof(input); at += mode) {
            size_t n = sizeof(input) - at < mode ? sizeof(input) - at : mode;
            bmp_transform(transformed + at, n, at, sizes, 0);
            bmp_transform(transformed + at, n, at, sizes, 1);
        }
        assert(memcmp(input, transformed, sizeof(input)) == 0);
    }
    assert(!adapt_bmp_probe(NULL, 0, &sizes));
    assert(!adapt_bmp_probe(input, sizeof(input), &sizes));
    sizes.bmp_width = 0;
    assert(!bmp_descriptor_valid(sizes));
    bmp_transform(input, sizeof(input), 0, sizes, 1);
    sizes = normal; sizes.adaptive = 1; sizes.fast = 1;
    assert(!adapt_sizes_valid(sizes));
    sizes = normal; sizes.bmp = 1; sizes.archive = 1;
    assert(!adapt_sizes_valid(sizes));
    for (i = 0; i < 4095; i++) squash_table[i] = (int16_t)(i + 1);
    for (mode = 1; mode <= 3; mode++) {
        adaptive_init(a, mode);
        adaptive_init(b, mode);
        history = 0;
        c0 = 1;
        for (t = 0; t < 20000; t++) {
            uint32_t p = adaptive_predict(a, 2048, 0, squash_table, history, c0, t % 8);
            uint32_t q = adaptive_predict(b, 2048, 0, squash_table, history, c0, t % 8);
            uint64_t sum = 0;
            assert(p == q && p >= 1 && p <= 4095);
            /* Long runs followed by changing bits exercise both rates. */
            bit = t < 5000 ? 1 : ((t * 19u + t / 13u) & 1);
            adaptive_update(a, bit);
            adaptive_update(b, bit);
            for (i = 0; i < a->experts; i++) {
                assert(a->weights[t % 8][i] > 0);
                sum += a->weights[t % 8][i];
            }
            assert(sum == ADAPT_WEIGHT_ONE);
            if (t == 4000) assert(p > 3000);
            c0 = c0 * 2 + bit;
            if (t % 8 == 7) { history = (history << 8) | (c0 & 255); c0 = 1; }
        }
        assert(memcmp(a, b, sizeof(*a)) == 0);
    }
    adaptive_init(a, 3);
    for (i = 0; i < 4095; i++) adaptive_observe_byte(a, 0);
    assert(a->used == 0); /* Never route using an unfinished descriptor. */
    adaptive_observe_byte(a, 0);
    assert(a->used == 1 && a->active == 0);
    a->retained[0][1][1] = 1234;
    for (mode = 1; mode < 6; mode++) {
        for (i = 0; i < 4096; i++) adaptive_observe_byte(a, mode);
    }
    assert(a->used == 4 && a->retained[0][1][1] == 0);
    for (i = 0; i < 4096; i++) adaptive_observe_byte(a, 5);
    assert(a->active < 4 && a->used == 4);
    free(a); free(b);
    puts("adaptive: normalization, replay, learning and retention passed");
    return 0;
}
