/*
 * test_adapt_text.c — the text transform on its own: codes and their
 * ranks, the book both sides build as they go, the forward and inverse
 * streams under every chunk size a pipe might deliver, the inverse's
 * refusal of every malformed stream — a code past the book, a flag
 * before anything but a code, a code byte below 0x80, the reserved byte
 * — and the detector.
 *
 *   test_adapt_text <vectors-dir>
 */
#include "../adapt_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                                                 \
    do {                                                                                            \
        checks++;                                                                                   \
        if (!(cond)) {                                                                              \
            failures++;                                                                             \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);                \
        }                                                                                           \
    } while (0)

static const char *vectors_dir;

static uint8_t *read_file(const char *name, size_t *len) {
    char path[4096];
    FILE *f;
    uint8_t *data;
    long n;
    snprintf(path, sizeof(path), "%s/%s", vectors_dir, name);
    f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = malloc((size_t)n + 1);
    if (fread(data, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(2);
    }
    fclose(f);
    *len = (size_t)n;
    return data;
}

/* Transform `data` in `chunk`-byte pieces and undo the result in
 * `chunk`-byte pieces; the bytes must come back and both books must
 * agree. Returns the transformed length (0 on any failure) and, when
 * asked, the transformed bytes. */
static size_t round_trip(const uint8_t *data, size_t len, size_t chunk, uint8_t **transformed_out) {
    text_forward fwd;
    text_inverse inv;
    uint8_t *t = malloc(2 * len + TEXT_FORWARD_SLACK + 1);
    uint8_t *back = malloc(TEXT_INVERSE_SLACK * (chunk ? chunk : 1) + 1);
    uint8_t *got = malloc(len + 1);
    size_t tl = 0, gl = 0, i, ok = 1;
    CHECK(text_forward_init(&fwd));
    for (i = 0; i < len; i += chunk) {
        tl += text_forward_feed(&fwd, data + i, len - i < chunk ? len - i : chunk, t + tl);
    }
    tl += text_forward_end(&fwd, t + tl);
    CHECK(tl <= 2 * len + TEXT_FORWARD_SLACK);
    CHECK(text_inverse_init(&inv));
    for (i = 0; i < tl && ok; i += chunk) {
        size_t n = tl - i < chunk ? tl - i : chunk;
        size_t w = text_inverse_feed(&inv, t + i, n, back);
        if (w == (size_t)-1 || gl + w > len) {
            ok = 0;
            break;
        }
        memcpy(got + gl, back, w);
        gl += w;
    }
    CHECK(ok);
    CHECK(text_inverse_complete(&inv));
    CHECK(gl == len && memcmp(got, data, len) == 0);
    /* the two books are the same book */
    CHECK(fwd.book.words == inv.book.words && fwd.book.arena_len == inv.book.arena_len);
    CHECK(memcmp(fwd.book.arena, inv.book.arena, fwd.book.arena_len) == 0);
    CHECK(memcmp(fwd.book.offset, inv.book.offset, fwd.book.words * sizeof(uint32_t)) == 0);
    if (transformed_out != NULL) {
        *transformed_out = t;
    } else {
        free(t);
    }
    free(back);
    free(got);
    text_inverse_free(&inv);
    text_forward_free(&fwd);
    return ok && gl == len ? tl : 0;
}

static void test_codes(void) {
    uint8_t code[3];
    uint32_t ranks[] = {0, 1, TEXT_ONE_CODES - 1, TEXT_ONE_CODES, TEXT_ONE_CODES + 127, TEXT_ONE_CODES + 128,
                        TEXT_ONE_CODES + TEXT_TWO_CODES - 1, TEXT_ONE_CODES + TEXT_TWO_CODES,
                        TEXT_ONE_CODES + TEXT_TWO_CODES + 16383, TEXT_ONE_CODES + TEXT_TWO_CODES + 16384,
                        TEXT_ONE_CODES + TEXT_TWO_CODES + TEXT_THREE_CODES - 1};
    size_t i;
    CHECK(TEXT_ONE_CODES == 60 && TEXT_TWO_CODES == 6144 && TEXT_THREE_CODES == 262144);
    CHECK(TEXT_BOOK_WORDS <= TEXT_ONE_CODES + TEXT_TWO_CODES + TEXT_THREE_CODES);
    CHECK(TEXT_STATE_BYTES < (size_t)3 << 20);
    for (i = 0; i < sizeof(ranks) / sizeof(ranks[0]); i++) {
        unsigned n = text_code(ranks[i], code);
        uint32_t back;
        CHECK(n == text_code_len(ranks[i]));
        CHECK(code[0] >= TEXT_ONE_FIRST);
        if (n == 1) {
            back = code[0] - TEXT_ONE_FIRST;
        } else if (n == 2) {
            CHECK(code[0] >= TEXT_TWO_FIRST && code[0] < TEXT_THREE_FIRST && code[1] >= 0x80);
            back = TEXT_ONE_CODES + (code[0] - TEXT_TWO_FIRST) * 128 + (code[1] - 0x80);
        } else {
            CHECK(code[0] >= TEXT_THREE_FIRST && code[1] >= 0x80 && code[2] >= 0x80);
            back = TEXT_ONE_CODES + TEXT_TWO_CODES + (code[0] - TEXT_THREE_FIRST) * 16384 + (code[1] - 0x80) * 128 +
                   (code[2] - 0x80);
        }
        CHECK(back == ranks[i]);
    }
    CHECK(text_shape((const uint8_t *)"word", 4) == 0);
    CHECK(text_shape((const uint8_t *)"Word", 4) == TEXT_CAP);
    CHECK(text_shape((const uint8_t *)"WORD", 4) == TEXT_UPPER);
    CHECK(text_shape((const uint8_t *)"WoRd", 4) == -1);
    CHECK(text_shape((const uint8_t *)"wOrd", 4) == -1);
    CHECK(text_shape((const uint8_t *)"A", 1) == TEXT_UPPER);
    CHECK(text_is_letter('a') && text_is_letter('Z') && !text_is_letter('0') && !text_is_letter(0x80) &&
          !text_is_letter(' '));
}

static void test_book(void) {
    /* a word enters at its third raw sighting and is coded from the
     * fourth; its case flags ride with the code; a two-letter word past
     * the one-byte codes is never coded, and is sighted every time */
    static const char text[] = "the The THE the the tHe of of of of";
    uint8_t *t;
    size_t tl = round_trip((const uint8_t *)text, sizeof(text) - 1, 4096, &t);
    /* "the" x3 raw (3+1+3+1+3+1), then code, ' ', code, ' ', "tHe" raw, ' ', "of" x3 raw, ' ', code */
    CHECK(tl == 3 + 1 + 3 + 1 + 3 + 1 + 1 + 1 + 1 + 1 + 3 + 1 + 2 + 1 + 2 + 1 + 2 + 1 + 1);
    CHECK(memcmp(t, "the The THE ", 12) == 0 && t[12] == TEXT_ONE_FIRST && t[13] == ' ' && t[14] == TEXT_ONE_FIRST);
    CHECK(memcmp(t + 15, " tHe of of of ", 14) == 0 && t[29] == TEXT_ONE_FIRST + 1);
    free(t);
    {
        /* sixty-one distinct three-letter words: the sixty-first gets a
         * two-byte code and a two-letter word after it is not coded */
        uint8_t buf[64 * 4 * 4 + 32];
        size_t at = 0, i, r;
        for (r = 0; r < 4; r++) {
            for (i = 0; i < 61; i++) {
                buf[at++] = (uint8_t)('a' + i / 26);
                buf[at++] = (uint8_t)('a' + i % 26);
                buf[at++] = 'x';
                buf[at++] = ' ';
            }
        }
        memcpy(buf + at, "of of of of ", 12);
        at += 12;
        tl = round_trip(buf, at, 4096, &t);
        /* three rounds raw (61 x 4), the fourth 60 one-byte codes and one two-byte code, each with a space,
         * then "of" four times raw */
        CHECK(tl == 3 * 61 * 4 + 60 * 2 + 3 + 12);
        CHECK(t[3 * 61 * 4] == TEXT_ONE_FIRST && t[3 * 61 * 4 + 60 * 2] == TEXT_TWO_FIRST &&
              t[3 * 61 * 4 + 60 * 2 + 1] == 0x80);
        CHECK(memcmp(t + tl - 12, "of of of of ", 12) == 0);
        free(t);
    }
    {
        /* a full book takes no more words; the sighting table's takeover
         * keeps both sides in step */
        uint8_t *buf = malloc((size_t)70000 * 8 * 3);
        size_t at = 0, i, r;
        text_forward f;
        for (r = 0; r < 3; r++) {
            for (i = 0; i < 70000; i++) {
                uint32_t v = (uint32_t)i;
                size_t k;
                for (k = 0; k < 6; k++) {
                    buf[at++] = (uint8_t)('a' + v % 26);
                    v /= 26;
                }
                buf[at++] = ' ';
            }
        }
        tl = round_trip(buf, at, 65536, NULL);
        CHECK(tl > 0);
        CHECK(text_forward_init(&f));
        {
            /* every word three times in a row: nothing takes another's
             * slot, so the book fills to its cap and stops */
            uint8_t word[7];
            uint8_t out[64];
            for (i = 0; i < 70000; i++) {
                uint32_t v = (uint32_t)i;
                size_t k;
                for (k = 0; k < 6; k++) {
                    word[k] = (uint8_t)('a' + v % 26);
                    v /= 26;
                }
                word[6] = ' ';
                for (r = 0; r < 3; r++) text_forward_feed(&f, word, 7, out);
                if (i == TEXT_BOOK_WORDS - 1) CHECK(f.book.words == TEXT_BOOK_WORDS);
            }
            CHECK(f.book.words == TEXT_BOOK_WORDS && f.book.arena_len == (size_t)TEXT_BOOK_WORDS * 7);
            /* the words past the cap stay raw */
            CHECK(text_forward_feed(&f, word, 7, out) == 7 && memcmp(out, word, 7) == 0);
        }
        text_forward_free(&f);
        free(buf);
    }
}

/* The inverse on a hand-built stream, its book primed by `raw` (fed
 * through the inverse first) then `body`. */
static size_t inverse_of(const char *raw, const uint8_t *body, size_t body_len, uint8_t *out, int *complete) {
    text_inverse v;
    size_t w, w2;
    CHECK(text_inverse_init(&v));
    w = text_inverse_feed(&v, (const uint8_t *)raw, strlen(raw), out);
    if (w != (size_t)-1) {
        w2 = text_inverse_feed(&v, body, body_len, out + w);
        w = w2 == (size_t)-1 ? w2 : w + w2;
    }
    *complete = text_inverse_complete(&v);
    text_inverse_free(&v);
    return w;
}

static void test_inverse_failures(void) {
    uint8_t out[4096];
    int complete;
    static const char primed[] = "the the the of of of ";
    static const uint8_t ok_body[] = {'a', ' ', TEXT_ONE_FIRST, ' ', TEXT_CAP, TEXT_ONE_FIRST + 1, ' ', TEXT_UPPER,
                                      TEXT_ONE_FIRST, TEXT_ESC, 0xC3, TEXT_ESC, TEXT_ESC, '!'};
    static const uint8_t past[] = {TEXT_ONE_FIRST + 2};
    static const uint8_t flag_literal[] = {TEXT_CAP, 'a'};
    static const uint8_t flag_flag[] = {TEXT_CAP, TEXT_UPPER, TEXT_ONE_FIRST};
    static const uint8_t flag_escape[] = {TEXT_UPPER, TEXT_ESC, 0x80};
    static const uint8_t flag_end[] = {'a', TEXT_CAP};
    static const uint8_t code_end[] = {TEXT_TWO_FIRST};
    static const uint8_t code_low[] = {TEXT_TWO_FIRST, 'a'};
    static const uint8_t escape_end[] = {'a', TEXT_ESC};
    static const uint8_t reserved[] = {TEXT_RESERVED};
    static const uint8_t two[] = {TEXT_TWO_FIRST, 0x80};
    size_t w;
    w = inverse_of(primed, ok_body, sizeof(ok_body), out, &complete);
    CHECK(w == strlen(primed) + 15 && complete && memcmp(out + strlen(primed), "a the Of THE\xC3\x80!", 15) == 0);
    w = inverse_of(primed, past, sizeof(past), out, &complete);
    CHECK(w == (size_t)-1);
    w = inverse_of(primed, two, sizeof(two), out, &complete);
    CHECK(w == (size_t)-1); /* code 60 of a two-word book */
    w = inverse_of(primed, flag_literal, sizeof(flag_literal), out, &complete);
    CHECK(w == (size_t)-1);
    w = inverse_of(primed, flag_flag, sizeof(flag_flag), out, &complete);
    CHECK(w == (size_t)-1);
    w = inverse_of(primed, flag_escape, sizeof(flag_escape), out, &complete);
    CHECK(w == (size_t)-1);
    w = inverse_of(primed, flag_end, sizeof(flag_end), out, &complete);
    CHECK(w == strlen(primed) + 1 && !complete);
    w = inverse_of(primed, code_end, sizeof(code_end), out, &complete);
    CHECK(w == strlen(primed) && !complete);
    w = inverse_of(primed, code_low, sizeof(code_low), out, &complete);
    CHECK(w == (size_t)-1);
    w = inverse_of(primed, escape_end, sizeof(escape_end), out, &complete);
    CHECK(w == strlen(primed) + 1 && !complete);
    w = inverse_of(primed, reserved, sizeof(reserved), out, &complete);
    CHECK(w == (size_t)-1);
    /* an empty book: every byte a literal, no code valid; a bad stream stays bad */
    w = inverse_of("", (const uint8_t *)"plain", 5, out, &complete);
    CHECK(w == 5 && complete && memcmp(out, "plain", 5) == 0);
    w = inverse_of("", past, 1, out, &complete);
    CHECK(w == (size_t)-1);
    {
        text_inverse v;
        CHECK(text_inverse_init(&v));
        CHECK(text_inverse_feed(&v, reserved, 1, out) == (size_t)-1);
        CHECK(text_inverse_feed(&v, (const uint8_t *)"a", 1, out) == (size_t)-1);
        CHECK(!text_inverse_complete(&v));
        text_inverse_free(&v);
    }
}

static void test_round_trips(void) {
    size_t len, chunk, tl;
    uint8_t *data = read_file("text.bin", &len);
    /* the vector at every chunk size, its transform shorter than it */
    for (chunk = 1; chunk <= len + 1; chunk = chunk < 7 ? chunk + 1 : chunk * 3) {
        tl = round_trip(data, len, chunk, NULL);
        CHECK(tl > 0 && tl < len);
    }
    free(data);
    /* the other vectors, whatever they hold */
    {
        static const char *names[] = {"abc.bin", "hello.bin", "records.bin", "cyrillic.bin", "cjk.bin", "empty.bin"};
        size_t i;
        for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            data = read_file(names[i], &len);
            for (chunk = 1; chunk <= len + 1; chunk = chunk < 3 ? chunk + 1 : chunk * 7) {
                round_trip(data, len, chunk, NULL);
            }
            free(data);
        }
    }
    /* hand-made edge cases, every chunk size */
    {
        static const char *cases[] = {
            "",
            "a",
            "the",
            "the the the the",
            "THE The the tHe ThE thE the THE The",
            "word\x80word\x81word\xFFword\x80\x80 word word word word",
            "\xC3\xA9t\xC3\xA9 the the the caf\xC3\xA9 the",
            "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz the the the the abcdefghijklmnopqrstuvwxyzabcdef",
            "abcdefghijklmnopqrstuvwxyzabcdef abcdefghijklmnopqrstuvwxyzabcdef abcdefghijklmnopqrstuvwxyzabcdef "
            "abcdefghijklmnopqrstuvwxyzabcdef",
            "abcdefghijklmnopqrstuvwxyzabcdefg abcdefghijklmnopqrstuvwxyzabcdefg abcdefghijklmnopqrstuvwxyzabcdefg "
            "abcdefghijklmnopqrstuvwxyzabcdefg!",
            "0123456789 of of of of 42of of42 OF Of oF of",
            "\x83\x84\xC0\xF0 the the the the \x83\x84\xC0\xF0",
            "the\nthe\nthe\nthe\nthe",
            "the the the1the the",
            "Word word WORD wOrd word Word WORD word",
        };
        size_t i;
        uint8_t high[300];
        for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            size_t n = strlen(cases[i]);
            for (chunk = 1; chunk <= n + 1; chunk++) {
                round_trip((const uint8_t *)cases[i], n, chunk, NULL);
            }
        }
        for (i = 0; i < sizeof(high); i++) high[i] = (uint8_t)(0x80 + i % 128);
        for (chunk = 1; chunk <= 64; chunk *= 2) {
            tl = round_trip(high, sizeof(high), chunk, NULL);
            CHECK(tl == 2 * sizeof(high)); /* every byte escaped */
        }
    }
}

