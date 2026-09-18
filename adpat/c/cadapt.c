/*
 * cadapt — command line for the C codec.
 *
 * Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-12.
 *
 *   cadapt encode <input> [-o <output>] [--force] [--verify | --no-verify] [--explain] [--fast | --mid | --max] [-0 .. -11]
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
 *
 * This unit is the commands, their arguments, their streams and the
 * staged file. The folder side — the walk into an archive stream, the
 * restore out of one, the staged directory — is cadapt_folder.c; the
 * extended attributes, sparse maps and metadata calls are cadapt_fs.c.
 */
#include "cadapt_platform.h"
#include "cadapt_fs.h"
#include "cadapt_folder.h"

/* --allocation: the context table before the recall window where the
 * window-first sizes would starve the table (auto, the default), for every
 * input (context), or never (window). */
enum { ALLOCATION_AUTO = 0, ALLOCATION_CONTEXT = 1, ALLOCATION_WINDOW = 2 };

/* The command line's own version, not a portability detail: it stays here,
 * where tools/provenance.py reads it. */
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

int cancelled(void) {
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
            "  cadapt encode <input> [-o <output>] [--force] [--verify | --no-verify] [--explain] [--fast | --mid | --max] [-0 .. -11]\n"
            "                [--threads <n>] [--segment-bytes <bytes>]\n"
            "                [--adaptive rates|sparse|retained|auto] [--transform bmp|text|none] [--allocation auto|window|context]\n"
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
            "the normal profile. Normal/fast/mid/max streams use versions %u/%u/%u/%u (the\n"
            "max profile %u or %u and the normal profile %u when their arbiters take the class\n"
            "or recall keys, chosen per file); named files and folders include metadata in\n"
            "versions %u/%u/%u/%u (%u, %u, %u).\n"
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
            ADAPT_FORMAT_VERSION, ADAPT_FAST_VERSION, ADAPT_MID_VERSION, ADAPT_MAX_PRESSURE_VERSION, ADAPT_MAX_KEYS_VERSION,
            ADAPT_MAX_KEYS_BINARY_VERSION, ADAPT_NORMAL_KEYS_VERSION, ADAPT_METADATA_VERSION, ADAPT_METADATA_FAST_VERSION,
            ADAPT_MID_METADATA_VERSION, ADAPT_MAX_PRESSURE_METADATA_VERSION, ADAPT_MAX_KEYS_METADATA_VERSION,
            ADAPT_MAX_KEYS_BINARY_METADATA_VERSION, ADAPT_NORMAL_KEYS_METADATA_VERSION);
    fprintf(stream,
            "The text transform (versions 62--66, 67--71 with metadata) replaces a word both\n"
            "sides have seen three times by a code the model reads as one symbol; the book\n"
            "is built as the file is coded, nothing is sent ahead. It is taken by itself for\n"
            "a file of 8 MiB or more whose first block looks like text (measured on\n"
            "enwik8 at 128 MiB: 3.5 %% fewer bytes in fewer seconds); --transform text asks\n"
            "for it whatever the input, --transform none refuses it.\n"
            "--allocation auto, the default, grows the context table before the recall\n"
            "window where the window-first sizes would leave the table below its target (an\n"
            "input longer than the window the budget allows); context or window force one.\n"
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
            "metadata bookkeeping is additional. The smallest budget the normal profile\n"
            "runs in is %llu bytes, the max profile's %llu (a preset below it is raised to\n"
            "it; the model written there may need less); verification and segments may need more.\n"
            "Named files and folders preserve modes and access/modification timestamps.\n"
            "--explain adds to encode's report what the first block decided: the tables\n"
            "and allocation, the class width, context set, record period and arbiter keys,\n"
            "the trials that chose them and whether the text transform is on.\n"
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

static int usage_error(const char *fmt, const char *arg) {
    fputs("error: ", stderr);
    fprintf(stderr, fmt, arg);
    fputs("\n\n", stderr);
    print_usage(stderr);
    return EXIT_USAGE;
}

int other_error(const char *path, const char *message) {
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

#ifdef _WIN32
int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f != NULL) {
        fclose(f);
        return 1;
    }
    return errno != ENOENT;
}
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

/* The codec's failure as a message against the path that failed: the
 * input, or, for an i/o error the sink raised and the source did not, the
 * output — a full disk is the output's fault, not the input's. */
static int codec_failure(const char *input, const char *output, const adapt_error *error, const stream *in,
                         const stream *out) {
    char message[160];
    const char *path = input;
    if (error->status == ADAPT_ERROR_IO) {
        int e = in->error != 0 ? in->error : out->error;
        if (in->error == 0 && out->error != 0 && output != NULL) {
            path = strcmp(output, "-") == 0 ? "standard output" : output;
        }
        snprintf(message, sizeof(message), "i/o error: %s", e != 0 ? strerror(e) : "callback failed");
    } else {
        adapt_error_message(error, message, sizeof(message));
    }
    return other_error(path, message);
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
    if (status != ADAPT_OK) return codec_failure(io->input, io->output, &error, &io->in, &io->out);
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
            if (status == ADAPT_OK && context_first == ALLOCATION_CONTEXT)
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
    int bmp, context_first, lean_contexts, text_transform, explain;
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
    /* --allocation auto took the table first (the window-first sizes starved it) */
    int table_first;
    uint64_t length, workers, running;
    adapt_sizes sizes;
    adapt_stats stats;
    const char *verification;
} encode_job;

