/* The C decoder under libFuzzer with the address and undefined-behaviour
 * sanitizers: arbitrary bytes into adapt_decode() and adapt_decode_stream(),
 * which return the original or a named fault, never read out of bounds,
 * never leak, and agree with each other. Built with clang (libFuzzer needs
 * its C++ runtime, libstdc++ or libc++, installed) and run from this
 * directory, seeded with copies of the committed vectors:
 *
 *   make fuzz-smoke
 *
 * One execution admits at most a 128 MiB decoder working-memory bound and
 * 32 KiB of output. The result gets its own headroom so the two decoder
 * APIs admit the same models even at the budget boundary. Regression tests
 * run this oracle on modeled normal, fast and max fixtures.
 *
 * Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-14. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fuzz_decode.h"

typedef struct {
    const uint8_t *src;
    size_t len;
    size_t pos;
} slice_reader;

static int slice_read(void *context, uint8_t *buf, size_t len, size_t *got) {
    slice_reader *r = context;
    size_t n = r->len - r->pos;
    if (n > len) {
        n = len;
    }
    if (n > 7) {
        n = 7; /* short reads, so every boundary is crossed */
    }
    if (n != 0) {
        memcpy(buf, r->src + r->pos, n);
    }
    r->pos += n;
    *got = n;
    return 0;
}

typedef struct {
    uint8_t *out;
    size_t len;
    size_t cap;
} growing;

static int growing_write(void *context, const uint8_t *data, size_t len) {
    growing *g = context;
    if (g->cap - g->len < len) {
        size_t cap = g->cap ? g->cap * 2 : 4096;
        while (cap - g->len < len) {
            cap *= 2;
        }
        uint8_t *grown = realloc(g->out, cap);
        if (grown == NULL) {
            return 12;
        }
        g->out = grown;
        g->cap = cap;
    }
    memcpy(g->out + g->len, data, len);
    g->len += len;
    return 0;
}

adapt_status adapt_fuzz_decode(const uint8_t *data, size_t size) {
    adapt_limits limits = adapt_default_limits();
    adapt_error error, stream_error;
    adapt_stats stats;
    uint8_t *out = NULL;
    size_t out_len = 0;
    slice_reader r = {data, size, 0};
    growing g = {NULL, 0, 0};
    adapt_status whole, streamed;
    adapt_sizes sizes;
    /* Refuse oversized models before allocation. Reserve the maximum
     * result separately: adapt_decode counts it, while the streaming API
     * leaves the output buffer to its caller. */
    if (adapt_read_header(data, size, &sizes, NULL) == ADAPT_OK &&
        adapt_decode_memory_bound(sizes) > ADAPT_FUZZ_MAX_MODEL_MEMORY) {
        return ADAPT_ERROR_MEMORY_LIMIT;
    }
    limits.max_output = ADAPT_FUZZ_MAX_OUTPUT;
    limits.max_memory = ADAPT_FUZZ_MAX_MODEL_MEMORY + ADAPT_FUZZ_MAX_OUTPUT;
    whole = adapt_decode(data, size, limits, &out, &out_len, &error);
    streamed = adapt_decode_stream(slice_read, &r, growing_write, &g, limits, &stats, &stream_error);
    if (whole == ADAPT_OK) {
        if (out_len > limits.max_output) {
            abort();
        }
        if (streamed != ADAPT_OK || g.len != out_len || (out_len != 0 && memcmp(g.out, out, out_len) != 0)) {
            abort(); /* the two decoders disagree on a file they both accept */
        }
    } else if (streamed == ADAPT_OK) {
        abort(); /* the stream decoder accepted what the in-memory decoder refused */
    }
    adapt_free(out);
    free(g.out);
    return whole;
}

/* The entry point, unless another harness (fuzz_header.c) brings its own
 * and takes the oracle above. */
#ifndef ADAPT_FUZZ_NO_ENTRY
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    (void)adapt_fuzz_decode(data, size);
    return 0;
}
#endif
