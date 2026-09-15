/*
 * adapt_encode.c — the encoding entry points: the sizes for an input and a
 * budget, the streaming and in-memory encoders, and the proof that what
 * was written decodes back. A port of the encoding half of the crate's
 * `container.rs`, kept in its order and under its names; the model and
 * the framing rules it shares with the decoder are in adapt_model.h.
 *
 * Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-12.
 *
 * Three block methods keep the encoder's and decoder's model states equal
 * no matter which way a block went: modeled (1), the payload an
 * arithmetic-coded stream; stored, observed (0), the raw bytes, which the
 * model has already learned and the decoder feeds through it too; stored,
 * skipped (2), the raw bytes of a block a histogram and a repeat scan found
 * no evidence of compressing, which neither side runs the predictor over.
 * The trailer carries the length and checksum because the encoder learns
 * them last: it reads its input once and writes as it goes. The class-
 * history width is chosen from the first block by three trial encodings of
 * its first 32 KiB and carried in the header; a first block the model
 * would not run on declines the context.
 */
/* For the tables' mapping (adapt_model.h): mmap, MAP_ANONYMOUS, madvise. */
#if defined(__linux__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1
#endif
#include "adapt.h"
#include "adapt_model.h"

#include <stdlib.h>
#include <string.h>
#ifdef ADAPT_THREADS
#include <pthread.h>
#endif

/* Bytes of the first block the encoder codes under each class-history
 * width before choosing one for the file. */
#define CLASS_SAMPLE ((size_t)1 << 15)
/* A lean context set must code the sample this fraction smaller than the
 * full set to be considered, and the whole first block this fraction
 * smaller to be kept. */
#define SET_MARGIN(size) ((size) - (size) / 512)
#define PROOF_MARGIN(size) ((size) - (size) / 256)
/* Coded bytes a sample must produce before its ranking of the widths
 * counts. */
#define CLASS_EVIDENCE 2048
/* Order-0 entropy, in 1/256 bits per byte, above which a block may be
 * stored without modeling if it has no repeated sequences: 7.94 bits. */
#define SKIP_ENTROPY_256 2033
/* Blocks the encoder may run ahead of its verifier thread. Built with
 * ADAPT_THREADS the port runs the same queue on a POSIX thread; without
 * it each block is proven before the next is coded, and the memory the
 * queue would take is counted either way so the two refuse the same
 * budgets. */
#define VERIFY_QUEUE 2
/* Bytes of the repeat scans' window. */
#define SCAN 8
/* The most bytes a block's coded form can take: at most four settled bytes
 * per bit, eight bits for the byte that crosses the line, four final. */
#define CODED_SLACK 36

/* ------------------------------------------------------------------------
 * Sizes
 * --------------------------------------------------------------------- */

int adapt_sizes_valid(adapt_sizes sizes) {
    return sizes_valid(sizes);
}

uint64_t adapt_encode_memory_bound(adapt_sizes sizes) {
    /* With the x86 transform the encoder keeps the untransformed block as
     * well, for the proof to compare against. */
    return adapt_decode_memory_bound(sizes) + ((sizes.x86 || sizes.bmp) ? (uint64_t)ADAPT_BLOCK_BYTES : 0);
}

uint64_t adapt_verified_memory_bound(adapt_sizes sizes) {
    return adapt_encode_memory_bound(sizes) + adapt_model_bytes(sizes) +
           (uint64_t)(VERIFY_QUEUE + 2) * 2 * ((uint64_t)ADAPT_BLOCK_BYTES + ADAPT_BLOCK_HEADER_BYTES + CODED_SLACK);
}

adapt_sizes adapt_sizes_max(adapt_sizes sizes) {
    sizes.max = 1;
    sizes.fast = 0;
    sizes.adaptive = 0;
    sizes.bmp = 0;
    sizes.bmp_offset = sizes.bmp_width = sizes.bmp_rows = 0;
    return sizes;
}

adapt_sizes adapt_sizes_fast(adapt_sizes sizes) {
    sizes.fast = 1;
    sizes.max = 0;
    sizes.compact = 0;
    sizes.adaptive = 0;
    sizes.bmp = 0;
    sizes.bmp_offset = sizes.bmp_width = sizes.bmp_rows = 0;
    if (sizes.table_log > ADAPT_FAST_TABLE_LOG_MAX) {
        sizes.table_log = ADAPT_FAST_TABLE_LOG_MAX;
    }
    sizes.set_code = 0;
    sizes.period = 0;
    return sizes;
}

/* container.rs — detect_period: byte equality per lag up to ADAPT_PERIOD_MAX
 * on the first sample; the strongest short lag is the stride, the period
 * the strongest multiple of it that stands out from the other multiples
 * and from its neighbours. Integer arithmetic, so both codecs decide alike. */
static int compare_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

uint16_t adapt_detect_period(const uint8_t *sample, size_t n) {
    const size_t STRIDE_MAX = 64;
    size_t max_lag, lag, stride = 0, period = 0, k, count;
    uint32_t stride_score = 0, best = 0, median, neighbours, *scores, *multiples;
    if (n > ((size_t)1 << 15)) {
        n = (size_t)1 << 15;
    }
    if (n < 4096) {
        return 0;
    }
    max_lag = ADAPT_PERIOD_MAX < n / 4 ? ADAPT_PERIOD_MAX : n / 4;
    scores = calloc(max_lag + 2, sizeof(uint32_t));
    if (scores == NULL) {
        return 0;
    }
    for (lag = 1; lag <= max_lag; lag++) {
        size_t i, equal = 0;
        for (i = lag; i < n; i++) {
            equal += sample[i] == sample[i - lag];
        }
        scores[lag] = (uint32_t)((uint64_t)equal << 16) / (uint32_t)(n - lag);
    }
    for (lag = 1; lag <= STRIDE_MAX && lag <= max_lag; lag++) {
        if (scores[lag] >= stride_score) {
            stride_score = scores[lag];
            stride = lag;
        }
    }
    if (stride == 0 || stride_score < (1u << 14)) {
        free(scores);
        return 0;
    }
    for (lag = 2 * stride; lag <= max_lag; lag += stride) {
        if (scores[lag] > best) {
            best = scores[lag];
            period = lag;
        }
    }
    if (period == 0 || best < stride_score / 4 * 3) {
        free(scores);
        return 0;
    }
    count = max_lag / stride - 1;
    multiples = malloc(count * sizeof(uint32_t));
    if (multiples == NULL) {
        free(scores);
        return 0;
    }
    for (k = 2; k <= max_lag / stride; k++) {
        multiples[k - 2] = scores[k * stride];
    }
    qsort(multiples, count, sizeof(uint32_t), compare_u32);
    median = multiples[count / 2];
    free(multiples);
    neighbours = scores[period - 1] > scores[period + 1] ? scores[period - 1] : scores[period + 1];
    free(scores);
    if (best < median / 20 * 21 || best < neighbours / 2 * 3) {
        return 0;
    }
    return (uint16_t)period;
}

/* floor(log2(max(n, 1))) + 1 */
static unsigned bits(uint64_t n) {
    unsigned b = 0;
    if (n == 0) {
        n = 1;
    }
    while (n != 0) {
        b++;
        n >>= 1;
    }
    return b;
}

static uint8_t clamp_u8(unsigned v, uint8_t lo, uint8_t hi) {
    return v < lo ? lo : (v > hi ? hi : (uint8_t)v);
}

uint8_t adapt_table_log_for(uint64_t length) {
    unsigned b = bits(length);
    uint8_t sweet = clamp_u8(b + 5, 18, 20);
    uint8_t grown = clamp_u8(b > 2 ? b - 2 : 0, 18, TABLE_LOG_MAX);
    return sweet > grown ? sweet : grown;
}

static adapt_status sizes_for_input_profile(int length_known, uint64_t length, uint64_t max_memory, int max, unsigned adaptive,
                                            adapt_sizes *sizes, adapt_error *error) {
    adapt_sizes floor = {0}, s = {0};
    uint64_t (*bound)(adapt_sizes);
    uint8_t cap;
    if (sizes == NULL) {
        return fail(error, ADAPT_ERROR_INVALID_ARGUMENT, 0, 0);
    }
    floor.table_log = TABLE_LOG_MIN;
    floor.match_log = MATCH_LOG_MIN;
    floor.window_log = WINDOW_LOG_MIN;
    floor.class_code = 0;
    floor.set_code = 0;
    floor.fast = 0;
    floor.period = 0;
    floor.segmented = 0;
    floor.max = (uint8_t)max;
    floor.x86 = 0;
    floor.archive = 0;
    floor.adaptive = (uint8_t)adaptive;
    bound = (!length_known && adapt_verified_memory_bound(floor) <= max_memory) ? adapt_verified_memory_bound
                                                                                : adapt_encode_memory_bound;
    if (length_known) {
        s.table_log = clamp_u8(bits(length) + 5, 18, 20);
        s.match_log = clamp_u8(bits(length), 12, 20);
    } else {
        s.table_log = 20;
        s.match_log = 20;
    }
    s.window_log = WINDOW_LOG_MIN;
    s.class_code = 0;
    s.set_code = 0;
    s.fast = 0;
    s.period = 0;
    s.segmented = 0;
    s.max = (uint8_t)max;
    s.x86 = 0;
    s.archive = 0;
    s.adaptive = (uint8_t)adaptive;
    while (bound(s) > max_memory) {
        uint64_t table_bytes = (uint64_t)16 << s.table_log;
        uint64_t match_bytes = (uint64_t)4 << s.match_log;
        if (s.table_log > TABLE_LOG_MIN && (table_bytes >= match_bytes || s.match_log == MATCH_LOG_MIN)) {
            s.table_log--;
        } else if (s.match_log > MATCH_LOG_MIN) {
            s.match_log--;
        } else {
            break;
        }
    }
    /* The largest window the budget holds, no larger than the input. */
    cap = length_known ? clamp_u8(bits(length > 0 ? length - 1 : 0), WINDOW_LOG_MIN, WINDOW_LOG_MAX) : WINDOW_LOG_MAX;
    for (;;) {
        adapt_sizes wider = s;
        if (s.window_log >= cap) {
            break;
        }
        wider.window_log = s.window_log + 1;
        if (bound(wider) > max_memory) {
            break;
        }
        s.window_log = wider.window_log;
    }
    /* The table past its sweet spot, once the window has all it can use. */
    if (length_known) {
        uint8_t target = adapt_table_log_for(length);
        while (s.window_log == cap && s.table_log < target) {
            adapt_sizes larger = s;
            larger.table_log = s.table_log + 1;
            if (bound(larger) > max_memory) {
                break;
            }
            s.table_log = larger.table_log;
        }
    }
    if (check_memory(adapt_encode_memory_bound(s), max_memory, error) != ADAPT_OK) {
        return ADAPT_ERROR_MEMORY_LIMIT;
    }
    *sizes = s;
    return fail(error, ADAPT_OK, 0, 0);
}

