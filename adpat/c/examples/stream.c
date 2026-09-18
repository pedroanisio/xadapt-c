/* Streaming: a reader and a writer callback, so memory is the model and
 * one block whatever the input's length. The callbacks here run over
 * memory; a program reads a file or a socket the same way.
 *
 *   cc -std=c99 -I.. stream.c ../build/libadapt.a -pthread
 *
 * Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-17. */
#include "example.h"

/* A reader over a buffer. Every callback's buffer is valid for that call
 * only; a reader fills `buf`, a writer copies `data` if it needs it later.
 * A reader stores 0 in *got only at the end of the input. */
typedef struct source {
    const uint8_t *data;
    size_t len, at;
} source;

static int read_source(void *context, uint8_t *buf, size_t len, size_t *got) {
    source *s = context;
    size_t n = s->len - s->at;
    if (n > len) n = len;
    if (n > 4093) n = 4093; /* short reads are fine: the codec reads again */
    memcpy(buf, s->data + s->at, n);
    s->at += n;
    *got = n;
    return 0; /* nonzero would become ADAPT_ERROR_IO with this value as its detail */
}

/* A writer into a growing buffer. */
typedef struct sink {
    uint8_t *data;
    size_t len, cap;
} sink;

static int write_sink(void *context, const uint8_t *data, size_t len) {
    sink *s = context;
    if (s->cap - s->len < len) {
        size_t cap = s->cap ? s->cap * 2 : 65536;
        uint8_t *grown;
        while (cap - s->len < len) cap *= 2;
        grown = realloc(s->data, cap);
        if (grown == NULL) return 12; /* ENOMEM: reported as an i/o error, detail 12 */
        s->data = grown;
        s->cap = cap;
    }
    memcpy(s->data + s->len, data, len);
    s->len += len;
    return 0;
}

int main(void) {
    size_t input_len;
    uint8_t *input = example_input(&input_len);
    source in = {NULL, 0, 0};
    sink coded = {NULL, 0, 0}, decoded = {NULL, 0, 0};
    adapt_sizes sizes;
    adapt_stats stats;
    adapt_error error;
    uint64_t budget = adapt_default_limits().max_memory;

    /* The sizes fit the model to the length and the budget: the tables
     * grow with the input to their sweet spot, the window to the input. A
     * pipe of unknown length passes length_known 0. */
    EXPECT_OK(adapt_sizes_for_input(1, input_len, budget, &sizes, &error), &error);
    printf("table 2^%u blocks, recall 2^%u, window 2^%u; the model takes %llu bytes\n", sizes.table_log,
           sizes.match_log, sizes.window_log, (unsigned long long)adapt_model_bytes(sizes));

    /* Encode as the bytes come, proven alongside by a second model (which
     * the budget must hold: adapt_verified_memory_bound). */
    in.data = input;
    in.len = input_len;
    EXPECT(adapt_verified_memory_bound(sizes) <= budget);
    EXPECT_OK(adapt_encode_stream_verified(read_source, &in, write_sink, &coded, sizes, budget, &stats, &error), &error);
    EXPECT(stats.input_bytes == input_len && stats.output_bytes == coded.len && stats.segments == 1);
    printf("%llu bytes -> %llu bytes, crc32 %08lx, %lu trial codings on the first block\n",
           (unsigned long long)stats.input_bytes, (unsigned long long)stats.output_bytes, (unsigned long)stats.crc32,
           (unsigned long)stats.trials);

    /* Decode the same way: each block reaches the writer as it is
     * recovered, and the checksum is checked last — bytes written before
     * a failure are not to be trusted until ADAPT_OK. */
    in.data = coded.data;
    in.len = coded.len;
    in.at = 0;
    EXPECT_OK(adapt_decode_stream(read_source, &in, write_sink, &decoded, adapt_default_limits(), &stats, &error), &error);
    EXPECT(decoded.len == input_len && memcmp(decoded.data, input, input_len) == 0);
    EXPECT(stats.crc32 == adapt_crc32(input, input_len));

    /* A reader that fails stops the decode with its value in the error. */
    {
        source cut = {coded.data, coded.len / 2, 0}; /* the input ends early */
        sink out = {NULL, 0, 0};
        EXPECT(adapt_decode_stream(read_source, &cut, write_sink, &out, adapt_default_limits(), NULL, &error) ==
               ADAPT_ERROR_TRUNCATED);
        free(out.data);
    }

    free(coded.data);
    free(decoded.data);
    free(input);
    return 0;
}
