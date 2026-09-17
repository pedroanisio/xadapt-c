/* Prices the two front-end shortcuts that make byte-oriented context-mixing
 * compressors fast, against an input, before either is built into the codec.
 *
 * `lzp` measures a Bloom-style literal-prediction bypass: hash the last N
 * bytes, remember the position that context last occurred at, predict the
 * byte that followed it, and code a flag instead of eight modelled bits
 * while the last K predictions were all right. (C. Bloom, "LZP: a new data
 * compression algorithm", DCC 1996; M. Mahoney, *Data Compression
 * Explained*, the LZP section.)
 *
 * `gate` measures a bypass that needs no flag at all: a context is settled
 * when it has occurred at least `--min-seen` times and was always followed
 * by the same byte, and the gate fires where two orders are both settled
 * and agree. Encoder and decoder derive that predicate from state they
 * already hold, so a gate on it carries no side channel (FMT-2) and costs
 * no flag bit — only whatever accuracy a reduced model loses on the bytes
 * it covers.
 *
 * Both are measured over the input alone and nothing is written, so the
 * probe says what a shortcut *could* cover, never what it would cost in
 * container bytes. That number comes from an encode.
 *
 * The prediction a context makes does not depend on the run threshold that
 * gates the bypass, so each order is hashed once and every threshold is
 * scored against that one pass — the same figures for a fifth of the work.
 *
 * Authored with Claude Opus 5 (Claude Code), 2026-09-16.
 */
#include "probe_util.h"

#include <inttypes.h>

#define WHO "redundancy_probe"

/* Slots in each probe table. Large enough that collisions do not drive the
 * answer on the corpus rows, small enough to stay in a few tens of MiB. */
#define PROBE_TABLE_LOG 22
#define PROBE_TABLE_SLOTS ((size_t)1 << PROBE_TABLE_LOG)

/* Orders the LZP pass reports, and the run lengths it gates on. */
static const int LZP_ORDERS[] = {4, 6, 8, 12, 16};
static const int LZP_RUNS[] = {1, 2, 4, 8, 16, 32};
/* Order pairs the settled-context gate reports. */
static const int GATE_PAIRS[][2] = {{2, 4}, {3, 5}, {4, 6}, {2, 6}, {4, 8}, {6, 8}};

/* What a gated byte is assumed to cost against a fully modelled one, used
 * for the work index only. A reduced path over the order-0 and order-1
 * memories is about a fifth of the max profile's twenty context slots. */
#define GATED_WORK_SHARE 0.2

/* A settled-context cell: the byte this context was followed by, how often
 * the context has been seen, and whether it was ever followed by another. */
typedef struct gate_cell {
    uint8_t byte;
    uint8_t seen;
    uint8_t mixed;
} gate_cell;

/* The `order` bytes before position `at`, hashed into a table slot. The
 * bytes are read from the input rather than from a packed history, so an
 * order above eight is a different context from order eight. */
static size_t context_slot(const uint8_t *bytes, size_t at, int order, uint64_t salt) {
    return (size_t)(probe_hash_seeded(salt, bytes + at - (size_t)order, (size_t)order) & (PROBE_TABLE_SLOTS - 1));
}

/* One pass over the input for a single order: fills `correct[i]` with
 * whether the context before byte i predicted it. Every LZP row for that
 * order is scored from this, because neither the prediction nor the run of
 * consecutive hits depends on the threshold that gates the bypass. */
static void lzp_predictions(const probe_input *in, int order, uint32_t *table, uint8_t *correct) {
    size_t i;
    memset(table, 0, PROBE_TABLE_SLOTS * sizeof(*table));
    memset(correct, 0, (size_t)order < in->length ? (size_t)order : in->length);
    for (i = (size_t)order; i < in->length; i++) {
        size_t slot = context_slot(in->bytes, i, order, 0x5bd1ULL);
        uint32_t seen_at = table[slot];
        correct[i] = (uint8_t)(seen_at != 0 && in->bytes[seen_at] == in->bytes[i]);
        table[slot] = (uint32_t)i;
    }
}

/* Score one bypass threshold against a finished prediction pass. */
static void report_lzp_row(const probe_input *in, const uint8_t *correct, uint64_t hits, int order,
                           int run_threshold) {
    uint64_t bypassed = 0, missed_in = 0, coded_bits = 0;
    int run = 0;
    size_t i;

    for (i = 0; i < in->length; i++) {
        if (run >= run_threshold) {
            bypassed++;
            coded_bits += 1; /* the flag */
            if (!correct[i]) {
                missed_in++;
                coded_bits += 8; /* the escaped byte, modelled as usual */
            }
        } else {
            coded_bits += 8;
        }
        run = correct[i] ? run + 1 : 0;
    }
    printf("lzp   %-5d %-5d %8.2f %10" PRIu64 " %9.2f %9.2f %8.1f\n", order, run_threshold,
           100.0 * (double)hits / (double)in->length, bypassed,
           100.0 * (double)bypassed / (double)in->length,
           bypassed ? 100.0 * (double)missed_in / (double)bypassed : 0.0,
           100.0 * (double)coded_bits / (8.0 * (double)in->length));
}