adapt_status adapt_sizes_for_input(int length_known, uint64_t length, uint64_t max_memory, adapt_sizes *sizes,
                                   adapt_error *error) {
    return sizes_for_input_profile(length_known, length, max_memory, 0, 0, sizes, error);
}

adapt_status adapt_sizes_for_input_max(int length_known, uint64_t length, uint64_t max_memory, adapt_sizes *sizes,
                                       adapt_error *error) {
    return sizes_for_input_profile(length_known, length, max_memory, 1, 0, sizes, error);
}

adapt_status adapt_sizes_for_input_adaptive(int length_known, uint64_t length, uint64_t max_memory,
                                           unsigned mode, adapt_sizes *sizes, adapt_error *error) {
    if (mode > 3) return fail(error, ADAPT_ERROR_INVALID_ARGUMENT, mode, 3);
    return sizes_for_input_profile(length_known, length, max_memory, 0, mode, sizes, error);
}

adapt_status adapt_sizes_for_input_max_adaptive(int length_known, uint64_t length, uint64_t max_memory,
                                           unsigned mode, adapt_sizes *sizes, adapt_error *error) {
    if (mode > 3) return fail(error, ADAPT_ERROR_INVALID_ARGUMENT, mode, 3);
    return sizes_for_input_profile(length_known, length, max_memory, 1, mode, sizes, error);
}

/* segments.rs — the crate's threads: the most bytes one segment can take
 * in the file, the memory a run of threads needs, and the sizes for one
 * thread's share of a budget. This port codes segments in order, but a
 * file's sizes are the crate's for the same budget and thread count. */
uint64_t adapt_segment_out_bound(uint64_t segment_bytes) {
    return 1 + segment_bytes + (segment_bytes + ADAPT_BLOCK_BYTES - 1) / ADAPT_BLOCK_BYTES * ADAPT_BLOCK_HEADER_BYTES;
}

uint64_t adapt_threads_encode_memory_bound(adapt_sizes sizes, uint64_t threads, uint64_t segment_bytes) {
    return (adapt_model_bytes(sizes) + block_workspace() + adapt_segment_out_bound(segment_bytes)) * threads + HEADROOM;
}

/* A policy affects resource allocation only; the selected sizes travel in
 * the ordinary header (FMT-2/FMT-6). Bounds include every live worker. */
static uint64_t context_allocation_bound(adapt_sizes s, uint64_t workers,
                                          uint64_t segment_bytes, int verified) {
    if (workers >= 2) {
        uint64_t per_worker = adapt_model_bytes(s) + block_workspace() + adapt_segment_out_bound(segment_bytes);
        if (workers > (UINT64_MAX - HEADROOM) / per_worker) return UINT64_MAX;
        return per_worker * workers + HEADROOM;
    }
    return verified ? adapt_verified_memory_bound(s) : adapt_encode_memory_bound(s);
}

adapt_status adapt_sizes_prioritize_context(int length_known, uint64_t length, uint64_t max_memory,
                                            uint64_t threads, uint64_t segment_bytes,
                                            adapt_sizes *sizes, adapt_error *error) {
    adapt_sizes s, floor, next;
    uint64_t workers = length_known ? threads : 1;
    uint8_t target, cap;
    int verified = 0;
    if (sizes == NULL || !sizes_valid(*sizes) || threads == 0 ||
        (threads >= 2 && !adapt_segment_bytes_valid(segment_bytes)))
        return fail(error, ADAPT_ERROR_INVALID_ARGUMENT, 0, 0);
    s = *sizes;
    floor = s;
    floor.table_log = TABLE_LOG_MIN;
    floor.match_log = MATCH_LOG_MIN;
    floor.window_log = WINDOW_LOG_MIN;
    if (!length_known && adapt_verified_memory_bound(floor) <= max_memory) verified = 1;
    /* The supplied sizes must already fit: this policy never silently shrinks
     * a caller's table or recall index to repair an invalid budget. */
    if (check_memory(context_allocation_bound(s, workers, segment_bytes, verified), max_memory, error) != ADAPT_OK)
        return ADAPT_ERROR_MEMORY_LIMIT;
    if (workers >= 2 && length > segment_bytes) length = segment_bytes;
    target = length_known ? adapt_table_log_for(length) : TABLE_LOG_MAX;
    if (s.fast && target > ADAPT_FAST_TABLE_LOG_MAX) target = ADAPT_FAST_TABLE_LOG_MAX;
    cap = length_known ? clamp_u8(bits(length > 0 ? length - 1 : 0), WINDOW_LOG_MIN, WINDOW_LOG_MAX) : WINDOW_LOG_MAX;
    s.window_log = WINDOW_LOG_MIN;
    while (s.table_log < target) {
        next = s;
        next.table_log++;
        if (context_allocation_bound(next, workers, segment_bytes, verified) > max_memory) break;
        s = next;
    }
    /* A budget that stops the max profile's table at ADAPT_COMPACT_TABLE_LOG
     * blocks or fewer, short of the target, buys more with the compact side
     * tables (ADAPT_MAX_COMPACT_VERSION): what they free is a doubling of
     * the table, measured 1.9-2.2 % smaller on enwik8 at 94-128 MiB. A
     * table already past that size keeps the full tables, which win there
     * (256 MiB: +0.34 % with the compact ones). The switch never leaves
     * the table smaller than the full tables allowed. */
    if (s.max && !s.adaptive && !s.bmp && !s.compact && s.table_log <= ADAPT_COMPACT_TABLE_LOG &&
        s.table_log < target) {
        next = s;
        next.compact = 1;
        if (context_allocation_bound(next, workers, segment_bytes, verified) <= max_memory) {
            s = next;
            while (s.table_log < target) {
                next = s;
                next.table_log++;
                if (context_allocation_bound(next, workers, segment_bytes, verified) > max_memory) break;
                s = next;
            }
        }
    }
    while (s.window_log < cap) {
        next = s;
        next.window_log++;
        if (context_allocation_bound(next, workers, segment_bytes, verified) > max_memory) break;
        s = next;
    }
    *sizes = s;
    return fail(error, ADAPT_OK, 0, 0);
}

static adapt_status sizes_for_threads_profile(uint64_t length, uint64_t max_memory, uint64_t threads,
                                              uint64_t segment_bytes, int max, unsigned adaptive, adapt_sizes *sizes, adapt_error *error) {
    adapt_sizes floor = {0}, s = {0};
    uint64_t least, share;
    adapt_status status;
    if (sizes == NULL) {
        return fail(error, ADAPT_ERROR_INVALID_ARGUMENT, 0, 0);
    }
    if (threads == 0 || !adapt_segment_bytes_valid(segment_bytes)) {
        return fail(error, ADAPT_ERROR_BAD_HEADER, 0, 0);
    }
    floor.table_log = TABLE_LOG_MIN;
    floor.match_log = MATCH_LOG_MIN;
    floor.window_log = WINDOW_LOG_MIN;
    floor.class_code = 0;
    floor.set_code = 0;
    floor.fast = 0;
    floor.period = 0;
    floor.segmented = 1;
    floor.max = (uint8_t)max;
    floor.x86 = 0;
    floor.archive = 0;
    floor.adaptive = (uint8_t)adaptive;
    least = adapt_threads_encode_memory_bound(floor, threads, segment_bytes);
    status = check_memory(least, max_memory, error);
    if (status != ADAPT_OK) {
        return status;
    }
    share = (max_memory - HEADROOM) / threads - adapt_segment_out_bound(segment_bytes);
    status = sizes_for_input_profile(1, length < segment_bytes ? length : segment_bytes, share + HEADROOM, max, adaptive, &s, error);
    if (status != ADAPT_OK) {
        return status;
    }
    s.segmented = 1;
    status = check_memory(adapt_threads_encode_memory_bound(s, threads, segment_bytes), max_memory, error);
    if (status != ADAPT_OK) {
        return status;
    }
    *sizes = s;
    return fail(error, ADAPT_OK, 0, 0);
}

adapt_status adapt_sizes_for_threads(uint64_t length, uint64_t max_memory, uint64_t threads, uint64_t segment_bytes,
                                     adapt_sizes *sizes, adapt_error *error) {
    return sizes_for_threads_profile(length, max_memory, threads, segment_bytes, 0, 0, sizes, error);
}

adapt_status adapt_sizes_for_threads_adaptive(uint64_t length, uint64_t max_memory, uint64_t threads,
                                             uint64_t segment_bytes, unsigned mode, adapt_sizes *sizes, adapt_error *error) {
    if (mode > 3) return fail(error, ADAPT_ERROR_INVALID_ARGUMENT, mode, 3);
    return sizes_for_threads_profile(length, max_memory, threads, segment_bytes, 0, mode, sizes, error);
}

adapt_status adapt_sizes_for_threads_max_adaptive(uint64_t length, uint64_t max_memory, uint64_t threads,
                                             uint64_t segment_bytes, unsigned mode, adapt_sizes *sizes, adapt_error *error) {
    if (mode > 3) return fail(error, ADAPT_ERROR_INVALID_ARGUMENT, mode, 3);
    return sizes_for_threads_profile(length, max_memory, threads, segment_bytes, 1, mode, sizes, error);
}

adapt_status adapt_sizes_for_threads_max(uint64_t length, uint64_t max_memory, uint64_t threads, uint64_t segment_bytes,
                                         adapt_sizes *sizes, adapt_error *error) {
    return sizes_for_threads_profile(length, max_memory, threads, segment_bytes, 1, 0, sizes, error);
}

