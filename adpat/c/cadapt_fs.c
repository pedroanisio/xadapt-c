/*
 * cadapt_fs.c — extended attributes and sparse maps, the filesystem side of
 * what FMT-7 preserves. See cadapt_fs.h for the interface.
 *
 * Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-12.
 * Extracted from cadapt.c, 2026-09-17: the code is unchanged, and the six
 * functions cadapt.c calls lost their `static`. The metadata calls — a
 * path's modes and times read, applied and checked, inherited ACLs cleared
 * — followed the same day, unchanged.
 */
#include "cadapt_platform.h"

#include "cadapt_fs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ------------------------------------------------------------------------
 * Extended attributes and sparse maps: what FMT-7 preserves beyond modes
 * and times. Attributes travel by name; on Linux the POSIX ACLs are two
 * of them (system.posix_acl_access, system.posix_acl_default), so they
 * come along. Only the namespaces a file's owner may set again are kept:
 * user.* and the ACLs. security.* and trusted.* belong to the kernel and
 * to root and are left where they are.
 * --------------------------------------------------------------------- */


void attrs_free(attrs *a) {
    uint32_t i;
    /* attrs_read() counts the names before it allocates their arrays, so
     * a failed allocation is freed with the count set and an array NULL. */
    for (i = 0; i < a->count; i++) {
        if (a->names != NULL) free(a->names[i]);
        if (a->values != NULL) free(a->values[i]);
    }
    free(a->names);
    free(a->values);
    free(a->lens);
    a->names = NULL;
    a->values = NULL;
    a->lens = NULL;
    a->count = 0;
}

/* The bytes an entry's attribute trailer takes in the archive. */
uint64_t attrs_bytes(const attrs *a) {
    uint64_t n = 2;
    uint32_t i;
    for (i = 0; i < a->count; i++) {
        n += adapt_archive_attribute(a->names[i], a->values[i], a->lens[i], NULL, 0);
    }
    return n;
}

#ifdef CADAPT_XATTR
static int attr_kept(const char *name) {
    return strncmp(name, "user.", 5) == 0 || strcmp(name, "system.posix_acl_access") == 0 ||
           strcmp(name, "system.posix_acl_default") == 0;
}

