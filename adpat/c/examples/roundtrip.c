/* In memory: encode bytes into one container, prove it, decode it back,
 * and read what a refusal says.
 *
 *   cc -std=c99 -I.. roundtrip.c ../build/libadapt.a -pthread
 *
 * Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-17. */
#include "example.h"

int main(void) {
    size_t input_len, file_len, back_len;
    uint8_t *input = example_input(&input_len);
    uint8_t *file = NULL, *back = NULL;
    adapt_limits limits = adapt_default_limits(); /* 1 GiB of output, 674 MiB of memory */
    adapt_error error;
    char message[160];

    /* adapt_encode_verified() codes the bytes and proves the container
     * decodes to them before returning it; `file` is malloc'd, ours to
     * adapt_free(). The sizes come from the input's length and the budget. */
    EXPECT_OK(adapt_encode_verified(input, input_len, limits, &file, &file_len, &error), &error);
    EXPECT(error.status == ADAPT_OK && error.detail == 0 && error.limit == 0);
    printf("%zu bytes -> %zu bytes (%.1f %%)\n", input_len, file_len, 100.0 * (double)file_len / (double)input_len);

    /* adapt_decode() publishes its result only after the checksum matched. */
    EXPECT_OK(adapt_decode(file, file_len, limits, &back, &back_len, &error), &error);
    EXPECT(back_len == input_len && memcmp(back, input, input_len) == 0);
    adapt_free(back);
    back = NULL;

    /* A cap the caller sets refuses a valid container before anything is
     * allocated, and the refusal names its numbers: the declared length
     * against max_output, the model's memory against max_memory. */
    limits.max_output = input_len - 1;
    EXPECT(adapt_decode(file, file_len, limits, &back, &back_len, &error) == ADAPT_ERROR_TOO_LARGE);
    EXPECT(error.detail == input_len && error.limit == input_len - 1 && back == NULL);
    adapt_error_message(&error, message, sizeof(message));
    printf("refused: %s\n", message);

    limits = adapt_default_limits();
    limits.max_memory = 1;
    EXPECT(adapt_decode(file, file_len, limits, &back, &back_len, &error) == ADAPT_ERROR_MEMORY_LIMIT);
    EXPECT(error.limit == 1 && error.detail > 1 && back == NULL);
    adapt_error_message(&error, message, sizeof(message));
    printf("refused: %s\n", message);

    /* Damage is found, never passed through: a flipped byte in the
     * payload fails the checksum (or the framing before it). */
    file[file_len / 2] ^= 0x40;
    EXPECT(adapt_decode(file, file_len, adapt_default_limits(), &back, &back_len, &error) != ADAPT_OK);
    EXPECT(back == NULL);
    adapt_error_message(&error, message, sizeof(message));
    printf("damaged: %s\n", message);

    adapt_free(file);
    adapt_free(NULL); /* accepted */
    free(input);
    return 0;
}