/* ------------------------------------------------------------------------
 * The block decisions
 * --------------------------------------------------------------------- */

/* Sum of -log2(p) * count over the histogram, in 1/256 bits per byte,
 * using an integer log table so the decision is platform-independent. */
static uint64_t log2_256(uint64_t x) {
    /* log2(1 + f/128) * 256 for f in 0..128, generated as integers */
    static const uint16_t FRAC[128] = {
        0,   3,   6,   9,   11,  14,  17,  20,  22,  25,  28,  30,  33,  36,  38,  41,  44,  46,  49,  51,  54,  56,
        59,  61,  63,  66,  68,  71,  73,  75,  78,  80,  82,  85,  87,  89,  92,  94,  96,  98,  100, 103, 105, 107,
        109, 111, 113, 116, 118, 120, 122, 124, 126, 128, 130, 132, 134, 136, 138, 140, 142, 144, 146, 148, 150, 152,
        154, 155, 157, 159, 161, 163, 165, 167, 169, 170, 172, 174, 176, 178, 179, 181, 183, 185, 186, 188, 190, 192,
        193, 195, 197, 198, 200, 202, 203, 205, 207, 208, 210, 212, 213, 215, 216, 218, 220, 221, 223, 224, 226, 228,
        229, 231, 232, 234, 235, 237, 238, 240, 241, 243, 244, 246, 247, 249, 250, 252, 253, 255,
    };
    uint64_t msb, frac;
    if (x == 0) {
        return 0;
    }
    msb = 63;
    while (((x >> msb) & 1) == 0) {
        msb--;
    }
    frac = msb >= 7 ? (x >> (msb - 7)) & 127 : (x << (7 - msb)) & 127;
    return msb * 256 + FRAC[frac];
}

static uint64_t entropy_256(const uint8_t *block, size_t n) {
    uint32_t hist[256];
    uint64_t total = 0;
    size_t i;
    memset(hist, 0, sizeof(hist));
    for (i = 0; i < n; i++) {
        hist[block[i]]++;
    }
    for (i = 0; i < 256; i++) {
        if (hist[i] > 0) {
            total += (uint64_t)hist[i] * (log2_256(n) - log2_256(hist[i]));
        }
    }
    return total / (n > 0 ? n : 1);
}

/* Isolated motifs do not justify eight model updates for every byte. */
static size_t repeat_threshold(size_t n) {
    size_t needed = (n + 127) / 128;
    return needed > 32 ? needed : 32;
}

/* Whether repeated sequences cover enough of `block` to justify modeling: the part of
 * model_has_repeat that needs no model, for the choice made before the
 * model exists. `scan` is the caller's 2^16-slot scratch. */
static int repeats_within(const uint8_t *block, size_t n, uint32_t *scan) {
    uint64_t c8 = 0;
    size_t i;
    size_t covered = 0, end = 0, needed = repeat_threshold(n);
    for (i = 0; i < SCAN_SLOTS; i++) {
        scan[i] = UINT32_MAX;
    }
    for (i = 0; i < n; i++) {
        size_t slot;
        uint32_t earlier;
        c8 = (c8 << 8) | block[i];
        if (i + 1 < SCAN) {
            continue;
        }
        slot = (size_t)(mix64(c8) >> 48);
        earlier = scan[slot];
        scan[slot] = (uint32_t)i;
        if (earlier != UINT32_MAX && memcmp(block + earlier + 1 - SCAN, block + i + 1 - SCAN, SCAN) == 0) {
            size_t fresh = i + 1 - end;
            covered += fresh < SCAN ? fresh : SCAN;
            end = i + 1;
            if (covered >= needed) {
                return 1;
            }
        }
    }
    return 0;
}

/* Whether repeated byte coverage justifies modeling: at least 1/128 of
 * the block and at least 32 bytes, with overlaps counted once. Matches
 * can be local or in the recall window. The model is not trained here. */
static int model_has_repeat(const model *m, const uint8_t *block, size_t n, uint32_t *local) {
    uint64_t start = m->n;
    uint64_t c8 = m->c8;
    uint64_t window = (uint64_t)m->hmask + 1;
    size_t i;
    size_t covered = 0, end = 0, needed = repeat_threshold(n);
    for (i = 0; i < SCAN_SLOTS; i++) {
        local[i] = UINT32_MAX;
    }
    for (i = 0; i < n; i++) {
        const uint8_t *here;
        size_t slot;
        uint32_t earlier, cand;
        uint64_t pos, age, in_block;
        int repeated;
        c8 = (c8 << 8) | block[i];
        if (i + 1 < SCAN) {
            continue;
        }
        here = block + i + 1 - SCAN;
        /* within the block */
        slot = (size_t)(mix64(c8) >> 48);
        earlier = local[slot];
        local[slot] = (uint32_t)i;
        repeated = earlier != UINT32_MAX && memcmp(block + earlier + 1 - SCAN, here, SCAN) == 0;
        /* against the window, where the recall table would find it */
        pos = start + i + 1;
        cand = m->mtab[recall_key(c8, m->mmask)];
        age = (uint32_t)((uint32_t)pos - cand);
        /* The candidate ends at pos - age; its sequence must lie before
         * this block (the ring holds nothing newer) and inside the window. */
        in_block = pos - start;
        if (!repeated && age >= in_block && age + SCAN <= in_block + window && pos - age >= SCAN) {
            uint64_t from = pos - age - SCAN;
            size_t k;
            for (k = 0; k < SCAN; k++) {
                if (m->hist[(size_t)(from + k) & m->hmask] != here[k]) {
                    break;
                }
            }
            repeated = k == SCAN;
        }
        if (repeated) {
            size_t fresh = i + 1 - end;
            covered += fresh < SCAN ? fresh : SCAN;
            end = i + 1;
            if (covered >= needed) {
                return 1;
            }
        }
    }
    return 0;
}

/* Model and code one complete block into `coded` (at least n + CODED_SLACK
 * bytes). An unprofitable prefix can precede long repeats, so the decision
 * to store is made only after measuring all of it; once the coded prefix
 * alone exceeds the raw size it cannot shrink, so the rest is observed
 * without retaining unusable output. Returns the payload length, or 0
 * when the raw block wins — both sides still observe every byte then. */
static size_t code_block(model *m, const uint8_t *block, size_t n, uint8_t *coded, size_t coded_cap) {
    arith_enc e;
    size_t i, payload;
    arith_enc_init(&e, coded, coded_cap);
    for (i = 0; i < n; i++) {
        if (e.len >= n) {
            observe_bytes(m, block + i, 1);
            continue;
        }
        encode_byte(m, &e, block[i]);
    }
    payload = arith_enc_finish(&e);
    if (e.overflow || payload >= n) {
        return 0;
    }
    return payload;
}

/* Code one block of `n` bytes that begins `position` bytes into the
 * input, under m, into `framed`: the transform the file has, in place;
 * then stored and skipped when the block is incompressible and repeats
 * nothing the model knows, modeled when the coder beats the raw bytes,
 * stored and observed otherwise; and the block header before the payload.
 * Returns the framed length. */
static size_t frame_block(model *m, adapt_sizes sizes, uint8_t *block, size_t n, uint64_t position, uint32_t *scan,
                          uint8_t *framed, size_t framed_cap) {
    uint8_t method;
    size_t payload_len;
    if (sizes.bmp) {
        bmp_transform(block, n, position, sizes, 0);
    } else if (sizes.x86) {
        x86_forward(block, n);
    }
    if (entropy_256(block, n) >= SKIP_ENTROPY_256 && !model_has_repeat(m, block, n, scan)) {
        skip_bytes(m, block, n);
        memcpy(framed + ADAPT_BLOCK_HEADER_BYTES, block, n);
        payload_len = n;
        method = METHOD_STORED_SKIPPED;
    } else {
        payload_len = code_block(m, block, n, framed + ADAPT_BLOCK_HEADER_BYTES, framed_cap - ADAPT_BLOCK_HEADER_BYTES);
        if (payload_len != 0) {
            method = METHOD_MODELED;
        } else {
            memcpy(framed + ADAPT_BLOCK_HEADER_BYTES, block, n);
            payload_len = n;
            method = METHOD_STORED_OBSERVED;
        }
    }
    framed[0] = method;
    framed[1] = (uint8_t)n;
    framed[2] = (uint8_t)(n >> 8);
    framed[3] = (uint8_t)(n >> 16);
    framed[4] = (uint8_t)(n >> 24);
    framed[5] = (uint8_t)payload_len;
    framed[6] = (uint8_t)(payload_len >> 8);
    framed[7] = (uint8_t)(payload_len >> 16);
    framed[8] = (uint8_t)(payload_len >> 24);
    return ADAPT_BLOCK_HEADER_BYTES + payload_len;
}

static unsigned popcount16(uint32_t x) {
    unsigned n = 0;
    while (x != 0) {
        n += x & 1;
        x >>= 1;
    }
    return n;
}

/* One trial: code `sample` under a fresh model with these codes and return
 * the coded length, or the sample's length when the raw bytes win. */
static adapt_status trial(adapt_sizes sizes, uint8_t class_code, uint8_t set_code, const uint8_t *sample, size_t n,
                          uint8_t *coded, size_t coded_cap, size_t *size_out, adapt_error *error) {
    model *m;
    size_t payload;
    sizes.class_code = class_code;
    sizes.set_code = set_code;
    m = model_new(sizes);
    if (m == NULL) {
        return fail(error, ADAPT_ERROR_ALLOCATION, 0, 0);
    }
    payload = code_block(m, sample, n, coded, coded_cap);
    *size_out = payload == 0 ? n : payload;
    model_free(m);
    return ADAPT_OK;
}

