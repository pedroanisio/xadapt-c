/* A folder as one stream: entries written with the archive writers, coded
 * as a container flagged as an archive, decoded into an unpacker whose
 * sink receives every entry as its bytes arrive. The sink here records
 * into memory; the command line's sink restores to a filesystem.
 *
 *   cc -std=c99 -I.. folder.c ../build/libadapt.a -pthread
 *
 * Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-17. */
#include "example.h"

/* The sink's record of what it saw. A callback's pointers — the path,
 * the target, an attribute's name and value, a file's data — are valid
 * for that call only, so the sink copies what it keeps. The order per
 * entry: the entry (directory, file open/data/close, link, sparse
 * open/extent/data/close), then each of its attributes, then its
 * metadata — so a filesystem sink applies the mode and times last. */
typedef struct seen {
    char log[2048];
    size_t at;
    uint64_t file_bytes;
} seen;

static int note(void *context, const char *line) {
    seen *s = context;
    size_t n = strlen(line);
    if (s->at + n + 2 > sizeof(s->log)) return 1; /* a nonzero return stops the unpacker with this status */
    memcpy(s->log + s->at, line, n);
    s->at += n;
    s->log[s->at++] = '\n';
    s->log[s->at] = '\0';
    return 0;
}

static int on_directory(void *c, const char *path) {
    char line[300];
    sprintf(line, "dir %s", path);
    return note(c, line);
}
static int on_file_open(void *c, const char *path, uint64_t size) {
    char line[300];
    sprintf(line, "file %s (%llu bytes)", path, (unsigned long long)size);
    return note(c, line);
}
static int on_file_data(void *c, const uint8_t *data, size_t len) {
    ((seen *)c)->file_bytes += len;
    (void)data;
    return 0;
}
static int on_file_close(void *c) { return note(c, "close"); }
static int on_symlink(void *c, const char *path, const char *target) {
    char line[300];
    sprintf(line, "link %s -> %s", path, target);
    return note(c, line);
}
static int on_hardlink(void *c, const char *path, const char *target) {
    char line[300];
    sprintf(line, "hard %s = %s", path, target);
    return note(c, line);
}
static int on_sparse_open(void *c, const char *path, uint64_t size, uint32_t extents) {
    char line[300];
    sprintf(line, "sparse %s (%llu bytes, %u extents)", path, (unsigned long long)size, (unsigned)extents);
    return note(c, line);
}
static int on_extent(void *c, uint64_t offset, uint64_t length) {
    char line[100];
    sprintf(line, "extent %llu+%llu", (unsigned long long)offset, (unsigned long long)length);
    return note(c, line);
}
static int on_attribute(void *c, const char *path, uint8_t kind, const char *name, const uint8_t *value, size_t len) {
    char line[400];
    (void)kind;
    sprintf(line, "attr %s %s=%.*s", path, name, (int)len, (const char *)value);
    return note(c, line);
}
static int on_metadata(void *c, const char *path, uint8_t kind, const adapt_metadata *m) {
    char line[300];
    (void)kind;
    sprintf(line, "meta %s mode %o", path, (unsigned)m->mode);
    return note(c, line);
}

/* A writer into a growing buffer, as in stream.c. */
typedef struct sink { uint8_t *data; size_t len, cap; } sink;
static int write_sink(void *context, const uint8_t *data, size_t len) {
    sink *s = context;
    if (s->cap - s->len < len) {
        size_t cap = s->cap ? s->cap * 2 : 65536;
        uint8_t *grown;
        while (cap - s->len < len) cap *= 2;
        grown = realloc(s->data, cap);
        if (grown == NULL) return 12;
        s->data = grown;
        s->cap = cap;
    }
    memcpy(s->data + s->len, data, len);
    s->len += len;
    return 0;
}
typedef struct source { const uint8_t *data; size_t len, at; } source;
static int read_source(void *context, uint8_t *buf, size_t len, size_t *got) {
    source *s = context;
    size_t n = s->len - s->at;
    if (n > len) n = len;
    memcpy(buf, s->data + s->at, n);
    s->at += n;
    *got = n;
    return 0;
}

