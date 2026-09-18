/*
 * adapt_constants.h — the constants of the model and the container, the profiles, the second max model, and the x86 branch transform.
 *
 * A section of adapt_model.h, which includes the sections in order into
 * one translation unit: every function stays static inline and the per-bit
 * path is specialized at compile time as before. Include adapt_model.h,
 * not this file. Moved out of adapt_model.h unchanged on 2026-09-17.
 */
#ifndef ADAPT_CONSTANTS_H
#define ADAPT_CONSTANTS_H

/* ------------------------------------------------------------------------
 * model.rs — constants
 * --------------------------------------------------------------------- */

#define TABLE_LOG_MIN 10
#define TABLE_LOG_MAX 26
#define MATCH_LOG_MIN 10
#define MATCH_LOG_MAX 24
#define WINDOW_LOG_MIN 16
#define WINDOW_LOG_MAX 30

/* Bytes a sequence must repeat before recall follows it. */
#define MATCH_MIN 5
/* Longest match the model bothers to verify backwards on a fresh candidate. */
#define MATCH_VERIFY 32
#define MATCH_BUCKETS 16
#define RUN_BUCKETS 16
/* Observation count at which a confidence cell or a state-to-probability
 * cell stops slowing its rate. */
#define CELL_LIMIT 255u
#define MAP_LIMIT 255u
#define MIXER_RATE_MAX 192
#define MIXER_RATE_MIN 8
#define MIXER_DECAY 96
#define MIXER_COUNT_MAX 4095
#define MIXER_SKIP 8
#define APM_RATE 7
/* Blocks a context may occupy within its cache line: four of sixteen
 * bytes fill the line, and a claim inspects it once. */
#define WAYS 4
ADAPT_STATIC_ASSERT(WAYS == 4, claim_reads_four_ways);

/* The hashed context kinds: ten every profile may run, then ten the max
 * profile adds — the previous word with the current one, the first and
 * third bytes back, the byte history that followed the order-2 context,
 * the high nibbles of the last four bytes, the markup nesting (open [[
 * and {{, the tag being read or last read), orders 5 and 8, the word
 * with its case folded, the last three words, and the word with the
 * classes of the three bytes before it. Each was measured on its own
 * against the profile without it, on 16 MiB of enwik8. */
#define HASHED 20
#define BASE_HASHED 10
#define H_O1 0
#define H_O2 1
#define H_O3 2
#define H_O4 3
#define H_O6 4
#define H_WORD 5
#define H_FIELD 6
#define H_SPARSE 7
#define H_RECORD 8
#define H_CLASS 9
#define H_WORD2 10
#define H_SPARSE2 11
#define H_INDIRECT 12
#define H_NIBBLES 13
#define H_NEST 14
#define H_O5 15
#define H_O8 16
#define H_WORDLC 17
#define H_WORD3 18
#define H_WORDC 19
/* The kinds only the max profile runs, as a mask over the hashed kinds. */
#define MAX_KINDS                                                                                                   \
    (1u << H_WORD2 | 1u << H_SPARSE2 | 1u << H_INDIRECT | 1u << H_NIBBLES | 1u << H_NEST | 1u << H_O5 | 1u << H_O8 | \
     1u << H_WORDLC | 1u << H_WORD3 | 1u << H_WORDC)
/* Run memories: order-2 and the word for every profile; the max profile
 * adds order-1, -3, -4 and -6, keyed by the hashes their blocks are. */
#define RUNS 6
#define BASE_RUNS 2
#define RUN_SLOTS (1u << 16)
/* Lanes each memory context (order-0 and the hashed ones) feeds the
 * arbiters: its stretched probability alone under the normal and fast
 * profiles; under the max profile two more — the probability in the
 * linear domain and, for a context that has seen one value only, that
 * value's probability signed by it, so an arbiter can weigh a
 * never-contradicted context apart from a mixed one. The lanes lie in
 * planes: every context's stretched probability first, then every
 * context's linear lane, then the third lanes (the order the vector path
 * stores them in); a weight follows its lane, and a sum of integers is
 * the same in any order, so the bytes do not depend on the arrangement. */
