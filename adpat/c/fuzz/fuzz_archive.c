/* The archive unpacker under libFuzzer with the address and undefined-
 * behaviour sanitizers: arbitrary bytes fed to adapt_archive_unpack() in
 * short pieces, into a sink that holds the unpacker to its contract — every
 * callback set, a file's data only between its open and its close and never
 * past the length it was opened with, an extent's bytes only inside the
 * extent, an entry's path and a link's target valid archive paths — and
 * then adapt_archive_finish(). The unpacker returns 0 or a named fault,
 * never reads out of bounds, never leaks. Built and run like fuzz_decode:
 *
 *   make fuzz-archive-smoke
 *
 * seeded with the well-formed archives tools/archive_seeds writes.
 *
 * Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-17. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../adapt.h"

typedef struct {
    int open;            /* inside a file or a sparse file */
    uint64_t remaining;  /* bytes a plain file may still deliver */
    uint64_t size;       /* a sparse file's apparent length */
    uint64_t next;       /* where a sparse file's next extent may start */
    uint64_t extent;     /* bytes the current extent may still deliver */
    int sparse;
    uint64_t entries;
} contract;

static void must(int ok) {
    if (!ok) abort();
}

static int on_directory(void *c, const char *path) {
    contract *k = c;
    must(!k->open && adapt_archive_path_valid(path));
    k->entries++;
    return 0;
}

static int on_file_open(void *c, const char *path, uint64_t size) {
    contract *k = c;
    must(!k->open && adapt_archive_path_valid(path));
    k->open = 1;
    k->sparse = 0;
    k->remaining = size;
    k->entries++;
    return 0;
}

static int on_file_data(void *c, const uint8_t *data, size_t len) {
    contract *k = c;
    volatile uint8_t sink = 0;
    size_t i;
    must(k->open);
    if (k->sparse) {
        /* only an extent's bytes come, never a hole's */
        must(len <= k->extent);
        k->extent -= len;
    } else {
        must(len <= k->remaining);
        k->remaining -= len;
    }
    for (i = 0; i < len; i++) sink ^= data[i]; /* every byte is readable */
    (void)sink;
    return 0;
}

static int on_file_close(void *c) {
    contract *k = c;
    must(k->open && (k->sparse ? k->extent == 0 : k->remaining == 0));
    k->open = 0;
    return 0;
}

static int on_symlink(void *c, const char *path, const char *target) {
    contract *k = c;
    must(!k->open && adapt_archive_path_valid(path) && target != NULL && target[0] != '\0');
    k->entries++;
    return 0;
}

static int on_hardlink(void *c, const char *path, const char *target) {
    contract *k = c;
    must(!k->open && adapt_archive_path_valid(path) && adapt_archive_path_valid(target) && strcmp(path, target) != 0);
    k->entries++;
    return 0;
}

static int on_sparse_open(void *c, const char *path, uint64_t size, uint32_t extents) {
    contract *k = c;
    (void)extents;
    must(!k->open && adapt_archive_path_valid(path));
    k->open = 1;
    k->sparse = 1;
    k->size = size;
    k->next = 0;
    k->extent = 0;
    k->entries++;
    return 0;
}

static int on_extent(void *c, uint64_t offset, uint64_t length) {
    contract *k = c;
    /* extents ascend, do not touch, and end inside the apparent length */
    must(k->open && k->sparse && k->extent == 0 && length > 0 && offset >= k->next && offset <= k->size &&
         length <= k->size - offset);
    k->next = offset + length;
    k->extent = length;
    return 0;
}

static int on_attribute(void *c, const char *path, uint8_t kind, const char *name, const uint8_t *value, size_t len) {
    contract *k = c;
    volatile uint8_t sink = 0;
    size_t i;
    (void)kind;
    must(!k->open && adapt_archive_path_valid(path) && name != NULL && name[0] != '\0');
    for (i = 0; i < len; i++) sink ^= value[i];
    (void)sink;
    return 0;
}

static int on_metadata(void *c, const char *path, uint8_t kind, const adapt_metadata *m) {
    contract *k = c;
    (void)kind;
    must(!k->open && adapt_archive_path_valid(path) && m != NULL);
    return 0;
}

static int on_root_attribute(void *c, const char *name, const uint8_t *value, size_t len) {
    contract *k = c;
    volatile uint8_t sink = 0;
    size_t i;
    must(!k->open && name != NULL && name[0] != '\0');
    for (i = 0; i < len; i++) sink ^= value[i];
    (void)sink;
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    contract k;
    adapt_archive_sink sink;
    adapt_archive_unpacker *u;
    adapt_error error;
    size_t at = 0;
    int rc = 0;
    memset(&k, 0, sizeof(k));
    sink.context = &k;
    sink.directory = on_directory;
    sink.file_open = on_file_open;
    sink.file_data = on_file_data;
    sink.file_close = on_file_close;
    sink.symlink = on_symlink;
    sink.hardlink = on_hardlink;
    sink.sparse_open = on_sparse_open;
    sink.extent = on_extent;
    sink.attribute = on_attribute;
    u = adapt_archive_unpacker_new(sink);
    if (u == NULL) return 0;
    adapt_archive_set_metadata_callback(u, on_metadata);
    adapt_archive_set_root_attribute_callback(u, on_root_attribute);
    /* pieces of one to seven bytes, so every field boundary is crossed */
    while (at < size && rc == 0) {
        size_t n = 1 + (size_t)(data[at] % 7);
        if (n > size - at) n = size - at;
        rc = adapt_archive_unpack(u, data + at, n);
        at += n;
    }
    if (rc == 0) {
        adapt_status status = adapt_archive_finish(u, &error);
        must(status == ADAPT_OK || status == ADAPT_ERROR_BAD_ARCHIVE);
        if (status == ADAPT_OK) must(!k.open && adapt_archive_entries(u) == k.entries);
    } else {
        uint64_t offset = 0;
        must(rc == ADAPT_ARCHIVE_MALFORMED);
        must(adapt_archive_problem(u, &offset) != NULL && offset <= at);
        /* malformed once, malformed for good */
        must(adapt_archive_unpack(u, data, size > 3 ? 3 : size) == ADAPT_ARCHIVE_MALFORMED);
    }
    adapt_archive_unpacker_free(u);
    return 0;
}
