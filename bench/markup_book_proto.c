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
 * This is a measurement instrument, not a codec path: nothing in the build,
 * the library or the CLI loads it, and the files it writes are not
 * containers. Standard library only.
 *
 *   markup_book_proto enc <in> <out>
 *   markup_book_proto dec <in> <out>
 *
 * Authored with Claude Opus 5 (Claude Code), 2026-09-17.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ESC 0x80u
#define ONE_FIRST 0x81u
#define TWO_FIRST 0xC0u
#define THREE_FIRST 0xF0u
#define ONE_CODES (TWO_FIRST - ONE_FIRST)
#define TWO_CODES ((THREE_FIRST - TWO_FIRST) * 128u)
#define THREE_CODES ((0x100u - THREE_FIRST) * 128u * 128u)
#define BOOK_MAX (ONE_CODES + TWO_CODES + THREE_CODES)
#define SIGHTINGS 3u
#define TAG_MAX 64u
#define SLOTS (1u << 22)

typedef struct entry { uint32_t off, len, index, used; } entry;

static uint8_t *book_bytes;
static size_t book_len, book_cap;
static entry *table;
static uint32_t *order_off, *order_len;
static uint32_t book_count;

static uint64_t mix64(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33; return x;
}

static size_t slot_of(const uint8_t *p, size_t len) {
    uint64_t h = 0xcbf29ce484222325ULL;
    size_t i;
    for (i = 0; i < len; i++) h = (h ^ p[i]) * 0x100000001b3ULL;
    return (size_t)(mix64(h) & (SLOTS - 1));
}

static entry *find(const uint8_t *p, size_t len) {
    size_t s = slot_of(p, len);
    while (table[s].used) {
        if (table[s].len == len && memcmp(book_bytes + table[s].off, p, len) == 0) return &table[s];
        s = (s + 1) & (SLOTS - 1);
    }
    table[s].used = 1;
    table[s].len = (uint32_t)len;
    table[s].off = (uint32_t)book_len;
    table[s].index = 0xFFFFFFFFu;
    if (book_len + len > book_cap) {
        book_cap = (book_cap + len) * 2;
        book_bytes = realloc(book_bytes, book_cap);
    }
    memcpy(book_bytes + book_len, p, len);
    book_len += len;
    return &table[s];
}

/* seen counts live in the high bits of index until the entry is admitted */
static uint8_t *seen;

static void emit_code(FILE *out, uint32_t v) {
    if (v < ONE_CODES) {
        fputc((int)(ONE_FIRST + v), out);
    } else if (v < ONE_CODES + TWO_CODES) {
        uint32_t k = v - ONE_CODES;
        fputc((int)(TWO_FIRST + k / 128u), out);
        fputc((int)(0x80u + k % 128u), out);
    } else {
        uint32_t k = v - ONE_CODES - TWO_CODES;
        fputc((int)(THREE_FIRST + k / (128u * 128u)), out);
        fputc((int)(0x80u + (k / 128u) % 128u), out);
        fputc((int)(0x80u + k % 128u), out);
    }
}

static int encode(const char *inp, const char *outp) {
    FILE *in = fopen(inp, "rb"), *out = fopen(outp, "wb");
    uint8_t *buf;
    long size;
    size_t n, i;
    if (!in || !out) { fprintf(stderr, "open\n"); return 1; }
    fseek(in, 0, SEEK_END); size = ftell(in); fseek(in, 0, SEEK_SET);
    n = (size_t)size;
    buf = malloc(n);
    if (fread(buf, 1, n, in) != n) { fprintf(stderr, "read\n"); return 1; }
    fclose(in);
    i = 0;
    while (i < n) {
        uint8_t b = buf[i];
        if (b == '<') {
            size_t close = i;
            while (close < n && buf[close] != '>' && close - i < TAG_MAX) close++;
            if (close < n && buf[close] == '>') {
                size_t len = close - i + 1;
                entry *e = find(buf + i, len);
                size_t s = (size_t)(e - table);
                if (e->index != 0xFFFFFFFFu) {
                    emit_code(out, e->index);
                    i = close + 1;
                    continue;
                }
                if (seen[s] < 255) seen[s]++;
                if (seen[s] >= SIGHTINGS && book_count < BOOK_MAX && len > 3) {
                    e->index = book_count;
                    order_off[book_count] = e->off;
                    order_len[book_count] = e->len;
                    book_count++;
                }
                /* written raw this time */
                { size_t j; for (j = 0; j < len; j++) {
                    if (buf[i + j] >= 0x80) fputc(ESC, out);
                    fputc(buf[i + j], out);
                } }
                i = close + 1;
                continue;
            }
        }
        if (b >= 0x80) fputc(ESC, out);
        fputc(b, out);
        i++;
    }
    fclose(out);
    free(buf);
    return 0;
}

