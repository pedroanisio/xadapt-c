/* What is left for a second transform to remove.
 *
 * The text transform replaces a word both sides have seen three times by one
 * code, and it is worth 1.43x of encode time on enwik8 because time falls
 * with the bytes the model is handed. What it leaves behind is the budget for
 * every transform after it, and this counts that residue.
 *
 * The census models the existing transform's rule — a maximal run of word
 * bytes, replaced once its third occurrence has been seen — and then, over
 * the bytes it does not claim, counts what each candidate front end could
 * take:
 *
 *   runs      a run of one byte repeated three or more times, coded as a
 *             marker, the byte and a length (the RLT idea kanzi carries from
 *             level 8 up)
 *   markup    a complete `<...>` tag treated as one token under the same
 *             seen-three-times rule as a word: enwik8's <page>, <revision>
 *             and <timestamp> skeleton
 *   digits    a maximal run of three or more digits, which timestamps and
 *             identifiers supply in quantity and which no word dictionary
 *             reaches, since every such run is new
 *   utf8      the bytes of a multi-byte UTF-8 sequence, which pack
 *
 * Every figure is an upper bound on bytes removed before the model has a say:
 * a transform that removes bytes the model predicted almost for free returns
 * time and not ratio. Ranking candidates is what this is for; only an encode
 * decides the payback.
 *
 * Standard library only. No codec source or constant is involved.
 * Authored with Claude Opus 5 (Claude Code), 2026-09-17.
 */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Slots in the token tables. Enough that collisions do not drive the answer
 * on a 100 MB text fixture. */
#define CENSUS_TABLE_LOG 22
#define CENSUS_SLOTS ((size_t)1 << CENSUS_TABLE_LOG)
/* Occurrences before the existing transform would code a token. */
#define SEEN_BEFORE_CODED 3
/* Shortest run or digit group a transform would bother with. */
#define MIN_RUN 3
/* Bytes a coded token costs on the wire: a marker and an index. */
#define TOKEN_COST 2
/* Bytes a coded run costs: a marker, the byte and a length. */
#define RUN_COST 3

/* Seven buckets, one per byte: every byte of the input lands in exactly
 * one, so the rows sum to the input and the report cannot double-count a
 * byte that two candidate transforms both want. */
typedef struct census {
    uint64_t total;
    uint64_t word_coded_bytes, word_coded_tokens; /* words the transform already reaches */
    uint64_t word_raw_bytes;                      /* word bytes not yet codeable */
    uint64_t digit_bytes, digit_groups;           /* pure digit runs, which no word book reaches */
    uint64_t utf8_bytes;                          /* bytes at or above 0x80 */
    uint64_t markup_bytes, markup_coded_bytes, markup_coded_tokens;
    uint64_t run_bytes, run_groups;               /* repeated-byte runs outside words */
    uint64_t other_bytes;                         /* separators and punctuation */
} census;

static uint8_t *seen_count;   /* occurrences of a word token, saturating */
static uint8_t *seen_markup;  /* occurrences of a markup token */

static uint64_t mix64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

static int is_word_byte(uint8_t b) {
    return (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9') || b >= 0x80;
}

static int is_digit_byte(uint8_t b) { return b >= '0' && b <= '9'; }

static size_t token_slot(const uint8_t *bytes, size_t at, size_t len) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    size_t j;
    for (j = 0; j < len; j++) hash = (hash ^ bytes[at + j]) * 0x100000001b3ULL;
    return (size_t)(mix64(hash) & (CENSUS_SLOTS - 1));
}

/* Count one token against a table; return 1 once it has been seen enough
 * times that a dictionary transform would code it. */
static int token_is_coded(uint8_t *table, const uint8_t *bytes, size_t at, size_t len) {
    size_t slot = token_slot(bytes, at, len);
    int coded = table[slot] >= SEEN_BEFORE_CODED;
    if (table[slot] < 255) table[slot]++;
    return coded;
}