/* The class-history width and the context set for a file, as header
 * codes, from the first block: the crate's `choose_model`. The width first,
 * from the block's first CLASS_SAMPLE bytes, each width on the full set, a
 * tie to the narrower; a first sample too compressible to rank anything
 * declines the class context and keeps every other. Then the context sets
 * on the same sample, family by family — the text family (sets 1, 2, 6)
 * drops record and field first, the binary family (3, 4, 5, 7) word and
 * field first, and a family whose head cannot beat the full set by
 * SET_MARGIN of the sample is not pursued; a block no longer than two
 * samples tries the two heads only. The set that codes the sample smallest
 * wins, ties to fewer contexts then the lower code; a lean choice on a
 * long block is proved on the whole block against the full set and
 * reverts unless it wins there by PROOF_MARGIN. The margins are what a
 * sample can be trusted for: measured on 16 MiB of enwik8, a lean set that
 * won its 32 KiB sample by a hair and its 256 KiB block by less than a
 * thousandth lost the whole file by seven. */
static adapt_status choose_model(adapt_sizes sizes, const uint8_t *block, size_t n, uint8_t *coded, size_t coded_cap,
                                 uint8_t *class_out, uint8_t *set_out, uint16_t *period_out, adapt_error *error) {
    size_t head = n < CLASS_SAMPLE ? n : CLASS_SAMPLE;
    /* The period first: the contexts every trial runs depend on it. */
    sizes.period = sizes.fast ? 0 : adapt_detect_period(block, head);
    *period_out = sizes.period;
    size_t coded_len[3];
    size_t none, best_size, full;
    uint8_t code, class_code, set_code;
    int deep = n > 2 * head;
    /* choice: smallest sample, then fewest contexts, then lower code */
    size_t choice_size;
    unsigned choice_contexts;
    uint8_t choice_code;
    adapt_status status;
    for (code = 0; code < 3; code++) {
        status = trial(sizes, code, 0, block, head, coded, coded_cap, &coded_len[code], error);
        if (status != ADAPT_OK) {
            return status;
        }
    }
    none = coded_len[0];
    if (none < CLASS_EVIDENCE) {
        *class_out = 0;
        *set_out = 0;
        return ADAPT_OK;
    }
    best_size = none - none / 1024;
    class_code = 0;
    for (code = 1; code < 3; code++) {
        if (coded_len[code] < best_size) {
            best_size = coded_len[code];
            class_code = code;
        }
    }
    full = coded_len[class_code];
    /* The fast profile's sets all cost the decoder the same, so every one
     * is tried and the smallest taken, on the terms below: it must beat
     * the first set by SET_MARGIN of the sample, and a choice on a
     * block longer than two samples is proved on the block. */
    if (sizes.fast) {
        choice_size = SET_MARGIN(full);
        choice_code = 0;
        for (code = 1; code < ADAPT_FAST_SETS; code++) {
            size_t size;
            status = trial(sizes, class_code, code, block, head, coded, coded_cap, &size, error);
            if (status != ADAPT_OK) {
                return status;
            }
            if (size < choice_size) {
                choice_size = size;
                choice_code = code;
            }
        }
        set_code = choice_code;
        if (set_code != 0 && deep) {
            size_t lean, whole;
            status = trial(sizes, class_code, set_code, block, n, coded, coded_cap, &lean, error);
            if (status != ADAPT_OK) {
                return status;
            }
            status = trial(sizes, class_code, 0, block, n, coded, coded_cap, &whole, error);
            if (status != ADAPT_OK) {
                return status;
            }
            if (lean > PROOF_MARGIN(whole)) {
                set_code = 0;
            }
        }
        *class_out = class_code;
        *set_out = set_code;
        return ADAPT_OK;
    }
    choice_size = SET_MARGIN(full);
    choice_contexts = HASHED + 1;
    choice_code = 0;
    {
        /* consider(code): the trial and the ranking; returns whether the
         * set beat the full one, which decides whether its family goes on */
        static const uint8_t families[2][4] = {{1, 2, 6, 0}, {3, 4, 5, 7}};
        size_t f, k;
        for (f = 0; f < 2; f++) {
            for (k = 0; k < 4; k++) {
                size_t size;
                unsigned contexts;
                int beats, better;
                code = families[f][k];
                if (code == 0) {
                    break;
                }
                if (k > 0 && !deep) {
                    break;
                }
                status = trial(sizes, class_code, code, block, head, coded, coded_cap, &size, error);
                if (status != ADAPT_OK) {
                    return status;
                }
                contexts = HASHED + 1 - popcount16(CONTEXT_SETS[code]);
                beats = size <= SET_MARGIN(full);
                better = size < choice_size ||
                         (size == choice_size && (contexts < choice_contexts || (contexts == choice_contexts && code < choice_code)));
                if (better) {
                    choice_size = size;
                    choice_contexts = contexts;
                    choice_code = code;
                }
                if (k == 0 && !beats) {
                    break;
                }
            }
        }
    }
    set_code = choice_code;
    if (set_code != 0 && deep) {
        size_t lean, whole;
        status = trial(sizes, class_code, set_code, block, n, coded, coded_cap, &lean, error);
        if (status != ADAPT_OK) {
            return status;
        }
        status = trial(sizes, class_code, 0, block, n, coded, coded_cap, &whole, error);
        if (status != ADAPT_OK) {
            return status;
        }
        if (lean > PROOF_MARGIN(whole)) {
            set_code = 0;
        }
    }
    *class_out = class_code;
    *set_out = set_code;
    return ADAPT_OK;
}

/* ------------------------------------------------------------------------
 * The verifier: a second model that decodes every framed block as the
 * encoder finishes it and compares the bytes with the source.
 * --------------------------------------------------------------------- */

/* A block handed to the verifier: its source bytes and its framed form,
 * copied so the encoder may reuse its own buffers at once. */
typedef struct verify_slot {
    uint8_t *source;
    uint8_t *framed;
    size_t n;
    size_t framed_len;
    size_t framed_cap;
} verify_slot;

typedef struct verifier {
    block_decoder decoder;
    adapt_sizes sizes;
    segment_counter segments;
    uint8_t *scratch;
    crc32_table crc_table;
    uint64_t seen;
    uint32_t crc;
    int ended;
#ifdef ADAPT_THREADS
    /* The proof on its own thread: a ring of VERIFY_QUEUE slots the
     * encoder fills and the thread drains in order; the encoder waits
     * when the ring is full, the thread when it is empty. The first fault
     * is kept and returned to the encoder at its next submission or at
     * the finish. Every field below is read and written under `lock`
     * except the decoder's own state, which only the thread touches
     * until the join. */
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    verify_slot slots[VERIFY_QUEUE];
    size_t head;
    size_t queued;
    int running;
    /* 0 nothing, 1 the mutex, 2 the mutex and the condition: what to destroy. */
    int synchronised;
    int closing;
    int failed;
    adapt_status status;
    adapt_error error;
#endif
} verifier;

static adapt_status verifier_block(verifier *v, const uint8_t *source, size_t n, const uint8_t *framed, size_t framed_len,
                                   adapt_error *error);

#ifdef ADAPT_THREADS
static void *verifier_thread(void *arg) {
    verifier *v = arg;
    pthread_mutex_lock(&v->lock);
    for (;;) {
        verify_slot *s;
        adapt_status status = ADAPT_OK;
        adapt_error cause;
        while (v->queued == 0 && !v->closing) {
            pthread_cond_wait(&v->changed, &v->lock);
        }
        if (v->queued == 0) {
            break;
        }
        s = &v->slots[v->head];
        if (!v->failed) {
            pthread_mutex_unlock(&v->lock);
            status = verifier_block(v, s->source, s->n, s->framed, s->framed_len, &cause);
            pthread_mutex_lock(&v->lock);
            if (status != ADAPT_OK) {
                v->failed = 1;
                v->status = status;
                v->error = cause;
            }
        }
        v->head = (v->head + 1) % VERIFY_QUEUE;
        v->queued -= 1;
        pthread_cond_broadcast(&v->changed);
    }
    pthread_mutex_unlock(&v->lock);
    return NULL;
}

/* Stop the thread and wait for it; the decoder's state is then this
 * thread's to read. */
static void verifier_join(verifier *v) {
    if (!v->running) {
        return;
    }
    pthread_mutex_lock(&v->lock);
    v->closing = 1;
    pthread_cond_broadcast(&v->changed);
    pthread_mutex_unlock(&v->lock);
    pthread_join(v->thread, NULL);
    v->running = 0;
}
#endif

static adapt_status verifier_init(verifier *v, adapt_sizes sizes, adapt_error *error) {
    v->decoder.m = model_new(sizes);
    v->sizes = sizes;
    counter_init(&v->segments, sizes.segmented);
    v->scratch = malloc(ADAPT_BLOCK_BYTES);
    crc32_table_init(&v->crc_table);
    v->seen = 0;
    v->crc = CRC32_INIT;
    v->ended = 0;
    if (v->decoder.m == NULL || v->scratch == NULL) {
        return fail(error, ADAPT_ERROR_ALLOCATION, 0, 0);
    }
#ifdef ADAPT_THREADS
    {
        size_t i;
        v->head = 0;
        v->queued = 0;
        v->closing = 0;
        v->failed = 0;
        v->status = ADAPT_OK;
        for (i = 0; i < VERIFY_QUEUE; i++) {
            v->slots[i].source = malloc(ADAPT_BLOCK_BYTES);
            v->slots[i].framed_cap = ADAPT_BLOCK_HEADER_BYTES + ADAPT_BLOCK_BYTES + CODED_SLACK;
            v->slots[i].framed = malloc(v->slots[i].framed_cap);
            if (v->slots[i].source == NULL || v->slots[i].framed == NULL) {
                return fail(error, ADAPT_ERROR_ALLOCATION, 0, 0);
            }
        }
        if (pthread_mutex_init(&v->lock, NULL) != 0) {
            return fail(error, ADAPT_ERROR_ALLOCATION, 0, 0);
        }
        v->synchronised = 1;
        if (pthread_cond_init(&v->changed, NULL) != 0) {
            return fail(error, ADAPT_ERROR_ALLOCATION, 0, 0);
        }
        v->synchronised = 2;
        if (pthread_create(&v->thread, NULL, verifier_thread, v) != 0) {
            return fail(error, ADAPT_ERROR_ALLOCATION, 0, 0);
        }
        v->running = 1;
    }
#endif
    return ADAPT_OK;
}