#define CTX_LANES(max) ((max) ? 3 : 1)
/* Inputs a first-layer arbiter takes at most: order-0, the hashed
 * contexts, the runs, recall's two, and a bias, under the max profile. */
#define INPUTS (CTX_LANES(1) * (1 + HASHED) + RUNS + 2 + 1)

/* ---- profiles ----
 * Format version ADAPT_FORMAT_VERSION (19) is the normal profile: the
 * model as written, with the contexts a file keeps chosen per file.
 * ADAPT_FAST_VERSION (20) is the fast profile: two hashed contexts chosen
 * per file from FAST_SETS, a direct order-1 memory, the word run, recall,
 * order-0 and the first arbiter keyed by the word run's state, no second
 * or final arbiter and no estimation stage, over a table of at most
 * ADAPT_FAST_TABLE_LOG_MAX blocks (the crate's `Model<FAST>`).
 * ADAPT_MAX_VERSION (21) is the max profile, described at adapt.h and
 * under the `max` flag below. predict() and update() are compiled once
 * per profile with `fast` and `max` constants, so a profile pays only for
 * the parts it runs. */

/* A set that keeps hashed contexts a and b: the contexts it leaves out. */
#define KEEP2(a, b) ((uint32_t)(((1u << HASHED) - 1) & ~(1u << (a) | 1u << (b))))
/* The fast profile's context sets, two hashed contexts apiece so every
 * set costs the decoder the same: order-2 and class, order-2 and order-4,
 * order-1 and order-2. */
static const uint32_t FAST_SETS[ADAPT_FAST_SETS] = {KEEP2(H_O2, H_CLASS), KEEP2(H_O2, H_O4), KEEP2(H_O1, H_O2)};
/* Blocks of the fast profile's direct order-1 memory: 256 contexts of 32
 * nibble blocks, 128 KiB, read like the order-0 memory with no hash. */
#define O1_BLOCKS (256 * 32)
#define O1_BYTES ((uint64_t)O1_BLOCKS * BLOCK)
/* Its arbiter lane, the order-2 run's, which the fast profile does not
 * run; and its map, the slot after the profile's two hashed contexts. */
#define O1_LANE RUN_BASE(1, 0)
#define O1_MAP 3
/* Whether run memory i (order-2, word) runs under a profile: the fast
 * profile keeps the word run only. */
#define RUN_ON(fast, i) (!(fast) || (i) == 1)
/* The arbiter lane of the first run input: after every context slot a
 * profile has, the fast profile's two. */
#define RUN_BASE(fast, max) ((fast) ? CTX_LANES(max) * (1 + 2) : CTX_LANES(max) * (1 + ((max) ? HASHED : BASE_HASHED)))
/* Run memories a profile keeps. */
#define RUNS_OF(max) ((max) ? RUNS : BASE_RUNS)
/* Arbiter lanes a profile uses: order-0, its context slots, its run
 * lanes, recall's two, and the bias. */
#define LANES(fast, max) (RUN_BASE(fast, max) + RUNS_OF(max) + 2 + 1)
#define MIXER_SETS (3 * 8 * 256)
#define MIXER2_SETS (3 * 2 * 256)
/* The third and fourth first-layer arbiters, keyed by eight bits of the
 * order-2 hash and of the word hash with the partial byte: measured
 * 1.5 % on text, where two more (order-3, the recalled byte) lost it.
 * Under the compact side tables (ADAPT_MAX_COMPACT_VERSION) seven bits
 * key them: half the sets, 9.6 MiB each instead of 19, the difference
 * going to the context table at a small budget. */