static int compare_attr_names(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static ssize_t xattr_list(const char *path, char *buf, size_t size) {
#ifdef __APPLE__
    return listxattr(path, buf, size, XATTR_NOFOLLOW);
#else
    return llistxattr(path, buf, size);
#endif
}

static ssize_t xattr_get(const char *path, const char *name, void *buf, size_t size) {
#ifdef __APPLE__
    return getxattr(path, name, buf, size, 0, XATTR_NOFOLLOW);
#else
    return lgetxattr(path, name, buf, size);
#endif
}

static int xattr_set_fd(int fd, const char *name, const void *value, size_t size) {
#ifdef __APPLE__
    return fsetxattr(fd, name, value, size, 0, 0);
#else
    return fsetxattr(fd, name, value, size, 0);
#endif
}

static ssize_t xattr_get_fd(int fd, const char *name, void *value, size_t size) {
#ifdef __APPLE__
    return fgetxattr(fd, name, value, size, 0, 0);
#else
    return fgetxattr(fd, name, value, size);
#endif
}

/* The attributes of the entry at `path` (a link's own, not its target's),
 * in byte order of their names. A filesystem without attributes gives
 * none. Returns 0 or an errno; E2BIG for a value the archive cannot hold. */
int attrs_read(const char *path, attrs *a) {
    char *list = NULL, *at, *end;
    ssize_t n;
    size_t cap = 4096;
    uint32_t i;
    memset(a, 0, sizeof(*a));
    for (;;) {
        list = malloc(cap);
        if (list == NULL) {
            return ENOMEM;
        }
        n = xattr_list(path, list, cap);
        if (n >= 0) {
            break;
        }
        free(list);
        if (errno == ERANGE) {
            cap *= 2;
            continue;
        }
        return errno == ENOTSUP || errno == EOPNOTSUPP || errno == ENOSYS ? 0 : errno;
    }
    for (at = list, end = list + n; at < end; at += strlen(at) + 1) {
        if (attr_kept(at)) {
            a->count++;
        }
    }
    if (a->count == 0) {
        free(list);
        return 0;
    }
    a->names = calloc(a->count, sizeof(*a->names));
    a->values = calloc(a->count, sizeof(*a->values));
    a->lens = calloc(a->count, sizeof(*a->lens));
    if (a->names == NULL || a->values == NULL || a->lens == NULL) {
        free(list);
        attrs_free(a);
        return ENOMEM;
    }
    for (at = list, i = 0; at < end; at += strlen(at) + 1) {
        if (attr_kept(at)) {
            a->names[i] = malloc(strlen(at) + 1);
            if (a->names[i] == NULL) {
                free(list);
                attrs_free(a);
                return ENOMEM;
            }
            strcpy(a->names[i++], at);
        }
    }
    free(list);
    qsort(a->names, a->count, sizeof(*a->names), compare_attr_names);
    for (i = 0; i < a->count; i++) {
        ssize_t len = xattr_get(path, a->names[i], NULL, 0);
        if (len < 0) {
            int e = errno;
            attrs_free(a);
            return e;
        }
        if ((size_t)len > ADAPT_ARCHIVE_ATTRIBUTE_VALUE_MAX || strlen(a->names[i]) > ADAPT_ARCHIVE_ATTRIBUTE_NAME_MAX) {
            attrs_free(a);
            return E2BIG;
        }
        a->values[i] = malloc((size_t)len + 1);
        if (a->values[i] == NULL) {
            attrs_free(a);
            return ENOMEM;
        }
        len = xattr_get(path, a->names[i], a->values[i], (size_t)len + 1);
        if (len < 0) {
            int e = errno;
            attrs_free(a);
            return e;
        }
        a->lens[i] = (size_t)len;
    }
    return 0;
}

/* Set one attribute and read it back: a filesystem that accepts the call
 * but keeps something else has not restored it. Returns 0 or an errno. */
int attr_apply(int fd, const char *name, const uint8_t *value, size_t len) {
    uint8_t *back;
    ssize_t got;
    if (xattr_set_fd(fd, name, value, len) != 0) {
        return errno;
    }
    back = malloc(len + 1);
    if (back == NULL) {
        return ENOMEM;
    }
    got = xattr_get_fd(fd, name, back, len + 1);
    if (got < 0 || (size_t)got != len || (len > 0 && memcmp(back, value, len) != 0)) {
        free(back);
        return got < 0 ? errno : ENOTSUP;
    }
    free(back);
    return 0;
}
#else
int attrs_read(const char *path, attrs *a) {
    (void)path;
    memset(a, 0, sizeof(*a));
    return 0;
}

int attr_apply(int fd, const char *name, const uint8_t *value, size_t len) {
    (void)fd;
    (void)name;
    (void)value;
    (void)len;
    return ENOTSUP;
}
#endif

/* The data extents of the file at `path`, `size` bytes long, when the
 * platform can tell holes from data and the file has any: *count extents
 * (malloc'd), or *count = UINT32_MAX for a dense file. A file of more
 * extents than the archive lists is stored dense. Returns 0 or an errno. */
int sparse_map(const char *path, uint64_t size, adapt_extent **extents, uint32_t *count) {
    *extents = NULL;
    *count = UINT32_MAX;
#if !defined(_WIN32) && defined(SEEK_HOLE) && defined(SEEK_DATA)
    {
        int fd = open(path, O_RDONLY);
        off_t at = 0, data, hole;
        adapt_extent *list = NULL;
        uint32_t n = 0, cap = 0;
        if (fd < 0) {
            return errno;
        }
        /* a file without holes has its first hole at its end */
        hole = lseek(fd, 0, SEEK_HOLE);
        if (hole < 0) {
            int e = errno;
            close(fd);
            return e == EINVAL || e == ENOTSUP || e == EOPNOTSUPP ? 0 : e;
        }
        if ((uint64_t)hole >= size) {
            close(fd);
            return 0;
        }
        while ((uint64_t)at < size) {
            data = lseek(fd, at, SEEK_DATA);
            if (data < 0) {
                if (errno == ENXIO) {
                    break; /* only a hole remains */
                }
                free(list);
                close(fd);
                return errno;
            }
            hole = lseek(fd, data, SEEK_HOLE);
            if (hole < 0) {
                free(list);
                close(fd);
                return errno;
            }
            if ((uint64_t)hole > size) {
                hole = (off_t)size;
            }
            if (n == cap) {
                adapt_extent *grown;
                cap = cap == 0 ? 16 : cap * 2;
                grown = realloc(list, cap * sizeof(*grown));
                if (grown == NULL) {
                    free(list);
                    close(fd);
                    return ENOMEM;
                }
                list = grown;
            }
            if (n == ADAPT_ARCHIVE_EXTENTS_MAX) {
                free(list);
                close(fd);
                return 0;
            }
            list[n].offset = (uint64_t)data;
            list[n].length = (uint64_t)(hole - data);
            n++;
            at = hole;
        }
        close(fd);
        *extents = list;
        *count = n;
        return 0;
    }
#else
    (void)path;
    (void)size;
    return 0;
#endif
}

/* ------------------------------------------------------------------------
 * Metadata: modes and times read from a path, applied to a path or a
 * descriptor and checked after, and inherited ACLs cleared before.
 * --------------------------------------------------------------------- */

/* Capture before reading a file or traversing a directory, which can change
 * its access time. Archive links are examined without following them. */
int metadata_read_path(const char *path, int nofollow, adapt_metadata *m) {
#ifdef _WIN32
    struct _stat64 st;
    (void)nofollow;
    if (_stat64(path, &st) != 0) {
        return errno != 0 ? errno : EIO;
    }
    m->atime_seconds = st.st_atime;
    m->mtime_seconds = st.st_mtime;
    m->atime_nanoseconds = m->mtime_nanoseconds = 0;
    m->mode = (uint32_t)st.st_mode & 07777u;
#else
    struct stat st;
    if ((nofollow ? lstat(path, &st) : stat(path, &st)) != 0) {
        return errno != 0 ? errno : EIO;
    }
    metadata_from_stat(&st, m);
#endif
    return 0;
}

#ifndef _WIN32
/* The instant a stat's timespec names, with the fraction in [0, 1e9):
 * Darwin reports a time before 1970 with tv_sec truncated toward zero and
 * tv_nsec negative, and the metadata record refuses that fraction. */
static void stat_time(struct timespec t, int64_t *seconds, uint32_t *nanoseconds) {
    int64_t sec = t.tv_sec, nsec = t.tv_nsec;
    if (nsec < 0) {
        sec -= 1;
        nsec += 1000000000;
    }
    *seconds = sec;
    *nanoseconds = (uint32_t)nsec;
}

void metadata_from_stat(const struct stat *st, adapt_metadata *m) {
#ifdef __APPLE__
    stat_time(st->st_atimespec, &m->atime_seconds, &m->atime_nanoseconds);
    stat_time(st->st_mtimespec, &m->mtime_seconds, &m->mtime_nanoseconds);
#else
    stat_time(st->st_atim, &m->atime_seconds, &m->atime_nanoseconds);
    stat_time(st->st_mtim, &m->mtime_seconds, &m->mtime_nanoseconds);
#endif
    m->mode = (uint32_t)st->st_mode & 07777u;
}
#endif

#ifdef _WIN32
int metadata_apply(const char *path, int symlink_entry, const adapt_metadata *m) {
    adapt_metadata restored = {0}; /* written by a read that succeeds; MinGW's gcc cannot see that */
    int rc;
    struct __utimbuf64 times;
    if (symlink_entry || m->atime_nanoseconds != 0 || m->mtime_nanoseconds != 0) {
        return ENOTSUP;
    }
    times.actime = m->atime_seconds;
    times.modtime = m->mtime_seconds;
    if (_utime64(path, &times) != 0 || _chmod(path, (int)(m->mode & (_S_IREAD | _S_IWRITE))) != 0) {
        return errno;
    }
    /* Filesystems may silently clamp dates or round away precision even
     * when their setter succeeds. Refuse publication if fidelity was lost. */
    rc = metadata_read_path(path, symlink_entry, &restored);
    if (rc != 0) {
        return rc;
    }
    if (restored.mode != m->mode || restored.atime_seconds != m->atime_seconds ||
        restored.atime_nanoseconds != m->atime_nanoseconds || restored.mtime_seconds != m->mtime_seconds ||
        restored.mtime_nanoseconds != m->mtime_nanoseconds) {
        return ENOTSUP;
    }
    return 0;
}

#endif

#ifndef _WIN32
/* A directory handle keeps metadata tied to the restored inode even when
 * someone can rename entries in the output's parent directory. */
int metadata_check_stat(const struct stat *st, const adapt_metadata *m, int with_mode) {
    adapt_metadata held;
    metadata_from_stat(st, &held);
    return (!with_mode || held.mode == m->mode) && held.atime_seconds == m->atime_seconds &&
                   held.atime_nanoseconds == m->atime_nanoseconds && held.mtime_seconds == m->mtime_seconds &&
                   held.mtime_nanoseconds == m->mtime_nanoseconds
               ? 0 : ENOTSUP;
}

int metadata_times(const adapt_metadata *m, struct timespec times[2]) {
    times[0].tv_sec = (time_t)m->atime_seconds;
    times[0].tv_nsec = (long)m->atime_nanoseconds;
    times[1].tv_sec = (time_t)m->mtime_seconds;
    times[1].tv_nsec = (long)m->mtime_nanoseconds;
    return (int64_t)times[0].tv_sec == m->atime_seconds && (int64_t)times[1].tv_sec == m->mtime_seconds ? 0 : ERANGE;
}

int metadata_apply_fd(int fd, const adapt_metadata *m) {
    struct timespec times[2];
    struct stat st;
    int rc = metadata_times(m, times);
    if (rc != 0) return rc;
    if (futimens(fd, times) != 0 || fchmod(fd, (mode_t)m->mode) != 0 || fstat(fd, &st) != 0) return errno;
    return metadata_check_stat(&st, m, 1);
}

/* chmod changes an ACL's mask, not its named entries. Remove inherited
 * ACLs before any archived permissions are installed or children created. */
int clear_inherited_acl(int fd) {
#ifdef __linux__
    const char *names[] = {"system.posix_acl_default", "system.posix_acl_access"};
    size_t i;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (fremovexattr(fd, names[i]) != 0 && errno != ENODATA && errno != ENOTSUP &&
            errno != EOPNOTSUPP && errno != ENOSYS) return errno;
    }
#else
    (void)fd;
#endif
    return 0;
}
#endif