/* The zero state verifier_free accepts before verifier_init ran. */
static void verifier_clear(verifier *v) {
    v->decoder.m = NULL;
    v->scratch = NULL;
    counter_init(&v->segments, 0);
#ifdef ADAPT_THREADS
    {
        size_t i;
        for (i = 0; i < VERIFY_QUEUE; i++) {
            v->slots[i].source = NULL;
            v->slots[i].framed = NULL;
        }
        v->running = 0;
        v->synchronised = 0;
    }
#endif
}

static void verifier_free(verifier *v) {
#ifdef ADAPT_THREADS
    {
        size_t i;
        verifier_join(v);
        if (v->synchronised >= 2) {
            pthread_cond_destroy(&v->changed);
        }
        if (v->synchronised >= 1) {
            pthread_mutex_destroy(&v->lock);
        }
        v->synchronised = 0;
        for (i = 0; i < VERIFY_QUEUE; i++) {
            free(v->slots[i].source);
            free(v->slots[i].framed);
            v->slots[i].source = NULL;
            v->slots[i].framed = NULL;
        }
    }
#endif
    model_free(v->decoder.m);
    counter_free(&v->segments);
    free(v->scratch);
    v->decoder.m = NULL;
    v->scratch = NULL;
}

/* Hand one framed block and its source to the proof. With a thread it is
 * queued, and a fault the thread found earlier is returned instead; the
 * encoder then stops, up to VERIFY_QUEUE blocks after the fault, and the
 * bytes written meanwhile are not to be trusted, as for the crate.
 * Without a thread the block is proven here. */
static adapt_status verifier_submit(verifier *v, const uint8_t *source, size_t n, const uint8_t *framed, size_t framed_len,
                                    adapt_error *error) {
#ifdef ADAPT_THREADS
    verify_slot *s;
    pthread_mutex_lock(&v->lock);
    while (v->queued == VERIFY_QUEUE && !v->failed) {
        pthread_cond_wait(&v->changed, &v->lock);
    }
    if (v->failed) {
        adapt_status status = v->status;
        if (error != NULL) {
            *error = v->error;
        }
        pthread_mutex_unlock(&v->lock);
        return status;
    }
    s = &v->slots[(v->head + v->queued) % VERIFY_QUEUE];
    if (framed_len > s->framed_cap) {
        /* the table of a file of many segments outgrows a block's slot */
        uint8_t *grown = realloc(s->framed, framed_len);
        if (grown == NULL) {
            pthread_mutex_unlock(&v->lock);
            return fail(error, ADAPT_ERROR_ALLOCATION, 0, 0);
        }
        s->framed = grown;
        s->framed_cap = framed_len;
    }
    memcpy(s->source, source, n);
    s->n = n;
    memcpy(s->framed, framed, framed_len);
    s->framed_len = framed_len;
    v->queued += 1;
    pthread_cond_broadcast(&v->changed);
    pthread_mutex_unlock(&v->lock);
    return ADAPT_OK;
#else
    return verifier_block(v, source, n, framed, framed_len, error);
#endif
}

/* One framed block — its header and payload exactly as written, or the
 * trailer with an empty source — against its source bytes. Every fault is
 * ADAPT_ERROR_VERIFICATION. */
static adapt_status verifier_block(verifier *v, const uint8_t *source, size_t n, const uint8_t *framed, size_t framed_len,
                                   adapt_error *error) {
    adapt_error cause;
    size_t pos = 0, raw, payload_len;
    const uint8_t *payload;
    uint8_t method;
    if (v->ended) {
        fail(&cause, ADAPT_ERROR_TRAILING_BYTES, framed_len, 0);
        return fail_verification(error, &cause);
    }
    if (read_block(framed, framed_len, &pos, &method, &raw, &payload, &payload_len, &cause) != ADAPT_OK) {
        return fail_verification(error, &cause);
    }
    if (method == METHOD_SEGMENT) {
        if (counter_marker(&v->segments, &cause) != ADAPT_OK) {
            return fail_verification(error, &cause);
        }
        model_free(v->decoder.m);
        v->decoder.m = model_new(v->sizes);
        if (v->decoder.m == NULL) {
            return fail(error, ADAPT_ERROR_ALLOCATION, 0, 0);
        }
    } else if (method == METHOD_TABLE) {
        size_t len;
        if (counter_table_len(&v->segments, &len, &cause) != ADAPT_OK) {
            return fail_verification(error, &cause);
        }
        if (framed_len - pos != len) {
            fail(&cause, ADAPT_ERROR_BAD_BLOCK, 0, 0);
            return fail_verification(error, &cause);
        }
        if (counter_table(&v->segments, framed + pos, &cause) != ADAPT_OK) {
            return fail_verification(error, &cause);
        }
        pos = framed_len;
    }
    if (pos != framed_len) {
        fail(&cause, ADAPT_ERROR_TRAILING_BYTES, framed_len - pos, 0);
        return fail_verification(error, &cause);
    }
    if (method == METHOD_SEGMENT || method == METHOD_TABLE) {
        return ADAPT_OK;
    }
    if (method == METHOD_END) {
        uint64_t len;
        uint32_t sum;
        if (counter_end(&v->segments, &cause) != ADAPT_OK) {
            return fail_verification(error, &cause);
        }
        read_trailer_payload(payload, &len, &sum);
        if (len != v->seen || sum != ~v->crc) {
            fail(&cause, ADAPT_ERROR_CHECKSUM, 0, 0);
            return fail_verification(error, &cause);
        }
        v->ended = 1;
        return ADAPT_OK;
    }
    if (counter_block(&v->segments, raw, framed_len, &cause) != ADAPT_OK) {
        return fail_verification(error, &cause);
    }
    if (decode_block(&v->decoder, method, raw, payload, payload_len, v->scratch, &cause) != ADAPT_OK) {
        return fail_verification(error, &cause);
    }
    if (v->sizes.x86) {
        x86_inverse(v->scratch, raw);
    }
    if (v->sizes.bmp) bmp_transform(v->scratch, raw, v->seen, v->sizes, 1);
    if (raw != n || memcmp(v->scratch, source, n) != 0) {
        return fail_verification(error, NULL);
    }
    v->seen += raw;
    v->crc = crc32_update(&v->crc_table, v->crc, v->scratch, raw);
    return ADAPT_OK;
}

/* After the last submission: every queued block proven, and the trailer
 * seen. */
static adapt_status verifier_finish(verifier *v, adapt_error *error) {
    adapt_error cause;
#ifdef ADAPT_THREADS
    verifier_join(v);
    if (v->failed) {
        if (error != NULL) {
            *error = v->error;
        }
        return v->status;
    }
#endif
    if (!v->ended) {
        fail(&cause, ADAPT_ERROR_TRUNCATED, 0, 0);
        return fail_verification(error, &cause);
    }
    return ADAPT_OK;
}

/* ------------------------------------------------------------------------
 * Segments on threads
 * --------------------------------------------------------------------- */

/* Bytes the parallel encoder needs for `workers` segments in flight: each
 * worker holds a model, its segment's input and coded form, and a block's
 * workspace (the block it transforms and codes, the one its proof decodes
 * into, the scan table); the coder itself holds a block's workspace and
 * the headroom, as the in-order encoder does. */
uint64_t adapt_parallel_encode_memory_bound(adapt_sizes sizes, uint64_t workers, uint64_t segment_bytes) {
    uint64_t each = adapt_model_bytes(sizes) + segment_bytes + adapt_segment_out_bound(segment_bytes) + CODED_SLACK +
                    block_workspace();
    return each * workers + block_workspace() + HEADROOM;
}

uint64_t adapt_encode_workers(adapt_sizes sizes, uint64_t max_memory, uint64_t segment_bytes, uint64_t threads) {
#ifdef ADAPT_THREADS
    uint64_t w = threads > ADAPT_WORKERS_MAX ? ADAPT_WORKERS_MAX : threads;
    if (!sizes.segmented || !adapt_segment_bytes_valid(segment_bytes)) {
        return 1;
    }
    while (w >= 2 && adapt_parallel_encode_memory_bound(sizes, w, segment_bytes) > max_memory) {
        w--;
    }
    return w < 1 ? 1 : w;
#else
    (void)sizes;
    (void)max_memory;
    (void)segment_bytes;
    (void)threads;
    return 1;
#endif
}

#ifdef ADAPT_THREADS
/* One segment's coding, on a worker: its input, whole blocks, is coded
 * under a fresh model into `framed`, block after block as the in-order
 * encoder frames them; then, when asked, proven — the framed blocks
 * decoded under another fresh model and compared with the input — before
 * the coder writes them. One model is live per worker at a time. */
typedef struct segment_job {
    adapt_sizes sizes;
    int verify;
    /* the segment's index, and its first byte's offset in the input */
    uint64_t index;
    uint64_t position;
    uint8_t *input;
    size_t n;
    uint8_t *framed;
    size_t framed_cap;
    size_t framed_len;
    /* the block being coded (a transformed copy), the proof's block, the
     * repeat scan */
    uint8_t *work;
    uint8_t *scratch;
    uint32_t *scan;
    adapt_status status;
    adapt_error error;
    pthread_t thread;
    int busy;
} segment_job;

static void verify_segment(segment_job *j) {
    block_decoder d;
    adapt_error cause;
    size_t pos = 0, seen = 0;
    d.m = model_new(j->sizes);
    if (d.m == NULL) {
        j->status = fail(&j->error, ADAPT_ERROR_ALLOCATION, 0, 0);
        return;
    }
    while (pos < j->framed_len) {
        uint8_t method;
        size_t raw, payload_len;
        const uint8_t *payload;
        if (read_block(j->framed, j->framed_len, &pos, &method, &raw, &payload, &payload_len, &cause) != ADAPT_OK) {
            j->status = fail_verification(&j->error, &cause);
            break;
        }
        if (method > METHOD_STORED_SKIPPED || raw > j->n - seen) {
            fail(&cause, ADAPT_ERROR_BAD_BLOCK, 0, 0);
            j->status = fail_verification(&j->error, &cause);
            break;
        }
        if (decode_block(&d, method, raw, payload, payload_len, j->scratch, &cause) != ADAPT_OK) {
            j->status = fail_verification(&j->error, &cause);
            break;
        }
        if (j->sizes.x86) {
            x86_inverse(j->scratch, raw);
        }
        if (j->sizes.bmp) {
            bmp_transform(j->scratch, raw, j->position + seen, j->sizes, 1);
        }
        if (memcmp(j->scratch, j->input + seen, raw) != 0) {
            j->status = fail_verification(&j->error, NULL);
            break;
        }
        seen += raw;
    }
    if (j->status == ADAPT_OK && seen != j->n) {
        fail(&cause, ADAPT_ERROR_TRUNCATED, 0, 0);
        j->status = fail_verification(&j->error, &cause);
    }
    model_free(d.m);
}

