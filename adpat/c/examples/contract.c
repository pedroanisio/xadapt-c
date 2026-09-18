/* The contract at the top of adapt.h, exercised: NULL arguments, what a
 * failure leaves behind, the error on success, callbacks and threads,
 * the limits and their numbers.
 *
 *   cc -std=c99 -I.. contract.c ../build/libadapt.a -pthread
 *
 * Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-17. */
#include "example.h"
#ifdef ADAPT_THREADS
#include <pthread.h>
#endif

typedef struct source { const uint8_t *data; size_t len, at; } source;
typedef struct sink { uint8_t *data; size_t len, cap; } sink;

#ifdef ADAPT_THREADS
static pthread_t calling_thread;
static int callbacks_on_calling_thread = 1;
static int callbacks_in_flight = 0, overlapping = 0;
#endif

static int read_source(void *context, uint8_t *buf, size_t len, size_t *got) {
    source *s = context;
    size_t n = s->len - s->at;
#ifdef ADAPT_THREADS
    if (!pthread_equal(pthread_self(), calling_thread)) callbacks_on_calling_thread = 0;
    if (callbacks_in_flight++ != 0) overlapping = 1;
#endif
    if (n > len) n = len;
    memcpy(buf, s->data + s->at, n);
    s->at += n;
    *got = n;
#ifdef ADAPT_THREADS
    callbacks_in_flight--;
#endif
    return 0;
}

static int write_sink(void *context, const uint8_t *data, size_t len) {
    sink *s = context;
#ifdef ADAPT_THREADS
    if (!pthread_equal(pthread_self(), calling_thread)) callbacks_on_calling_thread = 0;
    if (callbacks_in_flight++ != 0) overlapping = 1;
#endif
    if (s->cap - s->len < len) {
        size_t cap = s->cap ? s->cap * 2 : 65536;
        uint8_t *grown;
        while (cap - s->len < len) cap *= 2;
        grown = realloc(s->data, cap);
        if (grown == NULL) return 12;
        s->data = grown;
        s->cap = cap;
    }
    memcpy(s->data + s->len, data, len);
    s->len += len;
#ifdef ADAPT_THREADS
    callbacks_in_flight--;
#endif
    return 0;
}

static int failing_write(void *context, const uint8_t *data, size_t len) {
    (void)context;
    (void)data;
    (void)len;
    return 28; /* ENOSPC */
}

#ifdef ADAPT_THREADS
/* Two decodes at once on two threads: the library keeps no global state. */
typedef struct job { const uint8_t *file; size_t len; const uint8_t *want; size_t want_len; int ok; } job;
static void *decode_job(void *arg) {
    job *j = arg;
    uint8_t *out = NULL;
    size_t out_len;
    adapt_error error;
    j->ok = adapt_decode(j->file, j->len, adapt_default_limits(), &out, &out_len, &error) == ADAPT_OK &&
            out_len == j->want_len && memcmp(out, j->want, out_len) == 0;
    adapt_free(out);
    return NULL;
}
#endif

