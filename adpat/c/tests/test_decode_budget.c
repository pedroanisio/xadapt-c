/* Retained input counts even when threaded decoding falls back to one
 * worker. Source: OpenAI GPT-6 via Codex. */
#include "../adapt.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; \
} } while (0)

static int count_bytes(void *context, const uint8_t *bytes, size_t length) {
    uint64_t *count = context;
    (void)bytes;
    *count += length;
    return 0;
}

int main(int argc, char **argv) {
    static const char *const names[] = {"hello.adpt", "segmented.adpt"};
    size_t i;
    CHECK(argc == 2);
    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        char path[4096];
        FILE *input;
        long length;
        uint8_t *data;
        adapt_sizes sizes;
        adapt_error error;
        adapt_stats stats;
        uint64_t required, declared, threads;
        CHECK(snprintf(path, sizeof(path), "%s/%s", argv[1], names[i]) > 0);
        input = fopen(path, "rb");
        CHECK(input != NULL);
        CHECK(fseek(input, 0, SEEK_END) == 0);
        length = ftell(input);
        CHECK(length > 0);
        rewind(input);
        data = malloc((size_t)length);
        CHECK(data != NULL);
        CHECK(fread(data, 1, (size_t)length, input) == (size_t)length);
        CHECK(fclose(input) == 0);
        CHECK(adapt_read_header(data, (size_t)length, &sizes, &error) == ADAPT_OK);
        CHECK(adapt_declared_len(data, (size_t)length, &declared, &error) == ADAPT_OK);
        required = adapt_decode_memory_bound(sizes) + (uint64_t)length;
        for (threads = 1; threads <= 2; ++threads) {
            adapt_limits limits = adapt_default_limits();
            uint64_t written = 0;
            limits.max_memory = required - 1;
            CHECK(adapt_decode_threads(data, (size_t)length, limits, threads, count_bytes, &written,
                                       &stats, &error) == ADAPT_ERROR_MEMORY_LIMIT);
            CHECK(written == 0 && error.detail == required && error.limit == limits.max_memory);
            limits.max_memory = required;
            CHECK(adapt_decode_threads(data, (size_t)length, limits, threads, count_bytes, &written,
                                       &stats, &error) == ADAPT_OK);
            CHECK(written == declared && stats.workers == 1);
        }
        free(data);
    }
    puts("decode input budget: refusal and exact-bound fallback passed");
    return 0;
}