#define MIX34_LOG(compact) ((compact) ? 15 : 16)
#define MIXER34_SETS(compact) ((size_t)1 << MIX34_LOG(compact))
/* The hash bits that pick a set, above the partial byte's eight. */
#define MIX34_ROW_MASK(compact) ((size_t)(1u << (MIX34_LOG(compact) - 8)) - 1)
/* The final arbiter's inputs: the four verdicts and a bias; the second
 * max model (adapt_sizes.model2) adds a fifth verdict, see MODEL2 below. */
#define FINAL_INPUTS 6
#define FINAL_INPUTS_OF(m2) ((m2) ? 6 : 5)
#define FINAL_INPUTS_ON(m, m2) (((m2) && (m)->extra_on) ? 6 : 5)
/* ---- the second max model (ADAPT_SIZING_MODEL2) ----
 * First-layer weights are int16 in units of 1/32768, saturating at +-1.
 * The dot product and rounded training step have identical scalar and
 * vector semantics. Four slots carry class, indirect and recall contexts:
 * o5 uses the 21-byte class history; o8 the order-1 indirect context;
 * word3 the recalled byte, length bucket and preceding byte; sparse2 the
 * next two recalled bytes. A recall bucket keeps four recent positions and
 * chooses the longest verified match. A fifth first-layer arbiter is keyed
 * by bytes since the last space, preceding byte class and partial byte. */
/* The parts of model2 that pay under table pressure and cost without it
 * — the o8 and word3 slots re-purposed, and the fifth arbiter — run from
 * a table of 2^MODEL2_PRESSURE_LOG blocks, the size the header names,
 * which follows the input's length for a named file (150 KB is 2^20,
 * 8 MiB 2^22, enwik8 2^25). Measured on the mixed corpus at -6: with
 * them always on, five Canterbury rows broke their floors (kennedy.xls
 * +3.7 %, the order-1 indirect context in o8's slot; fields.c, grammar.lsp,
 * xargs.1 +1.5 to +3 %, the untrained fifth arbiter; alice29.txt +0.17 %,
 * recall in word3's slot); gated at 2^21 every row is under its floor,
 * kennedy.xls by 5.8 %, xml (2^21) keeps its 4.4 %, and nothing at 2^22
 * or above changes a byte. */
#define MODEL2_PRESSURE_LOG 21
#define RECALL_WAYS(m2) ((m2) ? 4 : 1)
#define EXTRA_SETS (128 * 256)
#define EXTRA_SETS_OF(m2) ((m2) ? EXTRA_SETS : 0)
#define FINAL_SETS (3 * 2 * 256)
#define APM_BUCKETS 33
#define APM_CONTEXTS (1u << 16)
/* Rows of the second estimation stage: the max profile keys it by ten
 * bits of the order-2 hash (nine under the compact side tables), the
 * normal profile by eight. */
#define APM2_LOG(max, compact) ((max) ? ((compact) ? 17 : 18) : 16)
#define APM2_CONTEXTS(max, compact) ((size_t)1 << APM2_LOG(max, compact))
#define APM2_ROW_MASK(max, compact) ((size_t)(1u << (APM2_LOG(max, compact) - 8)) - 1)
#define BLOCK 16
/* Bytes of a line the record context remembers. */
#define ROW_MAX 4096
#define RATE_LEN 1024
#define PROB_BITS 12
#define PROB_ONE (1u << PROB_BITS)

/* The class-history widths a file may choose, by header code. */
static const uint32_t CLASS_WIDTHS[3] = {0, 8, 21};
/* The bit of a context-set mask that leaves out both run models as well. */
#define NO_RUNS (1u << 31)
/* The contexts a lean compact model leaves out (adapt_sizes.lean). */
#define LEAN_DROPPED (1u << H_O8 | 1u << H_WORD3 | 1u << H_WORDC)
/* The contexts the mid profile leaves out of the normal profile's ten:
 * measured on the first 8 MiB of enwik8 under one arbiter with the
 * class width pinned, each kind left out in turn, field cost nothing,
 * sparse 0.09 %, order-1 0.14 %, every other kind 0.21 % (record) to
 * 0.91 % (the class history); the three together 0.26 % for 18 % fewer
 * cycles. The seven kept are orders 2, 3, 4 and 6, word, record and
 * the class history: with the class lanes, one lane group of eight. */