static void *segment_thread(void *arg) {
    segment_job *j = arg;
    model *m = model_new(j->sizes);
    size_t at = 0;
    j->framed_len = 0;
    j->status = ADAPT_OK;
    if (m == NULL) {
        j->status = fail(&j->error, ADAPT_ERROR_ALLOCATION, 0, 0);
        return NULL;
    }
    while (at < j->n) {
        size_t n = j->n - at < ADAPT_BLOCK_BYTES ? j->n - at : ADAPT_BLOCK_BYTES;
        memcpy(j->work, j->input + at, n);
        j->framed_len += frame_block(m, j->sizes, j->work, n, j->position + at, j->scan, j->framed + j->framed_len,
                                     j->framed_cap - j->framed_len);
        at += n;
    }
    model_free(m);
    if (j->verify) {
        verify_segment(j);
    }
    return NULL;
}

static void segment_job_free(segment_job *j) {
    free(j->input);
    free(j->framed);
    free(j->work);
    free(j->scratch);
    free(j->scan);
}

static int segment_job_alloc(segment_job *j, adapt_sizes sizes, uint64_t segment_bytes, int verify) {
    memset(j, 0, sizeof(*j));
    j->sizes = sizes;
    j->verify = verify;
    j->framed_cap = (size_t)adapt_segment_out_bound(segment_bytes) + CODED_SLACK;
    j->input = malloc((size_t)segment_bytes);
    j->framed = malloc(j->framed_cap);
    j->work = malloc(ADAPT_BLOCK_BYTES);
    j->scratch = verify ? malloc(ADAPT_BLOCK_BYTES) : NULL;
    j->scan = malloc(SCAN_SLOTS * sizeof(uint32_t));
    return j->input != NULL && j->framed != NULL && j->work != NULL && (!verify || j->scratch != NULL) && j->scan != NULL;
}

/* Start job j on a thread, or run it here when no thread can be made. */
static void segment_job_start(segment_job *j) {
    j->busy = 1;
    if (pthread_create(&j->thread, NULL, segment_thread, j) != 0) {
        segment_thread(j);
        j->busy = 2;
    }
}

static void segment_job_wait(segment_job *j) {
    if (j->busy == 1) {
        pthread_join(j->thread, NULL);
    }
    j->busy = 0;
}

/* The finished segment j, in order: the marker before every segment after
 * the first, then its framed blocks, into the coder's counter and output. */
static adapt_status segment_job_emit(segment_job *j, adapt_write_fn write, void *write_context, segment_counter *segments,
                                     uint64_t *output_bytes, adapt_error *error) {
    int rc;
    adapt_status status;
    if (j->status != ADAPT_OK) {
        *error = j->error;
        return j->status;
    }
    if (j->index > 0) {
        uint8_t marker = METHOD_SEGMENT;
        status = counter_close(segments, error);
        if (status != ADAPT_OK) {
            return status;
        }
        rc = write(write_context, &marker, 1);
        if (rc != 0) {
            return fail(error, ADAPT_ERROR_IO, (uint64_t)(rc < 0 ? -(int64_t)rc : rc), 0);
        }
        *output_bytes += 1;
        segments->coded += 1;
    }
    rc = write(write_context, j->framed, j->framed_len);
    if (rc != 0) {
        return fail(error, ADAPT_ERROR_IO, (uint64_t)(rc < 0 ? -(int64_t)rc : rc), 0);
    }
    *output_bytes += j->framed_len;
    segments->raw += j->n;
    segments->coded += j->framed_len;
    return ADAPT_OK;
}

/* The segments of the input after its first block, `first_n` bytes
 * already in `first`, coded `workers` at a time and written in order:
 * segment k on slot k mod workers, each slot's previous segment written
 * as the slot is reused, the last ones after the input ends. The bytes
 * are the in-order encoder's, which codes each segment under a fresh
 * model too. */
static adapt_status encode_segments_parallel(adapt_read_fn read, void *read_context, adapt_write_fn write,
                                             void *write_context, adapt_sizes sizes, uint64_t segment_bytes,
                                             uint64_t workers, int verify, const uint8_t *first, size_t first_n,
                                             segment_counter *segments, const crc32_table *crc_table, uint32_t *crc,
                                             uint64_t *input_bytes, uint64_t *output_bytes, adapt_error *error) {
    segment_job *jobs;
    size_t slots = (size_t)workers, i;
    uint64_t index = 0, position = 0;
    adapt_status status = ADAPT_OK;
    jobs = calloc(slots, sizeof(*jobs));
    if (jobs == NULL) {
        return fail(error, ADAPT_ERROR_ALLOCATION, 0, 0);
    }
    for (i = 0; i < slots; i++) {
        if (!segment_job_alloc(&jobs[i], sizes, segment_bytes, verify)) {
            status = fail(error, ADAPT_ERROR_ALLOCATION, 0, 0);
        }
    }
    while (status == ADAPT_OK) {
        segment_job *j = &jobs[index % slots];
        size_t n = 0, got = 0;
        if (j->busy) {
            segment_job_wait(j);
            status = segment_job_emit(j, write, write_context, segments, output_bytes, error);
            if (status != ADAPT_OK) {
                break;
            }
        }
        if (index == 0) {
            memcpy(j->input, first, first_n);
            n = first_n;
        }
        if (index > 0 || first_n == ADAPT_BLOCK_BYTES) {
            status = fill(read, read_context, j->input + n, (size_t)segment_bytes - n, &got, error);
            if (status != ADAPT_OK) {
                break;
            }
            n += got;
        }
        if (n == 0) {
            break;
        }
        *input_bytes += n;
        *crc = crc32_update(crc_table, *crc, j->input, n);
        j->index = index;
        j->position = position;
        j->n = n;
        position += n;
        index += 1;
        segment_job_start(j);
    }
    /* the segments still in flight, in order; after a failure they are
     * waited for and dropped */
    for (i = 0; i < slots; i++) {
        segment_job *j = &jobs[(index + i) % slots];
        if (!j->busy) {
            continue;
        }
        segment_job_wait(j);
        if (status == ADAPT_OK) {
            status = segment_job_emit(j, write, write_context, segments, output_bytes, error);
        }
    }
    for (i = 0; i < slots; i++) {
        segment_job_free(&jobs[i]);
    }
    free(jobs);
    return status;
}
#else
static adapt_status encode_segments_parallel(adapt_read_fn read, void *read_context, adapt_write_fn write,
                                             void *write_context, adapt_sizes sizes, uint64_t segment_bytes,
                                             uint64_t workers, int verify, const uint8_t *first, size_t first_n,
                                             segment_counter *segments, const crc32_table *crc_table, uint32_t *crc,
                                             uint64_t *input_bytes, uint64_t *output_bytes, adapt_error *error) {
    (void)read;
    (void)read_context;
    (void)write;
    (void)write_context;
    (void)sizes;
    (void)segment_bytes;
    (void)workers;
    (void)verify;
    (void)first;
    (void)first_n;
    (void)segments;
    (void)crc_table;
    (void)crc;
    (void)input_bytes;
    (void)output_bytes;
    return fail(error, ADAPT_ERROR_INVALID_ARGUMENT, 0, 0);
}
#endif

/* ------------------------------------------------------------------------
 * The encoder
 * --------------------------------------------------------------------- */

/* Encode `read` into `write`; when `v` is given, every finished block —
 * its source bytes, and its header and payload exactly as written — is
 * proven before the next one is coded, and finally the trailer. */
/* The segment table as the file carries it: the method byte, the entries,
 * the count; malloc'd into *out. */
static adapt_status segment_table_bytes(const segment_counter *c, uint8_t **out, size_t *out_len, adapt_error *error) {
    size_t len = adapt_segment_table_len((uint64_t)c->count), i;
    uint8_t *table = malloc(len);
    if (table == NULL) {
        return fail(error, ADAPT_ERROR_ALLOCATION, 0, 0);
    }
    table[0] = METHOD_TABLE;
    for (i = 0; i < 2 * c->count; i++) {
        uint32_t v = c->entries[i];
        table[1 + 4 * i] = (uint8_t)v;
        table[2 + 4 * i] = (uint8_t)(v >> 8);
        table[3 + 4 * i] = (uint8_t)(v >> 16);
        table[4 + 4 * i] = (uint8_t)(v >> 24);
    }
    table[len - 4] = (uint8_t)c->count;
    table[len - 3] = (uint8_t)(c->count >> 8);
    table[len - 2] = (uint8_t)(c->count >> 16);
    table[len - 1] = (uint8_t)(c->count >> 24);
    *out = table;
    *out_len = len;
    return ADAPT_OK;
}

