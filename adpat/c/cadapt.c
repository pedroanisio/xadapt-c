/*
 * cadapt — command line for the C codec.
 *
 * Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-12.
 *
 *   cadapt encode <input> [-o <output>] [--force] [--verify | --no-verify] [--fast | --max] [--max-memory <bytes>]
 *                 [--threads <n>] [--segment-bytes <bytes>]
 *   cadapt decode <input> [-o <output>] [--force] [--max-output <bytes>] [--max-memory <bytes>] [--threads <n>]
 *   cadapt info   <file>
 *
 * The commands and container follow `adapt` in the Rust crate; this CLI
 * makes encoding verification opt-in, and takes an <input> that is a
 * folder, which the crate does not: the folder is walked into one archive
 * stream (adapt.h, "Archives") and coded as one container, which `decode`
 * restores as a folder — every file byte for byte, every directory and
 * symbolic link in its place — under the output name, never to standard
 * output. Both directions stream: memory is
 * the model and one block, whatever the length. `encode --verify` proves
 * what it wrote decodes to the input — with a second model checking
 * every block as it is written when the budget holds two, or, for a file
 * input written to a file, in a second pass with one. Encoding skips this
 * proof by default; --no-verify remains accepted for existing scripts.
 * A file output is staged beside its target and
 * published only once complete and, when asked, proven; without --force an
 * existing target is kept. `-` names standard input or output; bytes
 * written to standard output before an error was found are not to be
 * trusted.
 *
 * C99 plus platform file APIs: exclusive creation for staging, POSIX
 * link() for publishing without replacing a target and for a folder's
 * hard links, a 64-bit ftell for `info` on large files, directory
 * listing, lstat, readlink, mkdir and symlink for folders, lseek's
 * SEEK_HOLE and SEEK_DATA for a file's holes, and the platform's
 * extended-attribute calls (Linux, macOS) for attributes and, on Linux,
 * POSIX ACLs. Windows equivalents are named below; there a symbolic link
 * is neither seen in a folder nor restored from one, a hard link is
 * restored as a copy, holes come back as zeros and attributes are neither
 * packed nor restored — the README states each rule. A filesystem without
 * hard links (FAT, exFAT, some network mounts) refuses link(), so
 * publishing there needs --force, which renames instead.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
/* Linux keeps SEEK_DATA and SEEK_HOLE, the sparse map, behind _GNU_SOURCE. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif
#if !defined(_WIN32) && !defined(_FILE_OFFSET_BITS)
#define _FILE_OFFSET_BITS 64
#endif

#include "adapt.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <process.h>
#include <sys/utime.h>
#define cadapt_getpid _getpid
#define cadapt_fseek _fseeki64
#define cadapt_ftell _ftelli64
typedef long long cadapt_off;
#else
#include <dirent.h>
#include <unistd.h>
#define cadapt_getpid getpid
#define cadapt_fseek fseeko
#define cadapt_ftell ftello
typedef off_t cadapt_off;
/* Extended attributes: Linux and macOS list, read and set them by name;
 * the calls differ in shape, so each is wrapped below. Elsewhere on POSIX
 * a folder is packed without them. */
#if defined(__linux__) || defined(__APPLE__)
#include <sys/xattr.h>
#define CADAPT_XATTR 1
#endif
#endif

#define CADAPT_VERSION "0.12.0"

/* Capture before reading a file or traversing a directory, which can change
 * its access time. Archive links are examined without following them. */
static int metadata_read_path(const char *path, int nofollow, adapt_metadata *m) {
#ifdef _WIN32
    struct _stat64 st;
    (void)nofollow;
    if (_stat64(path, &st) != 0) {
        return errno != 0 ? errno : EIO;
    }
    m->atime_seconds = st.st_atime;
    m->mtime_seconds = st.st_mtime;
    m->atime_nanoseconds = m->mtime_nanoseconds = 0;
#else
    struct stat st;
    if ((nofollow ? lstat(path, &st) : stat(path, &st)) != 0) {
        return errno != 0 ? errno : EIO;
    }
#ifdef __APPLE__
    m->atime_seconds = st.st_atimespec.tv_sec;
    m->atime_nanoseconds = (uint32_t)st.st_atimespec.tv_nsec;
    m->mtime_seconds = st.st_mtimespec.tv_sec;
    m->mtime_nanoseconds = (uint32_t)st.st_mtimespec.tv_nsec;
#else
    m->atime_seconds = st.st_atim.tv_sec;
    m->atime_nanoseconds = (uint32_t)st.st_atim.tv_nsec;
    m->mtime_seconds = st.st_mtim.tv_sec;
    m->mtime_nanoseconds = (uint32_t)st.st_mtim.tv_nsec;
#endif
#endif
    m->mode = (uint32_t)st.st_mode & 07777u;
    return 0;
}

