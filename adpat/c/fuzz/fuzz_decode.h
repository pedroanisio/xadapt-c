/* Testable decoder-fuzz oracle. Source: OpenAI GPT-6 via Codex. */
#ifndef ADAPT_FUZZ_DECODE_H
#define ADAPT_FUZZ_DECODE_H

#include "../adapt.h"

#define ADAPT_FUZZ_MAX_OUTPUT ((uint64_t)32 << 10)
#define ADAPT_FUZZ_MAX_MODEL_MEMORY ((uint64_t)128 << 20)

/* Refuse models above the fuzz budget; otherwise compare both decoders
 * and return the in-memory decoder's status. A disagreement aborts,
 * including when called by a regression test. */
adapt_status adapt_fuzz_decode(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#endif
