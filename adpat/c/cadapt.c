/*
 * cadapt — command line for the C codec.
 *
 * Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-12.
 *
 *   cadapt encode <input> [-o <output>] [--force] [--verify | --no-verify] [--fast | --mid | --max] [-0 .. -11]
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
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#if !defined(_WIN32) && !defined(_FILE_OFFSET_BITS)
#define _FILE_OFFSET_BITS 64
#endif

#include "adapt.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <process.h>
#include <sys/utime.h>
#include <windows.h>
#define cadapt_getpid _getpid
#define cadapt_fseek _fseeki64
#define cadapt_ftell _ftelli64
typedef long long cadapt_off;
#else
#include <dirent.h>
#include <poll.h>
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

/* Only the command thread reads cancellation state. A handler, including
 * one delivered to a codec worker, only writes to this nonblocking pipe.
 * Allocation, stdio and recursive staging cleanup stay on the normal path. */
static int interrupted_signal;
#ifndef _WIN32
static int interrupt_pipe[2] = {-1, -1};

static void request_interrupt(int number) {
    int saved_errno = errno;
    unsigned char byte = (unsigned char)number;
    ssize_t written = write(interrupt_pipe[1], &byte, 1);
    (void)written; /* A full pipe already contains a cancellation request. */
    errno = saved_errno;
}

static int setup_interrupts(void) {
    static const int signals[] = {
        SIGINT, SIGTERM, SIGHUP, SIGPIPE,
#ifdef SIGXFSZ
        SIGXFSZ,
#endif
    };
    struct sigaction action;
    size_t i;
    if (pipe(interrupt_pipe) != 0) return -1;
    for (i = 0; i < 2; i++) {
        /* Preserve a caller's closed stdin/stdout/stderr: private wakeup
         * descriptors must never turn one into our cancellation pipe. */
        if (interrupt_pipe[i] < 3) {
            int fd = fcntl(interrupt_pipe[i], F_DUPFD, 3);
            if (fd < 0) return -1;
            close(interrupt_pipe[i]);
            interrupt_pipe[i] = fd;
        }
        if (fcntl(interrupt_pipe[i], F_SETFL, O_NONBLOCK) != 0 ||
            fcntl(interrupt_pipe[i], F_SETFD, FD_CLOEXEC) != 0) return -1;
    }
    memset(&action, 0, sizeof(action));
    action.sa_handler = request_interrupt;
    sigemptyset(&action.sa_mask);
    for (i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
        if (sigaction(signals[i], &action, NULL) != 0) return -1;
    }
    return 0;
}
#endif

static int cancelled(void) {
#ifndef _WIN32
    if (!interrupted_signal && interrupt_pipe[0] >= 0) {
        unsigned char byte;
        int saved_errno = errno;
        if (read(interrupt_pipe[0], &byte, 1) == 1) interrupted_signal = byte;
        errno = saved_errno;
    }
#endif
    return interrupted_signal != 0;
}

#ifndef _WIN32
static int wait_stream(int fd, short events) {
    struct pollfd descriptors[2] = {{fd, events, 0}, {interrupt_pipe[0], POLLIN, 0}};
    for (;;) {
        int rc;
        if (cancelled()) return EINTR;
        rc = poll(descriptors, 2, -1);
        if (cancelled()) return EINTR;
        if (rc < 0) {
            if (errno == EINTR) continue;
            return errno;
        }
        if (descriptors[0].revents) return 0;
    }
}
#endif

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

