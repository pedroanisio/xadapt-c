/* The text transform: a word both sides have seen written out three times
 * becomes, from then on, a code of one to three bytes at or above 0x80,
 * so the model meets a word as one symbol in the contexts that predict
 * it and codes fewer bytes. Nothing is agreed in advance and nothing is
 * sent ahead: the encoder and the decoder keep the same book of words,
 * each entering a word when it has passed through raw for the third
 * time (TEXT_SIGHTINGS), each giving it the next code, so the decoder,
 * writing the same bytes the encoder read, makes the same book. The
 * book is bounded (TEXT_BOOK_WORDS, TEXT_BOOK_BYTES); when it is full
 * no more words enter it, on both sides alike.
 *
 * Wire form: a byte below 0x80 stands for itself; TEXT_ESC precedes an
 * original byte at or above 0x80; TEXT_CAP or TEXT_UPPER precedes a code
 * whose word is written with its first letter or all its letters
 * upper-case; a code's first byte says its length (TEXT_ONE_FIRST..0xBF
 * one byte, TEXT_TWO_FIRST..0xEF two, TEXT_THREE_FIRST..0xFF three; the
 * bytes after the first are 0x80 to 0xFF), its value the order the word
 * entered the book. A word is a maximal run of ASCII letters, two to
 * TEXT_WORD_MAX long, lower-case, capitalised or all upper-case; one that
 * is longer, mixes its case otherwise, is not yet in the book or is not
 * shorter than its code is written as it is — and, when its shape allows
 * it, counted as a sighting. Every other byte is written as it is, so the
 * transform is invertible byte for byte and its inverse never looks
 * ahead.
 *
 * Measured on enwik8 with the model unchanged, --max --allocation
 * context at 128 MiB: 19,657,760 -> 18,958,016 bytes (-3.56 %), the
 * model coding 62 % of the bytes. A dictionary built in a first pass
 * and sent ahead of the text measured 18,980,671 and was set aside:
 * both sides can build the book as they go (FMT-2, FMT-8). Keeping the
 * space before a word measured better than implying it; case flags
 * measured better than a case-sensitive book; entering a word at its
 * second sighting measured 18,993,159, at its third 18,958,016. */
#ifndef ADAPT_TEXT_H
#define ADAPT_TEXT_H
#include "adapt.h"

#include <stdlib.h>
#include <string.h>

#define TEXT_ESC 0x80u
#define TEXT_CAP 0x81u
#define TEXT_UPPER 0x82u
#define TEXT_RESERVED 0x83u
#define TEXT_ONE_FIRST 0x84u
#define TEXT_TWO_FIRST 0xC0u
#define TEXT_THREE_FIRST 0xF0u
#define TEXT_ONE_CODES (TEXT_TWO_FIRST - TEXT_ONE_FIRST)            /* 60 */
#define TEXT_TWO_CODES ((TEXT_THREE_FIRST - TEXT_TWO_FIRST) * 128u)  /* 6,144 */
#define TEXT_THREE_CODES ((0x100u - TEXT_THREE_FIRST) * 128u * 128u) /* 262,144 */
#define TEXT_WORD_MIN 2u
#define TEXT_WORD_MAX 32u
/* A word enters the book at this sighting. */
#define TEXT_SIGHTINGS 3u
/* The book's bounds: words, and bytes of their letters (each word takes
 * its length and its letters). Its index has TEXT_INDEX_SLOTS entries and
 * its sighting table TEXT_SEEN_SLOTS; everything together is
 * TEXT_STATE_BYTES, which the memory bounds' HEADROOM allows for. */
#define TEXT_BOOK_WORDS 65536u
#define TEXT_BOOK_BYTES ((size_t)1 << 20)
#define TEXT_INDEX_SLOTS ((size_t)1 << 17)
#define TEXT_SEEN_SLOTS ((size_t)1 << 17)
#define TEXT_STATE_BYTES                                                                              \
    (TEXT_BOOK_BYTES + (size_t)TEXT_BOOK_WORDS * sizeof(uint32_t) + TEXT_INDEX_SLOTS * sizeof(uint32_t) + \
     TEXT_SEEN_SLOTS * (sizeof(uint32_t) + 1))