static int metadata_apply(const char *path, int symlink_entry, const adapt_metadata *m) {
    adapt_metadata restored;
    int rc;
#ifdef _WIN32
    struct __utimbuf64 times;
    if (symlink_entry || m->atime_nanoseconds != 0 || m->mtime_nanoseconds != 0) {
        return ENOTSUP;
    }
    times.actime = m->atime_seconds;
    times.modtime = m->mtime_seconds;
    if (_utime64(path, &times) != 0 || _chmod(path, (int)(m->mode & (_S_IREAD | _S_IWRITE))) != 0) {
        return errno;
    }
#else
    struct timespec times[2];
    times[0].tv_sec = (time_t)m->atime_seconds;
    times[0].tv_nsec = (long)m->atime_nanoseconds;
    times[1].tv_sec = (time_t)m->mtime_seconds;
    times[1].tv_nsec = (long)m->mtime_nanoseconds;
    if ((int64_t)times[0].tv_sec != m->atime_seconds || (int64_t)times[1].tv_sec != m->mtime_seconds) {
        return ERANGE;
    }
    if (utimensat(AT_FDCWD, path, times, symlink_entry ? AT_SYMLINK_NOFOLLOW : 0) != 0 ||
        (!symlink_entry && chmod(path, (mode_t)m->mode) != 0)) {
        return errno;
    }
#endif
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

/* ------------------------------------------------------------------------
 * Extended attributes and sparse maps: what FMT-7 preserves beyond modes
 * and times. Attributes travel by name; on Linux the POSIX ACLs are two
 * of them (system.posix_acl_access, system.posix_acl_default), so they
 * come along. Only the namespaces a file's owner may set again are kept:
 * user.* and the ACLs. security.* and trusted.* belong to the kernel and
 * to root and are left where they are.
 * --------------------------------------------------------------------- */

typedef struct attrs {
    char **names;
    uint8_t **values;
    size_t *lens;
    uint32_t count;
} attrs;

static void attrs_free(attrs *a) {
    uint32_t i;
    for (i = 0; i < a->count; i++) {
        free(a->names[i]);
        free(a->values[i]);
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
static uint64_t attrs_bytes(const attrs *a) {
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

static int xattr_set(const char *path, const char *name, const void *value, size_t size) {
#ifdef __APPLE__
    return setxattr(path, name, value, size, 0, XATTR_NOFOLLOW);
#else
    return lsetxattr(path, name, value, size, 0);
#endif
}

/* The attributes of the entry at `path` (a link's own, not its target's),
 * in byte order of their names. A filesystem without attributes gives
 * none. Returns 0 or an errno; E2BIG for a value the archive cannot hold. */
static int attrs_read(const char *path, attrs *a) {
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
static int attr_apply(const char *path, const char *name, const uint8_t *value, size_t len) {
    uint8_t *back;
    ssize_t got;
    if (xattr_set(path, name, value, len) != 0) {
        return errno;
    }
    back = malloc(len + 1);
    if (back == NULL) {
        return ENOMEM;
    }
    got = xattr_get(path, name, back, len + 1);
    if (got < 0 || (size_t)got != len || (len > 0 && memcmp(back, value, len) != 0)) {
        free(back);
        return got < 0 ? errno : ENOTSUP;
    }
    free(back);
    return 0;
}
#else
static int attrs_read(const char *path, attrs *a) {
    (void)path;
    memset(a, 0, sizeof(*a));
    return 0;
}

static int attr_apply(const char *path, const char *name, const uint8_t *value, size_t len) {
    (void)path;
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
static int sparse_map(const char *path, uint64_t size, adapt_extent **extents, uint32_t *count) {
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

/* Format numbers and memory defaults come from the codec constants.
 * Verification is opt-in; segments go on threads when the budget holds
 * their workers. */
static void print_usage(FILE *stream) {
    adapt_sizes minimum_sizes;
    adapt_error minimum_error, minimum_max_error;
    (void)adapt_sizes_for_input(1, 0, 0, &minimum_sizes, &minimum_error);
    (void)adapt_sizes_for_input_max(1, 0, 0, &minimum_sizes, &minimum_max_error);
    fprintf(stream,
            "cadapt — the adapt self-adaptive lossless codec, in C\n"
            "\n"
            "usage:\n"
            "  cadapt encode <input> [-o <output>] [--force] [--verify | --no-verify] [--fast | --max] [-0 .. -9]\n"
            "                [--max-memory <bytes>] [--threads <n>] [--segment-bytes <bytes>]\n"
            "                [--adaptive rates|sparse|retained|auto] [--transform bmp] [--allocation window|context]\n"
            "  cadapt decode <input> [-o <output>] [--force] [--max-output <bytes>] [--max-memory <bytes>] [--threads <n>]\n"
            "  cadapt info   <file>\n"
            "  cadapt --version\n"
            "\n"
            "<input> or <output> may be '-' for standard input or output. Without -o,\n"
            "encode writes <input>.adpt and decode strips that suffix (or appends .out).\n"
            "Existing output files are kept unless --force is given. --fast writes the\n"
            "fast profile, which decodes about three times faster for about a twelfth\n"
            "more bytes; --max the max profile, which writes\n"
            "fewer bytes for about three times the time in the same memory; the default is\n"
            "the normal profile. Normal/fast/max streams use versions %u/%u/%u;\n"
            "named files and folders include metadata in versions %u/%u/%u.\n"
            "--adaptive rates/sparse/retained select experimental causal predictors\n"
            "over normal or --max. auto spools the\n"
            "source once to temporary disk and selects the smallest complete container\n"
            "from the base and these three profiles that fit the memory budget. Search\n"
            "also tries BMP24 color/row residuals on eligible files. Experiment formats occupy versions 25--52;\n"
            "the max profile's compact side tables, chosen by --allocation context when the budget\n"
            "would otherwise leave the context table at 2^22 blocks or fewer, are versions 53/56.\n"
            "--transform bmp explicitly selects that reversible transform. Search\n"
            "uses up to eight encodes, temporary disk of input plus two containers, and\n"
            "verifies only with --verify (one model, also for pipes).\n"
            "--allocation context grows the context table before the recall window;\n"
            "window is the default. Either policy can win depending on the input.\n"
            "-0 .. -9 set the memory budget to what xz takes to compress at the same\n"
            "preset: 3, 9, 17, 32, 48, 94, 94, 186, 370 and 674 MiB, a preset below the\n"
            "smallest model of the profile raised to it; -9 is the default, since memory buys ratio\n"
            "and decode takes the model's memory as the header\n"
            "names it. For encode, --max-memory <bytes> (default %llu) bounds codec\n"
            "model/window/workspace allocations. Folder path and metadata bookkeeping\n"
            "is additional.\n"
            "Models shrink only to their supported minimum; lower budgets fail\n"
            "before output is published and report the required bytes. The minimum\n"
            "normal single-model budget is %llu bytes, the max profile's %llu;\n"
            "verification/segments may need more.\n"
            "Named files and folders preserve modes and access/modification timestamps.\n"
            "encode skips round-trip verification by default. --verify proves its output\n"
            "as it writes it when the budget holds two models, or, for a file written\n"
            "to a file, in a second pass with one. --no-verify explicitly skips this proof.\n"
            "decode allocates the model its input's header names, whatever the size\n"
            "(FMT-6: the container alone decodes); --max-memory <bytes> caps that and\n"
            "refuses a container needing more, and --max-output <bytes> (default 1 GiB)\n"
            "caps the bytes written.\n"
            "--threads <n> (default 1) with n of two or more cuts the input into segments\n"
            "of --segment-bytes (default %llu, whole blocks of %llu) that start the\n"
            "model afresh, the file the crate's threads write, and codes them at once on\n"
            "as many of the n threads as the budget holds a model, a segment and a block's\n"
            "workspace for; decode --threads <n> decodes a file's segments the same way.\n"
            "The report says how many threads ran.\n",
            ADAPT_FORMAT_VERSION, ADAPT_FAST_VERSION, ADAPT_MAX_VERSION,
            ADAPT_METADATA_VERSION, ADAPT_METADATA_FAST_VERSION, ADAPT_METADATA_MAX_VERSION,
            (unsigned long long)ADAPT_DEFAULT_MAX_MEMORY,
            (unsigned long long)minimum_error.detail, (unsigned long long)minimum_max_error.detail,
            (unsigned long long)ADAPT_SEGMENT_BYTES, (unsigned long long)ADAPT_BLOCK_BYTES);
}

/* Exit codes, as the Rust command line uses them. */
enum { EXIT_OTHER = 1, EXIT_USAGE = 2 };


static int usage_error(const char *fmt, const char *arg) {
    fputs("error: ", stderr);
    fprintf(stderr, fmt, arg);
    fputs("\n\n", stderr);
    print_usage(stderr);
    return EXIT_USAGE;
}

static int other_error(const char *path, const char *message) {
    fprintf(stderr, "error: %s: %s\n", path, message);
    return EXIT_OTHER;
}

/* ------------------------------------------------------------------------
 * Arguments: positionals, flags, and options that take a value.
 * --------------------------------------------------------------------- */

typedef struct args {
    const char *positional[4];
    int positionals;
    const char *flags[8];
    int nflags;
    const char *names[8];
    const char *values[8];
    int nvalues;
} args;

static int takes_value(const char *arg, const char *const *with_value) {
    for (; *with_value != NULL; with_value++) {
        if (strcmp(arg, *with_value) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Returns 0, or an exit code after printing a usage error. */
static int parse_args(args *a, int argc, char **argv, const char *const *with_value) {
    int i;
    memset(a, 0, sizeof(*a));
    for (i = 0; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "-") == 0 || arg[0] != '-') {
            if (a->positionals < 4) {
                a->positional[a->positionals] = arg;
            }
            a->positionals++;
        } else if (takes_value(arg, with_value)) {
            if (i + 1 >= argc) {
                return usage_error("%s needs a value", arg);
            }
            if (a->nvalues < 8) {
                a->names[a->nvalues] = arg;
                a->values[a->nvalues] = argv[i + 1];
                a->nvalues++;
            }
            i++;
        } else {
            if (a->nflags < 8) {
                a->flags[a->nflags] = arg;
            }
            a->nflags++;
        }
    }
    return 0;
}

static int has_flag(const args *a, const char *name) {
    int i;
    for (i = 0; i < a->nflags && i < 8; i++) {
        if (strcmp(a->flags[i], name) == 0) {
            return 1;
        }
    }
    return 0;
}

/* The last value given for `name`, or NULL. */
static const char *value_of(const args *a, const char *name) {
    int i;
    for (i = a->nvalues; i-- > 0;) {
        if (strcmp(a->names[i], name) == 0) {
            return a->values[i];
        }
    }
    return NULL;
}

/* The encode presets -0 to -9: xz's compression-side memory, in MiB, at the
 * same numbers, so that a user of xz reads them as before. */
static const uint64_t PRESET_MIB[10] = {3, 9, 17, 32, 48, 94, 94, 186, 370, 674};
static const char *const PRESET_FLAGS[10] = {"-0", "-1", "-2", "-3", "-4", "-5", "-6", "-7", "-8", "-9"};

/* The budget a preset names: the last preset given, or -9; a preset below
 * the smallest model of the profile in force is raised to it. 0 when no
 * preset flag is present. */
static uint64_t preset_budget(const args *a) {
    uint64_t budget = 0;
    int i, p;
    for (i = 0; i < a->nflags && i < 8; i++) {
        for (p = 0; p < 10; p++) {
            if (strcmp(a->flags[i], PRESET_FLAGS[p]) == 0) {
                budget = PRESET_MIB[p] << 20;
            }
        }
    }
    if (budget != 0) {
        adapt_sizes floor;
        adapt_error error;
        if (has_flag(a, "--max")) {
            (void)adapt_sizes_for_input_max(1, 0, 0, &floor, &error);
        } else {
            (void)adapt_sizes_for_input(1, 0, 0, &floor, &error);
        }
        if (error.status == ADAPT_ERROR_MEMORY_LIMIT && budget < error.detail) {
            budget = error.detail;
        }
    }
    return budget;
}

static int reject_unknown(const args *a, const char *const *known) {
    int i;
    for (i = 0; i < a->nflags && i < 8; i++) {
        if (!takes_value(a->flags[i], known)) {
            return usage_error("unknown option %s", a->flags[i]);
        }
    }
    return 0;
}

/* A byte count: decimal digits only, as Rust's `usize` parser accepts. */
static int parse_bytes(const char *text, uint64_t *out) {
    char *end;
    unsigned long long v;
    const char *p;
    if (*text == '\0') {
        return 0;
    }
    for (p = text; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return 0;
        }
    }
    errno = 0;
    v = strtoull(text, &end, 10);
    if (errno == ERANGE || *end != '\0') {
        return 0;
    }
    *out = v;
    return 1;
}

/* --threads <n>: the worker count the crate would use, 1 when absent; 0
 * when the value is not a count of one or more. */
static uint64_t thread_count(const args *a) {
    const char *text = value_of(a, "--threads");
    uint64_t n;
    if (text == NULL) {
        return 1;
    }
    if (!parse_bytes(text, &n) || n == 0) {
        return 0;
    }
    return n;
}

/* ------------------------------------------------------------------------
 * Streams: standard input or a file in, standard output or a staged file out.
 * --------------------------------------------------------------------- */

typedef struct stream {
    FILE *file;
    int error; /* errno of the last failure, for the message */
} stream;

static int read_stream(void *context, uint8_t *buf, size_t len, size_t *got) {
    stream *s = context;
    size_t n = fread(buf, 1, len, s->file);
    if (n < len && ferror(s->file)) {
        s->error = errno;
        return s->error != 0 ? s->error : EIO;
    }
    *got = n;
    return 0;
}

static int write_stream(void *context, const uint8_t *buf, size_t len) {
    stream *s = context;
    if (fwrite(buf, 1, len, s->file) != len) {
        s->error = errno;
        return s->error != 0 ? s->error : EIO;
    }
    return 0;
}

typedef struct metadata_writer {
    stream *out;
    const adapt_metadata *metadata;
    int header_written;
} metadata_writer;

/* The byte codec still produces its original stream; extend only its first
 * header write, leaving prediction, block lengths and payload CRC unchanged. */
static int write_metadata_stream(void *context, const uint8_t *buf, size_t len) {
    metadata_writer *w = context;
    if (!w->header_written && w->metadata != NULL) {
        uint8_t header[ADAPT_HEADER_MAX_BYTES];
        size_t n = adapt_header_add_metadata(buf, len, w->metadata, header, sizeof(header));
        if (n == 0) {
            w->out->error = EINVAL;
            return EINVAL;
        }
        w->header_written = 1;
        return write_stream(w->out, header, n);
    }
    w->header_written = 1;
    return write_stream(w->out, buf, len);
}

static void binary_stdio(void) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

/* The staged output: a file beside the target, published by name only
 * when the work is complete, removed otherwise. */
typedef struct staged {
    char *path;
    FILE *file;
} staged;

static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f != NULL) {
        fclose(f);
        return 1;
    }
    return errno != ENOENT;
}

static void staged_discard(staged *s) {
    if (s->file != NULL) {
        fclose(s->file);
        s->file = NULL;
    }
    if (s->path != NULL) {
        remove(s->path);
        free(s->path);
        s->path = NULL;
    }
}

/* Open a staging file in the target's directory. Returns an exit code, or
 * 0 with `s` ready to write. */
static int staged_open(staged *s, const char *target, int force) {
    size_t dir_len, i;
    const char *slash;
    int attempt;
    s->path = NULL;
    s->file = NULL;
    if (!force && file_exists(target)) {
        fprintf(stderr, "error: %s exists; pass --force to overwrite\n", target);
        return EXIT_OTHER;
    }
    slash = strrchr(target, '/');
#ifdef _WIN32
    {
        const char *back = strrchr(target, '\\');
        if (back != NULL && (slash == NULL || back > slash)) {
            slash = back;
        }
    }
#endif
    dir_len = slash == NULL ? 0 : (size_t)(slash - target) + 1;
    for (attempt = 0; attempt < 128; attempt++) {
        char name[64];
        int n = snprintf(name, sizeof(name), ".cadapt-%ld-%d.part", (long)cadapt_getpid(), attempt);
        char *path = malloc(dir_len + (size_t)n + 1);
        int fd;
        if (path == NULL || n < 0) {
            free(path);
            return other_error(target, "memory allocation failed");
        }
        for (i = 0; i < dir_len; i++) {
            path[i] = target[i];
        }
        memcpy(path + dir_len, name, (size_t)n + 1);
        /* Exclusive creation rejects every existing entry, including a
         * dangling symlink. Only a file we created may be written or
         * discarded; probing first cannot establish that ownership. The
         * mode is a new file's, 0666 less the umask, since the staged
         * file becomes the target by name and keeps the mode it was
         * created with. */
#ifdef _WIN32
        fd = _open(path, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
#endif
        if (fd < 0) {
            int e = errno;
            free(path);
            if (e == EEXIST) {
                continue;
            }
            return other_error(target, strerror(e));
        }
#ifdef _WIN32
        s->file = _fdopen(fd, "wb");
#else
        s->file = fdopen(fd, "wb");
#endif
        if (s->file == NULL) {
            int e = errno;
#ifdef _WIN32
            _close(fd);
#else
            close(fd);
#endif
            remove(path);
            free(path);
            return other_error(target, strerror(e));
        }
        s->path = path;
        return 0;
    }
    return other_error(target, "cannot create a unique staging file");
}

/* Publish the staged file as `target`. Returns an exit code or 0. */
static int staged_publish(staged *s, const char *target, int force, const adapt_metadata *metadata) {
    int rc;
    if (s->file != NULL && fclose(s->file) != 0) {
        int e = errno;
        s->file = NULL;
        staged_discard(s);
        return other_error(target, strerror(e));
    }
    s->file = NULL;
    if (metadata != NULL && (rc = metadata_apply(s->path, 0, metadata)) != 0) {
        char message[160];
        snprintf(message, sizeof(message), "cannot restore metadata: %s", strerror(rc));
        staged_discard(s);
        return other_error(target, message);
    }
#ifndef _WIN32
    if (!force) {
        /* Linking a complete file is atomic and refuses every existing
         * directory entry, including one created after the initial check. */
        rc = link(s->path, target);
        if (rc != 0) {
            int e = errno;
            staged_discard(s);
            return other_error(target, e == EEXIST ? "exists; pass --force to overwrite" : strerror(e));
        }
        staged_discard(s);
        return 0;
    }
#else
    if (!force && file_exists(target)) {
        staged_discard(s);
        fprintf(stderr, "error: %s exists; pass --force to overwrite\n", target);
        return EXIT_OTHER;
    }
    remove(target);
#endif
    rc = rename(s->path, target);
    if (rc != 0) {
        int e = errno;
        staged_discard(s);
        return other_error(target, strerror(e));
    }
    free(s->path);
    s->path = NULL;
    return 0;
}

/* ------------------------------------------------------------------------
 * Folders: a directory walked into an archive stream on the way in, an
 * archive stream restored into a staged directory on the way out.
 * --------------------------------------------------------------------- */

/* `a/b` from `a` and `b`; `b` alone when `a` is empty. Malloc'd. */
static char *join(const char *a, const char *b) {
    size_t na = strlen(a), nb = strlen(b);
    char *p = malloc(na + nb + 2);
    if (p == NULL) {
        return NULL;
    }
    if (na == 0) {
        memcpy(p, b, nb + 1);
        return p;
    }
    memcpy(p, a, na);
    p[na] = '/';
    memcpy(p + na + 1, b, nb + 1);
    return p;
}

/* What a directory entry is: the kinds an archive holds, or none. */
enum {
    KIND_NONE = 0,
    KIND_DIRECTORY = ADAPT_ARCHIVE_DIRECTORY,
    KIND_FILE = ADAPT_ARCHIVE_FILE,
    KIND_SYMLINK = ADAPT_ARCHIVE_SYMLINK,
    KIND_HARDLINK = ADAPT_ARCHIVE_HARDLINK,
    KIND_SPARSE = ADAPT_ARCHIVE_SPARSE
};

/* A file's identity on its filesystem, for the hard links among a
 * folder's files: two names with the same identity share one inode. */
typedef struct identity {
    uint64_t device, inode;
    int shared; /* the inode has more than one name */
} identity;

/* The kind of the entry at `path`, its size when a file, and its target
 * when a link (malloc'd); KIND_NONE for anything else, or -1 with errno
 * when it cannot be examined. A link is examined, not followed. */
static int examine(const char *path, uint64_t *size, char **target, identity *id) {
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(path, &st) != 0) {
        return -1;
    }
    id->device = (uint64_t)st.st_dev;
    id->inode = (uint64_t)st.st_ino;
    id->shared = 0;
    if (st.st_mode & _S_IFDIR) {
        return KIND_DIRECTORY;
    }
    if (st.st_mode & _S_IFREG) {
        *size = (uint64_t)st.st_size;
        return KIND_FILE;
    }
    return KIND_NONE;
#else
    struct stat st;
    if (lstat(path, &st) != 0) {
        return -1;
    }
    id->device = (uint64_t)st.st_dev;
    id->inode = (uint64_t)st.st_ino;
    id->shared = st.st_nlink > 1;
    if (S_ISDIR(st.st_mode)) {
        return KIND_DIRECTORY;
    }
    if (S_ISREG(st.st_mode)) {
        *size = (uint64_t)st.st_size;
        return KIND_FILE;
    }
    if (S_ISLNK(st.st_mode)) {
        size_t cap = 256;
        for (;;) {
            char *buf = malloc(cap);
            ssize_t n;
            if (buf == NULL) {
                errno = ENOMEM;
                return -1;
            }
            n = readlink(path, buf, cap);
            if (n < 0) {
                int e = errno;
                free(buf);
                errno = e;
                return -1;
            }
            if ((size_t)n < cap) {
                buf[n] = '\0';
                *target = buf;
                return KIND_SYMLINK;
            }
            free(buf);
            cap *= 2;
        }
    }
    return KIND_NONE;
#endif
}

static int is_directory(const char *path) {
    uint64_t size;
    char *target = NULL;
    identity id;
    int kind = examine(path, &size, &target, &id);
    free(target);
    return kind == KIND_DIRECTORY;
}

/* The names in a directory, sorted by their bytes, each malloc'd; or -1
 * with errno. `.` and `..` are left out. */
static int compare_names(const void *a, const void *b) {
    const unsigned char *x = *(const unsigned char *const *)a, *y = *(const unsigned char *const *)b;
    size_t i;
    for (i = 0;; i++) {
        if (x[i] != y[i]) {
            return x[i] < y[i] ? -1 : 1;
        }
        if (x[i] == 0) {
            return 0;
        }
    }
}

static int list_directory(const char *path, char ***names, size_t *count) {
    char **list = NULL;
    size_t n = 0, cap = 0;
#ifdef _WIN32
    struct _finddata_t entry;
    intptr_t handle;
    char *pattern = join(path, "*");
    if (pattern == NULL) {
        errno = ENOMEM;
        return -1;
    }
    handle = _findfirst(pattern, &entry);
    free(pattern);
    if (handle == -1) {
        if (errno == ENOENT) {
            *names = NULL;
            *count = 0;
            return 0;
        }
        return -1;
    }
    do {
        const char *name = entry.name;
#else
    DIR *dir = opendir(path);
    struct dirent *entry;
    if (dir == NULL) {
        return -1;
    }
    errno = 0;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
#endif
        char *copy;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }
        if (n == cap) {
            size_t grown = cap == 0 ? 16 : cap * 2;
            char **bigger = realloc(list, grown * sizeof(*list));
            if (bigger == NULL) {
                break;
            }
            list = bigger;
            cap = grown;
        }
        copy = malloc(strlen(name) + 1);
        if (copy == NULL) {
            break;
        }
        strcpy(copy, name);
        list[n++] = copy;
#ifdef _WIN32
    } while (_findnext(handle, &entry) == 0);
    _findclose(handle);
#else
    }
    if (errno != 0) {
        int e = errno;
        size_t i;
        closedir(dir);
        for (i = 0; i < n; i++) {
            free(list[i]);
        }
        free(list);
        errno = e;
        return -1;
    }
    closedir(dir);
#endif
    if (n > 0) {
        qsort(list, n, sizeof(*list), compare_names);
    }
    *names = list;
    *count = n;
    return 0;
}

/* One entry of the folder being coded. */
typedef struct entry {
    char *path; /* the archive path */
    char *target; /* a link's target, or the path a hard link shares */
    uint64_t size;
    uint8_t kind;
    adapt_metadata metadata;
    /* a sparse file's data extents */
    adapt_extent *extents;
    uint32_t extent_count;
    attrs attrs;
} entry;

/* An inode seen with more than one name: the entry that carries it. */
typedef struct seen_inode {
    identity id;
    size_t index;
} seen_inode;

typedef struct folder {
    const char *root;
    entry *items;
    size_t count, cap;
    seen_inode *inodes;
    size_t inode_count, inode_cap;
    /* the bytes the archive stream takes, for the sizes and the report */
    uint64_t stream_bytes;
} folder;

static void folder_free(folder *f) {
    size_t i;
    for (i = 0; i < f->count; i++) {
        free(f->items[i].path);
        free(f->items[i].target);
        free(f->items[i].extents);
        attrs_free(&f->items[i].attrs);
    }
    free(f->items);
    free(f->inodes);
    f->items = NULL;
    f->inodes = NULL;
    f->count = f->cap = f->inode_count = f->inode_cap = 0;
}

/* The entry an inode already has, or -1. */
static long folder_inode(const folder *f, const identity *id) {
    size_t i;
    for (i = 0; i < f->inode_count; i++) {
        if (f->inodes[i].id.device == id->device && f->inodes[i].id.inode == id->inode) {
            return (long)f->inodes[i].index;
        }
    }
    return -1;
}

static int folder_remember_inode(folder *f, const identity *id, size_t index) {
    if (f->inode_count == f->inode_cap) {
        size_t grown = f->inode_cap == 0 ? 16 : f->inode_cap * 2;
        seen_inode *bigger = realloc(f->inodes, grown * sizeof(*bigger));
        if (bigger == NULL) {
            return ENOMEM;
        }
        f->inodes = bigger;
        f->inode_cap = grown;
    }
    f->inodes[f->inode_count].id = *id;
    f->inodes[f->inode_count].index = index;
    f->inode_count++;
    return 0;
}

/* Add an entry; takes the strings, the extents and the attributes.
 * Returns 0 or an errno. */
static int folder_add(folder *f, uint8_t kind, char *path, uint64_t size, char *target, adapt_extent *extents,
                      uint32_t extent_count, attrs *a, const adapt_metadata *metadata) {
    entry *e;
    size_t n;
    uint32_t i;
    if (f->count == f->cap) {
        size_t grown = f->cap == 0 ? 64 : f->cap * 2;
        entry *bigger = realloc(f->items, grown * sizeof(*bigger));
        if (bigger == NULL) {
            free(path);
            free(target);
            free(extents);
            attrs_free(a);
            return ENOMEM;
        }
        f->items = bigger;
        f->cap = grown;
    }
    n = adapt_archive_entry_full(kind, path, size, target, extents, extent_count, metadata, NULL, 0);
    if (n == 0) {
        free(path);
        free(target);
        free(extents);
        attrs_free(a);
        return ENAMETOOLONG;
    }
    e = &f->items[f->count++];
    e->kind = kind;
    e->path = path;
    e->size = size;
    e->target = target;
    e->metadata = *metadata;
    e->extents = extents;
    e->extent_count = extent_count;
    e->attrs = *a;
    memset(a, 0, sizeof(*a));
    f->stream_bytes += n;
    if (kind == KIND_FILE) {
        f->stream_bytes += size;
    } else if (kind == KIND_SPARSE) {
        for (i = 0; i < extent_count; i++) {
            f->stream_bytes += 16 + extents[i].length;
        }
    }
    if (kind != KIND_HARDLINK) {
        f->stream_bytes += attrs_bytes(&e->attrs);
    }
    return 0;
}

/* Walk the directory at root/rel, depth first, siblings in byte order,
 * each directory before what it holds. Returns 0, or an errno with
 * *culprit the path that failed (malloc'd) — an entry of a kind the
 * archive cannot hold is ENOTSUP. */
static int walk(folder *f, const char *rel, char **culprit) {
    char *dir = join(f->root, rel);
    char **names = NULL;
    size_t count = 0, i;
    int rc = 0;
    if (dir == NULL) {
        return ENOMEM;
    }
    if (list_directory(dir, &names, &count) != 0) {
        rc = errno;
        *culprit = dir;
        return rc;
    }
    free(dir);
    for (i = 0; i < count && rc == 0; i++) {
        char *path = join(rel, names[i]);
        char *full = path == NULL ? NULL : join(f->root, path);
        char *target = NULL;
        uint64_t size = 0;
        adapt_metadata metadata;
        adapt_extent *extents = NULL;
        uint32_t extent_count = UINT32_MAX;
        attrs a;
        identity id;
        int kind;
        memset(&a, 0, sizeof(a));
        if (path == NULL || full == NULL) {
            free(path);
            free(full);
            rc = ENOMEM;
            break;
        }
        rc = metadata_read_path(full, 1, &metadata);
        if (rc != 0) {
            *culprit = full;
            free(path);
            break;
        }
        kind = examine(full, &size, &target, &id);
        if (kind < 0) {
            rc = errno;
            *culprit = full;
            free(path);
            break;
        }
        if (kind == KIND_NONE) {
            rc = ENOTSUP;
            *culprit = full;
            free(path);
            break;
        }
        if (kind == KIND_FILE && id.shared) {
            /* a name an earlier entry's inode already has is a hard link
             * to that entry; the first name carries the bytes */
            long earlier = folder_inode(f, &id);
            if (earlier >= 0) {
                char *shared = malloc(strlen(f->items[earlier].path) + 1);
                if (shared == NULL) {
                    free(full);
                    free(path);
                    rc = ENOMEM;
                    break;
                }
                strcpy(shared, f->items[earlier].path);
                free(full);
                rc = folder_add(f, KIND_HARDLINK, path, 0, shared, NULL, 0, &a, &metadata);
                continue;
            }
            rc = folder_remember_inode(f, &id, f->count);
            if (rc != 0) {
                free(full);
                free(path);
                break;
            }
        }
        if (kind == KIND_FILE && size > 0) {
            rc = sparse_map(full, size, &extents, &extent_count);
            if (rc != 0) {
                *culprit = full;
                free(path);
                free(target);
                break;
            }
            if (extent_count != UINT32_MAX) {
                kind = KIND_SPARSE;
            }
        }
        rc = attrs_read(full, &a);
        if (rc != 0) {
            *culprit = full;
            free(path);
            free(target);
            free(extents);
            break;
        }
        free(full);
        if (kind == KIND_DIRECTORY) {
            char *again = malloc(strlen(path) + 1);
            if (again == NULL) {
                free(path);
                attrs_free(&a);
                rc = ENOMEM;
                break;
            }
            strcpy(again, path);
            rc = folder_add(f, KIND_DIRECTORY, path, 0, NULL, NULL, 0, &a, &metadata);
            if (rc == 0) {
                rc = walk(f, again, culprit);
            }
            free(again);
        } else {
            rc = folder_add(f, (uint8_t)kind, path, size, target, extents, kind == KIND_SPARSE ? extent_count : 0, &a,
                            &metadata);
        }
    }
    for (i = 0; i < count; i++) {
        free(names[i]);
    }
    free(names);
    return rc;
}

/* Collect a folder's entries and the length of its stream. */
static int folder_collect(folder *f, const char *root, char **culprit) {
    memset(f, 0, sizeof(*f));
    f->root = root;
    f->stream_bytes = ADAPT_ARCHIVE_MAGIC_BYTES + 1;
    return walk(f, "", culprit);
}

/* The reader that serialises a folder: the magic, then each entry's
 * header and its file's bytes, then the end. A file that no longer holds
 * the bytes it was listed with fails the read: the archive would lie. */
typedef struct packer {
    folder *f;
    size_t index; /* the next entry */
    /* the header, an extent's offset and length and the end go out
     * through `head`, the attribute trailer through `trailer`; a file's
     * or an extent's bytes are read straight from the file */
    enum { PACK_MAGIC, PACK_HEADER, PACK_EXTENT, PACK_DATA, PACK_ATTRS, PACK_END, PACK_DONE } stage;
    uint8_t head[ADAPT_ARCHIVE_MAGIC_BYTES + 1 + 2 + ADAPT_ARCHIVE_PATH_MAX + 2 + ADAPT_ARCHIVE_PATH_MAX + 12 +
                 ADAPT_METADATA_BYTES];
    size_t head_len, head_at;
    uint8_t *trailer; /* the entry's attribute trailer, malloc'd */
    size_t trailer_len, trailer_at;
    FILE *file;
    uint64_t remaining;
    uint32_t extent; /* the next extent of a sparse entry */
    int error; /* errno of the failure */
    const char *failed; /* the entry's path, or NULL */
    int changed; /* the failure: the file's bytes differed from its listing */
} packer;

static void packer_reset(packer *p, folder *f) {
    if (p->file != NULL) {
        fclose(p->file);
    }
    free(p->trailer);
    memset(p, 0, sizeof(*p));
    p->f = f;
    p->stage = PACK_MAGIC;
    memcpy(p->head, ADAPT_ARCHIVE_FULL_MAGIC, ADAPT_ARCHIVE_MAGIC_BYTES);
    p->head_len = ADAPT_ARCHIVE_MAGIC_BYTES;
}

static void packer_close(packer *p) {
    if (p->file != NULL) {
        fclose(p->file);
        p->file = NULL;
    }
    free(p->trailer);
    p->trailer = NULL;
}

/* Move to the next entry's header, or the end. */
static void packer_next(packer *p) {
    if (p->index == p->f->count) {
        p->head[0] = ADAPT_ARCHIVE_END;
        p->head_len = 1;
        p->head_at = 0;
        p->stage = PACK_END;
        return;
    }
    {
        const entry *e = &p->f->items[p->index];
        p->head_len = adapt_archive_entry_full(e->kind, e->path, e->size, e->target, e->extents, e->extent_count,
                                               &e->metadata, p->head, sizeof(p->head));
        p->head_at = 0;
        p->extent = 0;
        p->stage = PACK_HEADER;
    }
}

/* The entry's attribute trailer, built once and sent from `trailer`.
 * Returns 0 or an errno. */
static int packer_attrs(packer *p) {
    const entry *e = &p->f->items[p->index];
    size_t n = (size_t)attrs_bytes(&e->attrs), at;
    uint32_t i;
    uint8_t *t = malloc(n);
    if (t == NULL) {
        p->error = ENOMEM;
        return ENOMEM;
    }
    at = adapt_archive_attributes(e->attrs.count, t, n);
    for (i = 0; i < e->attrs.count; i++) {
        at += adapt_archive_attribute(e->attrs.names[i], e->attrs.values[i], e->attrs.lens[i], t + at, n - at);
    }
    free(p->trailer);
    p->trailer = t;
    p->trailer_len = n;
    p->trailer_at = 0;
    p->stage = PACK_ATTRS;
    return 0;
}

/* Open the entry's file for its bytes. Returns 0 or an errno. */
static int packer_open(packer *p) {
    const entry *e = &p->f->items[p->index];
    char *full = join(p->f->root, e->path);
    if (full == NULL) {
        p->error = ENOMEM;
        return ENOMEM;
    }
    p->file = fopen(full, "rb");
    free(full);
    if (p->file == NULL) {
        p->error = errno;
        p->failed = e->path;
        return p->error != 0 ? p->error : EIO;
    }
    return 0;
}

/* The next extent of a sparse entry: its offset and length through
 * `head`, the file positioned at its bytes. Returns 0 or an errno. */
static int packer_extent(packer *p) {
    const entry *e = &p->f->items[p->index];
    const adapt_extent *x = &e->extents[p->extent];
    p->head_len = adapt_archive_extent(x, p->head, sizeof(p->head));
    p->head_at = 0;
    if (cadapt_fseek(p->file, (cadapt_off)x->offset, SEEK_SET) != 0) {
        p->error = errno;
        p->failed = e->path;
        return p->error != 0 ? p->error : EIO;
    }
    p->remaining = x->length;
    p->stage = PACK_EXTENT;
    return 0;
}

static int read_packer(void *context, uint8_t *buf, size_t len, size_t *got) {
    packer *p = context;
    size_t at = 0;
    while (at < len) {
        if (p->stage == PACK_DONE) {
            break;
        }
        if (p->stage == PACK_ATTRS) {
            size_t n = p->trailer_len - p->trailer_at;
            if (n > len - at) {
                n = len - at;
            }
            memcpy(buf + at, p->trailer + p->trailer_at, n);
            at += n;
            p->trailer_at += n;
            if (p->trailer_at < p->trailer_len) {
                break;
            }
            packer_close(p);
            p->index++;
            packer_next(p);
            continue;
        }
        if (p->stage == PACK_MAGIC || p->stage == PACK_HEADER || p->stage == PACK_EXTENT || p->stage == PACK_END) {
            size_t n = p->head_len - p->head_at;
            const entry *e = p->index < p->f->count ? &p->f->items[p->index] : NULL;
            int rc = 0;
            if (n > len - at) {
                n = len - at;
            }
            memcpy(buf + at, p->head + p->head_at, n);
            at += n;
            p->head_at += n;
            if (p->head_at < p->head_len) {
                break;
            }
            if (p->stage == PACK_END) {
                p->stage = PACK_DONE;
            } else if (p->stage == PACK_MAGIC) {
                packer_next(p);
            } else if (p->stage == PACK_EXTENT) {
                p->stage = PACK_DATA;
            } else if (e->kind == KIND_FILE) {
                rc = packer_open(p);
                p->remaining = e->size;
                p->stage = PACK_DATA;
            } else if (e->kind == KIND_SPARSE) {
                rc = packer_open(p);
                if (rc == 0) {
                    rc = e->extent_count == 0 ? packer_attrs(p) : packer_extent(p);
                }
            } else if (e->kind == KIND_HARDLINK) {
                p->index++;
                packer_next(p);
            } else {
                rc = packer_attrs(p);
            }
            if (rc != 0) {
                return rc;
            }
            continue;
        }
        /* PACK_DATA: the file's or the extent's bytes, exactly as many as
         * were listed */
        {
            const entry *e = &p->f->items[p->index];
            size_t n = len - at;
            size_t r;
            int rc;
            if ((uint64_t)n > p->remaining) {
                n = (size_t)p->remaining;
            }
            r = n == 0 ? 0 : fread(buf + at, 1, n, p->file);
            if (r < n) {
                p->error = ferror(p->file) ? errno : 0;
                p->failed = e->path;
                p->changed = !ferror(p->file);
                return p->error != 0 ? p->error : EIO;
            }
            at += r;
            p->remaining -= r;
            if (p->remaining == 0) {
                if (e->kind == KIND_SPARSE && ++p->extent < e->extent_count) {
                    rc = packer_extent(p);
                    if (rc != 0) {
                        return rc;
                    }
                    continue;
                }
                /* the file must end where its listing said */
                if (e->kind == KIND_SPARSE) {
                    if (cadapt_fseek(p->file, 0, SEEK_END) != 0 || (uint64_t)cadapt_ftell(p->file) != e->size) {
                        p->failed = e->path;
                        p->changed = 1;
                        return EIO;
                    }
                } else if (fgetc(p->file) != EOF) {
                    p->failed = e->path;
                    p->changed = 1;
                    return EIO;
                }
                rc = packer_attrs(p);
                if (rc != 0) {
                    return rc;
                }
            }
        }
    }
    *got = at;
    return 0;
}

/* The message for a folder's read failure. */
static int packer_failure(const char *input, const packer *p) {
    char message[400];
    if (p->failed != NULL && p->changed) {
        snprintf(message, sizeof(message), "%s changed while it was being coded", p->failed);
    } else if (p->failed != NULL) {
        snprintf(message, sizeof(message), "%s: %s", p->failed, strerror(p->error != 0 ? p->error : EIO));
    } else {
        snprintf(message, sizeof(message), "%s", strerror(p->error != 0 ? p->error : EIO));
    }
    return other_error(input, message);
}

/* The sink that restores an archive under a staging directory. Every
 * path has passed the unpacker's rules, so it stays under the root. */
typedef struct deferred_metadata {
    char *path;
    adapt_metadata metadata;
    /* the directory's attributes, applied with its metadata once its
     * children exist: a default ACL set earlier would be inherited by
     * children the archive records differently */
    attrs attrs;
    struct deferred_metadata *next;
} deferred_metadata;

typedef struct restorer {
    const char *root;
    FILE *file;
    int error; /* errno of the failure */
    char *failed; /* the entry's path, malloc'd, or NULL */
    const char *why; /* a failure the errno does not name, or NULL */
    deferred_metadata *directories;
    /* the directory whose attributes are arriving, deferred with it */
    deferred_metadata *pending;
} restorer;

static int restore_fail(restorer *r, const char *path, int e, const char *why) {
    r->error = e;
    r->why = why;
    free(r->failed);
    r->failed = malloc(strlen(path) + 1);
    if (r->failed != NULL) {
        strcpy(r->failed, path);
    }
    return e != 0 ? e : EIO;
}

static int restore_directory(void *context, const char *path) {
    restorer *r = context;
    char *full = join(r->root, path);
    int rc;
    if (full == NULL) {
        return restore_fail(r, path, ENOMEM, NULL);
    }
#ifdef _WIN32
    rc = _mkdir(full);
#else
    rc = mkdir(full, 0777);
#endif
    free(full);
    if (rc != 0) {
        return restore_fail(r, path, errno, errno == EEXIST ? "named twice in the archive" : NULL);
    }
    return 0;
}

static int restore_file_open(void *context, const char *path, uint64_t size) {
    restorer *r = context;
    char *full = join(r->root, path);
    int fd;
    (void)size;
    if (full == NULL) {
        return restore_fail(r, path, ENOMEM, NULL);
    }
    /* exclusive: an archive that names a file twice is refused */
#ifdef _WIN32
    fd = _open(full, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
    fd = open(full, O_WRONLY | O_CREAT | O_EXCL, 0666);
#endif
    free(full);
    if (fd < 0) {
        return restore_fail(r, path, errno, errno == EEXIST ? "named twice in the archive" : NULL);
    }
#ifdef _WIN32
    r->file = _fdopen(fd, "wb");
#else
    r->file = fdopen(fd, "wb");
#endif
    if (r->file == NULL) {
        int e = errno;
#ifdef _WIN32
        _close(fd);
#else
        close(fd);
#endif
        return restore_fail(r, path, e, NULL);
    }
    free(r->failed);
    r->failed = malloc(strlen(path) + 1);
    if (r->failed != NULL) {
        strcpy(r->failed, path);
    }
    return 0;
}

static int restore_file_data(void *context, const uint8_t *data, size_t len) {
    restorer *r = context;
    if (fwrite(data, 1, len, r->file) != len) {
        r->error = errno;
        return r->error != 0 ? r->error : EIO;
    }
    return 0;
}

static int restore_file_close(void *context) {
    restorer *r = context;
    FILE *f = r->file;
    r->file = NULL;
    if (fclose(f) != 0) {
        r->error = errno;
        return r->error != 0 ? r->error : EIO;
    }
    free(r->failed);
    r->failed = NULL;
    return 0;
}

static int restore_symlink(void *context, const char *path, const char *target) {
    restorer *r = context;
#ifdef _WIN32
    (void)target;
    return restore_fail(r, path, ENOTSUP, "a symbolic link is not restored on this platform");
#else
    char *full = join(r->root, path);
    int rc;
    if (full == NULL) {
        return restore_fail(r, path, ENOMEM, NULL);
    }
    rc = symlink(target, full);
    free(full);
    if (rc != 0) {
        return restore_fail(r, path, errno, errno == EEXIST ? "named twice in the archive" : NULL);
    }
    return 0;
#endif
}

/* A hard link: the same inode under another name. Where the platform has
 * no link(), the target's bytes are copied instead, which the README
 * states. */
static int restore_hardlink(void *context, const char *path, const char *target) {
    restorer *r = context;
    char *full = join(r->root, path);
    char *to = join(r->root, target);
    int rc;
    if (full == NULL || to == NULL) {
        free(full);
        free(to);
        return restore_fail(r, path, ENOMEM, NULL);
    }
#ifdef _WIN32
    {
        FILE *in = fopen(to, "rb"), *out = NULL;
        uint8_t buf[65536];
        size_t n;
        rc = 0;
        if (in == NULL) {
            rc = errno;
        } else {
            int fd = _open(full, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
            out = fd < 0 ? NULL : _fdopen(fd, "wb");
            if (out == NULL) {
                rc = errno;
            }
        }
        while (rc == 0 && (n = fread(buf, 1, sizeof(buf), in)) > 0) {
            if (fwrite(buf, 1, n, out) != n) {
                rc = errno != 0 ? errno : EIO;
            }
        }
        if (in != NULL && ferror(in)) {
            rc = EIO;
        }
        if (in != NULL) {
            fclose(in);
        }
        if (out != NULL && fclose(out) != 0 && rc == 0) {
            rc = errno;
        }
    }
#else
    rc = link(to, full) == 0 ? 0 : errno;
#endif
    free(full);
    free(to);
    if (rc != 0) {
        return restore_fail(r, path, rc, rc == EEXIST ? "named twice in the archive" : "cannot restore hard link");
    }
    return 0;
}

/* A sparse file: created empty at its apparent length, each extent
 * written at its offset. Where the filesystem keeps no holes, the bytes
 * between extents are zeros all the same. */
static int restore_sparse_open(void *context, const char *path, uint64_t size, uint32_t extents) {
    restorer *r = context;
    int rc = restore_file_open(context, path, size);
    (void)extents;
    if (rc != 0) {
        return rc;
    }
#ifdef _WIN32
    rc = _chsize_s(_fileno(r->file), (long long)size) == 0 ? 0 : errno;
#else
    rc = ftruncate(fileno(r->file), (off_t)size) == 0 ? 0 : errno;
#endif
    if (rc != 0) {
        return restore_fail(r, path, rc, "cannot restore sparse file");
    }
    return 0;
}

static int restore_extent(void *context, uint64_t offset, uint64_t length) {
    restorer *r = context;
    (void)length;
    if (cadapt_fseek(r->file, (cadapt_off)offset, SEEK_SET) != 0) {
        r->error = errno;
        return r->error != 0 ? r->error : EIO;
    }
    return 0;
}

/* One attribute of the entry just restored: set now, or, for a
 * directory, with its metadata once its children exist. */
static int restore_attribute(void *context, const char *path, uint8_t kind, const char *name, const uint8_t *value,
                             size_t len) {
    restorer *r = context;
    int rc;
    if (kind == ADAPT_ARCHIVE_DIRECTORY) {
        deferred_metadata *d = r->pending;
        attrs *a;
        uint32_t n;
        if (d == NULL) {
            char *full = join(r->root, path);
            d = full == NULL ? NULL : calloc(1, sizeof(*d));
            if (d == NULL) {
                free(full);
                return restore_fail(r, path, ENOMEM, NULL);
            }
            d->path = full;
            r->pending = d;
        }
        a = &d->attrs;
        n = a->count;
        {
            char **names = realloc(a->names, (n + 1) * sizeof(*names));
            uint8_t **values = names == NULL ? NULL : realloc(a->values, (n + 1) * sizeof(*values));
            size_t *lens = values == NULL ? NULL : realloc(a->lens, (n + 1) * sizeof(*lens));
            if (names != NULL) {
                a->names = names;
            }
            if (values != NULL) {
                a->values = values;
            }
            if (lens != NULL) {
                a->lens = lens;
            }
            if (names == NULL || values == NULL || lens == NULL) {
                return restore_fail(r, path, ENOMEM, NULL);
            }
        }
        a->names[n] = malloc(strlen(name) + 1);
        a->values[n] = malloc(len + 1);
        if (a->names[n] == NULL || a->values[n] == NULL) {
            free(a->names[n]);
            free(a->values[n]);
            return restore_fail(r, path, ENOMEM, NULL);
        }
        strcpy(a->names[n], name);
        memcpy(a->values[n], value, len);
        a->lens[n] = len;
        a->count = n + 1;
        return 0;
    }
    {
        char *full = join(r->root, path);
        if (full == NULL) {
            return restore_fail(r, path, ENOMEM, NULL);
        }
        rc = attr_apply(full, name, value, len);
        free(full);
    }
    return rc != 0 ? restore_fail(r, path, rc, "cannot restore extended attribute") : 0;
}

static int restore_metadata(void *context, const char *path, uint8_t kind, const adapt_metadata *metadata) {
    restorer *r = context;
    char *full = join(r->root, path);
    int rc;
    if (full == NULL) {
        return restore_fail(r, path, ENOMEM, NULL);
    }
    if (kind == ADAPT_ARCHIVE_DIRECTORY) {
        deferred_metadata *item = r->pending;
        r->pending = NULL;
        if (item == NULL) {
            item = calloc(1, sizeof(*item));
            if (item == NULL) {
                free(full);
                return restore_fail(r, path, ENOMEM, NULL);
            }
            item->path = full;
        } else {
            free(full);
        }
        item->metadata = *metadata;
        item->next = r->directories;
        r->directories = item;
        return 0;
    }
    rc = metadata_apply(full, kind == ADAPT_ARCHIVE_SYMLINK, metadata);
    free(full);
    return rc != 0 ? restore_fail(r, path, rc, "cannot restore metadata") : 0;
}

/* Reverse traversal order finalizes children before parents. On failure
 * release the records without applying any remaining metadata. */
static int finish_directory_metadata(restorer *r, int apply) {
    int rc = 0;
    if (r->pending != NULL) {
        /* a directory whose attributes came but whose metadata did not:
         * the stream ended early, and the caller reports that */
        r->pending->next = r->directories;
        r->directories = r->pending;
        r->pending = NULL;
        apply = 0;
    }
    while (r->directories != NULL) {
        deferred_metadata *item = r->directories;
        uint32_t i;
        r->directories = item->next;
        if (apply && rc == 0) {
            int e = 0;
            for (i = 0; i < item->attrs.count && e == 0; i++) {
                e = attr_apply(item->path, item->attrs.names[i], item->attrs.values[i], item->attrs.lens[i]);
            }
            if (e != 0) {
                rc = restore_fail(r, item->path, e, "cannot restore extended attribute");
            } else {
                e = metadata_apply(item->path, 0, &item->metadata);
                if (e != 0) {
                    rc = restore_fail(r, item->path, e, "cannot restore directory metadata");
                }
            }
        }
        attrs_free(&item->attrs);
        free(item->path);
        free(item);
    }
    return rc;
}

/* Remove a directory we created and everything under it. Only the staging
 * directory is ever handed here. */
static void remove_tree(const char *path) {
    char **names = NULL;
    size_t count = 0, i;
#ifdef _WIN32
    (void)_chmod(path, _S_IREAD | _S_IWRITE);
#else
    /* Metadata may have made our staging directories read-only. */
    (void)chmod(path, 0700);
#endif
    if (list_directory(path, &names, &count) == 0) {
        for (i = 0; i < count; i++) {
            char *full = join(path, names[i]);
            if (full != NULL) {
                if (is_directory(full)) {
                    remove_tree(full);
                } else {
                    remove(full);
                }
                free(full);
            }
            free(names[i]);
        }
        free(names);
    }
#ifdef _WIN32
    _rmdir(path);
#else
    rmdir(path);
#endif
}

/* A staging directory beside the target, published by renaming. */
static int staged_dir_open(staged *s, const char *target, int force) {
    size_t dir_len, i;
    const char *slash;
    int attempt;
    s->path = NULL;
    s->file = NULL;
    if (!force && file_exists(target)) {
        fprintf(stderr, "error: %s exists; pass --force to overwrite\n", target);
        return EXIT_OTHER;
    }
    slash = strrchr(target, '/');
#ifdef _WIN32
    {
        const char *back = strrchr(target, '\\');
        if (back != NULL && (slash == NULL || back > slash)) {
            slash = back;
        }
    }
#endif
    dir_len = slash == NULL ? 0 : (size_t)(slash - target) + 1;
    for (attempt = 0; attempt < 128; attempt++) {
        char name[64];
        int n = snprintf(name, sizeof(name), ".cadapt-%ld-%d.part", (long)cadapt_getpid(), attempt);
        char *path = malloc(dir_len + (size_t)n + 1);
        int rc;
        if (path == NULL || n < 0) {
            free(path);
            return other_error(target, "memory allocation failed");
        }
        for (i = 0; i < dir_len; i++) {
            path[i] = target[i];
        }
        memcpy(path + dir_len, name, (size_t)n + 1);
#ifdef _WIN32
        rc = _mkdir(path);
#else
        rc = mkdir(path, 0777);
#endif
        if (rc != 0) {
            int e = errno;
            free(path);
            if (e == EEXIST) {
                continue;
            }
            return other_error(target, strerror(e));
        }
        s->path = path;
        return 0;
    }
    return other_error(target, "cannot create a unique staging directory");
}

static void staged_dir_discard(staged *s) {
    if (s->path != NULL) {
        remove_tree(s->path);
        free(s->path);
        s->path = NULL;
    }
}

/* Publish the staged directory as `target`: with --force an existing file
 * or empty directory is replaced; a directory with contents is not, so no
 * bytes of the user's are ever removed here. */
static int staged_dir_publish(staged *s, const char *target, int force) {
    if (force && file_exists(target)) {
        int rc = is_directory(target) ?
#ifdef _WIN32
                                      _rmdir(target)
#else
                                      rmdir(target)
#endif
                                      : remove(target);
        if (rc != 0) {
            int e = errno;
            staged_dir_discard(s);
            return other_error(target, e == ENOTEMPTY || e == EEXIST ? "exists and is not empty; remove it first"
                                                                      : strerror(e));
        }
    }
    if (rename(s->path, target) != 0) {
        int e = errno;
        staged_dir_discard(s);
        return other_error(target, e == EEXIST || e == ENOTEMPTY ? "exists; pass --force to overwrite" : strerror(e));
    }
    free(s->path);
    s->path = NULL;
    return 0;
}

/* A reader that hands back bytes already taken from a stream — the header
 * read ahead to learn what the container holds — before the rest. */
typedef struct replay {
    stream *in;
    uint8_t held[ADAPT_HEADER_MAX_BYTES];
    size_t held_len, held_at;
} replay;

static int read_replay(void *context, uint8_t *buf, size_t len, size_t *got) {
    replay *r = context;
    size_t n = r->held_len - r->held_at;
    if (n > 0) {
        if (n > len) {
            n = len;
        }
        memcpy(buf, r->held + r->held_at, n);
        r->held_at += n;
        *got = n;
        return 0;
    }
    return read_stream(r->in, buf, len, got);
}

/* The rest of the input after the replayed header, all of it, into one
 * buffer: the parallel decoder reads a file's segment table from its end.
 * Returns 0 with the buffer, or an errno. */
static int read_whole(replay *r, uint8_t **buf, size_t *len) {
    size_t cap = (size_t)1 << 20, at = 0, got;
    uint8_t *b = malloc(cap);
    if (b == NULL) {
        return ENOMEM;
    }
    for (;;) {
        int rc;
        if (at == cap) {
            uint8_t *grown;
            if (cap > ((size_t)-1) / 2) {
                free(b);
                return ENOMEM;
            }
            cap *= 2;
            grown = realloc(b, cap);
            if (grown == NULL) {
                free(b);
                return ENOMEM;
            }
            b = grown;
        }
        rc = read_replay(r, b + at, cap - at, &got);
        if (rc != 0) {
            free(b);
            return rc;
        }
        if (got == 0) {
            break;
        }
        at += got;
    }
    *buf = b;
    *len = at;
    return 0;
}

/* Decode through the stream decoder, or — for a segmented file on two or
 * more threads — the whole input read into memory and its segments
 * decoded at once; `stats->workers` says which. */
static adapt_status decode_input(replay *ahead, uint64_t threads, adapt_sizes sizes, adapt_limits limits,
                                 adapt_write_fn write, void *write_context, adapt_stats *stats, adapt_error *error) {
    uint8_t *whole;
    size_t whole_len;
    adapt_status status;
    int rc;
    if (threads < 2 || !sizes.segmented) {
        return adapt_decode_stream(read_replay, ahead, write, write_context, limits, stats, error);
    }
    rc = read_whole(ahead, &whole, &whole_len);
    if (rc != 0) {
        ahead->in->error = rc;
        memset(error, 0, sizeof(*error));
        error->status = ADAPT_ERROR_IO;
        return ADAPT_ERROR_IO;
    }
    status = adapt_decode_threads(whole, whole_len, limits, threads, write, write_context, stats, error);
    free(whole);
    return status;
}

/* ------------------------------------------------------------------------
 * decode
 * --------------------------------------------------------------------- */

static int codec_failure(const char *input, const adapt_error *error, const stream *in, const stream *out) {
    char message[160];
    if (error->status == ADAPT_ERROR_IO) {
        int e = in->error != 0 ? in->error : out->error;
        snprintf(message, sizeof(message), "i/o error: %s", e != 0 ? strerror(e) : "callback failed");
    } else {
        adapt_error_message(error, message, sizeof(message));
    }
    return other_error(input, message);
}

/* The header, read ahead of the decoder to learn whether the container
 * holds a file or a folder; `r` then replays it. Returns an exit code or
 * 0 with `sizes` filled. */
static int peek_header(const char *input, stream *in, replay *r, adapt_sizes *sizes) {
    adapt_error error;
    char message[160];
    size_t got = 0, need;
    r->in = in;
    r->held_at = 0;
    if (read_stream(in, r->held, ADAPT_HEADER_BYTES, &got) != 0) {
        return other_error(input, strerror(in->error != 0 ? in->error : EIO));
    }
    r->held_len = got;
    need = got >= ADAPT_HEADER_BYTES ? adapt_header_len(r->held) : ADAPT_HEADER_BYTES;
    if (got == ADAPT_HEADER_BYTES && need > got) {
        if (read_stream(in, r->held + got, need - got, &got) != 0) {
            return other_error(input, strerror(in->error != 0 ? in->error : EIO));
        }
        r->held_len += got;
    }
    if (adapt_read_header(r->held, r->held_len, sizes, &error) != ADAPT_OK) {
        adapt_error_message(&error, message, sizeof(message));
        return other_error(input, message);
    }
    return 0;
}

static int decode_command(int argc, char **argv) {
    static const char *const with_value[] = {"-o", "--max-output", "--max-memory", "--threads", NULL};
    static const char *const known[] = {"--force", NULL};
    args a;
    const char *input, *o, *text;
    char *output = NULL;
    int force, rc;
    adapt_limits limits = adapt_default_limits();
    stream in = {NULL, 0}, out = {NULL, 0};
    staged stage = {NULL, NULL};
    adapt_stats stats;
    /* FMT-6: the container alone decodes. Its header names the model, and
     * the model is allocated as named; --max-memory below is the one cap
     * that can refuse a valid container. The format bounds what a header
     * can name (the largest tables and window, about 1.2 GiB). */
    limits.max_memory = UINT64_MAX;
    adapt_error error;
    adapt_status status;
    uint64_t threads;
    adapt_sizes sizes;
    replay ahead;
    restorer restore = {NULL, NULL, 0, NULL, NULL, NULL, NULL};
    adapt_metadata metadata;
    int has_metadata;
    adapt_archive_unpacker *unpacker = NULL;
    rc = parse_args(&a, argc, argv, with_value);
    if (rc != 0) {
        return rc;
    }
    rc = reject_unknown(&a, known);
    if (rc != 0) {
        return rc;
    }
    if (a.positionals != 1) {
        return usage_error("decode takes exactly one input%s", "");
    }
    input = a.positional[0];
    threads = thread_count(&a);
    if (threads == 0) {
        return usage_error("--threads: not a count of one or more: %s", value_of(&a, "--threads"));
    }
    o = value_of(&a, "-o");
    if (o != NULL) {
        output = malloc(strlen(o) + 1);
        if (output == NULL) {
            return other_error(input, "memory allocation failed");
        }
        strcpy(output, o);
    } else if (strcmp(input, "-") == 0) {
        output = malloc(2);
        if (output == NULL) {
            return other_error(input, "memory allocation failed");
        }
        strcpy(output, "-");
    } else {
        size_t n = strlen(input);
        int strip = n > 5 && strcmp(input + n - 5, ".adpt") == 0;
        output = malloc(n + 5);
        if (output == NULL) {
            return other_error(input, "memory allocation failed");
        }
        if (strip) {
            memcpy(output, input, n - 5);
            output[n - 5] = '\0';
        } else {
            memcpy(output, input, n);
            memcpy(output + n, ".out", 5);
        }
    }
    text = value_of(&a, "--max-output");
    if (text != NULL && !parse_bytes(text, &limits.max_output)) {
        free(output);
        return usage_error("--max-output: not a byte count: %s", text);
    }
    text = value_of(&a, "--max-memory");
    if (text != NULL && !parse_bytes(text, &limits.max_memory)) {
        free(output);
        return usage_error("--max-memory: not a byte count: %s", text);
    }
    force = has_flag(&a, "--force");
    binary_stdio();
    if (strcmp(input, "-") == 0) {
        in.file = stdin;
    } else {
        in.file = fopen(input, "rb");
        if (in.file == NULL) {
            rc = other_error(input, strerror(errno));
            free(output);
            return rc;
        }
    }
    rc = peek_header(input, &in, &ahead, &sizes);
    if (rc != 0) {
        if (in.file != stdin) {
            fclose(in.file);
        }
        free(output);
        return rc;
    }
    has_metadata = adapt_header_has_metadata(ahead.held);
    if (has_metadata) {
        (void)adapt_read_metadata(ahead.held, ahead.held_len, &metadata, NULL);
    }
    if (sizes.archive) {
        /* a folder: restored under a staging directory beside the target */
        adapt_archive_sink sink;
        if (strcmp(output, "-") == 0) {
            rc = other_error(input, "holds a folder, which cannot be written to standard output; give -o <directory>");
            if (in.file != stdin) {
                fclose(in.file);
            }
            free(output);
            return rc;
        }
        rc = staged_dir_open(&stage, output, force);
        if (rc != 0) {
            if (in.file != stdin) {
                fclose(in.file);
            }
            free(output);
            return rc;
        }
        restore.root = stage.path;
        sink.context = &restore;
        sink.directory = restore_directory;
        sink.file_open = restore_file_open;
        sink.file_data = restore_file_data;
        sink.file_close = restore_file_close;
        sink.symlink = restore_symlink;
        sink.hardlink = restore_hardlink;
        sink.sparse_open = restore_sparse_open;
        sink.extent = restore_extent;
        sink.attribute = restore_attribute;
        unpacker = adapt_archive_unpacker_new(sink);
        if (unpacker == NULL) {
            rc = other_error(input, "memory allocation failed");
            if (in.file != stdin) {
                fclose(in.file);
            }
            staged_dir_discard(&stage);
            free(output);
            return rc;
        }
        adapt_archive_set_metadata_callback(unpacker, restore_metadata);
        status = decode_input(&ahead, threads, sizes, limits, adapt_archive_unpack, unpacker, &stats, &error);
        if (status == ADAPT_OK) {
            status = adapt_archive_finish(unpacker, &error);
        }
        if (in.file != stdin) {
            fclose(in.file);
        }
        if (restore.file != NULL) {
            fclose(restore.file);
        }
        rc = finish_directory_metadata(&restore, status == ADAPT_OK);
        if (status == ADAPT_OK && rc != 0) {
            status = ADAPT_ERROR_IO;
        }
        if (status == ADAPT_OK && has_metadata && (rc = metadata_apply(stage.path, 0, &metadata)) != 0) {
            (void)restore_fail(&restore, "root", rc, "cannot restore directory metadata");
            status = ADAPT_ERROR_IO;
        }
        if (status != ADAPT_OK) {
            char message[400];
            uint64_t at = 0;
            const char *problem = adapt_archive_problem(unpacker, &at);
            if (status == ADAPT_ERROR_IO && restore.failed != NULL) {
                snprintf(message, sizeof(message), "%s: %s", restore.failed,
                         restore.why != NULL ? restore.why : strerror(restore.error != 0 ? restore.error : EIO));
            } else if (status == ADAPT_ERROR_IO && restore.error != 0) {
                snprintf(message, sizeof(message), "%s", strerror(restore.error));
            } else if (status == ADAPT_ERROR_IO && problem != NULL) {
                snprintf(message, sizeof(message), "invalid archive at byte %llu: %s", (unsigned long long)at, problem);
            } else if (status == ADAPT_ERROR_BAD_ARCHIVE) {
                snprintf(message, sizeof(message), "invalid archive at byte %llu: %s", (unsigned long long)at,
                         problem != NULL ? problem : "cut short");
            } else if (status == ADAPT_ERROR_IO) {
                snprintf(message, sizeof(message), "i/o error: %s", in.error != 0 ? strerror(in.error) : "callback failed");
            } else {
                adapt_error_message(&error, message, sizeof(message));
            }
            rc = other_error(input, message);
            adapt_archive_unpacker_free(unpacker);
            free(restore.failed);
            staged_dir_discard(&stage);
            free(output);
            return rc;
        }
        free(restore.failed);
        rc = staged_dir_publish(&stage, output, force);
        if (rc != 0) {
            adapt_archive_unpacker_free(unpacker);
            free(output);
            return rc;
        }
        fprintf(stderr, "%llu bytes -> %llu bytes, checksum verified, %llu entries restored to %s\n",
                (unsigned long long)stats.output_bytes, (unsigned long long)stats.input_bytes,
                (unsigned long long)adapt_archive_entries(unpacker), output);
        adapt_archive_unpacker_free(unpacker);
        free(output);
        return 0;
    }
    if (strcmp(output, "-") == 0) {
        out.file = stdout;
    } else {
        rc = staged_open(&stage, output, force);
        if (rc != 0) {
            if (in.file != stdin) {
                fclose(in.file);
            }
            free(output);
            return rc;
        }
        out.file = stage.file;
    }
    status = decode_input(&ahead, threads, sizes, limits, write_stream, &out, &stats, &error);
    if (status == ADAPT_OK && out.file == stdout && fflush(stdout) != 0) {
        out.error = errno;
        error.status = ADAPT_ERROR_IO;
        status = ADAPT_ERROR_IO;
    }
    if (in.file != stdin) {
        fclose(in.file);
    }
    if (status != ADAPT_OK) {
        rc = codec_failure(input, &error, &in, &out);
        staged_discard(&stage);
        free(output);
        return rc;
    }
    if (stage.file != NULL) {
        rc = staged_publish(&stage, output, force, has_metadata ? &metadata : NULL);
        if (rc != 0) {
            free(output);
            return rc;
        }
    }
    free(output);
    /* The crate's line, with the workers that ran. */
    if (threads >= 2) {
        fprintf(stderr, "%llu bytes -> %llu bytes, checksum verified, %lu segments on %u thread%s\n",
                (unsigned long long)stats.output_bytes, (unsigned long long)stats.input_bytes,
                (unsigned long)stats.segments, stats.workers, stats.workers == 1 ? "" : "s");
    } else {
        fprintf(stderr, "%llu bytes -> %llu bytes, checksum verified\n", (unsigned long long)stats.output_bytes,
                (unsigned long long)stats.input_bytes);
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * encode
 * --------------------------------------------------------------------- */

/* The input's length, when it is a regular file: as the crate decides it,
 * by the file's kind where POSIX tells it, so a device or a pipe is sized
 * as a stream. Without POSIX, a seekable input counts as a file. */
static int input_length(FILE *file, uint64_t *length) {
    cadapt_off size;
    if (file == stdin) {
        return 0;
    }
#ifndef _WIN32
    {
        struct stat st;
        if (fstat(fileno(file), &st) != 0 || !S_ISREG(st.st_mode)) {
            return 0;
        }
        *length = (uint64_t)st.st_size;
        return 1;
    }
#endif
    if (cadapt_fseek(file, 0, SEEK_END) != 0) {
        return 0;
    }
    size = cadapt_ftell(file);
    if (size < 0 || cadapt_fseek(file, 0, SEEK_SET) != 0) {
        return 0;
    }
    *length = (uint64_t)size;
    return 1;
}

static const char *adaptive_name(unsigned mode) {
    static const char *const names[] = {"normal", "rates", "sparse", "retained"};
    return names[mode];
}

/* Spool the source once, encode each eligible profile sequentially, compare
 * complete containers including metadata, and publish only the winner.
 * No second model is live. The fixed 16 KiB buffer and stdio buffers fit in
 * the codec's existing 8 MiB headroom. Temporary disk use is input + at most
 * two candidate containers, with at most eight trials. I/O failure aborts; only budget ineligibility
 * skips a candidate. The original known/unknown length sizing is preserved. */
static adapt_status encode_search(adapt_read_fn read, void *context, stream *out,
                                  const adapt_metadata *metadata, adapt_sizes *sizes,
                                  int known_length, uint64_t length, uint64_t memory,
                                  uint64_t threads, uint64_t segment_bytes, int verify, int context_first,
                                  adapt_stats *stats, adapt_error *error) {
    stream source = {NULL, 0}, candidate = {NULL, 0}, best = {NULL, 0};
    uint8_t buffer[16384];
    size_t got;
    uint64_t best_bytes = UINT64_MAX;
    unsigned mode, trial_index;
    adapt_sizes bmp = {0};
    int has_bmp;
    adapt_sizes chosen = *sizes;
    adapt_status status = ADAPT_OK;
    source.file = tmpfile();
    if (source.file == NULL) goto io_failure;
    for (;;) {
        if (read(context, buffer, sizeof(buffer), &got) != 0) goto io_failure;
        if (got == 0) break;
        if (write_stream(&source, buffer, got) != 0) goto io_failure;
    }
    if (fflush(source.file) != 0) goto io_failure;
    if (fseek(source.file, 0, SEEK_SET) != 0 || read_stream(&source, buffer, 54, &got) != 0) goto io_failure;
    has_bmp = !sizes->archive && adapt_bmp_probe(buffer, got, &bmp);
    for (trial_index = 0; trial_index < (has_bmp ? 8u : 4u); trial_index++) {
        int transform = trial_index >= 4;
        uint64_t budget = transform ? memory - ADAPT_BLOCK_BYTES : memory;
        mode = trial_index % 4;
        adapt_sizes trial = *sizes;
        adapt_stats measured;
        metadata_writer writer;
        if (mode != 0 || transform) {
            status = threads >= 2 && known_length ?
                (sizes->max ? adapt_sizes_for_threads_max_adaptive : adapt_sizes_for_threads_adaptive)(length, budget, threads, segment_bytes, mode, &trial, error) :
                (sizes->max ? adapt_sizes_for_input_max_adaptive : adapt_sizes_for_input_adaptive)(known_length, length, budget, mode, &trial, error);
            if (status == ADAPT_OK && context_first)
                status = adapt_sizes_prioritize_context(known_length, length, budget, threads, segment_bytes, &trial, error);
            if (status == ADAPT_ERROR_MEMORY_LIMIT) {
                fprintf(stderr, "adaptive trial %s%s%s skipped: memory\n", sizes->max ? "max-" : "", adaptive_name(mode), transform ? "-bmp" : "");
                status = ADAPT_OK;
                continue;
            }
            if (status != ADAPT_OK) goto done;
            trial.archive = sizes->archive;
            trial.segmented = sizes->segmented;
        }
        trial.bmp = (uint8_t)transform;
        if (fseek(source.file, 0, SEEK_SET) != 0) goto io_failure;
        candidate.file = tmpfile();
        candidate.error = 0;
        if (candidate.file == NULL) goto io_failure;
        writer.out = &candidate;
        writer.metadata = metadata;
        writer.header_written = 0;
        status = adapt_encode_stream_segments(read_stream, &source, write_metadata_stream, &writer,
                                              trial, memory, segment_bytes, &measured, error);
        if (status != ADAPT_OK) goto done;
        if (fflush(candidate.file) != 0) goto io_failure;
        if (metadata != NULL) measured.output_bytes += ADAPT_METADATA_HEADER_BYTES;
        fprintf(stderr, "adaptive trial %s%s%s: %llu bytes\n", sizes->max ? "max-" : "", adaptive_name(mode), transform ? "-bmp" : "", (unsigned long long)measured.output_bytes);
        if (measured.output_bytes < best_bytes) {
            if (best.file != NULL && fclose(best.file) != 0) { best.file = NULL; goto io_failure; }
            best = candidate;
            candidate.file = NULL;
            best_bytes = measured.output_bytes;
            *stats = measured;
            chosen = trial;
        } else {
            int closed = fclose(candidate.file);
            candidate.file = NULL;
            if (closed != 0) goto io_failure;
        }
    }
    if (fseek(best.file, 0, SEEK_SET) != 0) goto io_failure;
    if (verify) {
        if (fseek(source.file, 0, SEEK_SET) != 0) goto io_failure;
        status = adapt_verify_stream(read_stream, &source, read_stream, &best, memory, NULL, error);
        if (status != ADAPT_OK) goto done;
        if (fseek(best.file, 0, SEEK_SET) != 0) goto io_failure;
    }
    for (;;) {
        if (read_stream(&best, buffer, sizeof(buffer), &got) != 0) goto io_failure;
        if (got == 0) break;
        if (write_stream(out, buffer, got) != 0) goto io_failure;
    }
    *sizes = chosen;
    fprintf(stderr, "adaptive selected %s%s%s: %llu bytes\n", chosen.max ? "max-" : "", adaptive_name(chosen.adaptive), chosen.bmp ? "-bmp" : "", (unsigned long long)best_bytes);
    goto done;

io_failure:
    status = ADAPT_ERROR_IO;
    out->error = errno != 0 ? errno : EIO;
    memset(error, 0, sizeof(*error));
    error->status = status;
done:
    if (source.file != NULL) fclose(source.file);
    if (candidate.file != NULL) fclose(candidate.file);
    if (best.file != NULL) fclose(best.file);
    return status;
}

static int encode_command(int argc, char **argv) {
    static const char *const with_value[] = {"-o", "--max-memory", "--threads", "--segment-bytes", "--adaptive", "--transform", "--allocation", NULL};
    static const char *const known[] = {"--force", "--verify", "--no-verify", "--fast", "--max", "-0", "-1", "-2", "-3",
                                        "-4",      "-5",       "-6",          "-7",     "-8",    "-9", NULL};
    args a;
    const char *input, *o, *text, *how;
    char *output = NULL;
    int force, verify, alongside, rc, known_length;
    unsigned adaptive = 0;
    int bmp_transform_requested = 0, context_first = 0;
    uint64_t profile_memory;
    uint64_t max_memory = ADAPT_DEFAULT_MAX_MEMORY, length = 0, hundredths, threads, segment_bytes = ADAPT_SEGMENT_BYTES;
    uint64_t workers = 1;
    char segments[96];
    adapt_sizes sizes;
    stream in = {NULL, 0}, out = {NULL, 0};
    staged stage = {NULL, NULL};
    adapt_stats stats;
    adapt_error error;
    adapt_status status;
    char message[160];
    /* a folder input: its entries, and the reader that streams them */
    folder dir;
    packer pack;
    int is_folder = 0;
    adapt_metadata source_metadata;
    int has_metadata = 0;
    metadata_writer metadata_output;
    char folder_name[4096];
    rc = parse_args(&a, argc, argv, with_value);
    if (rc != 0) {
        return rc;
    }
    rc = reject_unknown(&a, known);
    if (rc != 0) {
        return rc;
    }
    if (a.positionals != 1) {
        return usage_error("encode takes exactly one input%s", "");
    }
    input = a.positional[0];
    memset(&dir, 0, sizeof(dir));
    memset(&pack, 0, sizeof(pack));
    if (strcmp(input, "-") != 0 && is_directory(input)) {
        /* the folder's name without a trailing separator names the output */
        size_t n = strlen(input);
        while (n > 1 && (input[n - 1] == '/'
#ifdef _WIN32
                         || input[n - 1] == '\\'
#endif
                         )) {
            n--;
        }
        if (n >= sizeof(folder_name)) {
            return other_error(input, "path is too long");
        }
        memcpy(folder_name, input, n);
        folder_name[n] = '\0';
        is_folder = 1;
    }
    text = value_of(&a, "--allocation");
    if (text != NULL) {
        if (strcmp(text, "context") == 0) context_first = 1;
        else if (strcmp(text, "window") != 0)
            return usage_error("--allocation: expected window or context: %s", text);
    }
    text = value_of(&a, "--adaptive");
    if (text != NULL) {
        if (strcmp(text, "rates") == 0) adaptive = 1;
        else if (strcmp(text, "sparse") == 0) adaptive = 2;
        else if (strcmp(text, "retained") == 0) adaptive = 3;
        else if (strcmp(text, "auto") == 0) adaptive = 4;
        else return usage_error("--adaptive: expected rates, sparse, retained or auto: %s", text);
        if (has_flag(&a, "--fast"))
            return usage_error("--adaptive cannot be combined with --fast%s", "");
    }
    text = value_of(&a, "--transform");
    if (text != NULL) {
        if (strcmp(text, "bmp") != 0) return usage_error("--transform: expected bmp: %s", text);
        if (has_flag(&a, "--fast") || adaptive == 4 || is_folder)
            return usage_error("--transform bmp requires a file/pipe and normal, max or an explicit adaptive profile%s", "");
        bmp_transform_requested = 1;
    }
    if (has_flag(&a, "--fast") && has_flag(&a, "--max")) {
        return usage_error("--fast and --max are two profiles; pass one%s", "");
    }
    if (has_flag(&a, "--verify") && has_flag(&a, "--no-verify")) {
        return usage_error("--verify and --no-verify cannot be used together%s", "");
    }
    o = value_of(&a, "-o");
    if (o != NULL) {
        output = malloc(strlen(o) + 1);
        if (output != NULL) {
            strcpy(output, o);
        }
    } else if (strcmp(input, "-") == 0) {
        output = malloc(2);
        if (output != NULL) {
            strcpy(output, "-");
        }
    } else {
        const char *base = is_folder ? folder_name : input;
        size_t n = strlen(base);
        output = malloc(n + 6);
        if (output != NULL) {
            memcpy(output, base, n);
            memcpy(output + n, ".adpt", 6);
        }
    }
    if (output == NULL) {
        return other_error(input, "memory allocation failed");
    }
    /* a preset names the budget; an explicit --max-memory overrides it */
    if (preset_budget(&a) != 0) {
        max_memory = preset_budget(&a);
    }
    text = value_of(&a, "--max-memory");
    if (text != NULL && !parse_bytes(text, &max_memory)) {
        free(output);
        return usage_error("--max-memory: not a byte count: %s", text);
    }
    threads = thread_count(&a);
    if (threads == 0) {
        free(output);
        return usage_error("--threads: not a count of one or more: %s", value_of(&a, "--threads"));
    }
    text = value_of(&a, "--segment-bytes");
    if (text != NULL) {
        if (!parse_bytes(text, &segment_bytes) || !adapt_segment_bytes_valid(segment_bytes)) {
            free(output);
            return usage_error("--segment-bytes: not a whole number of 262144-byte blocks: %s", text);
        }
        if (threads < 2) {
            free(output);
            return usage_error("--segment-bytes needs --threads of two or more%s", "");
        }
    }
    force = has_flag(&a, "--force");
    verify = has_flag(&a, "--verify");
    binary_stdio();
    if (is_folder) {
        /* every entry listed and sized before a byte is coded, so the
         * stream's length is known and the sizes fit it */
        char *culprit = NULL;
        rc = metadata_read_path(folder_name, 0, &source_metadata);
        if (rc != 0) {
            free(output);
            return other_error(input, strerror(rc));
        }
        has_metadata = 1;
        rc = folder_collect(&dir, folder_name, &culprit);
        if (rc != 0) {
            snprintf(message, sizeof(message), "%s%s%s", culprit != NULL ? culprit : "",
                     culprit != NULL ? ": " : "",
                     rc == ENOTSUP ? "not a directory, a regular file or a symbolic link" : strerror(rc));
            free(culprit);
            folder_free(&dir);
            free(output);
            return other_error(input, message);
        }
        packer_reset(&pack, &dir);
        length = dir.stream_bytes;
        known_length = 1;
    } else if (strcmp(input, "-") == 0) {
        in.file = stdin;
    } else {
        in.file = fopen(input, "rb");
        if (in.file == NULL) {
            rc = other_error(input, strerror(errno));
            free(output);
            return rc;
        }
    }
    if (!is_folder) {
        known_length = input_length(in.file, &length);
        if (known_length) {
            rc = metadata_read_path(input, 0, &source_metadata);
            if (rc != 0) {
                fclose(in.file);
                free(output);
                return other_error(input, strerror(rc));
            }
            has_metadata = 1;
        }
    }
    profile_memory = bmp_transform_requested ? (max_memory > ADAPT_BLOCK_BYTES ? max_memory - ADAPT_BLOCK_BYTES : 0) : max_memory;
    if (adaptive > 0 && adaptive < 4) {
        status = threads >= 2 && known_length ?
            (has_flag(&a, "--max") ? adapt_sizes_for_threads_max_adaptive : adapt_sizes_for_threads_adaptive)(length, profile_memory, threads, segment_bytes, adaptive, &sizes, &error) :
            (has_flag(&a, "--max") ? adapt_sizes_for_input_max_adaptive : adapt_sizes_for_input_adaptive)(known_length, length, profile_memory, adaptive, &sizes, &error);
    } else if (threads >= 2 && known_length) {
        /* A file's sizes are the crate's for its threads: each model fitted
         * to a segment and to one thread's share of the budget. */
        status = has_flag(&a, "--max") ? adapt_sizes_for_threads_max(length, profile_memory, threads, segment_bytes, &sizes, &error)
                                       : adapt_sizes_for_threads(length, profile_memory, threads, segment_bytes, &sizes, &error);
    } else {
        status = has_flag(&a, "--max") ? adapt_sizes_for_input_max(known_length, length, profile_memory, &sizes, &error)
                                       : adapt_sizes_for_input(known_length, length, profile_memory, &sizes, &error);
    }
    if (status == ADAPT_OK && context_first) {
        if (has_flag(&a, "--fast")) sizes = adapt_sizes_fast(sizes);
        status = adapt_sizes_prioritize_context(known_length, length, profile_memory, threads, segment_bytes, &sizes, &error);
    }
    if (status != ADAPT_OK) {
        /* the crate's command line names this one without the input */
        adapt_error_message(&error, message, sizeof(message));
        fprintf(stderr, "error: %s\n", message);
        if (in.file != NULL && in.file != stdin) {
            fclose(in.file);
        }
        folder_free(&dir);
        free(output);
        return EXIT_OTHER;
    }
    if (has_flag(&a, "--fast")) {
        sizes = adapt_sizes_fast(sizes);
    }
    if (has_flag(&a, "--max") && adaptive == 0) {
        sizes = adapt_sizes_max(sizes);
    }
    sizes.archive = (uint8_t)is_folder;
    sizes.bmp = (uint8_t)bmp_transform_requested;
    if (threads >= 2) {
        /* The crate cuts a file's segments on threads and a pipe's in
         * order; this port cuts both on threads when the budget allows, to
         * the same bytes: every segment starts a fresh model, so the order
         * the segments are coded in changes nothing. */
        sizes.segmented = 1;
    }
    /* Segments go on threads when the budget holds a model, a segment
     * and a block's workspace for two or more of them; the workers then
     * prove their own segments, so the proof needs no second budget. */
    workers = threads >= 2 && adaptive != 4 ? adapt_encode_workers(sizes, max_memory, segment_bytes, threads) : 1;
    alongside = verify && adaptive != 4 && (workers >= 2 || adapt_verified_memory_bound(sizes) <= max_memory);
    if (verify && adaptive != 4 && !alongside && (in.file == stdin || !known_length || strcmp(output, "-") == 0)) {
        snprintf(message, sizeof(message),
                 "proving the output here needs a memory budget of %llu bytes for two models, "
                 "or a file input written to a file; raise --max-memory or omit --verify",
                 (unsigned long long)adapt_verified_memory_bound(sizes));
        rc = other_error(input, message);
        if (in.file != NULL && in.file != stdin) {
            fclose(in.file);
        }
        folder_free(&dir);
        free(output);
        return rc;
    }
    if (strcmp(output, "-") == 0) {
        out.file = stdout;
    } else {
        rc = staged_open(&stage, output, force);
        if (rc != 0) {
            if (in.file != NULL && in.file != stdin) {
                fclose(in.file);
            }
            folder_free(&dir);
            free(output);
            return rc;
        }
        out.file = stage.file;
    }
    {
        adapt_read_fn source = is_folder ? read_packer : read_stream;
        void *source_context = is_folder ? (void *)&pack : (void *)&in;
        metadata_output.out = &out;
        metadata_output.metadata = has_metadata ? &source_metadata : NULL;
        metadata_output.header_written = 0;
        if (adaptive == 4) {
            status = encode_search(source, source_context, &out, metadata_output.metadata, &sizes,
                                   known_length, length, max_memory, threads, segment_bytes, verify, context_first, &stats, &error);
        } else if (workers >= 2) {
            status = adapt_encode_stream_threads(source, source_context, write_metadata_stream, &metadata_output, sizes,
                                                 max_memory, segment_bytes, threads, verify, &stats, &error);
        } else if (alongside) {
            status = adapt_encode_stream_verified_segments(source, source_context, write_metadata_stream, &metadata_output, sizes, max_memory,
                                                           segment_bytes, &stats, &error);
        } else {
            status = adapt_encode_stream_segments(source, source_context, write_metadata_stream, &metadata_output, sizes, max_memory,
                                                  segment_bytes, &stats, &error);
        }
    }
    if (status == ADAPT_OK && has_metadata && adaptive != 4) {
        stats.output_bytes += ADAPT_METADATA_HEADER_BYTES;
    }
    packer_close(&pack);
    if (status == ADAPT_OK && out.file == stdout && fflush(stdout) != 0) {
        out.error = errno;
        error.status = ADAPT_ERROR_IO;
        status = ADAPT_ERROR_IO;
    }
    if (status != ADAPT_OK) {
        if (is_folder && status == ADAPT_ERROR_IO && out.error == 0) {
            rc = packer_failure(input, &pack);
        } else {
            rc = codec_failure(input, &error, &in, &out);
        }
        if (in.file != NULL && in.file != stdin) {
            fclose(in.file);
        }
        folder_free(&dir);
        staged_discard(&stage);
        free(output);
        return rc;
    }
    if (alongside || (adaptive == 4 && verify)) {
        how = "verified";
    } else if (verify) {
        /* One model at a time: prove the staged file against the input
         * again, which needs both to be files that can be read back — a
         * folder is walked again, and must hold the same bytes. */
        if ((is_folder || in.file != stdin) && known_length && stage.file != NULL) {
            stream again = {NULL, 0}, coded = {NULL, 0};
            if (in.file != NULL) {
                fclose(in.file);
                in.file = NULL;
            }
            if (fclose(stage.file) != 0) {
                rc = other_error(output, strerror(errno));
                stage.file = NULL;
                staged_discard(&stage);
                folder_free(&dir);
                free(output);
                return rc;
            }
            stage.file = NULL;
            again.file = is_folder ? NULL : fopen(input, "rb");
            coded.file = fopen(stage.path, "rb");
            if ((!is_folder && again.file == NULL) || coded.file == NULL) {
                rc = other_error(input, strerror(errno));
                if (again.file != NULL) {
                    fclose(again.file);
                }
                if (coded.file != NULL) {
                    fclose(coded.file);
                }
                staged_discard(&stage);
                folder_free(&dir);
                free(output);
                return rc;
            }
            if (is_folder) {
                packer_reset(&pack, &dir);
                status = adapt_verify_stream(read_packer, &pack, read_stream, &coded, max_memory, NULL, &error);
                packer_close(&pack);
            } else {
                status = adapt_verify_stream(read_stream, &again, read_stream, &coded, max_memory, NULL, &error);
                fclose(again.file);
            }
            fclose(coded.file);
            if (status != ADAPT_OK) {
                if (is_folder && status == ADAPT_ERROR_IO && pack.failed != NULL) {
                    rc = packer_failure(input, &pack);
                } else {
                    rc = codec_failure(input, &error, &again, &coded);
                }
                staged_discard(&stage);
                folder_free(&dir);
                free(output);
                return rc;
            }
            how = "verified after encoding";
        } else {
            rc = other_error(input, "could not reopen the input and staged output for verification");
            if (in.file != NULL && in.file != stdin) {
                fclose(in.file);
            }
            staged_discard(&stage);
            folder_free(&dir);
            free(output);
            return rc;
        }
    } else {
        how = has_flag(&a, "--no-verify") ? "not verified (--no-verify)" : "not verified (pass --verify to enable)";
    }
    if (in.file != NULL && in.file != stdin) {
        fclose(in.file);
    }
    if (stage.path != NULL) {
        rc = staged_publish(&stage, output, force, NULL);
        if (rc != 0) {
            folder_free(&dir);
            free(output);
            return rc;
        }
    }
    free(output);
    /* Hundredths of a percent in integers, as the crate prints them. */
    hundredths = stats.input_bytes == 0 ? 0 : (stats.output_bytes * 10000 + stats.input_bytes / 2) / stats.input_bytes;
    segments[0] = '\0';
    if (threads >= 2) {
        unsigned on = adaptive == 4 ? 1u : stats.workers;
        snprintf(segments, sizeof(segments), ", %lu segments on %u thread%s", (unsigned long)stats.segments, on,
                 on == 1 ? "" : "s");
    }
    if (is_folder) {
        fprintf(stderr, "%llu entries, %llu bytes -> %llu bytes (%llu.%02llu%%), %s%s\n", (unsigned long long)dir.count,
                (unsigned long long)stats.input_bytes, (unsigned long long)stats.output_bytes,
                (unsigned long long)(hundredths / 100), (unsigned long long)(hundredths % 100), how, segments);
    } else {
        fprintf(stderr, "%llu bytes -> %llu bytes (%llu.%02llu%%), %s%s\n", (unsigned long long)stats.input_bytes,
                (unsigned long long)stats.output_bytes, (unsigned long long)(hundredths / 100),
                (unsigned long long)(hundredths % 100), how, segments);
    }
    folder_free(&dir);
    return 0;
}

/* ------------------------------------------------------------------------
 * info
 * --------------------------------------------------------------------- */

static int info_command(int argc, char **argv) {
    static const char *const none[] = {NULL};
    args a;
    const char *input;
    FILE *file;
    uint8_t header[ADAPT_HEADER_MAX_BYTES];
    size_t header_len;
    /* the trailer, and before it the segment count of a segmented file */
    uint8_t tail[ADAPT_TRAILER_BYTES + ADAPT_SEGMENT_COUNT_BYTES];
    const uint8_t *trailer = tail + ADAPT_SEGMENT_COUNT_BYTES;
    uint64_t file_bytes, length;
    uint32_t crc, count = 0;
    adapt_sizes sizes;
    adapt_error error;
    char message[160];
    int rc;
    rc = parse_args(&a, argc, argv, none);
    if (rc != 0) {
        return rc;
    }
    rc = reject_unknown(&a, none);
    if (rc != 0) {
        return rc;
    }
    if (a.positionals != 1) {
        return usage_error("info takes exactly one file%s", "");
    }
    input = a.positional[0];
    binary_stdio();
    if (strcmp(input, "-") == 0) {
        file = stdin;
    } else {
        file = fopen(input, "rb");
        if (file == NULL) {
            return other_error(input, strerror(errno));
        }
    }
    if (fread(header, 1, ADAPT_HEADER_BYTES, file) != ADAPT_HEADER_BYTES) {
        rc = other_error(input, ferror(file) ? "cannot read header" : "cannot read header: failed to fill whole buffer");
        if (file != stdin) {
            fclose(file);
        }
        return rc;
    }
    header_len = adapt_header_len(header);
    if (header_len > ADAPT_HEADER_BYTES &&
        fread(header + ADAPT_HEADER_BYTES, 1, header_len - ADAPT_HEADER_BYTES, file) != header_len - ADAPT_HEADER_BYTES) {
        rc = other_error(input, ferror(file) ? "cannot read header" : "cannot read header: failed to fill whole buffer");
        if (file != stdin) {
            fclose(file);
        }
        return rc;
    }
    if (adapt_read_header(header, header_len, &sizes, &error) != ADAPT_OK) {
        adapt_error_message(&error, message, sizeof(message));
        if (file != stdin) {
            fclose(file);
        }
        return other_error(input, message);
    }
    /* A regular file gives up its trailer by seeking; a stream is read to
     * its end, keeping the last bytes, so `info -` costs a pass, not memory. */
    if (file != stdin && cadapt_fseek(file, 0, SEEK_END) == 0) {
        cadapt_off size = cadapt_ftell(file);
        if (size < 0) {
            rc = other_error(input, strerror(errno));
            fclose(file);
            return rc;
        }
        file_bytes = (uint64_t)size;
        if (file_bytes < header_len + ADAPT_TRAILER_BYTES) {
            fclose(file);
            return other_error(input, "file is truncated");
        }
        if (cadapt_fseek(file, -(cadapt_off)sizeof(tail), SEEK_END) != 0 || fread(tail, 1, sizeof(tail), file) != sizeof(tail)) {
            fclose(file);
            return other_error(input, "cannot read trailer");
        }
        fclose(file);
    } else {
        uint8_t window[2 * sizeof(tail)];
        size_t held = 0;
        file_bytes = header_len;
        for (;;) {
            size_t n = fread(window + held, 1, sizeof(window) - held, file);
            if (n == 0) {
                if (ferror(file)) {
                    rc = other_error(input, strerror(errno));
                    if (file != stdin) {
                        fclose(file);
                    }
                    return rc;
                }
                break;
            }
            file_bytes += n;
            held += n;
            if (held > sizeof(tail)) {
                memmove(window, window + held - sizeof(tail), sizeof(tail));
                held = sizeof(tail);
            }
        }
        if (file != stdin) {
            fclose(file);
        }
        if (held < ADAPT_TRAILER_BYTES) {
            return other_error(input, "file is truncated");
        }
        /* a stream shorter than the tail has its header in the window */
        memset(tail, 0, sizeof(tail));
        memcpy(tail + sizeof(tail) - held, window, held);
    }
    if (adapt_read_trailer(trailer, &length, &crc, &error) != ADAPT_OK) {
        adapt_error_message(&error, message, sizeof(message));
        return other_error(input, message);
    }
    if (sizes.segmented) {
        count = (uint32_t)tail[0] | (uint32_t)tail[1] << 8 | (uint32_t)tail[2] << 16 | (uint32_t)tail[3] << 24;
        if (count == 0 || file_bytes < header_len + adapt_segment_table_len(count) + ADAPT_TRAILER_BYTES) {
            return other_error(input, "invalid block");
        }
    }
    printf("format          adapt container version %u\n", header[4]);
    printf("file bytes      %llu\n", (unsigned long long)file_bytes);
    printf("declared bytes  %llu\n", (unsigned long long)length);
    if (sizes.segmented) {
        printf("segments        %lu\n", (unsigned long)count);
    }
    printf("context table   2^%u blocks (%llu MiB)\n", sizes.table_log,
           (unsigned long long)((((uint64_t)1 << sizes.table_log) * 16) >> 20));
    printf("recall table    2^%u entries (%llu MiB)\n", sizes.match_log,
           (unsigned long long)((((uint64_t)1 << sizes.match_log) * 4) >> 20));
    printf("window          2^%u bytes (%llu MiB)\n", sizes.window_log, (unsigned long long)(((uint64_t)1 << sizes.window_log) >> 20));
    if (adapt_class_width(sizes) == 0) {
        printf("class history   none\n");
    } else {
        printf("class history   %u bytes\n", adapt_class_width(sizes));
    }
    if (sizes.period != 0) {
        printf("record period   %u bytes\n", (unsigned)sizes.period);
    }
    if (sizes.fast) {
        char pair[64];
        adapt_fast_contexts(sizes, pair, sizeof(pair));
        printf("contexts        %s\n", pair);
    } else {
        char dropped[64];
        adapt_dropped_contexts(sizes, dropped, sizeof(dropped));
        if (dropped[0] == 0) {
            printf("contexts        all\n");
        } else {
            printf("contexts        all but %s\n", dropped);
        }
    }
    printf("profile         %s%s\n", sizes.max && sizes.adaptive ? "max-" : "", sizes.adaptive ? adaptive_name(sizes.adaptive) : (sizes.fast ? "fast" : (sizes.max ? "max" : "normal")));
    printf("x86 transform   %s\n", sizes.x86 ? "yes" : "no");
    if (sizes.bmp) printf("bmp transform   %lu x %lu, pixel offset %lu\n", (unsigned long)sizes.bmp_width,
                          (unsigned long)sizes.bmp_rows, (unsigned long)sizes.bmp_offset);
    printf("holds           %s\n", sizes.archive ? "a folder (an adapt archive)" : "a file");
    printf("model memory    ~%llu MiB, whatever the length\n", (unsigned long long)(adapt_model_bytes(sizes) >> 20));
    printf("crc32           %08lx\n", (unsigned long)crc);
    return fflush(stdout) == 0 ? 0 : other_error(input, strerror(errno));
}

/* ------------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */

int main(int argc, char **argv) {
    const char *cmd;
    if (argc < 2) {
        return usage_error("a command is required%s", "");
    }
    cmd = argv[1];
    if (strcmp(cmd, "encode") == 0) {
        return encode_command(argc - 2, argv + 2);
    }
    if (strcmp(cmd, "decode") == 0) {
        return decode_command(argc - 2, argv + 2);
    }
    if (strcmp(cmd, "info") == 0) {
        return info_command(argc - 2, argv + 2);
    }
    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "-V") == 0) {
        printf("cadapt %s (format version %u)\n", CADAPT_VERSION, ADAPT_FORMAT_VERSION);
        return 0;
    }
    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0 || strcmp(cmd, "help") == 0) {
        print_usage(stdout);
        return 0;
    }
    return usage_error("unknown command %s", cmd);
}