#ifdef _WIN32
static int metadata_apply(const char *path, int symlink_entry, const adapt_metadata *m) {
    adapt_metadata restored;
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
static int metadata_check_stat(const struct stat *st, const adapt_metadata *m) {
#ifdef __APPLE__
    struct timespec atime = st->st_atimespec, mtime = st->st_mtimespec;
#else
    struct timespec atime = st->st_atim, mtime = st->st_mtim;
#endif
    return ((uint32_t)st->st_mode & 07777u) == m->mode && atime.tv_sec == m->atime_seconds &&
                   (uint32_t)atime.tv_nsec == m->atime_nanoseconds && mtime.tv_sec == m->mtime_seconds &&
                   (uint32_t)mtime.tv_nsec == m->mtime_nanoseconds
               ? 0 : ENOTSUP;
}

static int metadata_times(const adapt_metadata *m, struct timespec times[2]) {
    times[0].tv_sec = (time_t)m->atime_seconds;
    times[0].tv_nsec = (long)m->atime_nanoseconds;
    times[1].tv_sec = (time_t)m->mtime_seconds;
    times[1].tv_nsec = (long)m->mtime_nanoseconds;
    return (int64_t)times[0].tv_sec == m->atime_seconds && (int64_t)times[1].tv_sec == m->mtime_seconds ? 0 : ERANGE;
}

static int metadata_apply_fd(int fd, const adapt_metadata *m) {
    struct timespec times[2];
    struct stat st;
    int rc = metadata_times(m, times);
    if (rc != 0) return rc;
    if (futimens(fd, times) != 0 || fchmod(fd, (mode_t)m->mode) != 0 || fstat(fd, &st) != 0) return errno;
    return metadata_check_stat(&st, m);
}

/* chmod changes an ACL's mask, not its named entries. Remove inherited
 * ACLs before any archived permissions are installed or children created. */
static int clear_inherited_acl(int fd) {
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
static int attr_apply(int fd, const char *name, const uint8_t *value, size_t len) {
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
static int attrs_read(const char *path, attrs *a) {
    (void)path;
    memset(a, 0, sizeof(*a));
    return 0;
}

static int attr_apply(int fd, const char *name, const uint8_t *value, size_t len) {
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
            "  cadapt encode <input> [-o <output>] [--force] [--verify | --no-verify] [--fast | --mid | --max] [-0 .. -11]\n"
            "                [--threads <n>] [--segment-bytes <bytes>]\n"
            "                [--adaptive rates|sparse|retained|auto] [--transform bmp|text|none] [--allocation window|context]\n"
            "                [--contexts auto|lean]\n"
            "  cadapt decode <input> [-o <output>] [--force] [--max-output <bytes>] [--max-memory <bytes>] [--threads <n>]\n"
            "  cadapt info   <file>\n"
            "  cadapt --version\n"
            "\n"
            "<input> or <output> may be '-' for standard input or output. Without -o,\n"
            "encode writes <input>.adpt and decode strips that suffix (or appends .out).\n"
            "Existing output files are kept unless --force is given. --fast writes the\n"
            "fast profile, which decodes about three times faster for about a twelfth\n"
            "more bytes; --mid the mid profile, seven fixed contexts and one arbiter over\n"
            "the normal profile's tables, about a quarter less time for 1.4 %% (enwik8) to\n"
            "2.4 %% (enwik9) more bytes; --max the max profile, which writes\n"
            "fewer bytes for about three times the time in the same memory; the default is\n"
            "the normal profile. Normal/fast/mid/max streams use versions %u/%u/%u/%u;\n"
            "named files and folders include metadata in versions %u/%u/%u/%u.\n"
            "--adaptive rates/sparse/retained select experimental causal predictors\n"
            "over normal or --max. auto spools the\n"
            "source once to temporary disk and selects the smallest complete container\n"
            "from the base and these three profiles that fit the memory budget. Search\n"
            "also tries BMP24 color/row residuals on eligible files. Experiment formats occupy versions 25--52;\n"
            "the max profile's compact side tables, chosen by --allocation context when the budget\n"
            "would otherwise leave the context table at 2^22 blocks or fewer, are versions 53/56, and\n"
            "with the lean context set they take at a table of 2^22 blocks or fewer, 54/57.\n"
            "--transform bmp explicitly selects that reversible transform. Search\n"
            "uses up to eight encodes, temporary disk of input plus two containers, and\n"
            "verifies only with --verify (one model, also for pipes).\n",
            ADAPT_FORMAT_VERSION, ADAPT_FAST_VERSION, ADAPT_MID_VERSION, ADAPT_MAX_PRESSURE_VERSION,
            ADAPT_METADATA_VERSION, ADAPT_METADATA_FAST_VERSION, ADAPT_MID_METADATA_VERSION, ADAPT_MAX_PRESSURE_METADATA_VERSION);
    fprintf(stream,
            "The text transform (versions 62--66, 67--71 with metadata) replaces a word both\n"
            "sides have seen three times by a code the model reads as one symbol; the book\n"
            "is built as the file is coded, nothing is sent ahead. It is taken by itself for\n"
            "a file of 8 MiB or more whose first block looks like text (measured on\n"
            "enwik8 at 128 MiB: 3.5 %% fewer bytes in fewer seconds); --transform text asks\n"
            "for it whatever the input, --transform none refuses it.\n"
            "--allocation context grows the context table before the recall window;\n"
            "window is the default. Either policy can win depending on the input.\n"
            "--contexts lean takes the lean context set outright (word and field left out;\n"
            "under --max o8, word3 and wordc too) instead of choosing the set from the first\n"
            "block: on text about 12 %% less encode time and 8 %% less decode time for about\n"
            "0.1 %% more bytes; auto, the default, chooses. Not with --fast.\n"
            "-0 .. -9 set the memory budget to what xz takes to compress at the same\n"
            "preset: 3, 9, 17, 32, 48, 94, 94, 186, 370 and 674 MiB; -10 and -11 are\n"
            "1 and 2 GiB, above xz's ladder. A preset below the smallest model of the\n"
            "profile is raised to it; -9 is the default. The preset is the only memory\n"
            "setting of encode: it bounds the model, window and workspace, and decode\n"
            "takes the model's memory as the header names it, so the preset a file was\n"
            "written with is the memory every decoder of it needs. Folder path and\n"
            "metadata bookkeeping is additional. The smallest normal model takes %llu\n"
            "bytes, the max profile's %llu; verification and segments may need more.\n"
            "Named files and folders preserve modes and access/modification timestamps.\n"
            "encode skips round-trip verification by default. --verify proves its output\n"
            "as it writes it when the budget holds two models, or, for a file written\n"
            "to a file, in a second pass with one. --no-verify explicitly skips this proof.\n"
            "decode allocates the model its input's header names, whatever the size\n"
            "(FMT-6: the container alone decodes); --max-memory <bytes> caps that and\n"
            "refuses a container needing more; --max-output <bytes> refuses a container\n"
            "declaring more decoded bytes than that (a folder's restored sizes together).\n"
            "Neither cap applies unless given.\n"
            "--threads <n> (default 1) with n of two or more cuts the input into segments\n"
            "of --segment-bytes (default %llu, whole blocks of %llu) that start the\n"
            "model afresh, the file the crate's threads write, and codes them at once on\n"
            "as many of the n threads as the budget holds a model, a segment and a block's\n"
            "workspace for, and never more than the CPUs online (the sizes stay fitted\n"
            "to n, so the bytes are the same on every machine); decode --threads <n>\n"
            "decodes a file's segments the same way. The report says how many threads ran.\n",
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
    const char *positional;
    int positionals;
    int argc;
    char **argv;
    const char *const *with_value;
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
    a->argc = argc;
    a->argv = argv;
    a->with_value = with_value;
    for (i = 0; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "-") == 0 || arg[0] != '-') {
            a->positional = arg;
            a->positionals++;
        } else if (takes_value(arg, with_value)) {
            if (i + 1 >= argc) {
                return usage_error("%s needs a value", arg);
            }
            i++;
        }
    }
    return 0;
}

/* Iterate the borrowed argument list after parse_args succeeds. Option
 * values are consumed with their names, even when they look like flags;
 * no fixed-size option array can silently discard a caller's limits. */
static const char *next_option(const args *a, int *at, const char **value) {
    while (*at < a->argc) {
        const char *name = a->argv[(*at)++];
        if (name[0] != '-' || strcmp(name, "-") == 0) {
            continue;
        }
        *value = takes_value(name, a->with_value) ? a->argv[(*at)++] : NULL;
        return name;
    }
    return NULL;
}

static int has_flag(const args *a, const char *name) {
    int at = 0;
    const char *option, *value;
    while ((option = next_option(a, &at, &value)) != NULL) {
        if (value == NULL && strcmp(option, name) == 0) {
            return 1;
        }
    }
    return 0;
}

/* The last value given for `name`, or NULL. */
static const char *value_of(const args *a, const char *name) {
    int at = 0;
    const char *option, *value, *last = NULL;
    while ((option = next_option(a, &at, &value)) != NULL) {
        if (value != NULL && strcmp(option, name) == 0) {
            last = value;
        }
    }
    return last;
}

/* The encode presets -0 to -9: xz's compression-side memory, in MiB, at the
 * same numbers, so that a user of xz reads them as before; -10 and -11
 * are 1 and 2 GiB, the tiers above xz's ladder that the max profile's
 * largest tables and a 1 GB input's window need. The preset is encode's
 * only memory setting. */
#define PRESETS 12
static const uint64_t PRESET_MIB[PRESETS] = {3, 9, 17, 32, 48, 94, 94, 186, 370, 674, 1024, 2048};
static const char *const PRESET_FLAGS[PRESETS] = {"-0", "-1", "-2", "-3", "-4", "-5", "-6", "-7", "-8", "-9", "-10", "-11"};

/* The budget a preset names: the last preset given, or -9; a preset below
 * the smallest model of the profile in force is raised to it. 0 when no
 * preset flag is present. */
static uint64_t preset_budget(const args *a) {
    uint64_t budget = 0;
    int at = 0, p;
    const char *option, *value;
    while ((option = next_option(a, &at, &value)) != NULL) {
        if (value != NULL) {
            continue;
        }
        for (p = 0; p < PRESETS; p++) {
            if (strcmp(option, PRESET_FLAGS[p]) == 0) {
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
    int at = 0;
    const char *option, *value;
    while ((option = next_option(a, &at, &value)) != NULL) {
        if (value == NULL && !takes_value(option, known)) {
            return usage_error("unknown option %s", option);
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

/* The CPUs online, at least one. */
static uint64_t online_cpus(void) {
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwNumberOfProcessors >= 1 ? info.dwNumberOfProcessors : 1;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n >= 1 ? (uint64_t)n : 1;
#endif
}

/* The workers that run at once for --threads n: n, or the CPUs online
 * when n is more — the coder's per-bit path is pure CPU, and a worker
 * past the cores only takes turns with another and evicts its tables.
 * The sizes stay fitted to the n asked for, so the bytes are the same on
 * every machine (RUN-5); the report names the workers that ran. */
static uint64_t running_threads(uint64_t threads) {
    uint64_t cpus = online_cpus();
    if (threads > cpus) {
        fprintf(stderr, "--threads %llu: %llu online CPU%s, so %llu run at once; the sizes are fitted to %llu models as asked\n",
                (unsigned long long)threads, (unsigned long long)cpus, cpus == 1 ? "" : "s", (unsigned long long)cpus,
                (unsigned long long)threads);
        return cpus;
    }
    return threads;
}

/* ------------------------------------------------------------------------
 * Streams: standard input or a file in, standard output or a staged file out.
 * --------------------------------------------------------------------- */

typedef struct stream {
    FILE *file;
    int error; /* errno of the last failure, for the message */
    /* How the sink takes bytes, found at the first write: 1 a regular
     * file, which takes a whole block in one write; 2 anything else — a
     * pipe, a socket, a terminal — which can block, and takes PIPE_BUF at
     * a time between cancellation checks. 0 until then. */
    int sink;
} stream;

static int read_stream(void *context, uint8_t *buf, size_t len, size_t *got) {
    stream *s = context;
#ifndef _WIN32
    size_t at = 0;
    *got = 0;
    while (at < len) {
        ssize_t n;
        int rc = wait_stream(fileno(s->file), POLLIN);
        if (rc != 0) return s->error = rc;
        n = read(fileno(s->file), buf + at, len - at);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return s->error = errno;
        }
        if (n == 0) break;
        at += (size_t)n;
    }
    *got = at;
#else
    size_t n = fread(buf, 1, len, s->file);
    if (n < len && ferror(s->file)) {
        s->error = errno;
        return s->error != 0 ? s->error : EIO;
    }
    *got = n;
#endif
    return 0;
}

static int write_stream(void *context, const uint8_t *buf, size_t len) {
    stream *s = context;
#ifndef _WIN32
    size_t at = 0;
    if (s->sink == 0) {
        struct stat st;
        s->sink = fstat(fileno(s->file), &st) == 0 && S_ISREG(st.st_mode) ? 1 : 2;
    }
    while (at < len) {
        /* A pipe reported writable can accept PIPE_BUF bytes without
         * blocking. Recheck cancellation before each further chunk. A
         * regular file never blocks that way and takes the block whole:
         * chunking it was four system calls per 4 KiB, a seventh of the
         * wall time of a stored-block decode. */
        size_t take = s->sink == 1 || len - at < PIPE_BUF ? len - at : PIPE_BUF;
        ssize_t n;
        int rc = wait_stream(fileno(s->file), POLLOUT);
        if (rc != 0) return s->error = rc;
        n = write(fileno(s->file), buf + at, take);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return s->error = errno;
        }
        if (n == 0) return s->error = EIO;
        at += (size_t)n;
    }
#else
    if (fwrite(buf, 1, len, s->file) != len) {
        s->error = errno;
        return s->error != 0 ? s->error : EIO;
    }
#endif
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
    unsigned mode; /* final mode for a file without recorded metadata */
#ifndef _WIN32
    /* Folders live inside a private wrapper whose mode never changes.
     * Publication moves "contents" out through guard_fd, not s->path. */
    int parent_fd, guard_fd, root_fd;
    int directory; /* contents is a directory or a regular output file */
    char *target_name;
#endif
} staged;

#ifdef _WIN32
static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f != NULL) {
        fclose(f);
        return 1;
    }
    return errno != ENOENT;
}
#endif

#ifndef _WIN32
static int staged_guard_open(staged *s, const char *target, int force, int directory);
static void staged_dir_release(staged *s);
static void staged_dir_discard(staged *s);
#endif

static void staged_discard(staged *s) {
    if (s->file != NULL) {
        fclose(s->file);
        s->file = NULL;
    }
    if (s->path != NULL) {
#ifndef _WIN32
        staged_dir_discard(s);
#else
        remove(s->path);
        free(s->path);
        s->path = NULL;
#endif
    }
}

/* Open a staging file in the target's directory. Returns an exit code, or
 * 0 with `s` ready to write. */
static int staged_open(staged *s, const char *target, int force) {
#ifndef _WIN32
    mode_t mask = umask(0);
    umask(mask);
    s->mode = (unsigned)(0666 & ~mask);
    return staged_guard_open(s, target, force, 0);
#else
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
    {
        const char *back = strrchr(target, '\\');
        if (back != NULL && (slash == NULL || back > slash)) {
            slash = back;
        }
    }
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
         * discarded; probing first cannot establish that ownership.
         * Keep incomplete plaintext private. The final recorded or
         * umask-derived mode is applied only after decoding succeeds. */
        fd = _open(path, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
        if (fd < 0) {
            int e = errno;
            free(path);
            if (e == EEXIST) {
                continue;
            }
            return other_error(target, strerror(e));
        }
        s->file = _fdopen(fd, "wb");
        if (s->file == NULL) {
            int e = errno;
            _close(fd);
            remove(path);
            free(path);
            return other_error(target, strerror(e));
        }
        s->path = path;
        return 0;
    }
    return other_error(target, "cannot create a unique staging file");
#endif
}

/* Publish the staged file as `target`. Returns an exit code or 0. */
static int staged_publish(staged *s, const char *target, int force, const adapt_metadata *metadata) {
    if (cancelled()) {
        staged_discard(s);
        return other_error(target, "interrupted");
    }
    int rc;
    if (s->file != NULL && fclose(s->file) != 0) {
        int e = errno;
        s->file = NULL;
        staged_discard(s);
        return other_error(target, strerror(e));
    }
    s->file = NULL;
#ifndef _WIN32
    rc = metadata != NULL ? metadata_apply_fd(s->root_fd, metadata)
                          : (fchmod(s->root_fd, (mode_t)s->mode) == 0 ? 0 : errno);
    if (rc != 0) {
        char message[256];
        snprintf(message, sizeof(message), "cannot restore file metadata: %s", strerror(rc));
        staged_discard(s);
        return other_error(target, message);
    }
    {
        rc = force ? renameat(s->guard_fd, "contents", s->parent_fd, s->target_name)
                   : linkat(s->guard_fd, "contents", s->parent_fd, s->target_name, 0);
        if (rc != 0) rc = errno;
    }
    if (rc != 0) {
        staged_discard(s);
        return other_error(target, rc == EEXIST ? "exists; pass --force to overwrite" : strerror(rc));
    }
    /* linkat leaves the private name; renameat has already removed it. */
    staged_discard(s);
    return 0;
#else
    if (metadata != NULL && (rc = metadata_apply(s->path, 0, metadata)) != 0) {
        staged_discard(s);
        return other_error(target, strerror(rc));
    }
    if (!force && file_exists(target)) {
        staged_discard(s);
        fprintf(stderr, "error: %s exists; pass --force to overwrite\n", target);
        return EXIT_OTHER;
    }
    remove(target);
    rc = rename(s->path, target);
    if (rc != 0) {
        int e = errno;
        staged_discard(s);
        return other_error(target, strerror(e));
    }
    free(s->path);
    s->path = NULL;
    return 0;
#endif
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

/* An inode seen with more than one name, and the entry that carries it —
 * a slot of the walker's inode table, the entry's position plus one so
 * that 0 marks an empty slot. */
typedef struct seen_inode {
    identity id;
    size_t index;
} seen_inode;

typedef struct folder {
    const char *root;
    attrs root_attrs;
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
    attrs_free(&f->root_attrs);
    f->items = NULL;
    f->inodes = NULL;
    f->count = f->cap = f->inode_count = f->inode_cap = 0;
}

/* The inode table is a hash table — open addressing over (device, inode),
 * a power of two of slots never more than half full — so a snapshot tree
 * of a million hard-linked names costs a million lookups, not a million
 * scans of everything seen so far (the flat list it replaced was measured
 * quadratic: 0.3 s over 30,000 inodes, 6.3 s over 100,000). */
static size_t inode_slot(const identity *id, size_t mask) {
    uint64_t h = id->inode ^ (id->device * 0x9E3779B97F4A7C15ull);
    h ^= h >> 31;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 29;
    return (size_t)h & mask;
}

/* The entry an inode already has, or -1. */
static long folder_inode(const folder *f, const identity *id) {
    size_t mask, i;
    if (f->inode_cap == 0) {
        return -1;
    }
    mask = f->inode_cap - 1;
    for (i = inode_slot(id, mask); f->inodes[i].index != 0; i = (i + 1) & mask) {
        if (f->inodes[i].id.device == id->device && f->inodes[i].id.inode == id->inode) {
            return (long)(f->inodes[i].index - 1);
        }
    }
    return -1;
}

static void inode_insert(seen_inode *slots, size_t mask, const identity *id, size_t index) {
    size_t i = inode_slot(id, mask);
    while (slots[i].index != 0) {
        i = (i + 1) & mask;
    }
    slots[i].id = *id;
    slots[i].index = index + 1;
}

static int folder_remember_inode(folder *f, const identity *id, size_t index) {
    if (2 * (f->inode_count + 1) > f->inode_cap) {
        size_t grown = f->inode_cap == 0 ? 64 : f->inode_cap * 2, i;
        seen_inode *bigger = calloc(grown, sizeof(*bigger));
        if (bigger == NULL) {
            return ENOMEM;
        }
        for (i = 0; i < f->inode_cap; i++) {
            if (f->inodes[i].index != 0) {
                inode_insert(bigger, grown - 1, &f->inodes[i].id, f->inodes[i].index - 1);
            }
        }
        free(f->inodes);
        f->inodes = bigger;
        f->inode_cap = grown;
    }
    inode_insert(f->inodes, f->inode_cap - 1, id, index);
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
    int rc;
    memset(f, 0, sizeof(*f));
    f->root = root;
    rc = attrs_read(root, &f->root_attrs);
    if (rc != 0) return rc;
    f->stream_bytes = ADAPT_ARCHIVE_MAGIC_BYTES + attrs_bytes(&f->root_attrs) + 1;
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
    enum { PACK_MAGIC, PACK_ROOT_ATTRS, PACK_HEADER, PACK_EXTENT, PACK_DATA, PACK_ATTRS, PACK_END, PACK_DONE } stage;
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
    memcpy(p->head, ADAPT_ARCHIVE_ROOT_MAGIC, ADAPT_ARCHIVE_MAGIC_BYTES);
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
static int packer_attrs(packer *p, const attrs *a) {
    size_t n = (size_t)attrs_bytes(a), at;
    uint32_t i;
    uint8_t *t = malloc(n);
    if (t == NULL) {
        p->error = ENOMEM;
        return ENOMEM;
    }
    at = adapt_archive_attributes(a->count, t, n);
    for (i = 0; i < a->count; i++) {
        at += adapt_archive_attribute(a->names[i], a->values[i], a->lens[i], t + at, n - at);
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
        if (cancelled()) return p->error = EINTR;
        if (p->stage == PACK_DONE) {
            break;
        }
        if (p->stage == PACK_ATTRS || p->stage == PACK_ROOT_ATTRS) {
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
            if (p->stage == PACK_ATTRS) p->index++;
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
                rc = packer_attrs(p, &p->f->root_attrs);
                p->stage = PACK_ROOT_ATTRS;
            } else if (p->stage == PACK_EXTENT) {
                p->stage = PACK_DATA;
            } else if (e->kind == KIND_FILE) {
                rc = packer_open(p);
                p->remaining = e->size;
                p->stage = PACK_DATA;
            } else if (e->kind == KIND_SPARSE) {
                rc = packer_open(p);
                if (rc == 0) {
                    rc = e->extent_count == 0 ? packer_attrs(p, &e->attrs) : packer_extent(p);
                }
            } else if (e->kind == KIND_HARDLINK) {
                p->index++;
                packer_next(p);
            } else {
                rc = packer_attrs(p, &e->attrs);
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
                rc = packer_attrs(p, &e->attrs);
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
 * path has passed the unpacker's syntax rules; filesystem parents still
 * have to be resolved without following an archive's symbolic links. */
/* A private, temporary journal retains attributes on disk. Each record
 * ends in a fixed footer so replay needs only one bounded attribute and
 * visits children before parents, without an in-memory directory index. */
typedef struct metadata_record {
    cadapt_off start;
    uint32_t path_bytes, attributes;
    int has_metadata;
    adapt_metadata metadata;
} metadata_record;

typedef struct restorer {
    const char *root;
#ifndef _WIN32
    int root_fd, entry_fd; /* retain the current file through ACL and metadata callbacks */
    unsigned file_mode, directory_mode; /* final modes for version 1 entries */
    uint8_t *file_acl; /* access ACL waits until other file attributes are set */
    size_t file_acl_len;
#endif
    FILE *journal;
    metadata_record pending;
    char *pending_path;
    FILE *file;
    uint64_t restored_size, max_output;
    int error; /* errno of the failure */
    char *failed; /* the entry's path, malloc'd, or NULL */
    const char *why; /* a failure the errno does not name, or NULL */
} restorer;

/* Reserve apparent file bytes before creating a file or link. Holes and
 * hard-link aliases count too; subtraction avoids cumulative overflow. */
static int restore_reserve(restorer *r, uint64_t size) {
    if (size > r->max_output - r->restored_size) {
        return EFBIG;
    }
    r->restored_size += size;
    return 0;
}

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

static int journal_write(FILE *file, const void *data, size_t len) {
    return fwrite(data, 1, len, file) == len ? 0 : (errno != 0 ? errno : EIO);
}

static int journal_read(FILE *file, void *data, size_t len) {
    return fread(data, 1, len, file) == len ? 0 : EIO;
}

static int journal_flush_record(restorer *r) {
    int rc = 0;
    if (r->pending_path != NULL) {
        rc = journal_write(r->journal, r->pending_path, r->pending.path_bytes);
        if (rc == 0) rc = journal_write(r->journal, &r->pending, sizeof(r->pending));
        free(r->pending_path);
        r->pending_path = NULL;
    }
    return rc;
}

static int journal_begin(restorer *r, const char *path) {
    size_t len = strlen(path) + 1;
    int rc = journal_flush_record(r);
    if (rc != 0) return rc;
    if (len > ADAPT_ARCHIVE_PATH_MAX + 1u) return ENAMETOOLONG;
    if (r->journal == NULL) {
        r->journal = tmpfile();
        if (r->journal == NULL) return errno != 0 ? errno : EIO;
    }
    memset(&r->pending, 0, sizeof(r->pending));
    r->pending.start = cadapt_ftell(r->journal);
    if (r->pending.start < 0) return errno != 0 ? errno : EIO;
    r->pending_path = malloc(len);
    if (r->pending_path == NULL) return ENOMEM;
    memcpy(r->pending_path, path, len);
    r->pending.path_bytes = (uint32_t)len;
    return 0;
}

static int journal_attribute(restorer *r, const char *name, const uint8_t *value, size_t len) {
    size_t name_len = strlen(name) + 1;
    uint32_t lengths[2];
    int rc;
    if (name_len > ADAPT_ARCHIVE_ATTRIBUTE_NAME_MAX + 1u || len > ADAPT_ARCHIVE_ATTRIBUTE_VALUE_MAX ||
        r->pending.attributes == UINT32_MAX) return EINVAL;
    lengths[0] = (uint32_t)name_len;
    lengths[1] = (uint32_t)len;
    rc = journal_write(r->journal, lengths, sizeof(lengths));
    if (rc == 0) rc = journal_write(r->journal, name, name_len);
    if (rc == 0) rc = journal_write(r->journal, value, len);
    if (rc == 0) r->pending.attributes++;
    return rc;
}

#ifndef _WIN32
/* Return the entry's parent descriptor, rooted in our private staging
 * directory. Every component must be a real directory, never a symlink.
 * The caller owns both the descriptor and the storage holding *leaf. */
static int restore_parent(restorer *r, const char *path, char **storage, char **leaf) {
    char *part, *slash;
    int parent;
    long path_limit;
    *storage = NULL;
    if (!adapt_archive_path_valid(path)) {
        errno = EINVAL;
        return -1;
    }
    /* Keep the CLI's existing path-length bound while traversing by fd. */
    path_limit = fpathconf(r->root_fd, _PC_PATH_MAX);
    if (path_limit < 0) {
#ifdef PATH_MAX
        path_limit = PATH_MAX;
#else
        errno = ENOTSUP;
        return -1;
#endif
    }
    if (strlen(r->root) + 1 + strlen(path) >= (size_t)path_limit) {
        errno = ENAMETOOLONG;
        return -1;
    }
    *storage = strdup(path);
    if (*storage == NULL) {
        return -1;
    }
    parent = fcntl(r->root_fd, F_DUPFD_CLOEXEC, 3);
    part = *storage;
    while (parent >= 0 && (slash = strchr(part, '/')) != NULL) {
        int next, e;
        *slash = '\0';
        next = openat(parent, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        e = errno;
        close(parent);
        parent = next;
        errno = e;
        part = slash + 1;
    }
    *leaf = part;
    return parent;
}
#else
/* Archive separators are '/', including on Windows. Reject native
 * separators and drive/stream syntax before handing paths to the CRT. */
static int restore_windows_path(const char *path) {
    return adapt_archive_path_valid(path) && strchr(path, '\\') == NULL && strchr(path, ':') == NULL;
}
#endif

/* Deferred records use archive-relative paths on both platforms. */
static int restore_apply_metadata(restorer *r, const char *path, int symlink_entry, const adapt_metadata *m,
                                  const attrs *attributes) {
#ifdef _WIN32
    char *full = join(r->root, path);
    int rc = full == NULL ? ENOMEM : attributes != NULL && attributes->count ? ENOTSUP
                                    : m == NULL ? 0 : metadata_apply(full, symlink_entry, m);
    free(full);
    return rc;
#else
    char *storage, *leaf;
    int parent = restore_parent(r, path, &storage, &leaf), rc;
    if (parent < 0) {
        rc = errno;
    } else if (symlink_entry) {
        struct timespec times[2];
        struct stat st;
        rc = metadata_times(m, times);
        if (rc == 0 && (utimensat(parent, leaf, times, AT_SYMLINK_NOFOLLOW) != 0 ||
                        fstatat(parent, leaf, &st, AT_SYMLINK_NOFOLLOW) != 0)) rc = errno;
        if (rc == 0) rc = metadata_check_stat(&st, m);
    } else {
        int fd = openat(parent, leaf, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        uint32_t i;
        rc = fd < 0 ? errno : 0;
        for (i = 0; attributes != NULL && i < attributes->count && rc == 0; i++) {
            rc = attr_apply(fd, attributes->names[i], attributes->values[i], attributes->lens[i]);
        }
        if (rc == 0) {
            if (m != NULL) rc = metadata_apply_fd(fd, m);
            else if (fchmod(fd, (mode_t)r->directory_mode) != 0) rc = errno;
        }
        if (fd >= 0) close(fd);
    }
    if (parent >= 0) close(parent);
    free(storage);
    return rc;
#endif
}

static int restore_apply_attribute(restorer *r, const char *path, int symlink_entry, const char *name,
                                    const uint8_t *value, size_t len) {
#ifdef _WIN32
    (void)r;
    (void)path;
    (void)symlink_entry;
    return attr_apply(-1, name, value, len);
#else
    char *storage, *leaf;
    int parent, fd = -1, rc, flags = O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC;
    if (symlink_entry) {
#ifdef __APPLE__
        flags |= O_SYMLINK;
#else
        /* Linux does not allow the preserved user/ACL namespaces on links. */
        return ENOTSUP;
#endif
    }
    parent = restore_parent(r, path, &storage, &leaf);
    if (parent >= 0) fd = openat(parent, leaf, flags);
    rc = fd < 0 ? errno : attr_apply(fd, name, value, len);
    if (fd >= 0) close(fd);
    if (parent >= 0) close(parent);
    free(storage);
    return rc;
#endif
}

static int restore_directory(void *context, const char *path) {
    restorer *r = context;
    int rc;
#ifdef _WIN32
    char *full;
    if (!restore_windows_path(path)) return restore_fail(r, path, EINVAL, "invalid archive path");
    full = join(r->root, path);
    if (full == NULL) {
        return restore_fail(r, path, ENOMEM, NULL);
    }
    rc = _mkdir(full) == 0 ? 0 : errno;
    free(full);
#else
    char *storage, *leaf;
    int parent = restore_parent(r, path, &storage, &leaf);
    rc = parent < 0 ? errno : (mkdirat(parent, leaf, 0700) == 0 ? 0 : errno);
    /* The entry was just created inside the private wrapper. Grant the
     * owner access even with umask 0777; final modes wait for children. */
    if (rc == 0 && fchmodat(parent, leaf, 0700, 0) != 0) rc = errno;
    if (parent >= 0) close(parent);
    free(storage);
#endif
    if (rc != 0) {
        return restore_fail(r, path, rc, rc == EEXIST ? "named twice in the archive" : NULL);
    }
    rc = journal_begin(r, path);
    return rc != 0 ? restore_fail(r, path, rc, "cannot defer directory metadata") : 0;
}

static int restore_file_open(void *context, const char *path, uint64_t size) {
    restorer *r = context;
    int fd, e;
    if (restore_reserve(r, size) != 0) {
        return restore_fail(r, path, EFBIG, "restored file sizes exceed --max-output");
    }
#ifdef _WIN32
    char *full;
    if (!restore_windows_path(path)) return restore_fail(r, path, EINVAL, "invalid archive path");
    full = join(r->root, path);
    if (full == NULL) {
        return restore_fail(r, path, ENOMEM, NULL);
    }
    /* exclusive: an archive that names a file twice is refused */
    fd = _open(full, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
    e = errno;
    free(full);
#else
    char *storage, *leaf;
    int parent = restore_parent(r, path, &storage, &leaf);
    fd = parent < 0 ? -1 : openat(parent, leaf, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    e = errno;
    if (fd >= 0 && fchmod(fd, 0600) != 0) {
        e = errno;
        close(fd);
        fd = -1;
    }
    if (parent >= 0) close(parent);
    free(storage);
#endif
    if (fd < 0) {
        return restore_fail(r, path, e, e == EEXIST ? "named twice in the archive" : NULL);
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

#ifndef _WIN32
/* Metadata callbacks close the handle on newer archives. A remaining
 * handle belongs to a version 1 file, or an entry whose parsing failed. */
static int finish_legacy_file(restorer *r, int apply) {
    int rc = 0;
    free(r->file_acl);
    r->file_acl = NULL;
    if (r->entry_fd >= 0) {
        if (apply && fchmod(r->entry_fd, (mode_t)r->file_mode) != 0) rc = errno;
        close(r->entry_fd);
        r->entry_fd = -1;
    }
    return rc;
}
#endif

static int restore_file_close(void *context) {
    restorer *r = context;
    FILE *f = r->file;
    r->file = NULL;
#ifndef _WIN32
    /* ACLs can remove owner read permission. Reopening by name afterward
     * would reject a valid archive; keep this inode open until metadata. */
    r->error = finish_legacy_file(r, 1);
    if (r->error != 0) {
        fclose(f);
        return r->error;
    }
    r->entry_fd = fcntl(fileno(f), F_DUPFD_CLOEXEC, 3);
    if (r->entry_fd < 0) {
        r->error = errno;
        fclose(f);
        return r->error;
    }
#endif
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
    char *storage, *leaf;
    int parent = restore_parent(r, path, &storage, &leaf);
    int rc = parent < 0 ? errno : (symlinkat(target, parent, leaf) == 0 ? 0 : errno);
    if (parent >= 0) close(parent);
    free(storage);
    if (rc != 0) {
        return restore_fail(r, path, rc, rc == EEXIST ? "named twice in the archive" : NULL);
    }
    return 0;
#endif
}

/* A hard link: the same inode under another name. Where the platform has
 * no link(), the target's bytes are copied instead, which the README
 * states. */
static int restore_hardlink(void *context, const char *path, const char *target) {
    restorer *r = context;
    int rc;
#ifdef _WIN32
    char *full = join(r->root, path);
    char *to = join(r->root, target);
    if (!restore_windows_path(path) || !restore_windows_path(target)) {
        free(full);
        free(to);
        return restore_fail(r, path, EINVAL, "invalid archive path");
    }
    if (full == NULL || to == NULL) {
        free(full);
        free(to);
        return restore_fail(r, path, ENOMEM, NULL);
    }
    {
        FILE *in = fopen(to, "rb"), *out = NULL;
        struct _stat64 st;
        uint8_t buf[65536];
        size_t n;
        rc = 0;
        if (in == NULL) {
            rc = errno;
        } else if (_fstat64(_fileno(in), &st) != 0) {
            rc = errno;
        } else if ((st.st_mode & _S_IFMT) != _S_IFREG || st.st_size < 0) {
            rc = EINVAL;
        } else {
            rc = restore_reserve(r, (uint64_t)st.st_size);
        }
        if (rc == 0) {
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
    free(full);
    free(to);
#else
    char *storage, *leaf, *target_storage = NULL, *target_leaf;
    int parent = restore_parent(r, path, &storage, &leaf), source = -1;
    struct stat st;
    rc = parent < 0 ? errno : 0;
    if (rc == 0) {
        source = restore_parent(r, target, &target_storage, &target_leaf);
        if (source < 0) rc = errno;
        else if (fstatat(source, target_leaf, &st, AT_SYMLINK_NOFOLLOW) != 0) rc = errno;
        else if (!S_ISREG(st.st_mode) || st.st_size < 0) rc = EINVAL;
        else {
            rc = restore_reserve(r, (uint64_t)st.st_size);
            if (rc == 0 && linkat(source, target_leaf, parent, leaf, 0) != 0) rc = errno;
        }
    }
    if (source >= 0) close(source);
    if (parent >= 0) close(parent);
    free(target_storage);
    free(storage);
#endif
    if (rc != 0) {
        return restore_fail(r, path, rc, rc == EFBIG ? "restored file sizes exceed --max-output" :
                            rc == EEXIST ? "named twice in the archive" : "cannot restore hard link");
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
        if (r->pending_path == NULL || strcmp(r->pending_path, path) != 0) {
            return restore_fail(r, path, EINVAL, "directory attributes without an entry");
        }
        rc = journal_attribute(r, name, value, len);
        return rc != 0 ? restore_fail(r, path, rc, "cannot defer directory metadata") : 0;
    }
#ifndef _WIN32
    if (kind == ADAPT_ARCHIVE_FILE || kind == ADAPT_ARCHIVE_SPARSE) {
        if (strcmp(name, "system.posix_acl_access") == 0) {
            uint8_t *acl = malloc(len + 1);
            if (acl == NULL) return restore_fail(r, path, ENOMEM, NULL);
            memcpy(acl, value, len);
            free(r->file_acl);
            r->file_acl = acl;
            r->file_acl_len = len;
            return 0;
        }
        rc = attr_apply(r->entry_fd, name, value, len);
    } else
#endif
    {
        rc = restore_apply_attribute(r, path, kind == ADAPT_ARCHIVE_SYMLINK, name, value, len);
    }
    return rc != 0 ? restore_fail(r, path, rc, "cannot restore extended attribute") : 0;
}

/* Root attributes precede the children on the wire but take effect last. */
static int restore_root_attribute(void *context, const char *name, const uint8_t *value, size_t len) {
    restorer *r = context;
    int rc = 0;
    if (r->pending_path == NULL) rc = journal_begin(r, "");
    if (rc == 0 && r->pending_path[0] != '\0') rc = EINVAL;
    if (rc == 0) rc = journal_attribute(r, name, value, len);
    return rc != 0 ? restore_fail(r, "root", rc, "cannot defer root metadata") : 0;
}

static int restore_metadata(void *context, const char *path, uint8_t kind, const adapt_metadata *metadata) {
    restorer *r = context;
    int rc;
    if (kind == ADAPT_ARCHIVE_DIRECTORY) {
        if (r->pending_path == NULL || strcmp(r->pending_path, path) != 0) {
            return restore_fail(r, path, EINVAL, "directory metadata without an entry");
        }
        r->pending.metadata = *metadata;
        r->pending.has_metadata = 1;
        return 0;
    }
#ifndef _WIN32
    if (kind == ADAPT_ARCHIVE_FILE || kind == ADAPT_ARCHIVE_SPARSE) {
        rc = r->file_acl == NULL ? 0 : attr_apply(r->entry_fd, "system.posix_acl_access", r->file_acl, r->file_acl_len);
        free(r->file_acl);
        r->file_acl = NULL;
        if (rc == 0) rc = metadata_apply_fd(r->entry_fd, metadata);
        close(r->entry_fd);
        r->entry_fd = -1;
    } else
#endif
    {
        rc = restore_apply_metadata(r, path, kind == ADAPT_ARCHIVE_SYMLINK, metadata, NULL);
    }
    return rc != 0 ? restore_fail(r, path, rc, "cannot restore metadata") : 0;
}

/* Only a checksummed archive may replay the journal. The workspace is
 * bounded by the wire limits, independent of entry or attribute count. */
static int finish_directory_metadata(restorer *r, int apply) {
    int rc = 0;
    cadapt_off end = 0;
    char path[ADAPT_ARCHIVE_PATH_MAX + 1u];
    char name[ADAPT_ARCHIVE_ATTRIBUTE_NAME_MAX + 1u];
    uint8_t value[ADAPT_ARCHIVE_ATTRIBUTE_VALUE_MAX];
    if (apply && r->journal != NULL) {
        rc = journal_flush_record(r);
        if (rc == 0 && fflush(r->journal) != 0) rc = errno != 0 ? errno : EIO;
        if (rc == 0) {
            end = cadapt_ftell(r->journal);
            if (end < 0) rc = EIO;
        }
    }
    while (apply && rc == 0 && end > 0) {
        metadata_record record;
        cadapt_off path_at;
        uint32_t i;
#ifndef _WIN32
        int fd = r->root_fd;
        cadapt_off acl_at = -1;
        uint32_t acl_len = 0;
#endif
        if (end < (cadapt_off)sizeof(record) ||
            cadapt_fseek(r->journal, end - (cadapt_off)sizeof(record), SEEK_SET) != 0 ||
            journal_read(r->journal, &record, sizeof(record)) != 0) { rc = EIO; break; }
        path_at = end - (cadapt_off)sizeof(record) - record.path_bytes;
        if (record.path_bytes == 0 || record.path_bytes > sizeof(path) || record.start < 0 ||
            record.start > path_at || cadapt_fseek(r->journal, path_at, SEEK_SET) != 0 ||
            journal_read(r->journal, path, record.path_bytes) != 0 || path[record.path_bytes - 1] != '\0' ||
            strlen(path) + 1 != record.path_bytes || cadapt_fseek(r->journal, record.start, SEEK_SET) != 0) {
            rc = EIO; break;
        }
#ifndef _WIN32
        /* Retain the directory before an access ACL can remove our read
         * permission. Its timestamps/mode must use this same handle. */
        if (path[0] != '\0') {
            char *storage, *leaf;
            int parent = restore_parent(r, path, &storage, &leaf);
            fd = parent < 0 ? -1 : openat(parent, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0) rc = errno;
            if (parent >= 0) close(parent);
            free(storage);
        }
#endif
        for (i = 0; i < record.attributes && rc == 0; i++) {
            uint32_t lengths[2];
            cadapt_off at = cadapt_ftell(r->journal);
            if (at < 0 || path_at - at < (cadapt_off)sizeof(lengths) ||
                journal_read(r->journal, lengths, sizeof(lengths)) != 0 ||
                lengths[0] == 0 || lengths[0] > sizeof(name) || lengths[1] > sizeof(value) ||
                (uint64_t)(path_at - at - (cadapt_off)sizeof(lengths)) < (uint64_t)lengths[0] + lengths[1] ||
                journal_read(r->journal, name, lengths[0]) != 0 || name[lengths[0] - 1] != '\0' ||
                strlen(name) + 1 != lengths[0] || journal_read(r->journal, value, lengths[1]) != 0) {
                rc = EIO; break;
            }
#ifdef _WIN32
            rc = attr_apply(-1, name, value, lengths[1]);
#else
            if (strcmp(name, "system.posix_acl_access") == 0) {
                /* Replay this one value last without retaining a second
                 * attribute buffer. Its mode may remove owner write access. */
                acl_at = at + (cadapt_off)sizeof(lengths) + lengths[0];
                acl_len = lengths[1];
            } else {
                rc = attr_apply(fd, name, value, lengths[1]);
            }
#endif
        }
        if (rc == 0 && cadapt_ftell(r->journal) != path_at) rc = EIO;
#ifndef _WIN32
        if (rc == 0 && acl_at >= 0) {
            if (cadapt_fseek(r->journal, acl_at, SEEK_SET) != 0 ||
                journal_read(r->journal, value, acl_len) != 0) rc = EIO;
            else rc = attr_apply(fd, "system.posix_acl_access", value, acl_len);
        }
#endif
        if (rc == 0 && path[0] != '\0') {
#ifdef _WIN32
            rc = restore_apply_metadata(r, path, 0, record.has_metadata ? &record.metadata : NULL, NULL);
#else
            if (record.has_metadata) rc = metadata_apply_fd(fd, &record.metadata);
            else if (fchmod(fd, (mode_t)r->directory_mode) != 0) rc = errno;
#endif
        }
#ifndef _WIN32
        if (path[0] != '\0' && fd >= 0) close(fd);
#endif
        if (rc != 0) restore_fail(r, path[0] ? path : "root", rc,
                                  path[0] ? "cannot restore directory metadata or attributes" : "cannot restore root extended attribute");
        end = record.start;
    }
    if (r->journal != NULL && fclose(r->journal) != 0 && rc == 0 && apply) rc = errno != 0 ? errno : EIO;
    r->journal = NULL;
    free(r->pending_path);
    r->pending_path = NULL;
    if (rc != 0 && r->error == 0) restore_fail(r, "metadata journal", rc, "cannot read deferred metadata");
    return rc;
}

#ifdef _WIN32
/* Remove a directory we created and everything under it. Only the staging
 * directory is ever handed here. */
static void remove_tree(const char *path) {
    char **names = NULL;
    size_t count = 0, i;
    (void)_chmod(path, _S_IREAD | _S_IWRITE);
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
    _rmdir(path);
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
        /* Keep other users out until all entries and metadata are final. */
        rc = mkdir(path, 0700);
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
#else
/* Descendants are protected by the private wrapper, including after their
 * recorded modes have been applied. Never recurse through the wrapper's
 * mutable pathname or through a symbolic link from the archive. */
static void remove_tree_fd(int fd) {
    DIR *directory;
    struct dirent *entry;
    int scan;
    (void)fchmod(fd, 0700);
    scan = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (scan < 0) return;
    directory = fdopendir(scan);
    if (directory == NULL) {
        close(scan);
        return;
    }
    while ((entry = readdir(directory)) != NULL) {
        struct stat st;
        const char *name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            int child;
            /* No other user can replace descendants inside the wrapper.
             * Restore owner access when recorded directory mode was 000. */
            if (fchmodat(fd, name, 0700, 0) != 0) continue;
            child = openat(fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child >= 0) {
                remove_tree_fd(child);
                close(child);
                (void)unlinkat(fd, name, AT_REMOVEDIR);
            }
        } else {
            (void)unlinkat(fd, name, 0);
        }
    }
    closedir(directory);
}

static void staged_dir_release(staged *s) {
    if (s->root_fd >= 0) close(s->root_fd);
    if (s->guard_fd >= 0) {
        struct stat held, named;
        const char *name = strrchr(s->path, '/');
        name = name == NULL ? s->path : name + 1;
        /* A moved wrapper can be left empty. Do not chase it or touch a
         * replacement. The final unlink itself never follows a symlink. */
        if (fstat(s->guard_fd, &held) == 0 &&
            fstatat(s->parent_fd, name, &named, AT_SYMLINK_NOFOLLOW) == 0 &&
            held.st_dev == named.st_dev && held.st_ino == named.st_ino) {
            (void)unlinkat(s->parent_fd, name, AT_REMOVEDIR);
        }
        close(s->guard_fd);
    }
    if (s->parent_fd >= 0) close(s->parent_fd);
    s->root_fd = s->guard_fd = s->parent_fd = -1;
    free(s->target_name);
    s->target_name = NULL;
    free(s->path);
    s->path = NULL;
}

static void staged_dir_discard(staged *s) {
    if (s->root_fd >= 0) {
        if (s->directory) remove_tree_fd(s->root_fd);
        (void)unlinkat(s->guard_fd, "contents", s->directory ? AT_REMOVEDIR : 0);
    }
    staged_dir_release(s);
}

static int staged_guard_open(staged *s, const char *target, int force, int directory) {
    const char *slash = strrchr(target, '/');
    size_t dir_len = slash == NULL ? 0 : (size_t)(slash - target) + 1;
    char *parent_path = malloc(dir_len + 2);
    int attempt, rc = 0;
    struct stat st;
    s->directory = directory;
    s->path = NULL;
    s->file = NULL;
    s->parent_fd = s->guard_fd = s->root_fd = -1;
    s->target_name = strdup(target + dir_len);
    if (parent_path == NULL || s->target_name == NULL) {
        rc = ENOMEM;
    } else {
        if (dir_len != 0) memcpy(parent_path, target, dir_len);
        else parent_path[0] = '.';
        parent_path[dir_len != 0 ? dir_len : 1] = '\0';
        s->parent_fd = open(parent_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (s->parent_fd < 0) rc = errno;
    }
    free(parent_path);
    if (rc == 0 && !force) {
        if (fstatat(s->parent_fd, s->target_name, &st, AT_SYMLINK_NOFOLLOW) == 0) rc = EEXIST;
        else if (errno != ENOENT) rc = errno;
    }
    for (attempt = 0; rc == 0 && attempt < 128; attempt++) {
        char name[64];
        mode_t mask;
        int made;
        snprintf(name, sizeof(name), ".cadapt-%ld-%d.part", (long)cadapt_getpid(), attempt);
        s->path = malloc(dir_len + strlen(name) + 1);
        if (s->path == NULL) {
            rc = ENOMEM;
            break;
        }
        memcpy(s->path, target, dir_len);
        strcpy(s->path + dir_len, name);
        /* This command has not started workers yet. Owner access must not
         * depend on umask; the wrapper stays 0700 until it is removed. */
        mask = umask(0);
        made = mkdirat(s->parent_fd, name, 0700);
        rc = made == 0 ? 0 : errno;
        umask(mask);
        if (rc == EEXIST) {
            free(s->path);
            s->path = NULL;
            rc = 0;
            continue;
        }
        if (rc != 0) break;
        s->guard_fd = openat(s->parent_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (s->guard_fd < 0) rc = errno;
        else if (fstat(s->guard_fd, &st) != 0) rc = errno;
        else if (st.st_uid != geteuid() || (st.st_mode & 0777) != 0700) rc = EACCES;
        if (rc != 0) {
            if (s->guard_fd >= 0) close(s->guard_fd);
            s->guard_fd = -1;
            break;
        }
        if (directory) {
            mask = umask(0);
            made = mkdirat(s->guard_fd, "contents", 0700);
            rc = made == 0 ? 0 : errno;
            umask(mask);
            if (rc != 0) break;
            s->root_fd = openat(s->guard_fd, "contents", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        } else {
            s->root_fd = openat(s->guard_fd, "contents", O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        }
        if (s->root_fd < 0) {
            rc = errno;
            (void)unlinkat(s->guard_fd, "contents", directory ? AT_REMOVEDIR : 0);
        } else {
            rc = clear_inherited_acl(s->root_fd);
            if (rc == 0 && fchmod(s->root_fd, directory ? 0700 : 0600) != 0) rc = errno;
            if (rc == 0 && !directory) {
                int fd = dup(s->root_fd);
                s->file = fd < 0 ? NULL : fdopen(fd, "wb");
                if (s->file == NULL) {
                    rc = errno;
                    if (fd >= 0) close(fd);
                }
            }
        }
        if (rc == 0) return 0;
    }
    if (rc == 0) rc = EEXIST;
    staged_dir_discard(s);
    return other_error(target, rc == EEXIST ? "exists; pass --force to overwrite"
                                           : strerror(rc));
}
static int staged_dir_open(staged *s, const char *target, int force) {
    return staged_guard_open(s, target, force, 1);
}

#endif

/* POSIX rename() may replace a directory created after the initial check.
 * Refuse safely when the platform cannot make the no-replace operation atomic. */
static int staged_dir_rename(staged *s, const char *target, int force, int *exchanged) {
    *exchanged = 0;
#ifdef _WIN32
    (void)force;
    return rename(s->path, target); /* Windows rename refuses existing names. */
#else
    (void)target;
    if (force) {
        struct stat previous;
        int rc;
        /* Rename already replaces an empty directory atomically. A file
         * needs an exchange because rename cannot replace unlike types. */
        rc = renameat(s->guard_fd, "contents", s->parent_fd, s->target_name);
        if (rc == 0 || (errno != ENOTDIR && errno != EISDIR)) return rc;
        if (fstatat(s->parent_fd, s->target_name, &previous, AT_SYMLINK_NOFOLLOW) != 0) return -1;
        if (S_ISDIR(previous.st_mode)) {
            errno = EEXIST; /* the destination changed; keep the new directory */
            return -1;
        }
#if defined(__linux__) && defined(RENAME_EXCHANGE)
        rc = renameat2(s->guard_fd, "contents", s->parent_fd, s->target_name, RENAME_EXCHANGE);
#elif defined(__APPLE__) && defined(RENAME_SWAP)
        rc = renameatx_np(s->guard_fd, "contents", s->parent_fd, s->target_name, RENAME_SWAP);
#else
        errno = ENOTSUP;
        rc = -1;
#endif
        if (rc == 0) *exchanged = 1;
        return rc;
    }
#if defined(__linux__) && defined(RENAME_NOREPLACE)
    return renameat2(s->guard_fd, "contents", s->parent_fd, s->target_name, RENAME_NOREPLACE);
#elif defined(__APPLE__) && defined(RENAME_EXCL)
    return renameatx_np(s->guard_fd, "contents", s->parent_fd, s->target_name, RENAME_EXCL);
#else
    errno = ENOTSUP;
    return -1;
#endif
#endif
}

/* Publish the staged directory as `target`: with --force an existing file
 * or empty directory is replaced; a directory with contents is not, so no
 * bytes of the user's are ever removed here. */
static int staged_dir_publish(staged *s, const char *target, int force) {
    int exchanged;
#ifndef _WIN32
    struct stat root;
    mode_t root_mode;
    int restore_mode;
#endif
    if (cancelled()) {
        staged_dir_discard(s);
        return other_error(target, "interrupted");
    }
#ifndef _WIN32
    if (fstat(s->root_fd, &root) != 0) {
        int e = errno;
        staged_dir_discard(s);
        return other_error(target, strerror(e));
    }
    root_mode = root.st_mode & 07777;
    restore_mode = !(root_mode & S_IWUSR);
    /* Moving a directory between parents can require write permission on
     * the directory itself. Grant it only to its owner, who already can
     * chmod it, then restore the validated mode through the held handle. */
    if (restore_mode && fchmod(s->root_fd, root_mode | S_IWUSR) != 0) {
        int e = errno;
        staged_dir_discard(s);
        return other_error(target, strerror(e));
    }
#endif
    if (staged_dir_rename(s, target, force, &exchanged) != 0) {
        int e = errno;
        staged_dir_discard(s);
        if (!force && (e == ENOSYS || e == EINVAL || e == ENOTSUP)) {
            return other_error(target, "atomic directory publication is unavailable; pass --force to allow replacement");
        }
        if (force && (e == ENOSYS || e == EINVAL || e == ENOTSUP)) {
            return other_error(target, "atomic folder replacement is unavailable; existing target preserved");
        }
        if (force && e == ENOTEMPTY) return other_error(target, "exists and is not empty; remove it first");
        if (force && e == EEXIST) return other_error(target, "existing directory changed; target preserved");
        return other_error(target, e == EEXIST || e == ENOTEMPTY ? "exists; pass --force to overwrite" : strerror(e));
    }
#ifdef _WIN32
    free(s->path);
    s->path = NULL;
#else
    if (restore_mode) {
        int e = 0;
        if (fchmod(s->root_fd, root_mode) != 0 || fstat(s->root_fd, &root) != 0) e = errno;
        else if ((root.st_mode & 07777) != root_mode) e = ENOTSUP;
        if (e != 0) {
            char message[512];
            /* Publication already succeeded. Recursive rollback here
             * could remove files another process added to the output. */
            snprintf(message, sizeof(message), "folder published but cannot restore root mode: %s%s%s", strerror(e),
                     exchanged ? "; previous target retained in staging wrapper " : "", exchanged ? s->path : "");
            staged_dir_release(s);
            return other_error(target, message);
        }
    }
    if (exchanged && unlinkat(s->guard_fd, "contents", 0) != 0) {
        char message[512];
        /* Never recurse here: a concurrent writer may have changed the
         * destination to a directory before the exchange. Preserve it. */
        snprintf(message, sizeof(message), "folder published; previous target retained in staging wrapper %s: %s",
                 s->path, strerror(errno));
        staged_dir_release(s);
        return other_error(target, message);
    }
    staged_dir_release(s);
#endif
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
static int read_whole(replay *r, size_t cap, uint8_t **buf, size_t *len) {
    size_t at = 0, got;
    uint8_t *b = malloc(cap);
    if (b == NULL) {
        return ENOMEM;
    }
    for (;;) {
        int rc;
        if (at == cap) {
            uint8_t extra;
            /* The source may have grown since its size was checked.
             * Refuse growth instead of allocating past the reservation. */
            rc = read_replay(r, &extra, 1, &got);
            if (rc != 0 || got != 0) {
                free(b);
                return rc != 0 ? rc : EFBIG;
            }
            break;
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

/* Only regular inputs have a bounded size we can reserve before reading.
 * Pipes keep the streaming decoder and never accumulate an input buffer. */
static int replay_length(replay *r, size_t *length) {
    cadapt_off position = cadapt_ftell(r->in->file);
    uint64_t remaining;
#ifdef _WIN32
    struct _stat64 st;
    if (position < 0 || _fstat64(_fileno(r->in->file), &st) != 0 || (st.st_mode & _S_IFMT) != _S_IFREG) return 0;
#else
    struct stat st;
    if (position < 0 || fstat(fileno(r->in->file), &st) != 0 || !S_ISREG(st.st_mode)) return 0;
#endif
    if (st.st_size < position) return 0;
    remaining = (uint64_t)(st.st_size - position) + (r->held_len - r->held_at);
    if (remaining > SIZE_MAX || remaining == 0) return 0;
    *length = (size_t)remaining;
    return 1;
}

/* Decode through the stream decoder, or — for a segmented file on two or
 * more threads — the whole input read into memory and its segments
 * decoded at once; `stats->workers` says which. */
static adapt_status decode_input(replay *ahead, uint64_t threads, adapt_sizes sizes, adapt_limits limits,
                                 adapt_write_fn write, void *write_context, adapt_stats *stats, adapt_error *error) {
    uint8_t *whole;
    size_t whole_len, capacity;
    uint64_t stream_memory = adapt_decode_memory_bound(sizes);
    adapt_status status;
    int rc;
    if (threads < 2 || !sizes.segmented || stream_memory > limits.max_memory ||
        !replay_length(ahead, &capacity) || capacity > limits.max_memory - stream_memory) {
        return adapt_decode_stream(read_replay, ahead, write, write_context, limits, stats, error);
    }
    rc = read_whole(ahead, capacity, &whole, &whole_len);
    if (rc != 0) {
        ahead->in->error = rc;
        memset(error, 0, sizeof(*error));
        error->status = ADAPT_ERROR_IO;
        return ADAPT_ERROR_IO;
    }
    /* The library counts the file length; a concurrently shortened source
     * still occupies the original reservation until this buffer is freed. */
    limits.max_memory -= capacity - whole_len;
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

/* Command-owned resources. The output stream borrows the staged file;
 * only the stage closes it. Standard input/output remain caller-owned. */
typedef struct command_io {
    const char *input;
    char *output;
    stream in, out;
    staged stage;
    int directory_output;
} command_io;

static void command_close_input(command_io *io) {
    if (io->in.file != NULL && io->in.file != stdin) {
        fclose(io->in.file);
    }
    io->in.file = NULL;
}

static void command_io_close(command_io *io) {
    command_close_input(io);
    if (io->directory_output) {
        if (io->stage.path != NULL) staged_dir_discard(&io->stage);
    } else {
        staged_discard(&io->stage);
    }
    free(io->output);
    io->output = NULL;
}

static int command_output_name(command_io *io, const char *explicit_name, const char *base, int decode) {
    const char *suffix = "";
    size_t n;
    if (explicit_name != NULL) {
        base = explicit_name;
    } else if (strcmp(base, "-") != 0) {
        suffix = decode ? ".out" : ".adpt";
    }
    n = strlen(base);
    if (decode && explicit_name == NULL && n > 5 && strcmp(base + n - 5, ".adpt") == 0) {
        n -= 5;
        suffix = "";
    }
    io->output = malloc(n + strlen(suffix) + 1);
    if (io->output == NULL) return other_error(io->input, "memory allocation failed");
    memcpy(io->output, base, n);
    strcpy(io->output + n, suffix);
    return 0;
}

static int command_open_input(command_io *io) {
    io->in.file = strcmp(io->input, "-") == 0 ? stdin : fopen(io->input, "rb");
    return io->in.file == NULL ? other_error(io->input, strerror(errno)) : 0;
}

static int command_open_output(command_io *io, int force) {
    int rc;
    if (strcmp(io->output, "-") == 0) {
        io->out.file = stdout;
        return 0;
    }
    rc = staged_open(&io->stage, io->output, force);
    if (rc == 0) io->out.file = io->stage.file;
    return rc;
}

static adapt_status command_flush_output(command_io *io, adapt_status status, adapt_error *error) {
    if (status == ADAPT_OK && io->out.file == stdout && fflush(stdout) != 0) {
        io->out.error = errno;
        error->status = ADAPT_ERROR_IO;
        return ADAPT_ERROR_IO;
    }
    return status;
}

typedef struct decode_options {
    adapt_limits limits;
    uint64_t threads;
    int force;
} decode_options;

typedef struct decode_job {
    decode_options options;
    command_io io;
    adapt_sizes sizes;
    replay ahead;
    adapt_metadata metadata;
    int has_metadata;
    adapt_stats stats;
} decode_job;

static int decode_parse_options(decode_job *job, int argc, char **argv) {
    static const char *const with_value[] = {"-o", "--max-output", "--max-memory", "--threads", NULL};
    static const char *const known[] = {"--force", NULL};
    decode_options *options = &job->options;
    args a;
    const char *text;
    int rc = parse_args(&a, argc, argv, with_value);
    if (rc != 0) return rc;
    rc = reject_unknown(&a, known);
    if (rc != 0) return rc;
    if (a.positionals != 1) return usage_error("decode takes exactly one input%s", "");
    job->io.input = a.positional;
    options->threads = thread_count(&a);
    if (options->threads == 0) {
        return usage_error("--threads: not a count of one or more: %s", value_of(&a, "--threads"));
    }
    if (options->threads >= 2) options->threads = running_threads(options->threads);
    /* FMT-6: a container names its model. Only explicit CLI caps may
     * refuse it; the library's conservative defaults do not apply here. */
    options->limits = adapt_default_limits();
    options->limits.max_memory = UINT64_MAX;
    options->limits.max_output = UINT64_MAX;
    text = value_of(&a, "--max-output");
    if (text != NULL && !parse_bytes(text, &options->limits.max_output)) {
        return usage_error("--max-output: not a byte count: %s", text);
    }
    text = value_of(&a, "--max-memory");
    if (text != NULL && !parse_bytes(text, &options->limits.max_memory)) {
        return usage_error("--max-memory: not a byte count: %s", text);
    }
    options->force = has_flag(&a, "--force");
    return command_output_name(&job->io, value_of(&a, "-o"), job->io.input, 1);
}

static adapt_status decode_finish_archive(decode_job *job, restorer *restore, adapt_status status) {
    int rc;
    if (restore->file != NULL) {
        fclose(restore->file);
        restore->file = NULL;
    }
#ifndef _WIN32
    rc = finish_legacy_file(restore, status == ADAPT_OK);
    if (status == ADAPT_OK && rc != 0) {
        (void)restore_fail(restore, "file", rc, "cannot restore legacy file mode");
        status = ADAPT_ERROR_IO;
    }
#endif
    rc = finish_directory_metadata(restore, status == ADAPT_OK);
    if (status == ADAPT_OK && rc != 0) status = ADAPT_ERROR_IO;
    if (status == ADAPT_OK && job->has_metadata) {
#ifdef _WIN32
        rc = metadata_apply(job->io.stage.path, 0, &job->metadata);
#else
        rc = metadata_apply_fd(job->io.stage.root_fd, &job->metadata);
#endif
        if (rc != 0) {
            (void)restore_fail(restore, "root", rc, "cannot restore directory metadata");
            status = ADAPT_ERROR_IO;
        }
    }
#ifndef _WIN32
    /* Legacy roots take their umask-derived mode only after staging. */
    if (status == ADAPT_OK && !job->has_metadata &&
        fchmod(job->io.stage.root_fd, (mode_t)restore->directory_mode) != 0) {
        (void)restore_fail(restore, "root", errno, "cannot restore directory mode");
        status = ADAPT_ERROR_IO;
    }
#endif
    return status;
}

static int decode_archive_failure(const decode_job *job, const restorer *restore,
                                  const adapt_archive_unpacker *unpacker, adapt_status status,
                                  const adapt_error *error) {
    char message[400];
    uint64_t at = 0;
    const char *problem = adapt_archive_problem(unpacker, &at);
    if (status == ADAPT_ERROR_IO && restore->failed != NULL) {
        snprintf(message, sizeof(message), "%s: %s", restore->failed,
                 restore->why != NULL ? restore->why : strerror(restore->error != 0 ? restore->error : EIO));
    } else if (status == ADAPT_ERROR_IO && restore->error != 0) {
        snprintf(message, sizeof(message), "%s", strerror(restore->error));
    } else if (status == ADAPT_ERROR_IO && problem != NULL) {
        snprintf(message, sizeof(message), "invalid archive at byte %llu: %s", (unsigned long long)at, problem);
    } else if (status == ADAPT_ERROR_BAD_ARCHIVE) {
        snprintf(message, sizeof(message), "invalid archive at byte %llu: %s", (unsigned long long)at,
                 problem != NULL ? problem : "cut short");
    } else if (status == ADAPT_ERROR_IO) {
        snprintf(message, sizeof(message), "i/o error: %s", job->io.in.error != 0 ? strerror(job->io.in.error) : "callback failed");
    } else {
        adapt_error_message(error, message, sizeof(message));
    }
    return other_error(job->io.input, message);
}

static int decode_archive(decode_job *job) {
    command_io *io = &job->io;
    restorer restore = {0};
    adapt_archive_sink sink;
    adapt_archive_unpacker *unpacker;
    adapt_status status;
    adapt_error error;
    int rc;
    if (strcmp(io->output, "-") == 0) {
        return other_error(io->input, "holds a folder, which cannot be written to standard output; give -o <directory>");
    }
    io->directory_output = 1;
    rc = staged_dir_open(&io->stage, io->output, job->options.force);
    if (rc != 0) return rc;
    restore.root = io->stage.path;
#ifndef _WIN32
    restore.root_fd = io->stage.root_fd;
    restore.entry_fd = -1;
    {
        mode_t mask = umask(0);
        umask(mask);
        restore.file_mode = (unsigned)(0666 & ~mask);
        restore.directory_mode = (unsigned)(0777 & ~mask);
    }
#endif
    restore.max_output = job->options.limits.max_output;
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
    if (unpacker == NULL) return other_error(io->input, "memory allocation failed");
    adapt_archive_set_metadata_callback(unpacker, restore_metadata);
    adapt_archive_set_root_attribute_callback(unpacker, restore_root_attribute);
    status = decode_input(&job->ahead, job->options.threads, job->sizes, job->options.limits,
                          adapt_archive_unpack, unpacker, &job->stats, &error);
    if (status == ADAPT_OK) status = adapt_archive_finish(unpacker, &error);
    command_close_input(io);
    status = decode_finish_archive(job, &restore, status);
    if (status != ADAPT_OK) {
        rc = decode_archive_failure(job, &restore, unpacker, status, &error);
    } else {
        rc = staged_dir_publish(&io->stage, io->output, job->options.force);
        if (rc == 0) {
            fprintf(stderr, "%llu bytes -> %llu bytes, checksum verified, %llu entries restored to %s\n",
                    (unsigned long long)job->stats.output_bytes, (unsigned long long)job->stats.input_bytes,
                    (unsigned long long)adapt_archive_entries(unpacker), io->output);
        }
    }
    adapt_archive_unpacker_free(unpacker);
    free(restore.failed);
    return rc;
}

static int decode_file(decode_job *job) {
    command_io *io = &job->io;
    adapt_error error;
    adapt_status status;
    int rc = command_open_output(io, job->options.force);
    if (rc != 0) return rc;
    status = decode_input(&job->ahead, job->options.threads, job->sizes, job->options.limits,
                          write_stream, &io->out, &job->stats, &error);
    status = command_flush_output(io, status, &error);
    command_close_input(io);
    if (status != ADAPT_OK) return codec_failure(io->input, &error, &io->in, &io->out);
    if (io->stage.file != NULL) {
        rc = staged_publish(&io->stage, io->output, job->options.force, job->has_metadata ? &job->metadata : NULL);
        if (rc != 0) return rc;
    }
    if (job->options.threads >= 2) {
        fprintf(stderr, "%llu bytes -> %llu bytes, checksum verified, %lu segments on %u thread%s\n",
                (unsigned long long)job->stats.output_bytes, (unsigned long long)job->stats.input_bytes,
                (unsigned long)job->stats.segments, job->stats.workers, job->stats.workers == 1 ? "" : "s");
    } else {
        fprintf(stderr, "%llu bytes -> %llu bytes, checksum verified\n", (unsigned long long)job->stats.output_bytes,
                (unsigned long long)job->stats.input_bytes);
    }
    return 0;
}

static int decode_command(int argc, char **argv) {
    decode_job job = {0};
    int rc = decode_parse_options(&job, argc, argv);
    if (rc != 0) goto done;
    binary_stdio();
    rc = command_open_input(&job.io);
    if (rc != 0) goto done;
    rc = peek_header(job.io.input, &job.io.in, &job.ahead, &job.sizes);
    if (rc != 0) goto done;
    job.has_metadata = adapt_header_has_metadata(job.ahead.held);
    if (job.has_metadata) {
        (void)adapt_read_metadata(job.ahead.held, job.ahead.held_len, &job.metadata, NULL);
    }
    rc = job.sizes.archive ? decode_archive(&job) : decode_file(&job);
done:
    command_io_close(&job.io);
    return rc;
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
    stream source = {NULL, 0, 0}, candidate = {NULL, 0, 0}, best = {NULL, 0, 0};
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
        candidate.sink = 0;
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

typedef struct encode_options {
    int force, verify, no_verify, fast, mid, max;
    unsigned adaptive;
    int bmp, context_first, lean_contexts, text_transform;
    uint64_t max_memory, threads, segment_bytes;
    int is_folder;
    char folder_name[4096];
} encode_options;

typedef struct encode_job {
    encode_options options;
    command_io io;
    folder dir;
    packer pack;
    adapt_metadata metadata;
    int has_metadata, known_length, alongside;
    uint64_t length, workers, running;
    adapt_sizes sizes;
    adapt_stats stats;
    const char *verification;
} encode_job;

static int encode_parse_profile(encode_options *options, const args *a) {
    const char *text = value_of(a, "--allocation");
    if (text != NULL) {
        if (strcmp(text, "context") == 0) options->context_first = 1;
        else if (strcmp(text, "window") != 0)
            return usage_error("--allocation: expected window or context: %s", text);
    }
    text = value_of(a, "--contexts");
    if (text != NULL) {
        if (strcmp(text, "lean") == 0) options->lean_contexts = 1;
        else if (strcmp(text, "auto") != 0)
            return usage_error("--contexts: expected auto or lean: %s", text);
        if (options->lean_contexts && options->fast)
            return usage_error("--contexts lean applies to the normal and max profiles, not --fast%s", "");
    }
    text = value_of(a, "--adaptive");
    if (text != NULL) {
        if (strcmp(text, "rates") == 0) options->adaptive = 1;
        else if (strcmp(text, "sparse") == 0) options->adaptive = 2;
        else if (strcmp(text, "retained") == 0) options->adaptive = 3;
        else if (strcmp(text, "auto") == 0) options->adaptive = 4;
        else return usage_error("--adaptive: expected rates, sparse, retained or auto: %s", text);
        if (options->fast) return usage_error("--adaptive cannot be combined with --fast%s", "");
    }
    text = value_of(a, "--transform");
    if (text != NULL) {
        if (strcmp(text, "bmp") == 0) {
            if (options->fast || options->adaptive == 4 || options->is_folder)
                return usage_error("--transform bmp requires a file/pipe and normal, max or an explicit adaptive profile%s", "");
            options->bmp = 1;
        } else if (strcmp(text, "text") == 0) {
            if (options->adaptive != 0) return usage_error("--transform text cannot be combined with --adaptive%s", "");
            options->text_transform = ADAPT_TEXT_ON;
        } else if (strcmp(text, "none") == 0) {
            options->text_transform = ADAPT_TEXT_OFF;
        } else {
            return usage_error("--transform: expected bmp, text or none: %s", text);
        }
    }
    if (options->fast && options->max) return usage_error("--fast and --max are two profiles; pass one%s", "");
    if (options->mid && (options->fast || options->max)) return usage_error("--mid, --fast and --max are profiles; pass one%s", "");
    if (options->mid && (options->adaptive != 0 || options->bmp))
        return usage_error("--mid cannot be combined with --adaptive or --transform bmp%s", "");
    if (options->mid && options->lean_contexts) return usage_error("--contexts lean does not apply to --mid, whose set is fixed%s", "");
    if (options->verify && options->no_verify) {
        return usage_error("--verify and --no-verify cannot be used together%s", "");
    }
    return 0;
}

static int encode_parse_budget(encode_options *options, const args *a) {
    const char *text;
    /* Only presets set encode's budget; byte caps belong to decode. */
    if (value_of(a, "--max-memory") != NULL) {
        return usage_error("%s is a decode option; encode sets its budget with a preset, -0 .. -11", "--max-memory");
    }
    options->max_memory = preset_budget(a);
    if (options->max_memory == 0) options->max_memory = ADAPT_DEFAULT_MAX_MEMORY;
    options->threads = thread_count(a);
    if (options->threads == 0) {
        return usage_error("--threads: not a count of one or more: %s", value_of(a, "--threads"));
    }
    options->segment_bytes = ADAPT_SEGMENT_BYTES;
    text = value_of(a, "--segment-bytes");
    if (text != NULL) {
        if (!parse_bytes(text, &options->segment_bytes) || !adapt_segment_bytes_valid(options->segment_bytes)) {
            return usage_error("--segment-bytes: not a whole number of 262144-byte blocks: %s", text);
        }
        if (options->threads < 2) return usage_error("--segment-bytes needs --threads of two or more%s", "");
    }
    return 0;
}

static int encode_parse_options(encode_job *job, int argc, char **argv) {
    static const char *const with_value[] = {"-o", "--max-memory", "--threads", "--segment-bytes", "--adaptive", "--transform", "--allocation", "--contexts", NULL};
    static const char *const known[] = {"--force", "--verify", "--no-verify", "--fast", "--mid", "--max", "-0", "-1", "-2", "-3",
                                        "-4", "-5", "-6", "-7", "-8", "-9", "-10", "-11", NULL};
    encode_options *options = &job->options;
    const char *input;
    args a;
    int rc = parse_args(&a, argc, argv, with_value);
    if (rc != 0) return rc;
    rc = reject_unknown(&a, known);
    if (rc != 0) return rc;
    if (a.positionals != 1) return usage_error("encode takes exactly one input%s", "");
    input = job->io.input = a.positional;
    if (strcmp(input, "-") != 0 && is_directory(input)) {
        /* A folder's name without a trailing separator names its output. */
        size_t n = strlen(input);
        while (n > 1 && (input[n - 1] == '/'
#ifdef _WIN32
                         || input[n - 1] == '\\'
#endif
                         )) {
            n--;
        }
        if (n >= sizeof(options->folder_name)) return other_error(input, "path is too long");
        memcpy(options->folder_name, input, n);
        options->folder_name[n] = '\0';
        options->is_folder = 1;
    }
    options->force = has_flag(&a, "--force");
    options->verify = has_flag(&a, "--verify");
    options->no_verify = has_flag(&a, "--no-verify");
    options->fast = has_flag(&a, "--fast");
    options->mid = has_flag(&a, "--mid");
    options->max = has_flag(&a, "--max");
    options->text_transform = ADAPT_TEXT_AUTO;
    rc = encode_parse_profile(options, &a);
    if (rc != 0) return rc;
    rc = encode_parse_budget(options, &a);
    if (rc != 0) return rc;
    return command_output_name(&job->io, value_of(&a, "-o"), options->is_folder ? options->folder_name : input, 0);
}

static int encode_prepare_input(encode_job *job) {
    const encode_options *options = &job->options;
    int rc;
    if (options->is_folder) {
        char *culprit = NULL;
        char message[160];
        rc = metadata_read_path(options->folder_name, 0, &job->metadata);
        if (rc != 0) return other_error(job->io.input, strerror(rc));
        job->has_metadata = 1;
        /* Inventory and size every entry before coding the folder stream. */
        rc = folder_collect(&job->dir, options->folder_name, &culprit);
        if (rc != 0) {
            snprintf(message, sizeof(message), "%s%s%s", culprit != NULL ? culprit : "",
                     culprit != NULL ? ": " : "",
                     rc == ENOTSUP ? "not a directory, a regular file or a symbolic link" : strerror(rc));
            free(culprit);
            return other_error(job->io.input, message);
        }
        packer_reset(&job->pack, &job->dir);
        job->length = job->dir.stream_bytes;
        job->known_length = 1;
    } else {
        rc = command_open_input(&job->io);
        if (rc != 0) return rc;
        job->known_length = input_length(job->io.in.file, &job->length);
        if (job->known_length) {
            rc = metadata_read_path(job->io.input, 0, &job->metadata);
            if (rc != 0) return other_error(job->io.input, strerror(rc));
            job->has_metadata = 1;
        }
    }
    return 0;
}

static int encode_choose_sizes(encode_job *job) {
    encode_options *options = &job->options;
    adapt_sizes *sizes = &job->sizes;
    adapt_error error;
    adapt_status status;
    uint64_t profile_memory = options->bmp ?
        (options->max_memory > ADAPT_BLOCK_BYTES ? options->max_memory - ADAPT_BLOCK_BYTES : 0) : options->max_memory;
    if (options->adaptive > 0 && options->adaptive < 4) {
        status = options->threads >= 2 && job->known_length ?
            (options->max ? adapt_sizes_for_threads_max_adaptive : adapt_sizes_for_threads_adaptive)
                (job->length, profile_memory, options->threads, options->segment_bytes, options->adaptive, sizes, &error) :
            (options->max ? adapt_sizes_for_input_max_adaptive : adapt_sizes_for_input_adaptive)
                (job->known_length, job->length, profile_memory, options->adaptive, sizes, &error);
    } else if (options->threads >= 2 && job->known_length) {
        /* Fit each model to a segment and one thread's share of the budget. */
        status = (options->max ? adapt_sizes_for_threads_max : adapt_sizes_for_threads)
            (job->length, profile_memory, options->threads, options->segment_bytes, sizes, &error);
    } else {
        status = (options->max ? adapt_sizes_for_input_max : adapt_sizes_for_input)
            (job->known_length, job->length, profile_memory, sizes, &error);
    }
    if (status == ADAPT_OK && options->context_first) {
        if (options->fast) *sizes = adapt_sizes_fast(*sizes);
        status = adapt_sizes_prioritize_context(job->known_length, job->length, profile_memory,
                                                options->threads, options->segment_bytes, sizes, &error);
    }
    if (status != ADAPT_OK) {
        char message[160];
        /* The crate's command line names sizing failures without the input. */
        adapt_error_message(&error, message, sizeof(message));
        fprintf(stderr, "error: %s\n", message);
        return EXIT_OTHER;
    }
    if (options->fast) *sizes = adapt_sizes_fast(*sizes);
    if (options->mid) *sizes = adapt_sizes_mid(*sizes);
    if (options->max && options->adaptive == 0) *sizes = adapt_sizes_max(*sizes);
#ifdef ADAPT_SIZES_OVERRIDE
    /* Development-only sizes, past the tier, for pressure experiments. */
    {
        const char *override = getenv("ADAPT_SIZES");
        unsigned t, w, r, c, l;
        if (override != NULL && sscanf(override, "%u:%u:%u:%u:%u", &t, &w, &r, &c, &l) == 5) {
            sizes->table_log = (uint8_t)t;
            sizes->table_frac = 0;
            sizes->window_log = (uint8_t)w;
            sizes->match_log = (uint8_t)r;
            sizes->compact = (uint8_t)c;
            sizes->lean = (uint8_t)l;
            if (!adapt_sizes_valid(*sizes)) {
                fprintf(stderr, "error: ADAPT_SIZES names invalid sizes\n");
                return EXIT_OTHER;
            }
            options->max_memory = adapt_encode_memory_bound(*sizes) + ((uint64_t)64 << 20);
        }
    }
#endif
    sizes->archive = (uint8_t)options->is_folder;
    sizes->bmp = (uint8_t)options->bmp;
    sizes->contexts = (uint8_t)(options->lean_contexts ? ADAPT_CONTEXTS_LEAN : ADAPT_CONTEXTS_AUTO);
    if (options->text_transform != ADAPT_TEXT_AUTO) sizes->text = (uint8_t)options->text_transform;
    if (options->adaptive != 0 || options->bmp) sizes->text = ADAPT_TEXT_OFF;
    /* Both files and pipes segment on threads when the budget permits. */
    if (options->threads >= 2) sizes->segmented = 1;
    return 0;
}

static int encode_plan_verification(encode_job *job) {
    const encode_options *options = &job->options;
    int parallel = options->threads >= 2 && options->adaptive != 4;
    job->running = parallel ? running_threads(options->threads) : options->threads;
    job->workers = parallel ? adapt_encode_workers(job->sizes, options->max_memory, options->segment_bytes, job->running) : 1;
    /* Workers prove their own segments without a second model budget. */
    job->alongside = options->verify && options->adaptive != 4 &&
        (job->workers >= 2 || adapt_verified_memory_bound(job->sizes) <= options->max_memory);
    if (options->verify && options->adaptive != 4 && !job->alongside &&
        (job->io.in.file == stdin || !job->known_length || strcmp(job->io.output, "-") == 0)) {
        char message[160];
        snprintf(message, sizeof(message),
                 "proving the output here needs a memory budget of %llu bytes for two models, "
                 "or a file input written to a file; choose a higher preset or omit --verify",
                 (unsigned long long)adapt_verified_memory_bound(job->sizes));
        return other_error(job->io.input, message);
    }
    return 0;
}

static int encode_run(encode_job *job) {
    const encode_options *options = &job->options;
    command_io *io = &job->io;
    adapt_read_fn source = options->is_folder ? read_packer : read_stream;
    void *source_context = options->is_folder ? (void *)&job->pack : (void *)&io->in;
    metadata_writer output;
    adapt_status status;
    adapt_error error;
    output.out = &io->out;
    output.metadata = job->has_metadata ? &job->metadata : NULL;
    output.header_written = 0;
    if (options->adaptive == 4) {
        status = encode_search(source, source_context, &io->out, output.metadata, &job->sizes,
                               job->known_length, job->length, options->max_memory, options->threads,
                               options->segment_bytes, options->verify, options->context_first, &job->stats, &error);
    } else if (job->workers >= 2) {
        status = adapt_encode_stream_threads(source, source_context, write_metadata_stream, &output, job->sizes,
                                             options->max_memory, options->segment_bytes, job->running, options->verify,
                                             &job->stats, &error);
    } else if (job->alongside) {
        status = adapt_encode_stream_verified_segments(source, source_context, write_metadata_stream, &output, job->sizes,
                                                       options->max_memory, options->segment_bytes, &job->stats, &error);
    } else {
        status = adapt_encode_stream_segments(source, source_context, write_metadata_stream, &output, job->sizes,
                                              options->max_memory, options->segment_bytes, &job->stats, &error);
    }
    if (status == ADAPT_OK && job->has_metadata && options->adaptive != 4) {
        job->stats.output_bytes += ADAPT_METADATA_HEADER_BYTES;
    }
    packer_close(&job->pack);
    status = command_flush_output(io, status, &error);
    if (status != ADAPT_OK) {
        if (options->is_folder && status == ADAPT_ERROR_IO && io->out.error == 0) {
            return packer_failure(io->input, &job->pack);
        }
        return codec_failure(io->input, &error, &io->in, &io->out);
    }
    return 0;
}

static int encode_verify_after(encode_job *job) {
    command_io *io = &job->io;
    stream again = {NULL, 0, 0}, coded = {NULL, 0, 0};
    adapt_error error;
    adapt_status status;
    int rc;
    if (!(job->options.is_folder || io->in.file != stdin) || !job->known_length || io->stage.file == NULL) {
        return other_error(io->input, "could not reopen the input and staged output for verification");
    }
    /* One model at a time: close the writer and compare its staged bytes
     * with a fresh input read. A folder is walked through the same inventory. */
    command_close_input(io);
    rc = fclose(io->stage.file);
    io->stage.file = NULL;
    io->out.file = NULL;
    if (rc != 0) return other_error(io->output, strerror(errno));
    again.file = job->options.is_folder ? NULL : fopen(io->input, "rb");
#ifdef _WIN32
    coded.file = fopen(io->stage.path, "rb");
#else
    {
        int fd = openat(io->stage.guard_fd, "contents", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        coded.file = fd < 0 ? NULL : fdopen(fd, "rb");
        if (fd >= 0 && coded.file == NULL) close(fd);
    }
#endif
    if ((!job->options.is_folder && again.file == NULL) || coded.file == NULL) {
        rc = other_error(io->input, strerror(errno));
        goto done;
    }
    if (job->options.is_folder) {
        packer_reset(&job->pack, &job->dir);
        status = adapt_verify_stream(read_packer, &job->pack, read_stream, &coded, job->options.max_memory, NULL, &error);
        packer_close(&job->pack);
    } else {
        status = adapt_verify_stream(read_stream, &again, read_stream, &coded, job->options.max_memory, NULL, &error);
    }
    rc = 0;
    if (status != ADAPT_OK) {
        rc = job->options.is_folder && status == ADAPT_ERROR_IO && job->pack.failed != NULL ?
            packer_failure(io->input, &job->pack) : codec_failure(io->input, &error, &again, &coded);
    }
done:
    if (again.file != NULL) fclose(again.file);
    if (coded.file != NULL) fclose(coded.file);
    return rc;
}

static int encode_verify_output(encode_job *job) {
    if (job->alongside || (job->options.adaptive == 4 && job->options.verify)) {
        job->verification = "verified";
    } else if (job->options.verify) {
        int rc = encode_verify_after(job);
        if (rc != 0) return rc;
        job->verification = "verified after encoding";
    } else {
        job->verification = job->options.no_verify ? "not verified (--no-verify)" : "not verified (pass --verify to enable)";
    }
    return 0;
}

static void encode_report(const encode_job *job) {
    const adapt_stats *stats = &job->stats;
    char segments[96] = "";
    /* Hundredths of a percent in integers, as the crate prints them. */
    uint64_t hundredths = stats->input_bytes == 0 ? 0 :
        (stats->output_bytes * 10000 + stats->input_bytes / 2) / stats->input_bytes;
    if (job->options.threads >= 2) {
        unsigned on = job->options.adaptive == 4 ? 1u : stats->workers;
        snprintf(segments, sizeof(segments), ", %lu segments on %u thread%s", (unsigned long)stats->segments, on,
                 on == 1 ? "" : "s");
    }
    if (job->options.is_folder) {
        fprintf(stderr, "%llu entries, %llu bytes -> %llu bytes (%llu.%02llu%%), %s%s\n", (unsigned long long)job->dir.count,
                (unsigned long long)stats->input_bytes, (unsigned long long)stats->output_bytes,
                (unsigned long long)(hundredths / 100), (unsigned long long)(hundredths % 100), job->verification, segments);
    } else {
        fprintf(stderr, "%llu bytes -> %llu bytes (%llu.%02llu%%), %s%s\n", (unsigned long long)stats->input_bytes,
                (unsigned long long)stats->output_bytes, (unsigned long long)(hundredths / 100),
                (unsigned long long)(hundredths % 100), job->verification, segments);
    }
}

static int encode_command(int argc, char **argv) {
    encode_job job = {0};
    int rc = encode_parse_options(&job, argc, argv);
    if (rc != 0) goto done;
    binary_stdio();
    rc = encode_prepare_input(&job);
    if (rc != 0) goto done;
    rc = encode_choose_sizes(&job);
    if (rc != 0) goto done;
    rc = encode_plan_verification(&job);
    if (rc != 0) goto done;
    rc = command_open_output(&job.io, job.options.force);
    if (rc != 0) goto done;
    rc = encode_run(&job);
    if (rc != 0) goto done;
    rc = encode_verify_output(&job);
    if (rc != 0) goto done;
    command_close_input(&job.io);
    if (job.io.stage.path != NULL) {
        rc = staged_publish(&job.io.stage, job.io.output, job.options.force, NULL);
        if (rc != 0) goto done;
    }
    encode_report(&job);
done:
    packer_close(&job.pack);
    folder_free(&job.dir);
    command_io_close(&job.io);
    return rc;
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
    input = a.positional;
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
    if (sizes.table_frac == 0) {
        printf("context table   2^%u blocks (%llu MiB)\n", sizes.table_log,
               (unsigned long long)((adapt_table_blocks(sizes) * 16) >> 20));
    } else {
        printf("context table   %llu blocks (%llu MiB, %u/8 past 2^%u)\n", (unsigned long long)adapt_table_blocks(sizes),
               (unsigned long long)((adapt_table_blocks(sizes) * 16) >> 20), 8u + sizes.table_frac, sizes.table_log);
    }
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
    printf("profile         %s%s%s\n", sizes.max && sizes.adaptive ? "max-" : "", sizes.adaptive ? adaptive_name(sizes.adaptive) : (sizes.fast ? "fast" : sizes.mid ? "mid" : (sizes.max ? "max" : "normal")),
           sizes.lean && sizes.compact ? " (compact side tables, lean contexts)"
           : sizes.lean                ? " (lean contexts)"
           : sizes.compact             ? " (compact side tables)"
                                       : "");
    printf("x86 transform   %s\n", sizes.x86 ? "yes" : "no");
    printf("text transform  %s\n", sizes.text == ADAPT_TEXT_ON ? "yes" : "no");
    printf("arbiter start   %s\n", sizes.low_start ? "1/32" : "1/4");
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

static int run_command(int argc, char **argv) {
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

int main(int argc, char **argv) {
    int rc;
#ifndef _WIN32
    if (argc > 1 && (strcmp(argv[1], "encode") == 0 || strcmp(argv[1], "decode") == 0) &&
        setup_interrupts() != 0) {
        return other_error("signal setup", strerror(errno));
    }
#endif
    rc = run_command(argc, argv);
    return cancelled() ? 128 + interrupted_signal : rc;
}