static void report(const census *c, const char *path) {
    double total = (double)c->total;
    uint64_t run_saved = c->run_bytes > c->run_groups * RUN_COST ? c->run_bytes - c->run_groups * RUN_COST : 0;
    uint64_t digit_saved = c->digit_bytes > c->digit_groups * RUN_COST ? c->digit_bytes - c->digit_groups * RUN_COST : 0;
    uint64_t markup_saved = c->markup_coded_bytes > c->markup_coded_tokens * TOKEN_COST
                                ? c->markup_coded_bytes - c->markup_coded_tokens * TOKEN_COST
                                : 0;
    uint64_t word_saved = c->word_coded_bytes > c->word_coded_tokens * TOKEN_COST
                              ? c->word_coded_bytes - c->word_coded_tokens * TOKEN_COST
                              : 0;

    printf("input %s  %" PRIu64 " bytes\n\n", path, c->total);
    printf("every byte in exactly one bucket\n");
    printf("  %-22s %12" PRIu64 " %7.2f%%   in %" PRIu64 " tokens the word book reaches\n", "word, codeable",
           c->word_coded_bytes, 100.0 * (double)c->word_coded_bytes / total, c->word_coded_tokens);
    printf("  %-22s %12" PRIu64 " %7.2f%%\n", "word, still raw", c->word_raw_bytes,
           100.0 * (double)c->word_raw_bytes / total);
    printf("  %-22s %12" PRIu64 " %7.2f%%   in %" PRIu64 " groups\n", "digit groups", c->digit_bytes,
           100.0 * (double)c->digit_bytes / total, c->digit_groups);
    printf("  %-22s %12" PRIu64 " %7.2f%%\n", "utf-8 high bytes", c->utf8_bytes,
           100.0 * (double)c->utf8_bytes / total);
    printf("  %-22s %12" PRIu64 " %7.2f%%   in %" PRIu64 " tags\n", "markup tokens", c->markup_bytes,
           100.0 * (double)c->markup_bytes / total, c->markup_coded_tokens);
    printf("  %-22s %12" PRIu64 " %7.2f%%   in %" PRIu64 " runs\n", "repeated-byte runs", c->run_bytes,
           100.0 * (double)c->run_bytes / total, c->run_groups);
    printf("  %-22s %12" PRIu64 " %7.2f%%   separators and punctuation\n", "everything else", c->other_bytes,
           100.0 * (double)c->other_bytes / total);

    printf("\nbytes a transform could remove, net of the codes it must write\n");
    printf("  %-22s %12" PRIu64 " %7.2f%%   the word transform, already built\n", "word book", word_saved,
           100.0 * (double)word_saved / total);
    printf("  %-22s %12" PRIu64 " %7.2f%%\n", "markup book", markup_saved, 100.0 * (double)markup_saved / total);
    printf("  %-22s %12" PRIu64 " %7.2f%%\n", "run-length", run_saved, 100.0 * (double)run_saved / total);
    printf("  %-22s %12" PRIu64 " %7.2f%%\n", "digit packing", digit_saved, 100.0 * (double)digit_saved / total);
    printf("  %-22s %12" PRIu64 " %7.2f%%   utf-8 packing, about half of them\n", "utf-8 packing",
           c->utf8_bytes / 2, 100.0 * (double)(c->utf8_bytes / 2) / total);
}

int main(int argc, char **argv) {
    const char *path;
    uint64_t limit = 0;
    FILE *file;
    long size;
    size_t want, i;
    uint8_t *buf;
    census c;

    if (argc < 2) {
        fprintf(stderr, "usage: stream_census <file> [--limit <bytes>]\n");
        return 2;
    }
    path = argv[1];
    if (argc >= 4 && strcmp(argv[2], "--limit") == 0) {
        limit = strtoull(argv[3], NULL, 10);
    } else if (argc != 2) {
        fprintf(stderr, "usage: stream_census <file> [--limit <bytes>]\n");
        return 2;
    }
    file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "stream_census: cannot open %s\n", path);
        return 1;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "stream_census: cannot size %s\n", path);
        fclose(file);
        return 1;
    }
    want = (size_t)size;
    if (limit && limit < (uint64_t)want) want = (size_t)limit;
    if (want == 0) {
        fprintf(stderr, "stream_census: %s is empty\n", path);
        fclose(file);
        return 1;
    }
    buf = malloc(want);
    seen_count = calloc(CENSUS_SLOTS, 1);
    seen_markup = calloc(CENSUS_SLOTS, 1);
    if (!buf || !seen_count || !seen_markup) {
        fprintf(stderr, "stream_census: out of memory\n");
        free(buf);
        free(seen_count);
        free(seen_markup);
        fclose(file);
        return 1;
    }
    if (fread(buf, 1, want, file) != want) {
        fprintf(stderr, "stream_census: short read on %s\n", path);
        free(buf);
        free(seen_count);
        free(seen_markup);
        fclose(file);
        return 1;
    }
    fclose(file);

    memset(&c, 0, sizeof(c));
    c.total = want;
    i = 0;
    while (i < want) {
        uint8_t b = buf[i];
        if (is_word_byte(b)) {
            size_t start = i, len, digits = 0, high = 0;
            while (i < want && is_word_byte(buf[i])) {
                if (is_digit_byte(buf[i])) digits++;
                if (buf[i] >= 0x80) high++;
                i++;
            }
            len = i - start;
            if (token_is_coded(seen_count, buf, start, len)) {
                c.word_coded_bytes += len;
                c.word_coded_tokens++;
            } else if (digits == len && len >= MIN_RUN) {
                /* a pure digit group: never repeats, so no word book reaches it */
                c.digit_bytes += len;
                c.digit_groups++;
            } else {
                c.utf8_bytes += high;
                c.word_raw_bytes += len - high;
            }
            continue;
        }
        if (b == '<') {
            size_t start = i, close = i;
            while (close < want && buf[close] != '>' && close - start < 64) close++;
            if (close < want && buf[close] == '>') {
                size_t len = close - start + 1;
                c.markup_bytes += len;
                if (token_is_coded(seen_markup, buf, start, len)) {
                    c.markup_coded_bytes += len;
                    c.markup_coded_tokens++;
                }
                i = close + 1;
                continue;
            }
        }
        {
            size_t start = i;
            while (i < want && buf[i] == b) i++;
            if (i - start >= MIN_RUN) {
                c.run_bytes += i - start;
                c.run_groups++;
            } else {
                c.other_bytes += i - start;
            }
        }
    }
    report(&c, path);
    free(buf);
    free(seen_count);
    free(seen_markup);
    return 0;
}
