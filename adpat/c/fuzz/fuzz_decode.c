/* The C decoder under libFuzzer with the address and undefined-behaviour
 * sanitizers: arbitrary bytes into adapt_decode() and adapt_decode_stream(),
 * which return the original or a named fault, never read out of bounds,
 * never leak, and agree with each other. Built with clang (libFuzzer needs
 * its C++ runtime, libstdc++ or libc++, installed) and run from this
 * directory, seeded with the committed vectors:
 *
 *   clang -std=c99 -O1 -g -fsanitize=fuzzer,address,undefined \
 *         adapt_decode.c adapt_encode.c fuzz/fuzz_decode.c -o build/fuzz_decode
 *   build/fuzz_decode tests/vectors -max_total_time=60
 *
 * The limits below hold one execution to the smallest models and 32 KiB of
 * output, so a seed that names a larger model is refused at its header:
 * to reach the modelled path, add seeds encoded under a budget the limits
 * hold (`cadapt encode --max-memory 15000000`).
 *
 * Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-14. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../adapt.h"

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
    memcpy(buf, r->src + r->pos, n);
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

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    adapt_limits limits = adapt_default_limits();
    adapt_error error, stream_error;
    adapt_stats stats;
    uint8_t *out = NULL;
    size_t out_len = 0;
    slice_reader r = {data, size, 0};
    growing g = {NULL, 0, 0};
    adapt_status whole, streamed;
    /* the limits bound one execution: the smallest models only, and no block
     * past 32 KiB decoded, as the Rust targets do */
    limits.max_output = (uint64_t)32 << 10;
    limits.max_memory = (uint64_t)16 << 20;
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
    return 0;
}
