/*
 * cadapt_fs.h — the extended attributes and sparse map an archive entry
 * carries, what FMT-7 preserves of a file beyond its bytes, and the modes
 * and times themselves as the operating system reads and applies them.
 *
 * Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-12.
 * Extracted from cadapt.c, 2026-09-17, unchanged.
 *
 * This is the only part of the command line that reads or writes a file's
 * metadata through the operating system's own calls, and the shape of those
 * calls differs on Linux, macOS and everywhere else. Keeping it here rather
 * than in cadapt.c is what lets it be read, and tested, as one thing.
 */
#ifndef CADAPT_FS_H
#define CADAPT_FS_H

/* cadapt_platform.h first, always: it sets _POSIX_C_SOURCE and _GNU_SOURCE,
 * and a system header that arrives before them is compiled without them.
 * adapt.h pulls in system headers, so putting it first is what silently
 * turned the sparse map into its no-holes fallback. */
#include "cadapt_platform.h"

#include "adapt.h"

#include <stddef.h>
#include <stdint.h>

/* One entry's kept attributes: names, values and their lengths, in the order
 * an archive writes them. `count` is 0 for a file that has none, and for every
 * file on a platform without extended attributes. */
typedef struct attrs {
    char **names;
    uint8_t **values;
    size_t *lens;
    uint32_t count;
} attrs;

/* Release everything `attrs_read` allocated. Safe on a zeroed `attrs`. */
void attrs_free(attrs *a);

/* The bytes an entry's attribute trailer takes in the archive. */
uint64_t attrs_bytes(const attrs *a);

/* The attributes of the entry at `path` (a link's own, not its target's),
 * sorted by name so an archive is reproducible. Returns 0, or -1 with errno
 * set; on a platform without extended attributes it returns 0 and no names. */
int attrs_read(const char *path, attrs *a);

/* Set one attribute on `fd` and read it back: a filesystem that accepts the
 * call and stores nothing must not pass as a restore. Returns 0 or -1. */
int attr_apply(int fd, const char *name, const uint8_t *value, size_t len);

/* The data extents of the file at `path`, `size` bytes long, when the
 * platform can report its holes. Returns 0 with `*count` 0 when it cannot,
 * so a caller treats the file as one extent; -1 with errno set on failure.
 * The caller frees `*extents`. */
int sparse_map(const char *path, uint64_t size, adapt_extent **extents, uint32_t *count);

/* A path's metadata (adapt.h) — modes and times — captured before it is
 * read or traversed, since reading changes the access time; links are
 * examined without following them. Applied back after a restore: on
 * Windows by path, and re-read to refuse a filesystem that clamped a date;
 * elsewhere through a descriptor, checked by fstat, with the inherited
 * ACLs cleared first since chmod changes an ACL's mask, not its entries.
 * 0, or an errno. */
int metadata_read_path(const char *path, int nofollow, adapt_metadata *m);
#ifdef _WIN32
int metadata_apply(const char *path, int symlink_entry, const adapt_metadata *m);
#else
/* The record a stat holds: the mode's permission bits and the two times,
 * each with its fraction in [0, 1e9) — a platform that reports a time
 * before 1970 as a negative fraction (Darwin) is read as the same instant. */
void metadata_from_stat(const struct stat *st, adapt_metadata *m);
/* 0 where the stat holds the record, ENOTSUP where it does not; with_mode 0
 * leaves the mode to the platform (a link's, where it cannot be set). */
int metadata_check_stat(const struct stat *st, const adapt_metadata *m, int with_mode);
int metadata_times(const adapt_metadata *m, struct timespec times[2]);
int metadata_apply_fd(int fd, const adapt_metadata *m);
int clear_inherited_acl(int fd);
#endif

#endif /* CADAPT_FS_H */
