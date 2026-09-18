/* Well-formed archives of every version, from the public writers, as the
 * seeds fuzz_archive.c starts from: a version-1 folder, a version-2 one
 * with metadata, a version-3 one with attributes, a hard link and a sparse
 * file, and a version-4 one with root attributes. Writes them into the
 * directory named on the command line (make fuzz-archive-smoke).
 *
 * Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-17. */
#include "../adapt.h"
#include <stdio.h>
#include <string.h>
static uint8_t buf[1 << 16]; static size_t len;
static void put(const void *d, size_t n) { memcpy(buf + len, d, n); len += n; }
static void save(const char *dir, const char *name) { char p[512]; FILE *f; snprintf(p, sizeof p, "%s/%s", dir, name); f = fopen(p, "wb"); fwrite(buf, 1, len, f); fclose(f); len = 0; }
int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    adapt_metadata meta; adapt_extent ex[2] = {{2, 3}, {8, 2}}; uint8_t end = ADAPT_ARCHIVE_END;
    memset(&meta, 0, sizeof meta); meta.mode = 0644;
    put(ADAPT_ARCHIVE_MAGIC, ADAPT_ARCHIVE_MAGIC_BYTES);
    len += adapt_archive_entry(ADAPT_ARCHIVE_DIRECTORY, "d", 0, NULL, buf + len, sizeof buf - len);
    len += adapt_archive_entry(ADAPT_ARCHIVE_FILE, "d/f", 11, NULL, buf + len, sizeof buf - len); put("hello world", 11);
    len += adapt_archive_entry(ADAPT_ARCHIVE_SYMLINK, "d/l", 0, "../f", buf + len, sizeof buf - len);
    len += adapt_archive_entry(ADAPT_ARCHIVE_FILE, "z", 0, NULL, buf + len, sizeof buf - len);
    put(&end, 1); save(dir, "v1.arc");
    put(ADAPT_ARCHIVE_METADATA_MAGIC, ADAPT_ARCHIVE_MAGIC_BYTES);
    len += adapt_archive_entry_metadata(ADAPT_ARCHIVE_DIRECTORY, "d", 0, NULL, &meta, buf + len, sizeof buf - len);
    len += adapt_archive_entry_metadata(ADAPT_ARCHIVE_FILE, "d/f", 3, NULL, &meta, buf + len, sizeof buf - len); put("abc", 3);
    len += adapt_archive_entry_metadata(ADAPT_ARCHIVE_SYMLINK, "l", 0, "d/f", &meta, buf + len, sizeof buf - len);
    put(&end, 1); save(dir, "v2.arc");
    put(ADAPT_ARCHIVE_FULL_MAGIC, ADAPT_ARCHIVE_MAGIC_BYTES);
    len += adapt_archive_entry_full(ADAPT_ARCHIVE_DIRECTORY, "d", 0, NULL, NULL, 0, &meta, buf + len, sizeof buf - len);
    len += adapt_archive_attributes(1, buf + len, sizeof buf - len);
    len += adapt_archive_attribute("system.posix_acl_access", (const uint8_t *)"acl", 3, buf + len, sizeof buf - len);
    len += adapt_archive_entry_full(ADAPT_ARCHIVE_FILE, "d/f", 3, NULL, NULL, 0, &meta, buf + len, sizeof buf - len); put("abc", 3);
    len += adapt_archive_attributes(2, buf + len, sizeof buf - len);
    len += adapt_archive_attribute("user.a", (const uint8_t *)"1", 1, buf + len, sizeof buf - len);
    len += adapt_archive_attribute("user.b", (const uint8_t *)"", 0, buf + len, sizeof buf - len);
    len += adapt_archive_entry_full(ADAPT_ARCHIVE_HARDLINK, "d/h", 0, "d/f", NULL, 0, &meta, buf + len, sizeof buf - len);
    len += adapt_archive_entry_full(ADAPT_ARCHIVE_SPARSE, "s", 10, NULL, ex, 2, &meta, buf + len, sizeof buf - len);
    len += adapt_archive_extent(&ex[0], buf + len, sizeof buf - len); put("xyz", 3);
    len += adapt_archive_extent(&ex[1], buf + len, sizeof buf - len); put("pq", 2);
    len += adapt_archive_attributes(0, buf + len, sizeof buf - len);
    len += adapt_archive_entry_full(ADAPT_ARCHIVE_SYMLINK, "l", 0, "d/f", NULL, 0, &meta, buf + len, sizeof buf - len);
    len += adapt_archive_attributes(0, buf + len, sizeof buf - len);
    put(&end, 1); save(dir, "v3.arc");
    /* v4: root attributes before the entries */
    put(ADAPT_ARCHIVE_ROOT_MAGIC, ADAPT_ARCHIVE_MAGIC_BYTES);
    len += adapt_archive_attributes(1, buf + len, sizeof buf - len);
    len += adapt_archive_attribute("user.root", (const uint8_t *)"r", 1, buf + len, sizeof buf - len);
    len += adapt_archive_entry_full(ADAPT_ARCHIVE_FILE, "f", 2, NULL, NULL, 0, &meta, buf + len, sizeof buf - len); put("hi", 2);
    len += adapt_archive_attributes(0, buf + len, sizeof buf - len);
    put(&end, 1); save(dir, "v4.arc");
    printf("seeds written to %s\n", dir);
    return 0;
}