int main(void) {
    static const char *const want =
        "dir docs\nattr docs user.owner=example\nmeta docs mode 755\n"
        "file docs/readme.txt (16 bytes)\nclose\nmeta docs/readme.txt mode 644\n"
        "hard docs/readme-again.txt = docs/readme.txt\n"
        "sparse docs/holes.bin (4096 bytes, 1 extents)\nextent 1024+5\nclose\nmeta docs/holes.bin mode 600\n"
        "link docs/link -> readme.txt\nmeta docs/link mode 777\n";
    uint8_t archive[4096], *file = NULL;
    size_t at = 0, file_len, coded_len = 0;
    adapt_metadata meta;
    adapt_extent extent = {1024, 5};
    adapt_limits limits = adapt_default_limits();
    adapt_error error;
    adapt_sizes sizes;
    adapt_archive_sink sink_fns;
    adapt_archive_unpacker *u;
    seen s;
    sink coded = {NULL, 0, 0}, decoded = {NULL, 0, 0};
    source src;

    /* The archive: a version-3 stream — the magic, entries depth first
     * with siblings in byte order, each with its metadata and an
     * attribute trailer, then the end byte. The writers return 0 for a
     * path or a target that breaks the rules (empty, '.', '..', a NUL). */
    memset(&meta, 0, sizeof(meta));
    memcpy(archive, ADAPT_ARCHIVE_FULL_MAGIC, ADAPT_ARCHIVE_MAGIC_BYTES);
    at = ADAPT_ARCHIVE_MAGIC_BYTES;
    meta.mode = 0755;
    at += adapt_archive_entry_full(ADAPT_ARCHIVE_DIRECTORY, "docs", 0, NULL, NULL, 0, &meta, archive + at, sizeof(archive) - at);
    at += adapt_archive_attributes(1, archive + at, sizeof(archive) - at);
    at += adapt_archive_attribute("user.owner", (const uint8_t *)"example", 7, archive + at, sizeof(archive) - at);
    meta.mode = 0644;
    at += adapt_archive_entry_full(ADAPT_ARCHIVE_FILE, "docs/readme.txt", 16, NULL, NULL, 0, &meta, archive + at, sizeof(archive) - at);
    memcpy(archive + at, "hello, archive!\n", 16);
    at += 16;
    at += adapt_archive_attributes(0, archive + at, sizeof(archive) - at);
    at += adapt_archive_entry_full(ADAPT_ARCHIVE_HARDLINK, "docs/readme-again.txt", 0, "docs/readme.txt", NULL, 0, &meta, archive + at, sizeof(archive) - at);
    meta.mode = 0600;
    at += adapt_archive_entry_full(ADAPT_ARCHIVE_SPARSE, "docs/holes.bin", 4096, NULL, &extent, 1, &meta, archive + at, sizeof(archive) - at);
    at += adapt_archive_extent(&extent, archive + at, sizeof(archive) - at);
    memcpy(archive + at, "sparse", 5);
    at += 5;
    at += adapt_archive_attributes(0, archive + at, sizeof(archive) - at);
    meta.mode = 0777;
    at += adapt_archive_entry_full(ADAPT_ARCHIVE_SYMLINK, "docs/link", 0, "readme.txt", NULL, 0, &meta, archive + at, sizeof(archive) - at);
    at += adapt_archive_attributes(0, archive + at, sizeof(archive) - at);
    archive[at++] = ADAPT_ARCHIVE_END;
    EXPECT(adapt_archive_entry(ADAPT_ARCHIVE_FILE, "../escape", 0, NULL, archive, sizeof(archive)) == 0);

    /* The container: sizes for the stream's length, flagged as an archive,
     * coded through the stream encoder. */
    EXPECT_OK(adapt_sizes_for_input(1, at, limits.max_memory, &sizes, &error), &error);
    sizes.archive = 1;
    src.data = archive;
    src.len = at;
    src.at = 0;
    EXPECT_OK(adapt_encode_stream(read_source, &src, write_sink, &coded, sizes, limits.max_memory, NULL, &error), &error);
    file = coded.data;
    file_len = coded.len;
    coded_len = file_len;
    EXPECT_OK(adapt_read_header(file, file_len, &sizes, &error), &error);
    EXPECT(sizes.archive == 1);

    /* Decoding hands the archive's bytes to the unpacker as they come;
     * the unpacker checks every rule and calls the sink per entry. */
    memset(&s, 0, sizeof(s));
    sink_fns.context = &s;
    sink_fns.directory = on_directory;
    sink_fns.file_open = on_file_open;
    sink_fns.file_data = on_file_data;
    sink_fns.file_close = on_file_close;
    sink_fns.symlink = on_symlink;
    sink_fns.hardlink = on_hardlink;
    sink_fns.sparse_open = on_sparse_open;
    sink_fns.extent = on_extent;
    sink_fns.attribute = on_attribute;
    u = adapt_archive_unpacker_new(sink_fns);
    EXPECT(u != NULL);
    adapt_archive_set_metadata_callback(u, on_metadata);
    src.data = file;
    src.len = file_len;
    src.at = 0;
    EXPECT_OK(adapt_decode_stream(read_source, &src, adapt_archive_unpack, u, limits, NULL, &error), &error);
    EXPECT_OK(adapt_archive_finish(u, &error), &error);
    EXPECT(adapt_archive_entries(u) == 5 && s.file_bytes == 21);
    EXPECT(strcmp(s.log, want) == 0);
    fputs(s.log, stdout);
    printf("%zu archive bytes -> %zu container bytes\n", at, coded_len);
    adapt_archive_unpacker_free(u);

    /* A malformed archive is refused where the rule breaks, with the
     * offset, and stays refused. */
    archive[ADAPT_ARCHIVE_MAGIC_BYTES] = 9; /* no such entry kind */
    u = adapt_archive_unpacker_new(sink_fns);
    EXPECT(adapt_archive_unpack(u, archive, at) == ADAPT_ARCHIVE_MALFORMED);
    {
        uint64_t offset = 0;
        const char *problem = adapt_archive_problem(u, &offset);
        EXPECT(problem != NULL && offset == ADAPT_ARCHIVE_MAGIC_BYTES);
        printf("refused at byte %llu: %s\n", (unsigned long long)offset, problem);
    }
    EXPECT(adapt_archive_unpack(u, archive, 1) == ADAPT_ARCHIVE_MALFORMED);
    adapt_archive_unpacker_free(u);
    adapt_archive_unpacker_free(NULL);

    free(decoded.data);
    free(file);
    return 0;
}