int main(void) {
    size_t input_len, file_len, out_len = 77;
    uint8_t *input = example_input(&input_len);
    uint8_t *file = NULL, *out = (uint8_t *)&out_len; /* a sentinel: a failure leaves NULL, never this */
    uint8_t header[ADAPT_HEADER_BYTES] = {'A', 'D', 'P', 'T', 19, 20, 16, 20};
    adapt_limits limits = adapt_default_limits();
    adapt_error error;
    adapt_sizes sizes;
    uint64_t length;
#ifdef ADAPT_THREADS
    calling_thread = pthread_self();
#endif

    /* --- errors: a required pointer NULL, and what a failure leaves --- */
    EXPECT(adapt_decode(NULL, 10, limits, &out, &out_len, &error) == ADAPT_ERROR_INVALID_ARGUMENT);
    EXPECT(error.status == ADAPT_ERROR_INVALID_ARGUMENT);
    EXPECT(adapt_decode(header, sizeof(header), limits, NULL, &out_len, &error) == ADAPT_ERROR_INVALID_ARGUMENT);
    EXPECT(adapt_decode(header, sizeof(header), limits, &out, NULL, &error) == ADAPT_ERROR_INVALID_ARGUMENT);
    EXPECT(adapt_decode(header, sizeof(header), limits, &out, &out_len, NULL) == ADAPT_ERROR_TRUNCATED); /* error may be NULL */
    EXPECT(out == NULL && out_len == 0); /* a failure leaves NULL and 0: adapt_free(out) is safe */
    adapt_free(out);
    EXPECT(adapt_encode(input, input_len, limits, NULL, &out_len, &error) == ADAPT_ERROR_INVALID_ARGUMENT);
    EXPECT(adapt_encode(NULL, 5, limits, &out, &out_len, &error) == ADAPT_ERROR_INVALID_ARGUMENT);
    EXPECT(adapt_read_header(NULL, 8, &sizes, &error) == ADAPT_ERROR_INVALID_ARGUMENT);
    EXPECT(adapt_read_header(header, 3, &sizes, &error) == ADAPT_ERROR_TRUNCATED);
    EXPECT(adapt_declared_len(NULL, 8, &length, &error) == ADAPT_ERROR_INVALID_ARGUMENT);
    EXPECT(adapt_sizes_for_input(1, 5, limits.max_memory, NULL, &error) == ADAPT_ERROR_INVALID_ARGUMENT);
    EXPECT(adapt_encode_stream(NULL, NULL, write_sink, NULL, sizes, limits.max_memory, NULL, &error) == ADAPT_ERROR_INVALID_ARGUMENT);
    EXPECT(adapt_decode_stream(read_source, NULL, NULL, NULL, limits, NULL, &error) == ADAPT_ERROR_INVALID_ARGUMENT);

    /* --- an empty input is a container; data may be NULL only then --- */
    out = NULL;
    EXPECT(adapt_encode(NULL, 0, limits, &out, &out_len, &error) == ADAPT_OK);
    EXPECT(out != NULL && out_len == ADAPT_HEADER_BYTES + ADAPT_TRAILER_BYTES);
    adapt_free(out);

    /* --- the error on success is whole and zero --- */
    memset(&error, 0x5A, sizeof(error));
    EXPECT(adapt_encode(input, input_len, limits, &file, &file_len, &error) == ADAPT_OK);
    EXPECT(error.status == ADAPT_OK && error.cause == ADAPT_OK && error.detail == 0 && error.limit == 0);

    /* --- limits refuse with their numbers, before allocating --- */
    limits.max_output = 10;
    out = NULL;
    EXPECT(adapt_decode(file, file_len, limits, &out, &out_len, &error) == ADAPT_ERROR_TOO_LARGE);
    EXPECT(error.detail == input_len && error.limit == 10 && out == NULL);
    limits = adapt_default_limits();
    limits.max_memory = 12345;
    EXPECT_OK(adapt_read_header(file, file_len, &sizes, &error), &error);
    EXPECT(adapt_decode(file, file_len, limits, &out, &out_len, &error) == ADAPT_ERROR_MEMORY_LIMIT);
    /* the in-memory decoder counts its result beside the model's bound */
    EXPECT(error.limit == 12345 && error.detail >= adapt_decode_memory_bound(sizes) && out == NULL);
    limits = adapt_default_limits();

    /* --- a writer that fails stops the call with its value as the detail --- */
    {
        source src = {file, file_len, 0};
        EXPECT(adapt_decode_stream(read_source, &src, failing_write, NULL, limits, NULL, &error) == ADAPT_ERROR_IO);
        EXPECT(error.detail == 28);
    }

    /* --- the sizes the format allows --- */
    EXPECT(adapt_sizes_valid(sizes));
    {
        adapt_sizes t = sizes;
        t.table_log = 9;
        EXPECT(!adapt_sizes_valid(t));
        t.table_log = 27;
        EXPECT(!adapt_sizes_valid(t));
        t = sizes;
        t.match_log = 25;
        EXPECT(!adapt_sizes_valid(t));
        t = sizes;
        t.window_log = 15;
        EXPECT(!adapt_sizes_valid(t));
        t.window_log = 31;
        EXPECT(!adapt_sizes_valid(t));
        t = sizes;
        t.period = ADAPT_PERIOD_MAX + 1;
        EXPECT(!adapt_sizes_valid(t));
        t = sizes;
        t.set_code = ADAPT_CONTEXT_SETS;
        EXPECT(!adapt_sizes_valid(t));
    }
    EXPECT(!adapt_segment_bytes_valid(0) && !adapt_segment_bytes_valid(ADAPT_BLOCK_BYTES - 1));
    EXPECT(adapt_segment_bytes_valid(ADAPT_BLOCK_BYTES) && !adapt_segment_bytes_valid((uint64_t)1 << 32));

    /* --- a thread count beyond the budget is refused as the budget's --- */
    EXPECT(adapt_threads_encode_memory_bound(sizes, (uint64_t)1 << 63, ADAPT_SEGMENT_BYTES) == UINT64_MAX);
    EXPECT(adapt_sizes_for_threads(input_len, limits.max_memory, (uint64_t)1 << 63, ADAPT_SEGMENT_BYTES, &sizes, &error) ==
           ADAPT_ERROR_MEMORY_LIMIT);
    EXPECT(error.limit == limits.max_memory);

#ifdef ADAPT_THREADS
    /* --- callbacks on the calling thread only, one at a time, even with
     * the segments on workers --- */
    {
        size_t long_len = input_len * 6, k; /* six segments' worth */
        uint8_t *long_input = malloc(long_len);
        source src = {long_input, long_len, 0};
        sink coded = {NULL, 0, 0}, back = {NULL, 0, 0};
        adapt_stats stats;
        adapt_sizes seg;
        uint64_t segment = ADAPT_BLOCK_BYTES; /* one block a segment: several segments, several workers */
        EXPECT(long_input != NULL);
        for (k = 0; k < 6; k++) memcpy(long_input + k * input_len, input, input_len);
        EXPECT_OK(adapt_sizes_for_threads(long_len, limits.max_memory, 4, segment, &seg, &error), &error);
        EXPECT_OK(adapt_encode_stream_threads(read_source, &src, write_sink, &coded, seg, limits.max_memory, segment, 4, 1,
                                              &stats, &error),
                  &error);
        EXPECT(stats.segments == 1 + (long_len - 1) / segment && stats.workers > 1);
        if (!callbacks_on_calling_thread) fprintf(stderr, "a callback ran on a worker thread\n");
        if (overlapping) fprintf(stderr, "two callbacks ran at once\n");
        EXPECT(callbacks_on_calling_thread && !overlapping);
        EXPECT_OK(adapt_decode_threads(coded.data, coded.len, limits, 4, write_sink, &back, &stats, &error), &error);
        EXPECT(callbacks_on_calling_thread && !overlapping);
        EXPECT(back.len == long_len && memcmp(back.data, long_input, long_len) == 0);
        printf("%lu segments on %lu workers, every callback on the calling thread\n", (unsigned long)stats.segments,
               (unsigned long)stats.workers);
        free(coded.data);
        free(back.data);
        free(long_input);
    }
    /* --- two calls at once, no shared state --- */
    {
        pthread_t a, b;
        job ja = {file, file_len, input, input_len, 0}, jb = {file, file_len, input, input_len, 0};
        EXPECT(pthread_create(&a, NULL, decode_job, &ja) == 0 && pthread_create(&b, NULL, decode_job, &jb) == 0);
        pthread_join(a, NULL);
        pthread_join(b, NULL);
        EXPECT(ja.ok && jb.ok);
    }
#endif

    adapt_free(file);
    adapt_free(NULL);
    free(input);
    printf("the contract holds\n");
    return 0;
}
