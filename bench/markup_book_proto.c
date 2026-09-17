/* A markup book, prototyped outside the container so its payback can be
 * measured before a format version is spent on it.
 *
 * The rule mirrors the text transform's (adapt_text.h): a complete `<...>`
 * tag that has been seen three times becomes a code from then on, the book is
 * built as the file is read, and nothing is sent ahead — so it is FMT-2 clean
 * and its inverse never looks ahead. Codes occupy 0x80..0xFF as a one-, two-
 * or three-byte value; a literal byte at or above 0x80 is escaped. The
 * inverse is implemented here, and the tests hold the pair to a byte-exact
 * round trip, because a transform that does not invert is worth nothing
 * whatever it measures.
 *
 * Why it exists: bench/stream_census.c reports that a markup book could reach
 * 53.22 % of the Silesia `xml` row, 17.19 % of `webster` and 1.88 % of
 * enwik8. A census counts bytes a transform could remove; only an encode says
 * what removing them is worth, and the two differ whenever the model was
 * already predicting those bytes for nothing. The measurements this settled
 * are in research/front-end-census-2026-09-17.md.
 *
 * Both sides build one book the same way, so any disagreement between them
 * would be silent corruption rather than a failed decode. Every bound here —
 * the table's occupancy, the arena's size, the reach of a read past the end
 * of the input — therefore fails loudly instead of wrapping or defaulting.
 *
 * This is a measurement instrument, not a codec path: nothing in the build,
 * the library or the CLI loads it, and the files it writes are not
 * containers. Standard library only.
 *
 *   markup_book_proto enc <in> <out>
 *   markup_book_proto dec <in> <out>
 *
 * Authored with Claude Opus 5 (Claude Code), 2026-09-17.
 */
#include "probe_util.h"

#define WHO "markup_book_proto"

/* Wire form, the text transform's shape: a byte below 0x80 stands for
 * itself, ESC precedes a literal byte at or above 0x80, and a code's first
 * byte says its length. */
#define ESC 0x80u
#define ONE_FIRST 0x81u
#define TWO_FIRST 0xC0u
#define THREE_FIRST 0xF0u
#define ONE_CODES (TWO_FIRST - ONE_FIRST)
#define TWO_CODES ((THREE_FIRST - TWO_FIRST) * 128u)
#define THREE_CODES ((0x100u - THREE_FIRST) * 128u * 128u)
#define BOOK_MAX (ONE_CODES + TWO_CODES + THREE_CODES)
/* A tag enters the book at this sighting, and only if a code is shorter. */
#define SIGHTINGS 3u
#define SHORTEST_WORTH_CODING 4u
/* The longest `<...>` this treats as one token. */
#define TAG_MAX 64u
/* Table slots, and the occupancy past which linear probing degrades. */
#define SLOTS (1u << 22)
#define SLOTS_MAX_USED ((SLOTS / 4) * 3)
/* The arena that holds every distinct tag seen, admitted or not. */
#define ARENA_MAX ((size_t)1 << 28)

typedef struct entry {
    uint32_t off, len, index;
    uint8_t used, seen;
} entry;

/* One book, built identically by both sides. */
typedef struct book {
    entry *table;
    size_t used;
    uint8_t *arena;
    size_t arena_len;
    uint32_t *code_off, *code_len;
    uint32_t count;
} book;

static int book_open(book *b) {
    b->table = probe_calloc(WHO, SLOTS, sizeof(*b->table));
    b->arena = probe_calloc(WHO, 1, ARENA_MAX);
    b->code_off = probe_calloc(WHO, BOOK_MAX, sizeof(*b->code_off));
    b->code_len = probe_calloc(WHO, BOOK_MAX, sizeof(*b->code_len));
    b->used = 0;
    b->arena_len = 0;
    b->count = 0;
    return PROBE_EXIT_OK;
}

static void book_close(book *b) {
    free(b->table);
    free(b->arena);
    free(b->code_off);
    free(b->code_len);
}

/* The entry for a token, created on first sight. NULL once the table or the
 * arena is full, which stops the book growing on both sides alike. */