static void test_detect(void) {
    uint8_t buf[4096];
    size_t i, len;
    uint8_t *data = read_file("text.bin", &len);
    CHECK(text_detect(data, len));
    CHECK(text_detect(data, 1024));
    CHECK(!text_detect(data, 1023));
    free(data);
    data = read_file("cjk.bin", &len);
    CHECK(!text_detect(data, len)); /* bytes at or above 0x80 throughout */
    free(data);
    for (i = 0; i < sizeof(buf); i++) buf[i] = (uint8_t)(i * 7 + 3);
    CHECK(!text_detect(buf, sizeof(buf)));
    for (i = 0; i < sizeof(buf); i++) buf[i] = (uint8_t)(i % 2 ? '1' : ' ');
    CHECK(!text_detect(buf, sizeof(buf)));
    for (i = 0; i < sizeof(buf); i++) buf[i] = (uint8_t)(i % 5 == 4 ? ' ' : 'a' + i % 5);
    CHECK(text_detect(buf, sizeof(buf)));
    for (i = 0; i < sizeof(buf); i++) buf[i] = (uint8_t)(i % 2 ? 'a' : ' '); /* single letters do not count */
    CHECK(!text_detect(buf, sizeof(buf)));
    for (i = 0; i < sizeof(buf); i++) buf[i] = (uint8_t)(i % 5 == 4 ? (i % 100 < 20 ? 0xC3 : ' ') : 'a' + i % 5);
    CHECK(!text_detect(buf, sizeof(buf))); /* one in twenty-five bytes high */
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: test_adapt_text <vectors-dir>\n");
        return 2;
    }
    vectors_dir = argv[1];
    test_codes();
    test_book();
    test_inverse_failures();
    test_round_trips();
    test_detect();
    if (failures != 0) {
        fprintf(stderr, "%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("%d checks passed\n", checks);
    return 0;
}