#define MID_DROPPED (1u << H_O1 | 1u << H_FIELD | 1u << H_SPARSE)
/* The context sets a file may choose, by header code: a mask of hashed
 * contexts the model leaves out (see CONTEXT_SETS in model.rs). */
static const uint32_t CONTEXT_SETS[ADAPT_CONTEXT_SETS] = {
    0,
    1 << H_RECORD | 1 << H_FIELD,
    1 << H_RECORD | 1 << H_FIELD | 1 << H_SPARSE,
    1 << H_WORD | 1 << H_FIELD,
    1 << H_WORD | 1 << H_FIELD | 1 << H_RECORD,
    1 << H_WORD | 1 << H_FIELD | 1 << H_SPARSE,
    1 << H_RECORD | 1 << H_FIELD | 1 << H_SPARSE | NO_RUNS,
    1 << H_WORD | 1 << H_FIELD | 1 << H_RECORD | NO_RUNS,
};
/* The hashed contexts by name, in slot order, for reports. */
static const char *const CONTEXT_NAMES[HASHED] = {"o1",    "o2",     "o3",       "o4",     "o6",    "word",  "field",
                                                   "sparse", "record", "class",   "word2",  "sparse2", "indirect", "nibbles",
                                                   "nest",   "o5",     "o8",      "wordlc", "word3",   "wordc"};

/* ------------------------------------------------------------------------
 * container.rs — constants
 * --------------------------------------------------------------------- */

#define METHOD_STORED_OBSERVED 0
#define METHOD_MODELED 1
#define METHOD_STORED_SKIPPED 2
#define METHOD_END 3
/* The byte before every segment after the first of a segmented file, and
 * the byte before its segment table. */
#define METHOD_SEGMENT 4
#define METHOD_TABLE 5
#define TRAILER_PAYLOAD 12
/* Slots of the encoder's per-block repeat scan; counted in the workspace
 * bound the two sides share. */
#define SCAN_SLOTS (1u << 16)
/* Stack, allocator slack and runtime overhead the memory bounds allow for. */
#define HEADROOM ((uint64_t)8 << 20)

/* Versions written by the current encoder. Older max profiles remain
 * readable, but every plain max encode now writes the sizing byte. */
static inline uint8_t encode_header_version(adapt_sizes sizes) {
    if (sizes.bmp) {
        return (uint8_t)((sizes.max ? ADAPT_MAX_BMP_VERSION : ADAPT_BMP_VERSION) + sizes.adaptive);
    }
    if (sizes.adaptive) {
        return (uint8_t)((sizes.max ? ADAPT_MAX_ADAPTIVE_VERSION : ADAPT_ADAPTIVE_VERSION) + sizes.adaptive - 1);
    }
    if (sizes.max) {
        return sizes.keys == ADAPT_KEYS_TEXT     ? ADAPT_MAX_KEYS_VERSION
               : sizes.keys == ADAPT_KEYS_BINARY ? ADAPT_MAX_KEYS_BINARY_VERSION
                                                 : ADAPT_MAX_PRESSURE_VERSION;
    }
    if (sizes.mid) {
        return sizes.text == ADAPT_TEXT_ON ? ADAPT_TEXT_MID_VERSION : ADAPT_MID_VERSION;
    }
    if (!sizes.fast && sizes.keys != ADAPT_KEYS_CLASSIC) {
        /* the keyed normal profile: the transform rides its policy byte */
        return ADAPT_NORMAL_KEYS_VERSION;
    }
    if (sizes.text == ADAPT_TEXT_ON) {
        return sizes.fast ? ADAPT_TEXT_FAST_VERSION : ADAPT_TEXT_VERSION;
    }
    return sizes.fast ? ADAPT_FAST_VERSION : ADAPT_FORMAT_VERSION;
}

