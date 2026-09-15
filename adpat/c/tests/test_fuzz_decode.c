/* Keep the real fuzz oracle on modeled decode paths as models grow.
 * Source: OpenAI GPT-6 via Codex. */
#include "../fuzz/fuzz_decode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; \
} } while (0)

static void store32(uint8_t *out, uint32_t value) {
    unsigned i;
    for (i = 0; i < 4; ++i) out[i] = (uint8_t)(value >> (8 * i));
}

static int test_limits(void) {
    size_t size = (size_t)ADAPT_FUZZ_MAX_OUTPUT, n;
    uint8_t *data = calloc(1, size + 31);
    CHECK(data != NULL);
    for (n = size; n <= size + 1; ++n) {
        /* An otherwise valid stored block exactly at or above the cap. */
        memcpy(data, "ADPT\x13\x0a\x0a\x10", 8);
        data[8] = 2;
        store32(data + 9, (uint32_t)n);
        store32(data + 13, (uint32_t)n);
        memset(data + 17, 0, n);
        data[17 + n] = 3;
        store32(data + 18 + n, (uint32_t)n);
        memset(data + 22 + n, 0, 4);
        store32(data + 26 + n, adapt_crc32(data + 17, n));
        CHECK(adapt_fuzz_decode(data, n + 30) == (n == size ? ADAPT_OK : ADAPT_ERROR_TOO_LARGE));
    }
    /* A valid oversized-model header is refused without allocating it. */
    data[5] = 26;
    CHECK(adapt_fuzz_decode(data, size + 31) == ADAPT_ERROR_MEMORY_LIMIT);
    CHECK(adapt_fuzz_decode(NULL, 0) == ADAPT_ERROR_TRUNCATED);
    free(data);
    return 0;
}

int main(int argc, char **argv) {
    static const char *const names[] = {"abc.adpt", "abc-fast.adpt", "abc-max.adpt", "cjk.adpt"};
    size_t i;
    CHECK(argc == 2);
    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        char path[4096];
        FILE *input;
        long length;
        uint8_t *data;
        adapt_sizes sizes;
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
        CHECK(adapt_read_header(data, (size_t)length, &sizes, NULL) == ADAPT_OK);
        CHECK(data[adapt_header_len(data)] == 1); /* modeled block */
        CHECK(adapt_fuzz_decode(data, (size_t)length) == ADAPT_OK);
        CHECK(LLVMFuzzerTestOneInput(data, (size_t)length) == 0);
        free(data);
    }
    CHECK(test_limits() == 0);
    puts("fuzz oracle: modeled fixtures, output boundary and model refusal passed");
    return 0;
}