static entry *book_find(book *b, const uint8_t *token, size_t len) {
    size_t slot = (size_t)(probe_hash_seeded(PROBE_FNV_BASIS, token, len) & (SLOTS - 1));
    while (b->table[slot].used) {
        entry *e = &b->table[slot];
        if (e->len == len && memcmp(b->arena + e->off, token, len) == 0) return e;
        slot = (slot + 1) & (SLOTS - 1);
    }
    if (b->used >= SLOTS_MAX_USED || b->arena_len + len > ARENA_MAX) return NULL;
    b->table[slot].used = 1;
    b->table[slot].len = (uint32_t)len;
    b->table[slot].off = (uint32_t)b->arena_len;
    b->table[slot].index = 0xFFFFFFFFu;
    b->table[slot].seen = 0;
    memcpy(b->arena + b->arena_len, token, len);
    b->arena_len += len;
    b->used++;
    return &b->table[slot];
}

/* Count a sighting of a token that was written out raw, and admit it to the
 * book once it has been seen enough times to be worth a code. Both sides run
 * this on exactly the same tokens in the same order. */
static void book_sight(book *b, entry *e) {
    if (!e || e->index != 0xFFFFFFFFu) return;
    if (e->seen < 255) e->seen++;
    if (e->seen >= SIGHTINGS && b->count < BOOK_MAX && e->len >= SHORTEST_WORTH_CODING) {
        e->index = b->count;
        b->code_off[b->count] = e->off;
        b->code_len[b->count] = e->len;
        b->count++;
    }
}

static void emit_code(probe_writer *w, uint32_t v) {
    if (v < ONE_CODES) {
        probe_write_byte(w, (uint8_t)(ONE_FIRST + v));
    } else if (v < ONE_CODES + TWO_CODES) {
        uint32_t k = v - ONE_CODES;
        probe_write_byte(w, (uint8_t)(TWO_FIRST + k / 128u));
        probe_write_byte(w, (uint8_t)(0x80u + k % 128u));
    } else {
        uint32_t k = v - ONE_CODES - TWO_CODES;
        probe_write_byte(w, (uint8_t)(THREE_FIRST + k / (128u * 128u)));
        probe_write_byte(w, (uint8_t)(0x80u + (k / 128u) % 128u));
        probe_write_byte(w, (uint8_t)(0x80u + k % 128u));
    }
}

/* Write a token out as it stands, escaping any byte at or above 0x80. */
static void emit_literal(probe_writer *w, const uint8_t *token, size_t len) {
    size_t j;
    for (j = 0; j < len; j++) {
        if (token[j] >= 0x80) probe_write_byte(w, (uint8_t)ESC);
        probe_write_byte(w, token[j]);
    }
}

/* The length of the `<...>` starting at `at`, or 0 when there is none within
 * TAG_MAX bytes of the end. */
static size_t tag_length(const uint8_t *bytes, size_t at, size_t end) {
    size_t close = at;
    while (close < end && bytes[close] != '>' && close - at < TAG_MAX) close++;
    return (close < end && bytes[close] == '>') ? close - at + 1 : 0;
}

static int encode(const char *in_path, const char *out_path) {
    probe_input in;
    probe_writer out;
    FILE *file;
    book b;
    size_t i;
    int status = probe_read_input(WHO, in_path, 0, &in);

    /* An empty input transforms to an empty output; an unreadable one is a
     * failure, and the two must not share a branch. */
    if (status != PROBE_EXIT_OK) return status;
    file = fopen(out_path, "wb");
    if (!file) {
        probe_free_input(&in);
        return probe_fail(WHO, PROBE_EXIT_FAILED, "cannot write %s", out_path);
    }
    probe_writer_open(&out, file);
    book_open(&b);

    for (i = 0; i < in.length;) {
        uint8_t here = in.bytes[i];
        if (here == '<') {
            size_t len = tag_length(in.bytes, i, in.length);
            if (len != 0) {
                entry *e = book_find(&b, in.bytes + i, len);
                if (e && e->index != 0xFFFFFFFFu) {
                    emit_code(&out, e->index);
                } else {
                    book_sight(&b, e);
                    emit_literal(&out, in.bytes + i, len);
                }
                i += len;
                continue;
            }
        }
        if (here >= 0x80) probe_write_byte(&out, (uint8_t)ESC);
        probe_write_byte(&out, here);
        i++;
    }
    status = probe_writer_close(&out);
    book_close(&b);
    probe_free_input(&in);
    if (fclose(file) != 0) status = PROBE_EXIT_FAILED;
    if (status != PROBE_EXIT_OK) return probe_fail(WHO, status, "cannot write %s", out_path);
    return PROBE_EXIT_OK;
}

