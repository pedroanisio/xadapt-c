/* Testable decoder-fuzz oracle. Source: OpenAI GPT-6 via Codex. */
#ifndef ADAPT_FUZZ_DECODE_H
#define ADAPT_FUZZ_DECODE_H

#include "../adapt.h"

#define ADAPT_FUZZ_MAX_OUTPUT ((uint64_t)32 << 10)
/* The largest model one execution admits. 128 MiB reaches every sizing
 * the vectors name and the ones a mutated header can ask for below it;
 * the vectors' own models decode within 31 MiB (normal) and 60 MiB
 * (max), and the smallest model is 27 MiB, so a run built with
 * -DADAPT_FUZZ_MAX_MODEL_MEMORY=64<<20 still admits every vector while
 * refusing the large tables that cost a run most of its time. */
#ifndef ADAPT_FUZZ_MAX_MODEL_MEMORY
#define ADAPT_FUZZ_MAX_MODEL_MEMORY ((uint64_t)128 << 20)
#endif

/* Refuse models above the fuzz budget; otherwise compare both decoders
 * and return the in-memory decoder's status. A disagreement aborts,
 * including when called by a regression test. */
adapt_status adapt_fuzz_decode(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#endif