/* One settled-context gate: orders `low` and `high`, both settled after
 * `min_seen` occurrences, firing only where they agree. */
static void report_gate_row(const probe_input *in, gate_cell *low_table, gate_cell *high_table, int low, int high,
                            int min_seen) {
    uint64_t gated = 0, wrong = 0;
    double share;
    size_t i;

    memset(low_table, 0, PROBE_TABLE_SLOTS * sizeof(*low_table));
    memset(high_table, 0, PROBE_TABLE_SLOTS * sizeof(*high_table));
    for (i = (size_t)high; i < in->length; i++) {
        gate_cell *a = &low_table[context_slot(in->bytes, i, low, 0x9e37ULL)];
        gate_cell *b = &high_table[context_slot(in->bytes, i, high, 0xc2b2ULL)];
        uint8_t here = in->bytes[i];
        if (a->seen >= min_seen && !a->mixed && b->seen >= min_seen && !b->mixed && a->byte == b->byte) {
            gated++;
            if (a->byte != here) wrong++;
        }
        if (a->seen == 0) {
            a->byte = here;
            a->seen = 1;
        } else {
            if (a->byte != here) a->mixed = 1;
            if (a->seen < 255) a->seen++;
        }
        if (b->seen == 0) {
            b->byte = here;
            b->seen = 1;
        } else {
            if (b->byte != here) b->mixed = 1;
            if (b->seen < 255) b->seen++;
        }
    }
    share = (double)gated / (double)in->length;
    printf("gate  %-5d %-5d %8.2f %10" PRIu64 " %9.2f %9.3f %8.1f\n", low, high, 100.0 * share, gated,
           100.0 * share, gated ? 100.0 * (double)wrong / (double)gated : 0.0,
           100.0 * (share * GATED_WORK_SHARE + (1.0 - share)));
}

static int usage(void) {
    fprintf(stderr,
            "usage: redundancy_probe <file> [--limit <bytes>] [--min-seen <n>]\n"
            "  --limit     read only the first <bytes> of the file (0: all)\n"
            "  --min-seen  occurrences before a gate context counts as settled "
            "(default 2)\n");
    return PROBE_EXIT_USAGE;
}

int main(int argc, char **argv) {
    static const char *const OPTIONS[] = {"--limit", "--min-seen"};
    probe_input in;
    uint64_t limit = 0;
    const char *text;
    int min_seen = 2, status;
    size_t row, k;
    uint32_t *lzp_table;
    uint8_t *correct;
    gate_cell *low_table, *high_table;

    if (argc < 2 || !probe_only_options(argc, argv, 2, OPTIONS, sizeof(OPTIONS) / sizeof(OPTIONS[0]))) {
        return usage();
    }
    if (probe_option(argc, argv, 2, "--limit", &text) == 1 && !probe_parse_size(text, &limit)) {
        return probe_fail(WHO, PROBE_EXIT_USAGE, "--limit: not a size: %s", text);
    }
    if (probe_option(argc, argv, 2, "--min-seen", &text) == 1) {
        min_seen = atoi(text);
        if (min_seen < 1 || min_seen > 255) {
            return probe_fail(WHO, PROBE_EXIT_USAGE, "--min-seen out of 1..255");
        }
    }
    status = probe_read_input(WHO, argv[1], limit, &in);
    if (status != PROBE_EXIT_OK) return status;
    if (in.length == 0) {
        return probe_fail(WHO, PROBE_EXIT_FAILED, "%s is empty", argv[1]);
    }

    lzp_table = probe_calloc(WHO, PROBE_TABLE_SLOTS, sizeof(*lzp_table));
    correct = probe_calloc(WHO, in.length, 1);

    printf("input %s  %zu bytes  min-seen %d\n", argv[1], in.length, min_seen);
    printf("pass  %-5s %-5s %8s %10s %9s %9s %8s\n", "loA", "hiB", "hit%", "covered", "cover%", "err%", "work%");
    for (row = 0; row < sizeof(LZP_ORDERS) / sizeof(LZP_ORDERS[0]); row++) {
        uint64_t hits = 0;
        size_t i;
        lzp_predictions(&in, LZP_ORDERS[row], lzp_table, correct);
        for (i = 0; i < in.length; i++) hits += correct[i];
        for (k = 0; k < sizeof(LZP_RUNS) / sizeof(LZP_RUNS[0]); k++) {
            report_lzp_row(&in, correct, hits, LZP_ORDERS[row], LZP_RUNS[k]);
        }
    }
    free(lzp_table);
    free(correct);

    low_table = probe_calloc(WHO, PROBE_TABLE_SLOTS, sizeof(*low_table));
    high_table = probe_calloc(WHO, PROBE_TABLE_SLOTS, sizeof(*high_table));
    for (row = 0; row < sizeof(GATE_PAIRS) / sizeof(GATE_PAIRS[0]); row++) {
        report_gate_row(&in, low_table, high_table, GATE_PAIRS[row][0], GATE_PAIRS[row][1], min_seen);
    }
    free(low_table);
    free(high_table);
    probe_free_input(&in);
    return PROBE_EXIT_OK;
}