static int decode(const char *in_path, const char *out_path) {
    probe_input in;
    probe_writer out;
    FILE *file;
    book b;
    size_t i;
    int status = probe_read_input(WHO, in_path, 0, &in);

    if (status != PROBE_EXIT_OK) return status;
    file = fopen(out_path, "wb");
    if (!file) {
        probe_free_input(&in);
        return probe_fail(WHO, PROBE_EXIT_FAILED, "cannot write %s", out_path);
    }
    probe_writer_open(&out, file);
    book_open(&b);

    for (i = 0; i < in.length;) {
        uint8_t here = in.bytes[i];
        uint32_t v;
        if (here == ESC) {
            if (i + 1 >= in.length) { status = PROBE_EXIT_FAILED; break; }
            probe_write_byte(&out, in.bytes[i + 1]);
            i += 2;
            continue;
        }
        if (here < 0x80) {
            if (here == '<') {
                /* a tag written out raw: rebuild it, and sight it exactly as
                 * the encoder did, so both books hold the same entries */
                uint8_t token[TAG_MAX + 1];
                size_t plain = 0, at = i;
                while (at < in.length && plain <= TAG_MAX) {
                    uint8_t t = in.bytes[at];
                    if (t == ESC) {
                        if (at + 1 >= in.length) break;
                        token[plain++] = in.bytes[at + 1];
                        at += 2;
                    } else {
                        token[plain++] = t;
                        at++;
                    }
                    if (token[plain - 1] == '>') break;
                }
                if (plain > 0 && token[plain - 1] == '>') {
                    book_sight(&b, book_find(&b, token, plain));
                    probe_write(&out, token, plain);
                    i = at;
                    continue;
                }
            }
            probe_write_byte(&out, here);
            i++;
            continue;
        }
        if (here >= THREE_FIRST) {
            if (i + 2 >= in.length) { status = PROBE_EXIT_FAILED; break; }
            v = ONE_CODES + TWO_CODES + (uint32_t)(here - THREE_FIRST) * 128u * 128u
                + (uint32_t)(in.bytes[i + 1] - 0x80u) * 128u + (uint32_t)(in.bytes[i + 2] - 0x80u);
            i += 3;
        } else if (here >= TWO_FIRST) {
            if (i + 1 >= in.length) { status = PROBE_EXIT_FAILED; break; }
            v = ONE_CODES + (uint32_t)(here - TWO_FIRST) * 128u + (uint32_t)(in.bytes[i + 1] - 0x80u);
            i += 2;
        } else {
            v = (uint32_t)(here - ONE_FIRST);
            i += 1;
        }
        if (v >= b.count) { status = PROBE_EXIT_FAILED; break; }
        probe_write(&out, b.arena + b.code_off[v], b.code_len[v]);
    }
    if (status == PROBE_EXIT_OK) status = probe_writer_close(&out);
    book_close(&b);
    probe_free_input(&in);
    if (fclose(file) != 0) status = PROBE_EXIT_FAILED;
    if (status != PROBE_EXIT_OK) return probe_fail(WHO, status, "%s is not this transform's output", in_path);
    return PROBE_EXIT_OK;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: markup_book_proto enc|dec <in> <out>\n");
        return PROBE_EXIT_USAGE;
    }
    if (strcmp(argv[1], "enc") == 0) return encode(argv[2], argv[3]);
    if (strcmp(argv[1], "dec") == 0) return decode(argv[2], argv[3]);
    fprintf(stderr, "usage: markup_book_proto enc|dec <in> <out>\n");
    return PROBE_EXIT_USAGE;
}