static int decode(const char *inp, const char *outp) {
    FILE *in = fopen(inp, "rb"), *out = fopen(outp, "wb");
    uint8_t *buf;
    long size;
    size_t n, i;
    if (!in || !out) { fprintf(stderr, "open\n"); return 1; }
    fseek(in, 0, SEEK_END); size = ftell(in); fseek(in, 0, SEEK_SET);
    n = (size_t)size;
    buf = malloc(n);
    if (fread(buf, 1, n, in) != n) { fprintf(stderr, "read\n"); return 1; }
    fclose(in);
    i = 0;
    while (i < n) {
        uint8_t b = buf[i];
        uint32_t v;
        if (b == ESC) { fputc(buf[i + 1], out); i += 2; continue; }
        if (b < 0x80) {
            if (b == '<') {
                /* a raw tag: copy it and count it exactly as the encoder did */
                size_t j = i, close;
                size_t plain = 0;
                uint8_t tmp[TAG_MAX + 2];
                close = i;
                while (close < n && plain < TAG_MAX + 1) {
                    uint8_t t = buf[close];
                    if (t == ESC) { tmp[plain++] = buf[close + 1]; close += 2; }
                    else { tmp[plain++] = t; close++; }
                    if (tmp[plain - 1] == '>') break;
                }
                if (plain > 0 && tmp[plain - 1] == '>') {
                    entry *e = find(tmp, plain);
                    size_t s = (size_t)(e - table);
                    if (e->index == 0xFFFFFFFFu) {
                        if (seen[s] < 255) seen[s]++;
                        if (seen[s] >= SIGHTINGS && book_count < BOOK_MAX && plain > 3) {
                            e->index = book_count;
                            order_off[book_count] = e->off;
                            order_len[book_count] = e->len;
                            book_count++;
                        }
                    }
                    fwrite(tmp, 1, plain, out);
                    i = close;
                    continue;
                }
                (void)j;
            }
            fputc(b, out);
            i++;
            continue;
        }
        if (b >= THREE_FIRST) {
            v = ONE_CODES + TWO_CODES + (uint32_t)(b - THREE_FIRST) * 128u * 128u
                + (uint32_t)(buf[i + 1] - 0x80u) * 128u + (uint32_t)(buf[i + 2] - 0x80u);
            i += 3;
        } else if (b >= TWO_FIRST) {
            v = ONE_CODES + (uint32_t)(b - TWO_FIRST) * 128u + (uint32_t)(buf[i + 1] - 0x80u);
            i += 2;
        } else {
            v = (uint32_t)(b - ONE_FIRST);
            i += 1;
        }
        fwrite(book_bytes + order_off[v], 1, order_len[v], out);
    }
    fclose(out);
    free(buf);
    return 0;
}

int main(int argc, char **argv) {
    int rc;
    if (argc != 4) { fprintf(stderr, "usage: markup_proto enc|dec <in> <out>\n"); return 2; }
    table = calloc(SLOTS, sizeof(entry));
    seen = calloc(SLOTS, 1);
    order_off = malloc(BOOK_MAX * sizeof(uint32_t));
    order_len = malloc(BOOK_MAX * sizeof(uint32_t));
    book_cap = 1 << 20;
    book_bytes = malloc(book_cap);
    if (!table || !seen || !order_off || !order_len || !book_bytes) { fprintf(stderr, "oom\n"); return 1; }
    rc = strcmp(argv[1], "enc") == 0 ? encode(argv[2], argv[3])
       : strcmp(argv[1], "dec") == 0 ? decode(argv[2], argv[3])
       : 2;
    return rc;
}
