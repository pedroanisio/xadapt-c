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

static int rec_hardlink(void *context, const char *path, const char *target) {
    char line[300];
    snprintf(line, sizeof(line), "hard %s = %s", path, target);
    return note(context, line);
}

static int rec_sparse_open(void *context, const char *path, uint64_t size, uint32_t extents) {
    record *r = context;
    char line[300];
    snprintf(line, sizeof(line), "sparse %s %llu %u", path, (unsigned long long)size, (unsigned)extents);
    CHECK(!r->open);
    r->open = 1;
    return note(context, line);
}

static int rec_extent(void *context, uint64_t offset, uint64_t length) {
    record *r = context;
    char line[300];
    snprintf(line, sizeof(line), "extent %llu+%llu", (unsigned long long)offset, (unsigned long long)length);
    CHECK(r->open);
    return note(context, line);
}

static int rec_attribute(void *context, const char *path, uint8_t kind, const char *name, const uint8_t *value, size_t len) {
    record *r = context;
    char line[400];
    size_t i;
    CHECK(!r->open);
    snprintf(line, sizeof(line), "attr %s %u %s=", path, (unsigned)kind, name);
    for (i = 0; i < len && strlen(line) + 1 < sizeof(line); i++) {
        line[strlen(line) + 1] = '\0';
        line[strlen(line)] = (char)(value[i] == 0 ? '.' : value[i]);
    }
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
    s.hardlink = rec_hardlink;
    s.sparse_open = rec_sparse_open;
    s.extent = rec_extent;
    s.attribute = rec_attribute;
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

/* One stream that breaks one rule at a known byte. A rule broken among a
 * sparse entry's extents, which stream with their bytes, is met after the
 * sink opened the file; `open_ok` says so, and the sink's cleanup is its own. */
static void refuse_at(const build *b, uint64_t at, const char *why, size_t piece, int open_ok) {
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
    CHECK(open_ok || !r.open);
    adapt_archive_unpacker_free(u);
}

static void refuse(const build *b, uint64_t at, const char *why, size_t piece) {
    refuse_at(b, at, why, piece, 0);
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

/* ---- version 3: hard links, sparse files, attributes ----------------------- */

static const adapt_metadata META = {0644, 10, 0, 20, 5};

static void put_full(build *b, uint8_t kind, const char *path, uint64_t size, const char *target,
                     const adapt_extent *extents, uint32_t count) {
    size_t n = adapt_archive_entry_full(kind, path, size, target, extents, count, &META, b->bytes + b->len,
                                        sizeof(b->bytes) - b->len);
    CHECK(n > 0);
    b->len += n;
}

static void put_attrs(build *b, uint32_t count) {
    b->len += adapt_archive_attributes(count, b->bytes + b->len, sizeof(b->bytes) - b->len);
}

static void put_attr(build *b, const char *name, const char *value) {
    size_t n = adapt_archive_attribute(name, (const uint8_t *)value, strlen(value), b->bytes + b->len, sizeof(b->bytes) - b->len);
    CHECK(n > 0);
    b->len += n;
}

static void put_extent(build *b, uint64_t offset, const char *data) {
    adapt_extent e;
    e.offset = offset;
    e.length = strlen(data);
    b->len += adapt_archive_extent(&e, b->bytes + b->len, sizeof(b->bytes) - b->len);
    put(b, data, strlen(data));
}

static void start3(build *b) {
    b->len = 0;
    put(b, ADAPT_ARCHIVE_FULL_MAGIC, ADAPT_ARCHIVE_MAGIC_BYTES);
}

/* A version-3 folder: a directory with an ACL, a file with two attributes,
 * a hard link to it, a sparse file of two extents around a hole, an
 * attribute-less symlink and an empty sparse file. */
static void build_full(build *b) {
    adapt_extent extents[2] = {{2, 3}, {8, 2}};
    start3(b);
    put_full(b, ADAPT_ARCHIVE_DIRECTORY, "d", 0, NULL, NULL, 0);
    put_attrs(b, 1);
    put_attr(b, "system.posix_acl_access", "acl");
    put_full(b, ADAPT_ARCHIVE_FILE, "d/f", 3, NULL, NULL, 0);
    put(b, "abc", 3);
    put_attrs(b, 2);
    put_attr(b, "user.a", "1");
    put_attr(b, "user.b", "");
    put_full(b, ADAPT_ARCHIVE_HARDLINK, "d/h", 0, "d/f", NULL, 0);
    put_full(b, ADAPT_ARCHIVE_SPARSE, "s", 10, NULL, extents, 2);
    put_extent(b, 2, "xyz");
    put_extent(b, 8, "pq");
    put_attrs(b, 0);
    put_full(b, ADAPT_ARCHIVE_SYMLINK, "l", 0, "d/f", NULL, 0);
    put_attrs(b, 0);
    put_full(b, ADAPT_ARCHIVE_SPARSE, "z", 4, NULL, NULL, 0);
    put_attrs(b, 0);
    end(b);
}

static int rec_meta(void *context, const char *path, uint8_t kind, const adapt_metadata *m) {
    char line[300];
    snprintf(line, sizeof(line), "meta %s %u %o", path, (unsigned)kind, (unsigned)m->mode);
    return note(context, line);
}

/* Join consecutive `data` lines of a log in place: a small piece size
 * splits a file's bytes over several calls, and the sink sees the same
 * bytes either way. */
static void join_data_lines(char *log) {
    char *out = log, *line = log;
    int last_was_data = 0;
    while (*line != '\0') {
        char *nl = strchr(line, '\n');
        size_t n = nl == NULL ? strlen(line) : (size_t)(nl - line) + 1;
        int is_data = strncmp(line, "data ", 5) == 0;
        if (is_data && last_was_data) {
            out--; /* over the previous newline */
            memmove(out, line + 5, n - 5);
            out += n - 5;
        } else {
            memmove(out, line, n);
            out += n;
        }
        last_was_data = is_data;
        line += n;
    }
    *out = '\0';
}

static void test_full_well_formed(void) {
    static const char *const want =
        "dir d\nattr d 1 system.posix_acl_access=acl\nmeta d 1 644\n"
        "open d/f 3\ndata abc\nclose\nattr d/f 2 user.a=1\nattr d/f 2 user.b=\nmeta d/f 2 644\n"
        "hard d/h = d/f\n"
        "sparse s 10 2\nextent 2+3\ndata xyz\nextent 8+2\ndata pq\nclose\nmeta s 5 644\n"
        "link l -> d/f\nmeta l 3 644\n"
        "sparse z 4 0\nclose\nmeta z 5 644\n";
    build b;
    size_t piece;
    build_full(&b);
    for (piece = 1; piece <= b.len; piece = piece < 8 ? piece + 1 : piece * 3) {
        record r;
        adapt_archive_unpacker *u = adapt_archive_unpacker_new(sink_of(&r));
        adapt_error error;
        size_t at = 0;
        int rc = 0;
        memset(&r, 0, sizeof(r));
        adapt_archive_set_metadata_callback(u, rec_meta);
        while (at < b.len && rc == 0) {
            size_t n = piece < b.len - at ? piece : b.len - at;
            rc = adapt_archive_unpack(u, b.bytes + at, n);
            at += n;
        }
        CHECK(rc == 0);
        CHECK(adapt_archive_finish(u, &error) == ADAPT_OK);
        CHECK(adapt_archive_entries(u) == 6);
        join_data_lines(r.log);
        if (strcmp(r.log, want) != 0) {
            fprintf(stderr, "piece %zu: got\n%s\nwant\n%s\n", piece, r.log, want);
        }
        CHECK(strcmp(r.log, want) == 0);
        adapt_archive_unpacker_free(u);
    }
    /* a sink that fails on any call stops the unpacker there */
    {
        record r;
        adapt_archive_unpacker *u;
        memset(&r, 0, sizeof(r));
        r.fail_on = 10; /* the hard link */
        CHECK(feed(b.bytes, b.len, 7, &r, &u) == 77);
        CHECK(adapt_archive_unpack(u, b.bytes, 1) == 77);
        adapt_archive_unpacker_free(u);
    }
}

static void test_full_writers(void) {
    adapt_extent ok[2] = {{0, 1}, {1, 1}}, empty[1] = {{0, 0}}, overlap[2] = {{0, 2}, {1, 1}}, past[1] = {{3, 2}};
    uint8_t out[600];
    CHECK(adapt_archive_entry_full(ADAPT_ARCHIVE_HARDLINK, "a", 0, "b", NULL, 0, NULL, NULL, 0) == 1 + 2 + 1 + 2 + 1);
    CHECK(adapt_archive_entry_full(ADAPT_ARCHIVE_HARDLINK, "a", 0, "a", NULL, 0, NULL, NULL, 0) == 0);
    CHECK(adapt_archive_entry_full(ADAPT_ARCHIVE_HARDLINK, "a", 0, "../b", NULL, 0, NULL, NULL, 0) == 0);
    CHECK(adapt_archive_entry_full(ADAPT_ARCHIVE_HARDLINK, "a", 0, NULL, NULL, 0, NULL, NULL, 0) == 0);
    CHECK(adapt_archive_entry_full(ADAPT_ARCHIVE_SPARSE, "s", 2, NULL, ok, 2, &META, NULL, 0) ==
          1 + 2 + 1 + 8 + 4 + ADAPT_METADATA_BYTES);
    CHECK(adapt_archive_entry_full(ADAPT_ARCHIVE_SPARSE, "s", 4, NULL, NULL, 0, &META, NULL, 0) > 0);
    CHECK(adapt_archive_entry_full(ADAPT_ARCHIVE_SPARSE, "s", 2, NULL, empty, 1, &META, NULL, 0) == 0);
    CHECK(adapt_archive_entry_full(ADAPT_ARCHIVE_SPARSE, "s", 3, NULL, overlap, 2, &META, NULL, 0) == 0);
    CHECK(adapt_archive_entry_full(ADAPT_ARCHIVE_SPARSE, "s", 4, NULL, past, 1, &META, NULL, 0) == 0);
    CHECK(adapt_archive_entry_full(ADAPT_ARCHIVE_SPARSE, "s", 2, NULL, ok, 2, NULL, NULL, 0) == 0);
    CHECK(adapt_archive_entry_full(9, "s", 2, NULL, ok, 2, &META, NULL, 0) == 0);
    CHECK(adapt_archive_entry_full(ADAPT_ARCHIVE_FILE, "f", 2, NULL, NULL, 0, &META, NULL, 0) ==
          adapt_archive_entry_metadata(ADAPT_ARCHIVE_FILE, "f", 2, NULL, &META, NULL, 0));
    CHECK(adapt_archive_extent(&ok[0], out, sizeof(out)) == 16 && out[0] == 0 && out[8] == 1);
    CHECK(adapt_archive_extent(&empty[0], out, sizeof(out)) == 0);
    CHECK(adapt_archive_attributes(3, out, sizeof(out)) == 2 && out[0] == 3 && out[1] == 0);
    CHECK(adapt_archive_attributes(70000, out, sizeof(out)) == 0);
    CHECK(adapt_archive_attribute("user.x", (const uint8_t *)"v", 1, out, sizeof(out)) == 2 + 6 + 4 + 1);
    CHECK(out[0] == 6 && memcmp(out + 2, "user.x", 6) == 0 && out[8] == 1 && out[12] == 'v');
    CHECK(adapt_archive_attribute("", NULL, 0, out, sizeof(out)) == 0);
    CHECK(adapt_archive_attribute("user.x", NULL, 1, out, sizeof(out)) == 0);
    CHECK(adapt_archive_attribute("user.x", out, ADAPT_ARCHIVE_ATTRIBUTE_VALUE_MAX + 1, NULL, 0) == 0);
    {
        char longest[ADAPT_ARCHIVE_ATTRIBUTE_NAME_MAX + 2];
        memset(longest, 'n', sizeof(longest) - 1);
        longest[sizeof(longest) - 1] = '\0';
        CHECK(adapt_archive_attribute(longest, NULL, 0, NULL, 0) == 0);
        longest[ADAPT_ARCHIVE_ATTRIBUTE_NAME_MAX] = '\0';
        CHECK(adapt_archive_attribute(longest, NULL, 0, NULL, 0) == 2 + ADAPT_ARCHIVE_ATTRIBUTE_NAME_MAX + 4);
    }
}

static void test_full_malformed(void) {
    build b;
    size_t piece, head;
    adapt_extent one[1] = {{0, 2}};
    for (piece = 1; piece <= 64; piece = piece == 1 ? 5 : piece * 4) {
        /* the new kinds are not version 2's */
        start(&b);
        b.bytes[7] = '2';
        b.bytes[b.len++] = ADAPT_ARCHIVE_HARDLINK;
        refuse(&b, 8, "kind", piece);
        start(&b);
        b.bytes[7] = '2';
        b.bytes[b.len++] = ADAPT_ARCHIVE_SPARSE;
        refuse(&b, 8, "kind", piece);
        /* a hard link whose target leaves the folder, or is itself */
        start3(&b);
        b.bytes[b.len++] = ADAPT_ARCHIVE_HARDLINK;
        b.bytes[b.len++] = 1;
        b.bytes[b.len++] = 0;
        put(&b, "a", 1);
        b.bytes[b.len++] = 4;
        b.bytes[b.len++] = 0;
        put(&b, "../a", 4);
        refuse(&b, 8 + 3 + 1 + 2 + 3, "hard link target", piece);
        start3(&b);
        b.bytes[b.len++] = ADAPT_ARCHIVE_HARDLINK;
        b.bytes[b.len++] = 1;
        b.bytes[b.len++] = 0;
        put(&b, "a", 1);
        b.bytes[b.len++] = 1;
        b.bytes[b.len++] = 0;
        put(&b, "a", 1);
        refuse(&b, 8 + 3 + 1 + 2 + 0, "hard link names itself", piece);
        /* extents: empty, out of order, past the length; too many */
        start3(&b);
        put_full(&b, ADAPT_ARCHIVE_SPARSE, "s", 4, NULL, one, 1);
        head = b.len;
        put(&b, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16);
        refuse_at(&b, head + 15, "extent is empty", piece, 1);
        start3(&b);
        put_full(&b, ADAPT_ARCHIVE_SPARSE, "s", 8, NULL, one, 1);
        b.bytes[b.len - ADAPT_METADATA_BYTES - 4] = 2; /* two extents */
        head = b.len;
        put_extent(&b, 4, "ab");
        put_extent(&b, 3, "cd");
        refuse_at(&b, head + 18 + 15, "out of order", piece, 1);
        start3(&b);
        put_full(&b, ADAPT_ARCHIVE_SPARSE, "s", 4, NULL, one, 1);
        head = b.len;
        put_extent(&b, 3, "ab");
        refuse_at(&b, head + 15, "past the file", piece, 1);
        start3(&b);
        put_full(&b, ADAPT_ARCHIVE_SPARSE, "s", 4, NULL, one, 1);
        b.bytes[b.len - ADAPT_METADATA_BYTES - 2] = 1; /* count 65536 + 1 */
        b.bytes[b.len - ADAPT_METADATA_BYTES - 4] = 1;
        refuse(&b, b.len - ADAPT_METADATA_BYTES - 1, "too many extents", piece);
        /* attributes: an empty name, a name too long, a value too long,
         * names out of order, a name twice, a NUL in a name */
        start3(&b);
        put_full(&b, ADAPT_ARCHIVE_DIRECTORY, "d", 0, NULL, NULL, 0);
        put_attrs(&b, 1);
        head = b.len;
        b.bytes[b.len++] = 0;
        b.bytes[b.len++] = 0;
        refuse(&b, head + 1, "attribute name", piece);
        start3(&b);
        put_full(&b, ADAPT_ARCHIVE_DIRECTORY, "d", 0, NULL, NULL, 0);
        put_attrs(&b, 1);
        head = b.len;
        b.bytes[b.len++] = 0;
        b.bytes[b.len++] = 1;
        refuse(&b, head + 1, "attribute name", piece);
        start3(&b);
        put_full(&b, ADAPT_ARCHIVE_DIRECTORY, "d", 0, NULL, NULL, 0);
        put_attrs(&b, 1);
        head = b.len;
        b.bytes[b.len++] = 1;
        b.bytes[b.len++] = 0;
        put(&b, "n", 1);
        b.bytes[b.len++] = 1;
        b.bytes[b.len++] = 0;
        b.bytes[b.len++] = 1;
        b.bytes[b.len++] = 0;
        refuse(&b, head + 6, "attribute value too long", piece);
        start3(&b);
        put_full(&b, ADAPT_ARCHIVE_DIRECTORY, "d", 0, NULL, NULL, 0);
        put_attrs(&b, 2);
        put_attr(&b, "user.b", "1");
        head = b.len;
        put_attr(&b, "user.a", "2");
        refuse(&b, b.len - 1, "out of byte order", piece);
        start3(&b);
        put_full(&b, ADAPT_ARCHIVE_DIRECTORY, "d", 0, NULL, NULL, 0);
        put_attrs(&b, 2);
        put_attr(&b, "user.a", "1");
        put_attr(&b, "user.a", "");
        refuse(&b, b.len - 1, "repeated", piece);
        start3(&b);
        put_full(&b, ADAPT_ARCHIVE_DIRECTORY, "d", 0, NULL, NULL, 0);
        put_attrs(&b, 1);
        head = b.len;
        b.bytes[b.len++] = 2;
        b.bytes[b.len++] = 0;
        put(&b, "a\0", 2);
        refuse(&b, head + 3, "NUL", piece);
        /* a version-3 entry cut before its attributes is not complete */
        {
            record r;
            adapt_archive_unpacker *u;
            adapt_error error;
            start3(&b);
            put_full(&b, ADAPT_ARCHIVE_DIRECTORY, "d", 0, NULL, NULL, 0);
            end(&b);
            memset(&r, 0, sizeof(r));
            CHECK(feed(b.bytes, b.len - 1, piece, &r, &u) == 0);
            CHECK(adapt_archive_finish(u, &error) == ADAPT_ERROR_BAD_ARCHIVE);
            adapt_archive_unpacker_free(u);
        }
    }
}

int main(void) {
    test_paths_and_entries();
    test_well_formed();
    test_malformed();
    test_metadata();
    test_full_writers();
    test_full_well_formed();
    test_full_malformed();
    if (failures != 0) {
        fprintf(stderr, "%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("%d checks passed\n", checks);
    return 0;
}
