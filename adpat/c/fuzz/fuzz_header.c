/* The decoder's header under libFuzzer: the same oracle as fuzz_decode, on
 * bytes whose header checksum is made good first. A header is refused on its
 * CRC-32 before any field is read, so mutations of the version, the sizes,
 * the period, the sizing or policy byte, the BMP descriptor and the metadata
 * record hardly ever reach the code that reads them; this harness recomputes
 * the checksum over the header the bytes name, so every field is exercised
 * and only the payload and the trailer keep their own defences.
 *
 *   make fuzz-header-smoke
 *
 * Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-17. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fuzz_decode.h"

/* CRC-32 (IEEE 802.3), as the container header carries it. */
static uint32_t crc32_ieee(const uint8_t *data, size_t len) {
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    int k;
    for (i = 0; i < len; i++) {
        c ^= data[i];
        for (k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    uint8_t *copy;
    if (size < ADAPT_HEADER_BYTES) return 0;
    copy = malloc(size);
    if (copy == NULL) return 0;
    memcpy(copy, data, size);
    {
        size_t n = adapt_header_len(copy);
        if (n >= 4 && n <= size) {
            uint32_t crc = crc32_ieee(copy, n - 4);
            copy[n - 4] = (uint8_t)crc;
            copy[n - 3] = (uint8_t)(crc >> 8);
            copy[n - 2] = (uint8_t)(crc >> 16);
            copy[n - 1] = (uint8_t)(crc >> 24);
        }
    }
    (void)adapt_fuzz_decode(copy, size);
    free(copy);
    return 0;
}