static int encode_parse_profile(encode_options *options, const args *a) {
    /* --allocation: auto, the default, grows the context table before the
     * recall window only where the window-first sizes would leave the table
     * below its target for the input (ALLOCATION_AUTO); context and window
     * force one policy for every input */
    const char *text = value_of(a, "--allocation");
    options->context_first = ALLOCATION_AUTO;
    if (text != NULL) {
        if (strcmp(text, "context") == 0) options->context_first = ALLOCATION_CONTEXT;
        else if (strcmp(text, "window") == 0) options->context_first = ALLOCATION_WINDOW;
        else if (strcmp(text, "auto") != 0)
            return usage_error("--allocation: expected auto, window or context: %s", text);
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
    static const char *const known[] = {"--force", "--verify", "--no-verify", "--explain", "--fast", "--mid", "--max", "-0", "-1", "-2", "-3",
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
    options->explain = has_flag(&a, "--explain");
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
    if (status == ADAPT_OK && options->context_first != ALLOCATION_WINDOW) {
        /* ALLOCATION_AUTO takes the table first only where the window-first
         * sizes starve it: an input longer than the window the budget
         * allows, whose table then stays at its floor (enwik9 at -9: a
         * 2^20-block table under a 512 MiB window, 165,865,476 bytes
         * against 149,702,998 with the table first; on the mixed corpus at
         * -6 the table wins every row it changes but one, by 56 bytes —
         * LOCKS.md, 2026-09-17). A segmented input's target is a segment's. */
        uint64_t unit = options->threads >= 2 && job->length > options->segment_bytes ? options->segment_bytes : job->length;
        int starved = job->known_length && sizes->table_log < adapt_table_log_for(unit);
        job->table_first = options->context_first == ALLOCATION_CONTEXT || starved;
        if (job->table_first) {
            if (options->fast) *sizes = adapt_sizes_fast(*sizes);
            status = adapt_sizes_prioritize_context(job->known_length, job->length, profile_memory,
                                                    options->threads, options->segment_bytes, sizes, &error);
        }
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
        return codec_failure(io->input, io->output, &error, &io->in, &io->out);
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
            packer_failure(io->input, &job->pack) : codec_failure(io->input, io->stage.path, &error, &again, &coded);
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

/* --explain: what the encoder decided from the first block and the sizes
 * it took — the choices an experiment otherwise needs a development build
 * to see — after the report, on stderr, in the shape `info` prints. */
static void encode_explain(const encode_job *job) {
    const adapt_stats *stats = &job->stats;
    adapt_sizes sizes = job->sizes;
    char contexts[80];
    sizes.class_code = stats->class_code;
    sizes.set_code = stats->set_code;
    sizes.keys = stats->keys;
    sizes.lean = stats->lean;
    sizes.period = stats->period;
    fprintf(stderr, "explain: tables   2^%u blocks, recall 2^%u entries, window 2^%u bytes, %s allocation\n", sizes.table_log,
            sizes.match_log, sizes.window_log,
            job->options.context_first == ALLOCATION_WINDOW ? "window-first" : job->table_first ? "table-first" : "window-first (auto)");
    if (sizes.fast) {
        adapt_fast_contexts(sizes, contexts, sizeof(contexts));
    } else {
        char dropped[64];
        adapt_dropped_contexts(sizes, dropped, sizeof(dropped));
        snprintf(contexts, sizeof(contexts), "%s%s", dropped[0] ? "all but " : "all", dropped);
    }
    fprintf(stderr, "explain: model    class history %u bytes, contexts %s, record period %u, arbiter keys %s\n",
            adapt_class_width(sizes), contexts, (unsigned)sizes.period,
            sizes.keys == ADAPT_KEYS_TEXT ? "classes and nesting" : sizes.keys == ADAPT_KEYS_BINARY ? "classes and recall" : "classic");
    fprintf(stderr, "explain: trials   %lu codings of %llu bytes on the first block; text transform %s\n", (unsigned long)stats->trials,
            (unsigned long long)stats->trial_bytes, stats->text_bytes != 0 ? "on" : "off");
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
    if (job.options.explain) encode_explain(&job);
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
    if (!sizes.fast && !sizes.mid) {
        printf("arbiter keys    %s\n", sizes.keys == ADAPT_KEYS_TEXT ? "classes and nesting" : sizes.keys == ADAPT_KEYS_BINARY ? "classes and recall" : "classic");
    }
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