/* ------------------------------------------------------------------------
 * container.rs — the x86 branch transform
 * --------------------------------------------------------------------- */

/* Rewrite, in place, the four-byte little-endian operand after every E8
 * (call) and E9 (jmp) opcode from a target relative to the next
 * instruction into one relative to the block's start, when the
 * displacement is small — a sign-extended 25-bit value, as a branch
 * within a module is — so that repeated calls to one function become
 * identical byte sequences. The result is stored in the same 25-bit
 * form, so the inverse recognises a converted operand by the test that
 * selected it, and an unconverted one, which failed that test, still
 * fails it. Every operand is skipped by both passes and an opcode byte is
 * never rewritten, so the two passes visit the same positions. Per block,
 * so a segment and a stream see the same bytes. (Measured against the
 * alternatives: converting every operand, or storing the address most
 * significant byte first, both coded machine code larger.) */
static inline uint32_t x86_load(const uint8_t *b) {
    return (uint32_t)b[3] << 24 | (uint32_t)b[2] << 16 | (uint32_t)b[1] << 8 | (uint32_t)b[0];
}

static inline void x86_store(uint8_t *b, uint32_t v) {
    b[3] = (uint8_t)(v >> 24);
    b[2] = (uint8_t)(v >> 16);
    b[1] = (uint8_t)(v >> 8);
    b[0] = (uint8_t)v;
}

/* Sign-extend from 25 bits: the operands the transform takes are the
 * fixed points of this function, and it stores results in the same form. */
static inline uint32_t x86_sext(uint32_t v) {
    return (v & 0x01FFFFFFu) | ((v >> 24 & 1) ? 0xFE000000u : 0);
}

static inline void x86_forward(uint8_t *b, size_t n) {
    size_t i = 0;
    while (i + 5 <= n) {
        if (b[i] == 0xE8 || b[i] == 0xE9) {
            uint32_t v = x86_load(b + i + 1);
            if (x86_sext(v) == v) {
                x86_store(b + i + 1, x86_sext(v + (uint32_t)(i + 5)));
            }
            i += 5;
        } else {
            i += 1;
        }
    }
}

static inline void x86_inverse(uint8_t *b, size_t n) {
    size_t i = 0;
    while (i + 5 <= n) {
        if (b[i] == 0xE8 || b[i] == 0xE9) {
            uint32_t v = x86_load(b + i + 1);
            if (x86_sext(v) == v) {
                x86_store(b + i + 1, x86_sext(v - (uint32_t)(i + 5)));
            }
            i += 5;
        } else {
            i += 1;
        }
    }
}

/* Whether the first block announces machine code the transform helps:
 * an ELF, MZ (DOS, PE) or Mach-O (thin, either byte order, or fat)
 * signature. */
static inline int x86_detect(const uint8_t *b, size_t n) {
    if (n >= 4 && b[0] == 0x7F && b[1] == 'E' && b[2] == 'L' && b[3] == 'F') {
        return 1;
    }
    if (n >= 2 && b[0] == 'M' && b[1] == 'Z') {
        return 1;
    }
    if (n >= 4 && ((b[0] == 0xCF && b[1] == 0xFA && b[2] == 0xED && b[3] == 0xFE) ||
                   (b[0] == 0xCE && b[1] == 0xFA && b[2] == 0xED && b[3] == 0xFE) ||
                   (b[0] == 0xFE && b[1] == 0xED && b[2] == 0xFA && b[3] == 0xCF) ||
                   (b[0] == 0xFE && b[1] == 0xED && b[2] == 0xFA && b[3] == 0xCE) ||
                   (b[0] == 0xCA && b[1] == 0xFE && b[2] == 0xBA && b[3] == 0xBE))) {
        return 1;
    }
    return 0;
}

#endif /* ADAPT_CONSTANTS_H */
