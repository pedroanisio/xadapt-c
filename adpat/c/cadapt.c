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
 * makes encoding verification opt-in. Both directions stream: memory is
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
 * link() for publishing without replacing a target, and a 64-bit ftell
 * for `info` on large files. Windows equivalents are named below.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200112L
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
#include <io.h>
#include <process.h>
#define cadapt_getpid _getpid
#define cadapt_fseek _fseeki64
#define cadapt_ftell _ftelli64
typedef long long cadapt_off;
#else
#include <unistd.h>
#define cadapt_getpid getpid
#define cadapt_fseek fseeko
#define cadapt_ftell ftello
typedef off_t cadapt_off;
#endif

#define CADAPT_VERSION "0.10.0"

/* Format numbers and memory defaults come from the codec constants.
 * Verification is opt-in, and segment coding runs on one thread. */
static void print_usage(FILE *stream) {
    fprintf(stream,
            "cadapt — the adapt self-adaptive lossless codec, in C\n"
            "\n"
            "usage:\n"
            "  cadapt encode <input> [-o <output>] [--force] [--verify | --no-verify] [--fast | --max] [--max-memory <bytes>]\n"
            "                [--threads <n>] [--segment-bytes <bytes>]\n"
            "  cadapt decode <input> [-o <output>] [--force] [--max-output <bytes>] [--max-memory <bytes>] [--threads <n>]\n"
            "  cadapt info   <file>\n"
            "  cadapt --version\n"
            "\n"
            "<input> or <output> may be '-' for standard input or output. Without -o,\n"
            "encode writes <input>.adpt and decode strips that suffix (or appends .out).\n"
            "Existing output files are kept unless --force is given. --fast writes the\n"
            "fast profile (format version %u), which decodes about three times faster\n"
            "for about a twelfth more bytes; --max the max profile (%u), which writes\n"
            "fewer bytes for more memory and time and wants a budget above the default\n"
            "on a large input; the default is the normal profile (%u).\n"
            "Both encode and decode stream, so an input of any length takes the memory\n"
            "of the model and its window; --max-memory <bytes> (default %llu) bounds\n"
            "that, and a smaller budget means a smaller model, never a refused input.\n"
            "encode skips round-trip verification by default. --verify proves its output\n"
            "as it writes it when the budget holds two models, or, for a file written\n"
            "to a file, in a second pass with one. --no-verify explicitly skips this proof.\n"
            "decode also accepts --max-output <bytes> (default 1 GiB).\n"
            "--threads <n> (default 1) with n of two or more cuts the input into segments\n"
            "of --segment-bytes (default %llu, whole blocks of %llu) that start the\n"
            "model afresh, the file the crate's threads write; this port codes and\n"
            "decodes the segments in order, on one thread.\n",
            ADAPT_FAST_VERSION, ADAPT_MAX_VERSION, ADAPT_FORMAT_VERSION, (unsigned long long)ADAPT_DEFAULT_MAX_MEMORY,
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
         * discarded; probing first cannot establish that ownership. */
#ifdef _WIN32
        fd = _open(path, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
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
static int staged_publish(staged *s, const char *target, int force) {
    int rc;
    if (s->file != NULL && fclose(s->file) != 0) {
        int e = errno;
        s->file = NULL;
        staged_discard(s);
        return other_error(target, strerror(e));
    }
    s->file = NULL;
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
    adapt_error error;
    adapt_status status;
    uint64_t threads;
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
    status = adapt_decode_stream(read_stream, &in, write_stream, &out, limits, &stats, &error);
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
        rc = staged_publish(&stage, output, force);
        if (rc != 0) {
            free(output);
            return rc;
        }
    }
    free(output);
    /* The crate's line: on threads for a file, in order for a pipe; this
     * port decodes in order either way and says so. */
    if (threads >= 2 && in.file != stdin) {
        fprintf(stderr, "%llu bytes -> %llu bytes, checksum verified, %lu segments on 1 thread\n",
                (unsigned long long)stats.output_bytes, (unsigned long long)stats.input_bytes,
                (unsigned long)stats.segments);
    } else if (threads >= 2) {
        fprintf(stderr, "%llu bytes -> %llu bytes, checksum verified on 1 thread (a pipe is read in order)\n",
                (unsigned long long)stats.output_bytes, (unsigned long long)stats.input_bytes);
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

static int encode_command(int argc, char **argv) {
    static const char *const with_value[] = {"-o", "--max-memory", "--threads", "--segment-bytes", NULL};
    static const char *const known[] = {"--force", "--verify", "--no-verify", "--fast", "--max", NULL};
    args a;
    const char *input, *o, *text, *how;
    char *output = NULL;
    int force, verify, alongside, rc, known_length;
    uint64_t max_memory = ADAPT_DEFAULT_MAX_MEMORY, length = 0, hundredths, threads, segment_bytes = ADAPT_SEGMENT_BYTES;
    char segments[96];
    adapt_sizes sizes;
    stream in = {NULL, 0}, out = {NULL, 0};
    staged stage = {NULL, NULL};
    adapt_stats stats;
    adapt_error error;
    adapt_status status;
    char message[160];
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
        size_t n = strlen(input);
        output = malloc(n + 6);
        if (output != NULL) {
            memcpy(output, input, n);
            memcpy(output + n, ".adpt", 6);
        }
    }
    if (output == NULL) {
        return other_error(input, "memory allocation failed");
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
    known_length = input_length(in.file, &length);
    if (threads >= 2 && known_length) {
        /* A file's sizes are the crate's for its threads: each model fitted
         * to a segment and to one thread's share of the budget. */
        status = has_flag(&a, "--max") ? adapt_sizes_for_threads_max(length, max_memory, threads, segment_bytes, &sizes, &error)
                                       : adapt_sizes_for_threads(length, max_memory, threads, segment_bytes, &sizes, &error);
    } else {
        status = has_flag(&a, "--max") ? adapt_sizes_for_input_max(known_length, length, max_memory, &sizes, &error)
                                       : adapt_sizes_for_input(known_length, length, max_memory, &sizes, &error);
    }
    if (status != ADAPT_OK) {
        /* the crate's command line names this one without the input */
        adapt_error_message(&error, message, sizeof(message));
        fprintf(stderr, "error: %s\n", message);
        if (in.file != stdin) {
            fclose(in.file);
        }
        free(output);
        return EXIT_OTHER;
    }
    if (has_flag(&a, "--fast")) {
        sizes = adapt_sizes_fast(sizes);
    }
    if (has_flag(&a, "--max")) {
        sizes = adapt_sizes_max(sizes);
    }
    if (threads >= 2) {
        /* The crate cuts a file's segments on threads and a pipe's in
         * order; this port cuts them in order either way, to the same
         * bytes: every segment starts a fresh model, so the order the
         * segments are coded in changes nothing. */
        sizes.segmented = 1;
    }
    alongside = verify && adapt_verified_memory_bound(sizes) <= max_memory;
    if (verify && !alongside && (in.file == stdin || !known_length || strcmp(output, "-") == 0)) {
        snprintf(message, sizeof(message),
                 "proving the output here needs a memory budget of %llu bytes for two models, "
                 "or a file input written to a file; raise --max-memory or omit --verify",
                 (unsigned long long)adapt_verified_memory_bound(sizes));
        rc = other_error(input, message);
        if (in.file != stdin) {
            fclose(in.file);
        }
        free(output);
        return rc;
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
    if (alongside) {
        status = adapt_encode_stream_verified_segments(read_stream, &in, write_stream, &out, sizes, max_memory,
                                                       segment_bytes, &stats, &error);
    } else {
        status = adapt_encode_stream_segments(read_stream, &in, write_stream, &out, sizes, max_memory, segment_bytes,
                                              &stats, &error);
    }
    if (status == ADAPT_OK && out.file == stdout && fflush(stdout) != 0) {
        out.error = errno;
        error.status = ADAPT_ERROR_IO;
        status = ADAPT_ERROR_IO;
    }
    if (status != ADAPT_OK) {
        rc = codec_failure(input, &error, &in, &out);
        if (in.file != stdin) {
            fclose(in.file);
        }
        staged_discard(&stage);
        free(output);
        return rc;
    }
    if (alongside) {
        how = "verified";
    } else if (verify) {
        /* One model at a time: prove the staged file against the input
         * again, which needs both to be files that can be read back. */
        if (in.file != stdin && known_length && stage.file != NULL) {
            stream again = {NULL, 0}, coded = {NULL, 0};
            fclose(in.file);
            in.file = NULL;
            if (fclose(stage.file) != 0) {
                rc = other_error(output, strerror(errno));
                stage.file = NULL;
                staged_discard(&stage);
                free(output);
                return rc;
            }
            stage.file = NULL;
            again.file = fopen(input, "rb");
            coded.file = fopen(stage.path, "rb");
            if (again.file == NULL || coded.file == NULL) {
                rc = other_error(input, strerror(errno));
                if (again.file != NULL) {
                    fclose(again.file);
                }
                if (coded.file != NULL) {
                    fclose(coded.file);
                }
                staged_discard(&stage);
                free(output);
                return rc;
            }
            status = adapt_verify_stream(read_stream, &again, read_stream, &coded, max_memory, NULL, &error);
            fclose(again.file);
            fclose(coded.file);
            if (status != ADAPT_OK) {
                rc = codec_failure(input, &error, &again, &coded);
                staged_discard(&stage);
                free(output);
                return rc;
            }
            how = "verified after encoding";
        } else {
            rc = other_error(input, "could not reopen the input and staged output for verification");
            if (in.file != stdin) {
                fclose(in.file);
            }
            staged_discard(&stage);
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
        rc = staged_publish(&stage, output, force);
        if (rc != 0) {
            free(output);
            return rc;
        }
    }
    free(output);
    /* Hundredths of a percent in integers, as the crate prints them. */
    hundredths = stats.input_bytes == 0 ? 0 : (stats.output_bytes * 10000 + stats.input_bytes / 2) / stats.input_bytes;
    segments[0] = '\0';
    if (threads >= 2) {
        snprintf(segments, sizeof(segments), ", %lu segments on 1 thread%s", (unsigned long)stats.segments,
                 known_length ? "" : " (a pipe is read in order)");
    }
    fprintf(stderr, "%llu bytes -> %llu bytes (%llu.%02llu%%), %s%s\n", (unsigned long long)stats.input_bytes,
            (unsigned long long)stats.output_bytes, (unsigned long long)(hundredths / 100),
            (unsigned long long)(hundredths % 100), how, segments);
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
    uint8_t header[ADAPT_HEADER_BYTES + ADAPT_PERIOD_BYTES];
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
    printf("profile         %s\n", sizes.fast ? "fast" : (sizes.max ? "max" : "normal"));
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
