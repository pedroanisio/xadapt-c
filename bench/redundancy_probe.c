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
 * Authored with Claude Opus 5 (Claude Code), 2026-09-16.
 */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

typedef struct probe_input {
    uint8_t *bytes;
    size_t length;
} probe_input;

static uint64_t mix64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

/* The `order` bytes before position `at`, hashed into a table slot. The
 * bytes are read from the input rather than from a packed history, so an
 * order above eight is a different context from order eight. */
static size_t context_slot(const uint8_t *bytes, size_t at, int order, uint64_t salt) {
    uint64_t hash = salt;
    int j;
    for (j = order; j > 0; j--) hash = (hash ^ bytes[at - (size_t)j]) * 0x100000001b3ULL;
    return (size_t)(mix64(hash) & (PROBE_TABLE_SLOTS - 1));
}

/* Reads the whole file, or the first `limit` bytes of it when limit is not
 * zero. Returns 0 on success; on failure the reason is already printed and
 * the input is left empty. */
static int read_input(const char *path, uint64_t limit, probe_input *out) {
    FILE *file = fopen(path, "rb");
    long size;
    size_t want;

    out->bytes = NULL;
    out->length = 0;
    if (!file) {
        fprintf(stderr, "redundancy_probe: cannot open %s\n", path);
        return 1;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "redundancy_probe: cannot size %s\n", path);
        fclose(file);
        return 1;
    }
    want = (size_t)size;
    if (limit && limit < (uint64_t)want) want = (size_t)limit;
    if (want == 0) {
        fprintf(stderr, "redundancy_probe: %s is empty\n", path);
        fclose(file);
        return 1;
    }
    out->bytes = malloc(want);
    if (!out->bytes) {
        fprintf(stderr, "redundancy_probe: cannot hold %zu bytes\n", want);
        fclose(file);
        return 1;
    }
    if (fread(out->bytes, 1, want, file) != want) {
        fprintf(stderr, "redundancy_probe: short read on %s\n", path);
        free(out->bytes);
        out->bytes = NULL;
        fclose(file);
        return 1;
    }
    out->length = want;
    fclose(file);
    return 0;
}

/* One LZP configuration: `order` bytes of context, bypass after `run`
 * consecutive correct predictions. */
static int report_lzp_row(const probe_input *in, int order, int run_threshold) {
    uint32_t *table = calloc(PROBE_TABLE_SLOTS, sizeof(uint32_t));
    uint64_t hits = 0, bypassed = 0, missed_in = 0, coded_bits = 0;
    int run = 0;
    size_t i;

    if (!table) {
        fprintf(stderr, "redundancy_probe: cannot hold the LZP table\n");
        return 1;
    }
    for (i = 0; i < in->length; i++) {
        int correct = 0;
        if (i >= (size_t)order) {
            size_t slot = context_slot(in->bytes, i, order, 0x5bd1ULL);
            uint32_t seen_at = table[slot];
            if (seen_at) correct = in->bytes[seen_at] == in->bytes[i];
            table[slot] = (uint32_t)i;
        }
        if (correct) hits++;
        if (run >= run_threshold) {
            bypassed++;
            coded_bits += 1; /* the flag */
            if (!correct) {
                missed_in++;
                coded_bits += 8; /* the escaped byte, modelled as usual */
            }
        } else {
            coded_bits += 8;
        }
        run = correct ? run + 1 : 0;
    }
    free(table);
    printf("lzp   %-5d %-5d %8.2f %10" PRIu64 " %9.2f %9.2f %8.1f\n", order, run_threshold,
           100.0 * (double)hits / (double)in->length, bypassed,
           100.0 * (double)bypassed / (double)in->length,
           bypassed ? 100.0 * (double)missed_in / (double)bypassed : 0.0,
           100.0 * (double)coded_bits / (8.0 * (double)in->length));
    return 0;
}

/* One settled-context gate: orders `low` and `high`, both settled after
 * `min_seen` occurrences, firing only where they agree. */