static adapt_status encode_blocks(adapt_read_fn read, void *read_context, adapt_write_fn write, void *write_context,
                                  adapt_sizes sizes, uint64_t max_memory, uint64_t segment_bytes, verifier *v,
                                  uint64_t workers, int verify, adapt_stats *stats, adapt_error *error) {
    adapt_status status;
    model *m = NULL;
    uint32_t *scan = NULL;
    uint8_t *block = NULL;
    uint8_t *framed = NULL;
    uint8_t *table = NULL;
    /* The block as read, kept for the proof when `block` is transformed. */
    uint8_t *orig = NULL;
    segment_counter segments;
    crc32_table crc_table;
    uint32_t crc = CRC32_INIT;
    uint64_t input_bytes = 0;
    uint64_t output_bytes = ADAPT_HEADER_BYTES;
    size_t n, framed_cap = ADAPT_BLOCK_HEADER_BYTES + ADAPT_BLOCK_BYTES + CODED_SLACK;
    uint8_t header[ADAPT_HEADER_MAX_BYTES];
    size_t header_len;
    uint8_t trailer[ADAPT_TRAILER_BYTES];
    int first = 1;
    if (!sizes_valid(sizes)) {
        return fail(error, ADAPT_ERROR_BAD_HEADER, 0, 0);
    }
    if (sizes.segmented && !adapt_segment_bytes_valid(segment_bytes)) {
        return fail(error, ADAPT_ERROR_BAD_HEADER, 0, 0);
    }
    status = check_memory(adapt_encode_memory_bound(sizes), max_memory, error);
    if (status != ADAPT_OK) {
        return status;
    }
    counter_init(&segments, sizes.segmented);
    scan = malloc(SCAN_SLOTS * sizeof(uint32_t));
    block = malloc(ADAPT_BLOCK_BYTES);
    framed = malloc(framed_cap);
    if (scan == NULL || block == NULL || framed == NULL) {
        status = fail(error, ADAPT_ERROR_ALLOCATION, 0, 0);
        goto done;
    }
    crc32_table_init(&crc_table);
    /* The first block is read before the header is written: the file's
     * class-history width and its context set are chosen from it. A block
     * the model would not run on declines the class context and keeps
     * every other. */
    status = fill(read, read_context, block, ADAPT_BLOCK_BYTES, &n, error);
    if (status != ADAPT_OK) {
        goto done;
    }
    if (n == 0 || (entropy_256(block, n) >= SKIP_ENTROPY_256 && !repeats_within(block, n, scan))) {
        sizes.class_code = 0;
        sizes.set_code = 0;
        sizes.period = 0;
    } else {
        status = choose_model(sizes, block, n, framed, framed_cap, &sizes.class_code, &sizes.set_code, &sizes.period, error);
        if (status != ADAPT_OK) {
            goto done;
        }
    }
    /* Machine code goes through the branch transform, when the budget
     * holds the extra block the proof needs; otherwise it is declined. */
    if (sizes.bmp && !bmp_probe(block, n, &sizes)) {
        status = fail(error, ADAPT_ERROR_BAD_HEADER, 0, 0);
        goto done;
    }
    sizes.x86 = !sizes.bmp && x86_detect(block, n);
    if (sizes.x86 || sizes.bmp) {
        if (adapt_encode_memory_bound(sizes) > max_memory) {
            sizes.x86 = 0;
        } else {
            orig = malloc(ADAPT_BLOCK_BYTES);
            if (orig == NULL) {
                status = fail(error, ADAPT_ERROR_ALLOCATION, 0, 0);
                goto done;
            }
        }
    }
    /* The verifier's model must share the width just chosen. */
    if (v != NULL) {
        status = verifier_init(v, sizes, error);
        if (status != ADAPT_OK) {
            goto done;
        }
    }
    header[0] = 'A';
    header[1] = 'D';
    header[2] = 'P';
    header[3] = 'T';
    header[4] = sizes.adaptive ? (uint8_t)((sizes.max ? ADAPT_MAX_ADAPTIVE_VERSION : ADAPT_ADAPTIVE_VERSION) + sizes.adaptive - 1) :
                (sizes.fast ? ADAPT_FAST_VERSION
                            : (sizes.max ? (sizes.compact ? ADAPT_MAX_COMPACT_VERSION : ADAPT_MAX_VERSION)
                                         : ADAPT_FORMAT_VERSION));
    if (sizes.bmp) header[4] = (uint8_t)((sizes.max ? ADAPT_MAX_BMP_VERSION : ADAPT_BMP_VERSION) + sizes.adaptive);
    header[5] = (uint8_t)(sizes.table_log | sizes.class_code << 5 | (sizes.archive ? ADAPT_ARCHIVE_FLAG : 0));
    header[6] = (uint8_t)(sizes.match_log | sizes.set_code << 5);
    header[7] = (uint8_t)(sizes.window_log | (sizes.period != 0 ? ADAPT_PERIOD_FLAG : 0) |
                          (sizes.segmented ? ADAPT_SEGMENT_FLAG : 0) | (sizes.x86 ? ADAPT_X86_FLAG : 0));
    header_len = ADAPT_HEADER_BYTES;
    if (sizes.period != 0) {
        header[8] = (uint8_t)sizes.period;
        header[9] = (uint8_t)(sizes.period >> 8);
        header_len += ADAPT_PERIOD_BYTES;
    }
    if (sizes.bmp) {
        bmp_store32(header + header_len, sizes.bmp_offset);
        bmp_store32(header + header_len + 4, sizes.bmp_width);
        bmp_store32(header + header_len + 8, sizes.bmp_rows);
        header_len += ADAPT_BMP_BYTES;
    }
    output_bytes = header_len;
    {
        int rc = write(write_context, header, header_len);
        if (rc != 0) {
            status = fail(error, ADAPT_ERROR_IO, (uint64_t)(rc < 0 ? -(int64_t)rc : rc), 0);
            goto done;
        }
    }
    if (workers >= 2 && sizes.segmented) {
        status = encode_segments_parallel(read, read_context, write, write_context, sizes, segment_bytes, workers, verify,
                                          block, n, &segments, &crc_table, &crc, &input_bytes, &output_bytes, error);
        if (status != ADAPT_OK) {
            goto done;
        }
        n = 0;
        first = 0;
    }
    for (;;) {
        size_t framed_len;
        int rc;
        if (!first) {
            status = fill(read, read_context, block, ADAPT_BLOCK_BYTES, &n, error);
            if (status != ADAPT_OK) {
                goto done;
            }
        }
        first = 0;
        if (n == 0) {
            break;
        }
        if (sizes.segmented && segments.raw == segment_bytes) {
            /* The segment is full and input follows: a marker, a fresh model. */
            uint8_t marker = METHOD_SEGMENT;
            status = counter_close(&segments, error);
            if (status != ADAPT_OK) {
                goto done;
            }
            model_free(m);
            m = NULL;
            rc = write(write_context, &marker, 1);
            if (rc != 0) {
                status = fail(error, ADAPT_ERROR_IO, (uint64_t)(rc < 0 ? -(int64_t)rc : rc), 0);
                goto done;
            }
            output_bytes += 1;
            segments.coded += 1;
            if (v != NULL) {
                status = verifier_submit(v, block, 0, &marker, 1, error);
                if (status != ADAPT_OK) {
                    goto done;
                }
            }
        }
        input_bytes += n;
        crc = crc32_update(&crc_table, crc, block, n);
        if (sizes.x86 || sizes.bmp) {
            memcpy(orig, block, n);
        }
        if (m == NULL) {
            m = model_new(sizes);
            if (m == NULL) {
                status = fail(error, ADAPT_ERROR_ALLOCATION, 0, 0);
                goto done;
            }
        }
        framed_len = frame_block(m, sizes, block, n, input_bytes - n, scan, framed, framed_cap);
        rc = write(write_context, framed, framed_len);
        if (rc != 0) {
            status = fail(error, ADAPT_ERROR_IO, (uint64_t)(rc < 0 ? -(int64_t)rc : rc), 0);
            goto done;
        }
        output_bytes += framed_len;
        segments.raw += n;
        segments.coded += framed_len;
        if (v != NULL) {
            status = verifier_submit(v, (sizes.x86 || sizes.bmp) ? orig : block, n, framed, framed_len, error);
            if (status != ADAPT_OK) {
                goto done;
            }
        }
    }
    if (sizes.segmented) {
        size_t table_len;
        int rc;
        status = counter_close(&segments, error);
        if (status != ADAPT_OK) {
            goto done;
        }
        status = segment_table_bytes(&segments, &table, &table_len, error);
        if (status != ADAPT_OK) {
            goto done;
        }
        rc = write(write_context, table, table_len);
        if (rc != 0) {
            status = fail(error, ADAPT_ERROR_IO, (uint64_t)(rc < 0 ? -(int64_t)rc : rc), 0);
            goto done;
        }
        output_bytes += table_len;
        if (v != NULL) {
            status = verifier_submit(v, block, 0, table, table_len, error);
            if (status != ADAPT_OK) {
                goto done;
            }
        }
    }
    crc = ~crc;
    trailer[0] = METHOD_END;
    {
        int i;
        for (i = 0; i < 8; i++) {
            trailer[1 + i] = (uint8_t)(input_bytes >> (8 * i));
        }
        for (i = 0; i < 4; i++) {
            trailer[9 + i] = (uint8_t)(crc >> (8 * i));
        }
    }
    {
        int rc = write(write_context, trailer, ADAPT_TRAILER_BYTES);
        if (rc != 0) {
            status = fail(error, ADAPT_ERROR_IO, (uint64_t)(rc < 0 ? -(int64_t)rc : rc), 0);
            goto done;
        }
    }
    output_bytes += ADAPT_TRAILER_BYTES;
    if (v != NULL) {
        status = verifier_submit(v, block, 0, trailer, ADAPT_TRAILER_BYTES, error);
        if (status != ADAPT_OK) {
            goto done;
        }
        status = verifier_finish(v, error);
        if (status != ADAPT_OK) {
            goto done;
        }
    }
    if (stats != NULL) {
        stats->input_bytes = input_bytes;
        stats->output_bytes = output_bytes;
        stats->crc32 = crc;
        stats->segments = sizes.segmented ? (uint32_t)segments.count : 1;
        /* the workers that coded: never more than there were segments */
        stats->workers = workers >= 2 && sizes.segmented ? (uint32_t)(workers < segments.count ? workers : segments.count) : 1;
    }
    status = fail(error, ADAPT_OK, 0, 0);
done:
    model_free(m);
    counter_free(&segments);
    free(table);
    free(scan);
    free(block);
    free(orig);
    free(framed);
    return status;
}

adapt_status adapt_encode_stream_segments(adapt_read_fn read, void *read_context, adapt_write_fn write,
                                          void *write_context, adapt_sizes sizes, uint64_t max_memory,
                                          uint64_t segment_bytes, adapt_stats *stats, adapt_error *error) {
    if (read == NULL || write == NULL) {
        return fail(error, ADAPT_ERROR_INVALID_ARGUMENT, 0, 0);
    }
    return encode_blocks(read, read_context, write, write_context, sizes, max_memory, segment_bytes, NULL, 1, 0, stats, error);
}