static inline int text_is_letter(uint8_t b) {
    return (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z');
}

static inline uint8_t text_lower(uint8_t b) {
    return (uint8_t)((b >= 'A' && b <= 'Z') ? b + 32 : b);
}

static inline unsigned text_code_len(uint32_t rank) {
    return rank < TEXT_ONE_CODES ? 1u : (rank < TEXT_ONE_CODES + TEXT_TWO_CODES ? 2u : 3u);
}

/* The code of the word ranked `rank`, into `out`; its length. */
static inline unsigned text_code(uint32_t rank, uint8_t *out) {
    if (rank < TEXT_ONE_CODES) {
        out[0] = (uint8_t)(TEXT_ONE_FIRST + rank);
        return 1;
    }
    rank -= TEXT_ONE_CODES;
    if (rank < TEXT_TWO_CODES) {
        out[0] = (uint8_t)(TEXT_TWO_FIRST + rank / 128);
        out[1] = (uint8_t)(0x80 + rank % 128);
        return 2;
    }
    rank -= TEXT_TWO_CODES;
    out[0] = (uint8_t)(TEXT_THREE_FIRST + rank / 16384);
    out[1] = (uint8_t)(0x80 + (rank / 128) % 128);
    out[2] = (uint8_t)(0x80 + rank % 128);
    return 3;
}

/* The case a word is written in: 0 lower, TEXT_CAP, TEXT_UPPER, or -1
 * for a pattern the transform leaves as it is. */
static inline int text_shape(const uint8_t *w, size_t n) {
    size_t i, upper = 0;
    for (i = 0; i < n; i++) {
        upper += (w[i] >= 'A' && w[i] <= 'Z');
    }
    if (upper == 0) return 0;
    if (upper == n) return TEXT_UPPER;
    if (upper == 1 && w[0] >= 'A' && w[0] <= 'Z') return TEXT_CAP;
    return -1;
}

static inline uint64_t text_hash(const uint8_t *w, size_t n) {
    uint64_t h = 0xCBF29CE484222325ull ^ n;
    size_t i;
    for (i = 0; i < n; i++) {
        h = (h ^ w[i]) * 0x100000001B3ull;
    }
    h ^= h >> 31;
    h *= 0x9E3779B97F4A7C15ull;
    h ^= h >> 29;
    return h;
}

/* ---- the book both sides keep ------------------------------------- */

typedef struct text_book {
    uint8_t *arena;    /* each word: its length, then its letters */
    uint32_t *offset;  /* per code, where its word starts */
    uint32_t *index;   /* open addressing over hashes: code + 1, 0 empty */
    uint32_t *seen_tag;
    uint8_t *seen_count;
    uint32_t words;
    size_t arena_len;
} text_book;

static inline void text_book_free(text_book *b) {
    free(b->arena);
    free(b->offset);
    free(b->index);
    free(b->seen_tag);
    free(b->seen_count);
    memset(b, 0, sizeof(*b));
}

static inline int text_book_init(text_book *b) {
    memset(b, 0, sizeof(*b));
    b->arena = malloc(TEXT_BOOK_BYTES);
    b->offset = malloc((size_t)TEXT_BOOK_WORDS * sizeof(uint32_t));
    b->index = calloc(TEXT_INDEX_SLOTS, sizeof(uint32_t));
    b->seen_tag = calloc(TEXT_SEEN_SLOTS, sizeof(uint32_t));
    b->seen_count = calloc(TEXT_SEEN_SLOTS, 1);
    if (b->arena == NULL || b->offset == NULL || b->index == NULL || b->seen_tag == NULL || b->seen_count == NULL) {
        text_book_free(b);
        return 0;
    }
    return 1;
}

static inline const uint8_t *text_book_word(const text_book *b, uint32_t code, size_t *n) {
    const uint8_t *at = b->arena + b->offset[code];
    *n = at[0];
    return at + 1;
}

/* The code of a lower-case word, or -1. */
static inline int64_t text_book_find(const text_book *b, const uint8_t *w, size_t n, uint64_t h) {
    size_t i = (size_t)h & (TEXT_INDEX_SLOTS - 1);
    while (b->index[i] != 0) {
        size_t m;
        const uint8_t *v = text_book_word(b, b->index[i] - 1, &m);
        if (m == n && memcmp(v, w, n) == 0) return (int64_t)b->index[i] - 1;
        i = (i + 1) & (TEXT_INDEX_SLOTS - 1);
    }
    return -1;
}

/* A word passed through written out: its sighting. At the third it
 * enters the book, when the book has room. The sighting table is direct
 * mapped and a word that lands on another's slot takes it over — the
 * same on both sides, so the books agree. */
static inline void text_book_sight(text_book *b, const uint8_t *w, size_t n, uint64_t h) {
    size_t i = (size_t)(h >> 20) & (TEXT_SEEN_SLOTS - 1);
    uint32_t tag = (uint32_t)(h >> 40) | 1u;
    if (b->seen_tag[i] != tag) {
        b->seen_tag[i] = tag;
        b->seen_count[i] = 0;
    }
    if (b->seen_count[i] < TEXT_SIGHTINGS) b->seen_count[i] += 1;
    if (b->seen_count[i] == TEXT_SIGHTINGS && b->words < TEXT_BOOK_WORDS && b->arena_len + n + 1 <= TEXT_BOOK_BYTES &&
        text_book_find(b, w, n, h) < 0) {
        size_t j = (size_t)h & (TEXT_INDEX_SLOTS - 1);
        b->offset[b->words] = (uint32_t)b->arena_len;
        b->arena[b->arena_len] = (uint8_t)n;
        memcpy(b->arena + b->arena_len + 1, w, n);
        b->arena_len += n + 1;
        while (b->index[j] != 0) j = (j + 1) & (TEXT_INDEX_SLOTS - 1);
        b->index[j] = ++b->words;
        b->seen_count[i] = TEXT_SIGHTINGS + 1; /* in the book: sightings no longer matter */
    }
}

/* ---- the forward transform, as a stream ----------------------------- */

/* Bytes the forward transform may write past two per input byte: a word
 * held over from the last call, written out. */
#define TEXT_FORWARD_SLACK (TEXT_WORD_MAX + 2)

typedef struct text_forward {
    text_book book;
    uint8_t tok[TEXT_WORD_MAX];
    unsigned tok_len, overflow;
} text_forward;

static inline int text_forward_init(text_forward *f) {
    memset(f, 0, sizeof(*f));
    return text_book_init(&f->book);
}

static inline void text_forward_free(text_forward *f) {
    text_book_free(&f->book);
    memset(f, 0, sizeof(*f));
}

/* The word held: as its code, or written out and sighted. */
static inline size_t text_forward_flush(text_forward *f, uint8_t *out) {
    size_t n = 0;
    if (f->tok_len >= TEXT_WORD_MIN && !f->overflow) {
        int shape = text_shape(f->tok, f->tok_len);
        if (shape >= 0) {
            uint8_t lower[TEXT_WORD_MAX];
            unsigned i;
            uint64_t h;
            int64_t code;
            for (i = 0; i < f->tok_len; i++) lower[i] = text_lower(f->tok[i]);
            h = text_hash(lower, f->tok_len);
            code = text_book_find(&f->book, lower, f->tok_len, h);
            if (code >= 0 && text_code_len((uint32_t)code) < f->tok_len) {
                if (shape != 0) out[n++] = (uint8_t)shape;
                n += text_code((uint32_t)code, out + n);
                f->tok_len = 0;
                return n;
            }
            text_book_sight(&f->book, lower, f->tok_len, h);
        }
    }
    memcpy(out, f->tok, f->tok_len);
    n = f->tok_len;
    f->tok_len = 0;
    return n;
}

/* Transform `n` input bytes into `out`, which holds at least
 * 2 * n + TEXT_FORWARD_SLACK bytes; the bytes written. A word still open
 * at the end stays held until the next call or text_forward_end(). */
static inline size_t text_forward_feed(text_forward *f, const uint8_t *p, size_t n, uint8_t *out) {
    size_t i, w = 0;
    for (i = 0; i < n; i++) {
        uint8_t b = p[i];
        if (text_is_letter(b)) {
            if (!f->overflow && f->tok_len < TEXT_WORD_MAX) {
                f->tok[f->tok_len++] = b;
            } else {
                /* past the longest word: written as it is, to the run's end */
                if (!f->overflow) {
                    memcpy(out + w, f->tok, f->tok_len);
                    w += f->tok_len;
                    f->tok_len = 0;
                    f->overflow = 1;
                }
                out[w++] = b;
            }
        } else {
            if (f->tok_len) w += text_forward_flush(f, out + w);
            f->overflow = 0;
            if (b >= 0x80) out[w++] = TEXT_ESC;
            out[w++] = b;
        }
    }
    return w;
}

/* The bytes of a word still held when the input ends: at most
 * TEXT_FORWARD_SLACK. */
static inline size_t text_forward_end(text_forward *f, uint8_t *out) {
    size_t n = f->tok_len ? text_forward_flush(f, out) : 0;
    f->overflow = 0;
    return n;
}

/* ---- the inverse transform, as a stream ----------------------------- */

typedef struct text_inverse {
    text_book book;
    uint8_t tok[TEXT_WORD_MAX]; /* letters written out, for the sighting */
    unsigned tok_len, overflow;
    unsigned flag;     /* TEXT_CAP, TEXT_UPPER or 0 */
    unsigned pending;  /* code bytes still to come: 0..2 */
    unsigned escaped;  /* the next byte is an escaped literal */
    unsigned bad;
    uint32_t rank, base;
} text_inverse;

static inline int text_inverse_init(text_inverse *v) {
    memset(v, 0, sizeof(*v));
    return text_book_init(&v->book);
}

static inline void text_inverse_free(text_inverse *v) {
    text_book_free(&v->book);
    memset(v, 0, sizeof(*v));
}

/* A written-out word has ended: the sighting the encoder made. */
static inline void text_inverse_word_end(text_inverse *v) {
    if (v->tok_len >= TEXT_WORD_MIN && !v->overflow && text_shape(v->tok, v->tok_len) >= 0) {
        uint8_t lower[TEXT_WORD_MAX];
        unsigned i;
        for (i = 0; i < v->tok_len; i++) lower[i] = text_lower(v->tok[i]);
        text_book_sight(&v->book, lower, v->tok_len, text_hash(lower, v->tok_len));
    }
    v->tok_len = 0;
    v->overflow = 0;
}

/* A literal the inverse writes: a letter extends the word being written
 * out, anything else ends it. */
static inline void text_inverse_literal(text_inverse *v, uint8_t b) {
    if (text_is_letter(b)) {
        if (v->tok_len < TEXT_WORD_MAX) {
            v->tok[v->tok_len++] = b;
        } else {
            v->overflow = 1;
        }
    } else {
        text_inverse_word_end(v);
    }
}

/* Bytes the inverse may write for one input byte: a word. */
#define TEXT_INVERSE_SLACK TEXT_WORD_MAX

/* Undo `n` transformed bytes into `out`, which holds at least
 * TEXT_INVERSE_SLACK * n bytes; the bytes written, or (size_t)-1 when the
 * stream is malformed: a code past the book, a flag before anything but
 * a code, a code byte below 0x80, the reserved byte. */
static inline size_t text_inverse_feed(text_inverse *v, const uint8_t *p, size_t n, uint8_t *out) {
    size_t i, w = 0;
    if (v->bad) return (size_t)-1;
    for (i = 0; i < n; i++) {
        uint8_t b = p[i];
        uint32_t code;
        size_t m, k;
        const uint8_t *word;
        if (v->escaped) {
            /* an original byte at or above 0x80: no letter, it ends a word */
            text_inverse_word_end(v);
            out[w++] = b;
            v->escaped = 0;
            continue;
        }
        if (v->pending) {
            if (b < 0x80) {
                v->bad = 1;
                return (size_t)-1;
            }
            v->rank = v->rank * 128 + (b - 0x80);
            if (--v->pending) continue;
        } else if (b < 0x80) {
            if (v->flag) {
                v->bad = 1;
                return (size_t)-1;
            }
            text_inverse_literal(v, b);
            out[w++] = b;
            continue;
        } else if (b == TEXT_ESC) {
            if (v->flag) {
                v->bad = 1;
                return (size_t)-1;
            }
            v->escaped = 1;
            continue;
        } else if (b == TEXT_CAP || b == TEXT_UPPER) {
            if (v->flag) {
                v->bad = 1;
                return (size_t)-1;
            }
            text_inverse_word_end(v);
            v->flag = b;
            continue;
        } else if (b == TEXT_RESERVED) {
            v->bad = 1;
            return (size_t)-1;
        } else if (b < TEXT_TWO_FIRST) {
            v->rank = b - TEXT_ONE_FIRST;
            v->base = 0;
        } else if (b < TEXT_THREE_FIRST) {
            v->rank = b - TEXT_TWO_FIRST;
            v->base = TEXT_ONE_CODES;
            v->pending = 1;
            continue;
        } else {
            v->rank = b - TEXT_THREE_FIRST;
            v->base = TEXT_ONE_CODES + TEXT_TWO_CODES;
            v->pending = 2;
            continue;
        }
        /* a code is complete: the word it names, in the case flagged */
        text_inverse_word_end(v);
        code = v->base + v->rank;
        if (code >= v->book.words) {
            v->bad = 1;
            return (size_t)-1;
        }
        word = text_book_word(&v->book, code, &m);
        if (v->flag == TEXT_UPPER) {
            for (k = 0; k < m; k++) out[w++] = (uint8_t)(word[k] - 32);
        } else if (v->flag == TEXT_CAP) {
            out[w++] = (uint8_t)(word[0] - 32);
            for (k = 1; k < m; k++) out[w++] = word[k];
        } else {
            memcpy(out + w, word, m);
            w += m;
        }
        v->flag = 0;
    }
    return w;
}

/* Whether the stream ended whole: no code, flag or escape left open. */
static inline int text_inverse_complete(const text_inverse *v) {
    return !v->bad && v->pending == 0 && v->flag == 0 && !v->escaped;
}

/* Whether a block looks like the text the transform pays on: at least
 * two fifths of its bytes in runs of two or more ASCII letters and fewer
 * than one in twenty-five at or above 0x80 (each of those costs an
 * escape). The encoder decides from the first block, as it does for the
 * x86 transform; measured on enwik8 the codes take a third of the bytes
 * away, on binaries the escapes would add them. */
static inline int text_detect(const uint8_t *p, size_t n) {
    size_t i, letters = 0, high = 0, run = 0;
    if (n < 1024) return 0;
    for (i = 0; i < n; i++) {
        if (text_is_letter(p[i])) {
            run++;
        } else {
            if (run >= TEXT_WORD_MIN) letters += run;
            run = 0;
            high += p[i] >= 0x80;
        }
    }
    if (run >= TEXT_WORD_MIN) letters += run;
    return letters * 5 >= n * 2 && high * 25 < n;
}

#endif