static int report_gate_row(const probe_input *in, int low, int high, int min_seen) {
    gate_cell *low_table = calloc(PROBE_TABLE_SLOTS, sizeof(gate_cell));
    gate_cell *high_table = calloc(PROBE_TABLE_SLOTS, sizeof(gate_cell));
    uint64_t gated = 0, wrong = 0;
    double share;
    size_t i;

    if (!low_table || !high_table) {
        fprintf(stderr, "redundancy_probe: cannot hold the gate tables\n");
        free(low_table);
        free(high_table);
        return 1;
    }
    for (i = (size_t)high; i < in->length; i++) {
        gate_cell *a = &low_table[context_slot(in->bytes, i, low, 0x9e37ULL)];
        gate_cell *b = &high_table[context_slot(in->bytes, i, high, 0xc2b2ULL)];
        int a_settled = a->seen >= min_seen && !a->mixed;
        int b_settled = b->seen >= min_seen && !b->mixed;
        if (a_settled && b_settled && a->byte == b->byte) {
            gated++;
            if (a->byte != in->bytes[i]) wrong++;
        }
        if (a->seen == 0) {
            a->byte = in->bytes[i];
            a->seen = 1;
        } else {
            if (a->byte != in->bytes[i]) a->mixed = 1;
            if (a->seen < 255) a->seen++;
        }
        if (b->seen == 0) {
            b->byte = in->bytes[i];
            b->seen = 1;
        } else {
            if (b->byte != in->bytes[i]) b->mixed = 1;
            if (b->seen < 255) b->seen++;
        }
    }
    free(low_table);
    free(high_table);
    share = (double)gated / (double)in->length;
    printf("gate  %-5d %-5d %8.2f %10" PRIu64 " %9.2f %9.3f %8.1f\n", low, high, 100.0 * share, gated,
           100.0 * share, gated ? 100.0 * (double)wrong / (double)gated : 0.0,
           100.0 * (share * GATED_WORK_SHARE + (1.0 - share)));
    return 0;
}

static void usage(void) {
    fprintf(stderr,
            "usage: redundancy_probe <file> [--limit <bytes>] [--min-seen <n>]\n"
            "  --limit     read only the first <bytes> of the file (0: all)\n"
            "  --min-seen  occurrences before a gate context counts as settled "
            "(default 2)\n");
}

int main(int argc, char **argv) {
    probe_input in;
    uint64_t limit = 0;
    int min_seen = 2;
    int argi;
    size_t row;

    if (argc < 2) {
        usage();
        return 2;
    }
    for (argi = 2; argi < argc; argi++) {
        if (argi + 1 < argc && strcmp(argv[argi], "--limit") == 0) {
            limit = strtoull(argv[++argi], NULL, 10);
        } else if (argi + 1 < argc && strcmp(argv[argi], "--min-seen") == 0) {
            min_seen = atoi(argv[++argi]);
            if (min_seen < 1 || min_seen > 255) {
                fprintf(stderr, "redundancy_probe: --min-seen out of 1..255\n");
                return 2;
            }
        } else {
            usage();
            return 2;
        }
    }
    if (read_input(argv[1], limit, &in)) return 1;

    printf("input %s  %zu bytes  min-seen %d\n", argv[1], in.length, min_seen);
    printf("pass  %-5s %-5s %8s %10s %9s %9s %8s\n", "loA", "hiB", "hit%", "covered", "cover%", "err%", "work%");
    for (row = 0; row < sizeof(LZP_ORDERS) / sizeof(LZP_ORDERS[0]); row++) {
        size_t k;
        for (k = 0; k < sizeof(LZP_RUNS) / sizeof(LZP_RUNS[0]); k++) {
            if (report_lzp_row(&in, LZP_ORDERS[row], LZP_RUNS[k])) {
                free(in.bytes);
                return 1;
            }
        }
    }
    for (row = 0; row < sizeof(GATE_PAIRS) / sizeof(GATE_PAIRS[0]); row++) {
        if (report_gate_row(&in, GATE_PAIRS[row][0], GATE_PAIRS[row][1], min_seen)) {
            free(in.bytes);
            return 1;
        }
    }
    free(in.bytes);
    return 0;
}
