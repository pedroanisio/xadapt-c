/*
 * test_adapt_archive.c — the archive stream: entry headers as the format
 * defines them, the unpacker's calls on a well-formed stream fed in every
 * piece size, and its refusal of each rule a stream can break, at the byte
 * that breaks it.
 *
 * Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-14.
 *
 *   test_adapt_archive
 */
#include "../adapt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks = 0, failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        checks++;                                                                \
        if (!(cond)) {                                                           \
            failures++;                                                          \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

/* ---- a recording sink ---------------------------------------------------- */

/* Every call the unpacker makes, as one line of text, so a stream's
 * unpacking can be compared with what it should be. */
typedef struct record {
    char log[4096];
    size_t len;
    int fail_on; /* the call number that fails, or 0 */
    int calls;
    int open; /* a file is open */
} record;

static int note(record *r, const char *text) {
    size_t n = strlen(text);
    r->calls++;
    if (r->len + n + 1 < sizeof(r->log)) {
        memcpy(r->log + r->len, text, n);
        r->len += n;
        r->log[r->len++] = '\n';
        r->log[r->len] = '\0';
    }
    return r->calls == r->fail_on ? 77 : 0;
}

static int rec_directory(void *context, const char *path) {
    char line[300];
    snprintf(line, sizeof(line), "dir %s", path);
    return note(context, line);
}

static int rec_file_open(void *context, const char *path, uint64_t size) {
    record *r = context;
    char line[300];
    snprintf(line, sizeof(line), "open %s %llu", path, (unsigned long long)size);
    CHECK(!r->open);
    r->open = 1;
    return note(context, line);
}

static int rec_file_data(void *context, const uint8_t *data, size_t len) {
    record *r = context;
    char line[300];
    size_t i;
    CHECK(r->open);
    CHECK(len > 0);
    snprintf(line, sizeof(line), "data ");
    for (i = 0; i < len && i < 32; i++) {
        line[5 + i] = (char)(data[i] == 0 ? '.' : data[i]);
    }
    line[5 + (len < 32 ? len : 32)] = '\0';
    return note(context, line);
}

static int rec_file_close(void *context) {
    record *r = context;
    CHECK(r->open);
    r->open = 0;
    return note(context, "close");
}

static int rec_symlink(void *context, const char *path, const char *target) {
    char line[300];
    snprintf(line, sizeof(line), "link %s -> %s", path, target);
    return note(context, line);
}

static adapt_archive_sink sink_of(record *r) {
    adapt_archive_sink s;
    s.context = r;
    s.directory = rec_directory;
    s.file_open = rec_file_open;
    s.file_data = rec_file_data;
    s.file_close = rec_file_close;
    s.symlink = rec_symlink;
    return s;
}

/* Feed `stream` in pieces of `piece` bytes and return the unpacker's
 * first nonzero status, with the record filled. */
static int feed(const uint8_t *stream, size_t len, size_t piece, record *r, adapt_archive_unpacker **out) {
    adapt_archive_unpacker *u = adapt_archive_unpacker_new(sink_of(r));
    size_t at = 0;
    int rc = 0;
    CHECK(u != NULL);
    while (at < len && rc == 0) {
        size_t n = piece < len - at ? piece : len - at;
        rc = adapt_archive_unpack(u, stream + at, n);
        at += n;
    }
    *out = u;
    return rc;
}

/* ---- building streams ---------------------------------------------------- */

typedef struct build {
    uint8_t bytes[4096];
    size_t len;
} build;

static void put(build *b, const void *data, size_t n) {
    CHECK(b->len + n <= sizeof(b->bytes));
    memcpy(b->bytes + b->len, data, n);
    b->len += n;
}

static void put_entry(build *b, uint8_t kind, const char *path, uint64_t size, const char *target) {
    size_t n = adapt_archive_entry(kind, path, size, target, b->bytes + b->len, sizeof(b->bytes) - b->len);
    CHECK(n > 0);
    b->len += n;
}

static void start(build *b) {
    b->len = 0;
    put(b, ADAPT_ARCHIVE_MAGIC, ADAPT_ARCHIVE_MAGIC_BYTES);
}

static void end(build *b) {
    uint8_t e = ADAPT_ARCHIVE_END;
    put(b, &e, 1);
}

/* ---- the tests ------------------------------------------------------------ */

static void test_paths_and_entries(void) {
    uint8_t out[80];
    size_t n;
    char *longest = malloc(ADAPT_ARCHIVE_PATH_MAX + 2);
    CHECK(adapt_archive_path_valid("a"));
    CHECK(adapt_archive_path_valid("a/b/c.txt"));
    CHECK(adapt_archive_path_valid("..a/b..") && adapt_archive_path_valid(".hidden"));
    CHECK(!adapt_archive_path_valid("") && !adapt_archive_path_valid(NULL));
    CHECK(!adapt_archive_path_valid("/a") && !adapt_archive_path_valid("a/") && !adapt_archive_path_valid("a//b"));
    CHECK(!adapt_archive_path_valid(".") && !adapt_archive_path_valid("..") && !adapt_archive_path_valid("a/../b"));
    CHECK(!adapt_archive_path_valid("a/./b") && !adapt_archive_path_valid("./a"));
    memset(longest, 'x', ADAPT_ARCHIVE_PATH_MAX + 1);
    longest[ADAPT_ARCHIVE_PATH_MAX] = '\0';
    CHECK(adapt_archive_path_valid(longest));
    longest[ADAPT_ARCHIVE_PATH_MAX] = 'x';
    longest[ADAPT_ARCHIVE_PATH_MAX + 1] = '\0';
    CHECK(!adapt_archive_path_valid(longest));
    /* a directory: kind, length, path */
    n = adapt_archive_entry(ADAPT_ARCHIVE_DIRECTORY, "ab", 0, NULL, out, sizeof(out));
    CHECK(n == 5 && out[0] == 1 && out[1] == 2 && out[2] == 0 && out[3] == 'a' && out[4] == 'b');
    /* a file: then its length, little-endian */
    n = adapt_archive_entry(ADAPT_ARCHIVE_FILE, "f", 0x0102030405060708ull, NULL, out, sizeof(out));
    CHECK(n == 12 && out[0] == 2 && out[1] == 1 && out[2] == 0 && out[3] == 'f');
    CHECK(out[4] == 8 && out[5] == 7 && out[6] == 6 && out[7] == 5 && out[8] == 4 && out[9] == 3 && out[10] == 2 &&
          out[11] == 1);
    /* a link: then its target */
    n = adapt_archive_entry(ADAPT_ARCHIVE_SYMLINK, "l", 0, "../t", out, sizeof(out));
    CHECK(n == 10 && out[0] == 3 && out[4] == 4 && out[5] == 0 && memcmp(out + 6, "../t", 4) == 0);
    /* measured without room, refused when invalid */
    CHECK(adapt_archive_entry(ADAPT_ARCHIVE_SYMLINK, "l", 0, "../t", out, 9) == 10 &&
          adapt_archive_entry(ADAPT_ARCHIVE_SYMLINK, "l", 0, "../t", NULL, 0) == 10);
    CHECK(adapt_archive_entry(ADAPT_ARCHIVE_END, "a", 0, NULL, out, sizeof(out)) == 0);
    CHECK(adapt_archive_entry(4, "a", 0, NULL, out, sizeof(out)) == 0);
    CHECK(adapt_archive_entry(ADAPT_ARCHIVE_FILE, "a/../b", 0, NULL, out, sizeof(out)) == 0);
    CHECK(adapt_archive_entry(ADAPT_ARCHIVE_SYMLINK, "l", 0, "", out, sizeof(out)) == 0);
    CHECK(adapt_archive_entry(ADAPT_ARCHIVE_SYMLINK, "l", 0, NULL, out, sizeof(out)) == 0);
    memset(longest, 't', ADAPT_ARCHIVE_PATH_MAX + 1);
    longest[ADAPT_ARCHIVE_PATH_MAX + 1] = '\0';
    CHECK(adapt_archive_entry(ADAPT_ARCHIVE_SYMLINK, "l", 0, longest, NULL, 0) == 0);
    longest[ADAPT_ARCHIVE_PATH_MAX] = '\0';
    CHECK(adapt_archive_entry(ADAPT_ARCHIVE_SYMLINK, "l", 0, longest, NULL, 0) == 1 + 2 + 1 + 2 + ADAPT_ARCHIVE_PATH_MAX);
    free(longest);
}

static const char WANT[] = "dir d\n"
                           "open d/empty 0\n"
                           "close\n"
                           "open d/f 11\n"
                           "data hello world\n"
                           "close\n"
                           "dir d/sub\n"
                           "link d/sub/l -> ../f\n"
                           "open z 3\n"
                           "data a.b\n"
                           "close\n";

static void test_well_formed(void) {
    build b;
    size_t piece;
    start(&b);
    put_entry(&b, ADAPT_ARCHIVE_DIRECTORY, "d", 0, NULL);
    put_entry(&b, ADAPT_ARCHIVE_FILE, "d/empty", 0, NULL);
    put_entry(&b, ADAPT_ARCHIVE_FILE, "d/f", 11, NULL);
    put(&b, "hello world", 11);
    put_entry(&b, ADAPT_ARCHIVE_DIRECTORY, "d/sub", 0, NULL);
    put_entry(&b, ADAPT_ARCHIVE_SYMLINK, "d/sub/l", 0, "../f");
    put_entry(&b, ADAPT_ARCHIVE_FILE, "z", 3, NULL);
    put(&b, "a\0b", 3);
    end(&b);
    /* every piece size, whole or not, gives the same calls — save that a
     * file's bytes arrive in as many pieces as they are cut into */
    for (piece = 1; piece <= b.len + 1; piece = piece < 8 ? piece + 1 : piece * 3) {
        record r;
        adapt_archive_unpacker *u;
        adapt_error error;
        uint64_t offset = 1;
        memset(&r, 0, sizeof(r));
        CHECK(feed(b.bytes, b.len, piece, &r, &u) == 0);
        CHECK(adapt_archive_finish(u, &error) == ADAPT_OK && error.status == ADAPT_OK);
        CHECK(adapt_archive_problem(u, &offset) == NULL && offset == 0);
        CHECK(adapt_archive_entries(u) == 6);
        if (piece >= b.len) {
            CHECK(strcmp(r.log, WANT) == 0);
        } else {
            /* the same events, the data lines merged */
            CHECK(strstr(r.log, "open d/f 11\ndata ") != NULL && strstr(r.log, "\nclose\ndir d/sub\n") != NULL);
            CHECK(strstr(r.log, "open z 3\ndata ") != NULL);
        }
        adapt_archive_unpacker_free(u);
    }
    /* a sink that fails stops the unpacker with its status */
    {
        record r;
        adapt_archive_unpacker *u;
        memset(&r, 0, sizeof(r));
        r.fail_on = 4; /* open d/f */
        CHECK(feed(b.bytes, b.len, 7, &r, &u) == 77);
        CHECK(adapt_archive_problem(u, NULL) == NULL);
        adapt_archive_unpacker_free(u);
    }
    /* the empty archive: the magic and the end */
    {
        record r;
        adapt_archive_unpacker *u;
        adapt_error error;
        build e;
        start(&e);
        end(&e);
        memset(&r, 0, sizeof(r));
        CHECK(feed(e.bytes, e.len, 100, &r, &u) == 0);
        CHECK(adapt_archive_finish(u, &error) == ADAPT_OK && adapt_archive_entries(u) == 0 && r.calls == 0);
        adapt_archive_unpacker_free(u);
    }
    adapt_archive_unpacker_free(NULL);
}

/* One stream that breaks one rule at a known byte. */
static void refuse(const build *b, uint64_t at, const char *why, size_t piece) {
    record r;
    adapt_archive_unpacker *u;
    adapt_error error;
    uint64_t offset = 0;
    const char *problem;
    memset(&r, 0, sizeof(r));
    CHECK(feed(b->bytes, b->len, piece, &r, &u) == ADAPT_ARCHIVE_MALFORMED);
    problem = adapt_archive_problem(u, &offset);
    CHECK(problem != NULL && strstr(problem, why) != NULL);
    if (problem == NULL || strstr(problem, why) == NULL) {
        fprintf(stderr, "  wanted '%s', got '%s' at %llu\n", why, problem ? problem : "(none)", (unsigned long long)offset);
    }
    CHECK(offset == at);
    if (offset != at) {
        fprintf(stderr, "  offset %llu, wanted %llu (%s)\n", (unsigned long long)offset, (unsigned long long)at, why);
    }
    CHECK(adapt_archive_finish(u, &error) == ADAPT_ERROR_BAD_ARCHIVE && error.status == ADAPT_ERROR_BAD_ARCHIVE &&
          error.detail == at);
    /* more bytes change nothing */
    CHECK(adapt_archive_unpack(u, b->bytes, 1) == ADAPT_ARCHIVE_MALFORMED);
    CHECK(!r.open);
    adapt_archive_unpacker_free(u);
}

static void test_malformed(void) {
    build b;
    size_t piece;
    for (piece = 1; piece <= 64; piece = piece == 1 ? 5 : piece * 4) {
        /* the wrong signature, at the byte that differs */
        start(&b);
        b.bytes[3] = 'X';
        end(&b);
        refuse(&b, 3, "signature", piece);
        /* an unknown kind */
        start(&b);
        b.bytes[b.len++] = 9;
        refuse(&b, 8, "kind", piece);
        /* an empty path */
        start(&b);
        b.bytes[b.len++] = ADAPT_ARCHIVE_DIRECTORY;
        b.bytes[b.len++] = 0;
        b.bytes[b.len++] = 0;
        refuse(&b, 10, "path", piece);
        /* a path that leaves the folder, refused at its last byte */
        start(&b);
        b.bytes[b.len++] = ADAPT_ARCHIVE_FILE;
        b.bytes[b.len++] = 5;
        b.bytes[b.len++] = 0;
        put(&b, "a/../", 5);
        refuse(&b, 8 + 3 + 4, "path", piece);
        start(&b);
        b.bytes[b.len++] = ADAPT_ARCHIVE_FILE;
        b.bytes[b.len++] = 2;
        b.bytes[b.len++] = 0;
        put(&b, "/a", 2);
        refuse(&b, 8 + 3 + 0, "path", piece);
        start(&b);
        b.bytes[b.len++] = ADAPT_ARCHIVE_FILE;
        b.bytes[b.len++] = 3;
        b.bytes[b.len++] = 0;
        put(&b, "a\0b", 3);
        refuse(&b, 8 + 3 + 1, "path", piece);
        /* an empty link target */
        start(&b);
        put_entry(&b, ADAPT_ARCHIVE_DIRECTORY, "d", 0, NULL);
        b.bytes[b.len++] = ADAPT_ARCHIVE_SYMLINK;
        b.bytes[b.len++] = 1;
        b.bytes[b.len++] = 0;
        b.bytes[b.len++] = 'l';
        b.bytes[b.len++] = 0;
        b.bytes[b.len++] = 0;
        refuse(&b, 8 + 4 + 5, "target", piece);
        /* a NUL in a link target */
        start(&b);
        b.bytes[b.len++] = ADAPT_ARCHIVE_SYMLINK;
        b.bytes[b.len++] = 1;
        b.bytes[b.len++] = 0;
        b.bytes[b.len++] = 'l';
        b.bytes[b.len++] = 2;
        b.bytes[b.len++] = 0;
        b.bytes[b.len++] = 't';
        b.bytes[b.len++] = 0;
        refuse(&b, 8 + 4 + 2 + 1, "target", piece);
        /* bytes after the end */
        start(&b);
        end(&b);
        b.bytes[b.len++] = 0;
        refuse(&b, 9, "after", piece);
    }
    /* cut short: no end byte, a file's bytes missing, a header half read */
    {
        record r;
        adapt_archive_unpacker *u;
        adapt_error error;
        uint64_t offset;
        start(&b);
        put_entry(&b, ADAPT_ARCHIVE_FILE, "f", 4, NULL);
        put(&b, "ab", 2);
        memset(&r, 0, sizeof(r));
        CHECK(feed(b.bytes, b.len, 3, &r, &u) == 0);
        CHECK(adapt_archive_finish(u, &error) == ADAPT_ERROR_BAD_ARCHIVE && error.detail == b.len);
        CHECK(adapt_archive_problem(u, &offset) != NULL && strstr(adapt_archive_problem(u, NULL), "cut short") != NULL);
        CHECK(offset == b.len && r.open);
        adapt_archive_unpacker_free(u);
        start(&b);
        put_entry(&b, ADAPT_ARCHIVE_DIRECTORY, "d", 0, NULL);
        memset(&r, 0, sizeof(r));
        CHECK(feed(b.bytes, b.len, 100, &r, &u) == 0);
        CHECK(adapt_archive_finish(u, &error) == ADAPT_ERROR_BAD_ARCHIVE && error.detail == b.len);
        adapt_archive_unpacker_free(u);
        start(&b);
        memset(&r, 0, sizeof(r));
        CHECK(feed(b.bytes, 5, 100, &r, &u) == 0);
        CHECK(adapt_archive_finish(u, &error) == ADAPT_ERROR_BAD_ARCHIVE && error.detail == 5);
        adapt_archive_unpacker_free(u);
        /* finishing twice, and after nothing at all */
        u = adapt_archive_unpacker_new(sink_of(&r));
        CHECK(adapt_archive_finish(u, &error) == ADAPT_ERROR_BAD_ARCHIVE && error.detail == 0);
        CHECK(adapt_archive_finish(u, NULL) == ADAPT_ERROR_BAD_ARCHIVE);
        CHECK(adapt_archive_unpack(u, NULL, 0) == 0);
        adapt_archive_unpacker_free(u);
    }
}

static int rec_metadata(void *context, const char *path, uint8_t kind, const adapt_metadata *m) {
    record *r = context;
    char line[300];
    CHECK(!r->open);
    CHECK(m->mode == 0640 && m->atime_seconds == -1 && m->atime_nanoseconds == 1234);
    CHECK(m->mtime_seconds == 0 && m->mtime_nanoseconds == 999999999u);
    if (r->fail_on == -(int)kind) {
        return 77;
    }
    snprintf(line, sizeof(line), "metadata %s", path);
    return note(r, line);
}

static void test_metadata(void) {
    static const uint8_t encoded[] = {
        0xa0, 1, 0, 0, 255, 255, 255, 255, 255, 255, 255, 255,
        0xd2, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 0xc9, 0x9a, 0x3b
    };
    adapt_metadata m = {0640, -1, 1234, 0, 999999999u}, decoded;
    uint8_t bytes[ADAPT_METADATA_BYTES], short_out[1] = {99};
    build b;
    size_t n, piece, offset, at;
    int kind;
    CHECK(adapt_metadata_write(&m, bytes) && memcmp(bytes, encoded, sizeof(encoded)) == 0);
    CHECK(adapt_metadata_read(encoded, &decoded));
    CHECK(decoded.mode == m.mode && decoded.atime_seconds == -1 && decoded.mtime_nanoseconds == 999999999u);
    CHECK(!adapt_metadata_write(NULL, bytes) && !adapt_metadata_write(&m, NULL));
    CHECK(!adapt_metadata_read(NULL, &decoded) && !adapt_metadata_read(bytes, NULL));
    m.atime_seconds = INT64_MIN;
    m.mtime_seconds = INT64_MAX;
    CHECK(adapt_metadata_write(&m, bytes) && adapt_metadata_read(bytes, &decoded));
    CHECK(decoded.atime_seconds == INT64_MIN && decoded.mtime_seconds == INT64_MAX);
    m.atime_seconds = -1;
    m.mtime_seconds = 0;
    m.mode = 010000;
    CHECK(!adapt_metadata_write(&m, bytes));
    CHECK(adapt_archive_entry_metadata(1, "d", 0, NULL, &m, NULL, 0) == 0);
    m.mode = 0640;
    m.atime_nanoseconds = 1000000000;
    CHECK(!adapt_metadata_write(&m, bytes));
    m.atime_nanoseconds = 1234;
    m.mtime_nanoseconds = 1000000000;
    CHECK(!adapt_metadata_write(&m, bytes));
    m.mtime_nanoseconds = 999999999u;
    CHECK(adapt_archive_entry_metadata(9, "d", 0, NULL, &m, NULL, 0) == 0);
    CHECK(adapt_archive_entry_metadata(1, "d", 0, NULL, &m, short_out, 1) == 4 + ADAPT_METADATA_BYTES);
    CHECK(short_out[0] == 99);
    adapt_archive_set_metadata_callback(NULL, rec_metadata);

    b.len = 0;
    put(&b, ADAPT_ARCHIVE_METADATA_MAGIC, ADAPT_ARCHIVE_MAGIC_BYTES);
    n = adapt_archive_entry_metadata(1, "d", 0, NULL, &m, b.bytes + b.len, sizeof(b.bytes) - b.len);
    b.len += n;
    n = adapt_archive_entry_metadata(2, "d/f", 3, NULL, &m, b.bytes + b.len, sizeof(b.bytes) - b.len);
    b.len += n;
    put(&b, "abc", 3);
    n = adapt_archive_entry_metadata(2, "d/empty", 0, NULL, &m, b.bytes + b.len, sizeof(b.bytes) - b.len);
    b.len += n;
    n = adapt_archive_entry_metadata(3, "d/link", 0, "f", &m, b.bytes + b.len, sizeof(b.bytes) - b.len);
    b.len += n;
    end(&b);
    for (piece = 1; piece <= b.len; piece++) {
        record r = {{0}, 0, 0, 0, 0};
        adapt_archive_unpacker *u = adapt_archive_unpacker_new(sink_of(&r));
        CHECK(u != NULL);
        adapt_archive_set_metadata_callback(u, rec_metadata);
        for (at = 0; at < b.len; at += n) {
            n = piece < b.len - at ? piece : b.len - at;
            CHECK(adapt_archive_unpack(u, b.bytes + at, n) == 0);
        }
        CHECK(adapt_archive_finish(u, NULL) == ADAPT_OK);
        CHECK(adapt_archive_entries(u) == 4);
        CHECK(strstr(r.log, "dir d\nmetadata d\nopen d/f") != NULL);
        CHECK(strstr(r.log, "close\nmetadata d/f\n") != NULL);
        CHECK(strstr(r.log, "close\nmetadata d/empty\n") != NULL);
        CHECK(strstr(r.log, "link d/link -> f\nmetadata d/link\n") != NULL);
        adapt_archive_unpacker_free(u);
    }
    for (kind = 1; kind <= 3; kind++) {
        record r = {{0}, 0, -kind, 0, 0};
        adapt_archive_unpacker *u = adapt_archive_unpacker_new(sink_of(&r));
        adapt_archive_set_metadata_callback(u, rec_metadata);
        CHECK(adapt_archive_unpack(u, b.bytes, b.len) == 77);
        CHECK(adapt_archive_unpack(u, b.bytes, b.len) == 77);
        adapt_archive_unpacker_free(u);
    }
    /* Content-only consumers may ignore validated metadata. */
    {
        record r = {{0}, 0, 0, 0, 0};
        adapt_archive_unpacker *u;
        CHECK(feed(b.bytes, b.len, 1, &r, &u) == 0);
        CHECK(adapt_archive_finish(u, NULL) == ADAPT_OK);
        adapt_archive_unpacker_free(u);
    }
    /* Each invalid metadata field is rejected before creating the entry. */
    for (offset = 3; offset <= 27; offset += 12) {
        record r = {{0}, 0, 0, 0, 0};
        adapt_archive_unpacker *u;
        uint8_t old = b.bytes[12 + offset];
        b.bytes[12 + offset] = 255;
        CHECK(feed(b.bytes, b.len, 1, &r, &u) == ADAPT_ARCHIVE_MALFORMED);
        CHECK(r.calls == 0);
        CHECK(adapt_archive_finish(u, NULL) == ADAPT_ERROR_BAD_ARCHIVE);
        adapt_archive_unpacker_free(u);
        b.bytes[12 + offset] = old;
    }
    for (offset = 12; offset < 12 + ADAPT_METADATA_BYTES; offset++) {
        record r = {{0}, 0, 0, 0, 0};
        adapt_archive_unpacker *u;
        CHECK(feed(b.bytes, offset, 1, &r, &u) == 0);
        CHECK(adapt_archive_finish(u, NULL) == ADAPT_ERROR_BAD_ARCHIVE);
        CHECK(r.calls == 0);
        adapt_archive_unpacker_free(u);
    }
}

int main(void) {
    test_paths_and_entries();
    test_well_formed();
    test_malformed();
    test_metadata();
    if (failures != 0) {
        fprintf(stderr, "%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("%d checks passed\n", checks);
    return 0;
}