adapt_status adapt_encode_stream_threads(adapt_read_fn read, void *read_context, adapt_write_fn write, void *write_context,
                                         adapt_sizes sizes, uint64_t max_memory, uint64_t segment_bytes, uint64_t threads,
                                         int verify, adapt_stats *stats, adapt_error *error) {
    uint64_t workers;
    if (read == NULL || write == NULL || threads == 0) {
        return fail(error, ADAPT_ERROR_INVALID_ARGUMENT, 0, 0);
    }
    if (!sizes_valid(sizes)) {
        return fail(error, ADAPT_ERROR_BAD_HEADER, 0, 0);
    }
    workers = adapt_encode_workers(sizes, max_memory, segment_bytes, threads);
    if (workers >= 2) {
        return encode_blocks(read, read_context, write, write_context, sizes, max_memory, segment_bytes, NULL, workers,
                             verify, stats, error);
    }
    if (verify) {
        return adapt_encode_stream_verified_segments(read, read_context, write, write_context, sizes, max_memory,
                                                     segment_bytes, stats, error);
    }
    return adapt_encode_stream_segments(read, read_context, write, write_context, sizes, max_memory, segment_bytes, stats,
                                        error);
}

adapt_status adapt_encode_stream(adapt_read_fn read, void *read_context, adapt_write_fn write, void *write_context,
                                 adapt_sizes sizes, uint64_t max_memory, adapt_stats *stats, adapt_error *error) {
    return adapt_encode_stream_segments(read, read_context, write, write_context, sizes, max_memory, ADAPT_SEGMENT_BYTES,
                                        stats, error);
}

adapt_status adapt_encode_stream_verified_segments(adapt_read_fn read, void *read_context, adapt_write_fn write,
                                                   void *write_context, adapt_sizes sizes, uint64_t max_memory,
                                                   uint64_t segment_bytes, adapt_stats *stats, adapt_error *error) {
    verifier v;
    adapt_status status;
    if (read == NULL || write == NULL) {
        return fail(error, ADAPT_ERROR_INVALID_ARGUMENT, 0, 0);
    }
    if (!sizes_valid(sizes)) {
        return fail(error, ADAPT_ERROR_BAD_HEADER, 0, 0);
    }
    status = check_memory(adapt_verified_memory_bound(sizes), max_memory, error);
    if (status != ADAPT_OK) {
        return status;
    }
    verifier_clear(&v);
    status = encode_blocks(read, read_context, write, write_context, sizes, max_memory, segment_bytes, &v, 1, 0, stats, error);
    verifier_free(&v);
    return status;
}

adapt_status adapt_encode_stream_verified(adapt_read_fn read, void *read_context, adapt_write_fn write,
                                          void *write_context, adapt_sizes sizes, uint64_t max_memory,
                                          adapt_stats *stats, adapt_error *error) {
    return adapt_encode_stream_verified_segments(read, read_context, write, write_context, sizes, max_memory,
                                                 ADAPT_SEGMENT_BYTES, stats, error);
}

/* ------------------------------------------------------------------------
 * The proof after the fact, with one model
 * --------------------------------------------------------------------- */

typedef struct compare_sink {
    adapt_read_fn source;
    void *source_context;
    uint8_t *expected;
    int mismatch;
    adapt_error read_error;
} compare_sink;

/* The decoder's writer: each recovered block is read from the source and
 * compared. A difference, or a source that ends early, is a mismatch; a
 * failing source read is kept as the cause. */
static int compare_write(void *context, const uint8_t *decoded, size_t len) {
    compare_sink *c = context;
    size_t got = 0;
    if (fill(c->source, c->source_context, c->expected, len, &got, &c->read_error) != ADAPT_OK) {
        return 2;
    }
    if (got != len || memcmp(c->expected, decoded, len) != 0) {
        c->mismatch = 1;
        return 1;
    }
    return 0;
}

adapt_status adapt_verify_stream(adapt_read_fn source, void *source_context, adapt_read_fn encoded,
                                 void *encoded_context, uint64_t max_memory, adapt_stats *stats, adapt_error *error) {
    compare_sink c;
    adapt_limits limits;
    adapt_error cause;
    adapt_stats local;
    adapt_status status;
    size_t got = 0;
    if (source == NULL || encoded == NULL) {
        return fail(error, ADAPT_ERROR_INVALID_ARGUMENT, 0, 0);
    }
    c.source = source;
    c.source_context = source_context;
    c.expected = malloc(ADAPT_BLOCK_BYTES);
    c.mismatch = 0;
    fail(&c.read_error, ADAPT_OK, 0, 0);
    if (c.expected == NULL) {
        return fail(error, ADAPT_ERROR_ALLOCATION, 0, 0);
    }
    limits.max_output = UINT64_MAX;
    limits.max_memory = max_memory;
    status = adapt_decode_stream(encoded, encoded_context, compare_write, &c, limits, &local, &cause);
    if (status == ADAPT_OK) {
        /* The source must end where the container says it does. */
        if (fill(source, source_context, c.expected, 1, &got, &cause) != ADAPT_OK) {
            status = fail_verification(error, &cause);
        } else if (got != 0) {
            status = fail_verification(error, NULL);
        } else {
            if (stats != NULL) {
                *stats = local;
            }
            status = fail(error, ADAPT_OK, 0, 0);
        }
    } else if (c.mismatch) {
        status = fail_verification(error, NULL);
    } else if (c.read_error.status != ADAPT_OK) {
        status = fail_verification(error, &c.read_error);
    } else {
        status = fail_verification(error, &cause);
    }
    free(c.expected);
    return status;
}

/* ------------------------------------------------------------------------
 * In memory
 * --------------------------------------------------------------------- */

typedef struct memory_source {
    const uint8_t *src;
    size_t len;
    size_t pos;
} memory_source;

static int memory_source_read(void *context, uint8_t *buf, size_t len, size_t *got) {
    memory_source *r = context;
    size_t n = r->len - r->pos < len ? r->len - r->pos : len;
    /* An empty source may be NULL, which may not be offset or handed to
     * memcpy even for zero bytes. */
    if (n != 0) {
        memcpy(buf, r->src + r->pos, n);
    }
    r->pos += n;
    *got = n;
    return 0;
}

typedef struct growing_sink {
    uint8_t *out;
    size_t len;
    size_t cap;
} growing_sink;

static int growing_write(void *context, const uint8_t *buf, size_t len) {
    growing_sink *s = context;
    if (len > s->cap - s->len) {
        size_t cap = s->cap;
        uint8_t *grown;
        while (cap - s->len < len) {
            if (cap > ((size_t)-1) / 2) {
                return 1;
            }
            cap = cap == 0 ? 64 : cap * 2;
        }
        grown = realloc(s->out, cap);
        if (grown == NULL) {
            return 1;
        }
        s->out = grown;
        s->cap = cap;
    }
    memcpy(s->out + s->len, buf, len);
    s->len += len;
    return 0;
}

static adapt_status encode_in_memory(const uint8_t *data, size_t data_len, adapt_limits limits, int verified, uint8_t **out,
                                     size_t *out_len, adapt_error *error) {
    adapt_sizes sizes;
    adapt_status status;
    memory_source reader;
    growing_sink sink;
    adapt_sizes floor = {TABLE_LOG_MIN, MATCH_LOG_MIN, WINDOW_LOG_MIN, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint64_t budget, minimum = adapt_encode_memory_bound(floor);
    if ((data == NULL && data_len != 0) || out == NULL || out_len == NULL) {
        return fail(error, ADAPT_ERROR_INVALID_ARGUMENT, 0, 0);
    }
    *out = NULL;
    *out_len = 0;
    /* Reserve the matching decoder's result when it fits. A larger buffer
     * uses the smallest model and needs a larger decode limit or a sink. */
    budget = limits.max_memory > data_len ? limits.max_memory - data_len : 0;
    if (budget < minimum) {
        budget = minimum;
    }
    if (budget > limits.max_memory) {
        budget = limits.max_memory;
    }
    status = adapt_sizes_for_input(1, data_len, budget, &sizes, error);
    if (status != ADAPT_OK) {
        return status;
    }
    reader.src = data;
    reader.len = data_len;
    reader.pos = 0;
    sink.cap = data_len / 2 + ADAPT_HEADER_BYTES + ADAPT_TRAILER_BYTES;
    sink.len = 0;
    sink.out = malloc(sink.cap);
    if (sink.out == NULL) {
        return fail(error, ADAPT_ERROR_ALLOCATION, 0, 0);
    }
    if (verified && adapt_verified_memory_bound(sizes) <= limits.max_memory) {
        status = adapt_encode_stream_verified(memory_source_read, &reader, growing_write, &sink, sizes, limits.max_memory,
                                              NULL, error);
    } else {
        status = adapt_encode_stream(memory_source_read, &reader, growing_write, &sink, sizes, limits.max_memory, NULL,
                                     error);
        if (status == ADAPT_OK && verified) {
            /* The budget holds one model at a time: the same proof, after
             * the fact, with the encoder's memory released. */
            memory_source source, encoded;
            source.src = data;
            source.len = data_len;
            source.pos = 0;
            encoded.src = sink.out;
            encoded.len = sink.len;
            encoded.pos = 0;
            status = adapt_verify_stream(memory_source_read, &source, memory_source_read, &encoded, limits.max_memory,
                                         NULL, error);
        }
    }
    if (status == ADAPT_ERROR_IO) {
        /* The only writer here is the growing buffer: its failure is an
         * allocation's. */
        status = fail(error, ADAPT_ERROR_ALLOCATION, 0, 0);
    }
    if (status != ADAPT_OK) {
        free(sink.out);
        return status;
    }
    *out = sink.out;
    *out_len = sink.len;
    return ADAPT_OK;
}

adapt_status adapt_encode(const uint8_t *data, size_t data_len, adapt_limits limits, uint8_t **out, size_t *out_len,
                          adapt_error *error) {
    return encode_in_memory(data, data_len, limits, 0, out, out_len, error);
}

adapt_status adapt_encode_verified(const uint8_t *data, size_t data_len, adapt_limits limits, uint8_t **out,
                                   size_t *out_len, adapt_error *error) {
    return encode_in_memory(data, data_len, limits, 1, out, out_len, error);
}
