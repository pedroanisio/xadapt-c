/* Shared machinery for the measurement probes in this directory.
 *
 * Each probe reads an input once, hashes tokens or contexts into a table,
 * and reports. Before this header each carried its own copy of the mixer,
 * the loader, the size parser and the failure path, which is three places
 * for one bug to hide. The hash functions here reproduce the ones the probes
 * used byte for byte, so every figure already recorded in research/ still
 * comes out of a run.
 *
 * Header-only, standard library only, no codec source or constant. Nothing
 * in the build, the library or the CLI loads it.
 *
 * Authored with Claude Opus 5 (Claude Code), 2026-09-17.
 */
#ifndef ADAPT_BENCH_PROBE_UTIL_H
#define ADAPT_BENCH_PROBE_UTIL_H

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The FNV-1a offset basis and prime, as the probes used them. */
#define PROBE_FNV_BASIS 0xcbf29ce484222325ULL
#define PROBE_FNV_PRIME 0x100000001b3ULL

/* Exit codes every probe shares: 0 fine, 1 the run failed, 2 the arguments
 * were wrong. */
#define PROBE_EXIT_OK 0
#define PROBE_EXIT_FAILED 1
#define PROBE_EXIT_USAGE 2

/* ---- failure -------------------------------------------------------- */

/* Report on stderr, prefixed with the probe's name, and return `code` so a
 * caller can write `return probe_fail(...)`. A probe never fails silently
 * and never continues past a failure with a defaulted value. */
static inline int probe_fail(const char *who, int code, const char *fmt, ...) {
    va_list args;
    fprintf(stderr, "%s: ", who);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    return code;
}

/* ---- hashing -------------------------------------------------------- */

static inline uint64_t probe_mix64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

/* FNV-1a over `len` bytes from a chosen seed, then mixed. The seed is the
 * FNV basis for a token book and a per-kind salt for a context, which is
 * the only way the probes' two hashes ever differed. */
static inline uint64_t probe_hash_seeded(uint64_t seed, const uint8_t *bytes, size_t len) {
    size_t i;
    for (i = 0; i < len; i++) {
        seed = (seed ^ bytes[i]) * PROBE_FNV_PRIME;
    }
    return probe_mix64(seed);
}

/* ---- input ---------------------------------------------------------- */

typedef struct probe_input {
    uint8_t *bytes;
    size_t length;
} probe_input;

/* Read the whole file, or its first `limit` bytes when limit is not zero.
 * On failure the reason is printed, the input is left empty and the probe's
 * exit code comes back. An empty file is not a failure here — it reads as
 * zero bytes, and a probe that cannot use one says so itself, so that "no
 * such file" and "nothing in it" never take the same branch. */
static inline int probe_read_input(const char *who, const char *path, uint64_t limit, probe_input *out) {
    FILE *file;
    long size;
    size_t want;

    out->bytes = NULL;
    out->length = 0;
    file = fopen(path, "rb");
    if (!file) {
        return probe_fail(who, PROBE_EXIT_FAILED, "cannot open %s", path);
    }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return probe_fail(who, PROBE_EXIT_FAILED, "cannot size %s", path);
    }
    want = (size_t)size;
    if (limit && limit < (uint64_t)want) {
        want = (size_t)limit;
    }
    if (want == 0) {
        fclose(file);
        return PROBE_EXIT_OK;
    }
    out->bytes = malloc(want);
    if (!out->bytes) {
        fclose(file);
        return probe_fail(who, PROBE_EXIT_FAILED, "cannot hold %zu bytes", want);
    }
    if (fread(out->bytes, 1, want, file) != want) {
        free(out->bytes);
        out->bytes = NULL;
        fclose(file);
        return probe_fail(who, PROBE_EXIT_FAILED, "short read on %s", path);
    }
    out->length = want;
    fclose(file);
    return PROBE_EXIT_OK;
}

static inline void probe_free_input(probe_input *in) {
    free(in->bytes);
    in->bytes = NULL;
    in->length = 0;
}

/* ---- allocation ----------------------------------------------------- */

/* Zeroed, or the probe stops. A table that silently came back NULL would
 * make every figure below it wrong. */
static inline void *probe_calloc(const char *who, size_t count, size_t size) {
    void *p = calloc(count, size);
    if (!p) {
        (void)probe_fail(who, PROBE_EXIT_FAILED, "cannot hold %zu x %zu bytes", count, size);
        exit(PROBE_EXIT_FAILED);
    }
    return p;
}

/* ---- buffered output ------------------------------------------------ */

/* stdio one byte at a time dominates a transform that writes a byte per
 * input byte; this writes through a private buffer instead. */
#define PROBE_WRITER_CAPACITY (1u << 16)

typedef struct probe_writer {
    FILE *file;
    size_t held;
    int failed;
    uint8_t buffer[PROBE_WRITER_CAPACITY];
} probe_writer;

static inline void probe_writer_open(probe_writer *w, FILE *file) {
    w->file = file;
    w->held = 0;
    w->failed = 0;
}

static inline void probe_write(probe_writer *w, const uint8_t *bytes, size_t len) {
    if (w->held + len > PROBE_WRITER_CAPACITY) {
        if (w->held && fwrite(w->buffer, 1, w->held, w->file) != w->held) w->failed = 1;
        w->held = 0;
        if (len >= PROBE_WRITER_CAPACITY) {
            if (fwrite(bytes, 1, len, w->file) != len) w->failed = 1;
            return;
        }
    }
    memcpy(w->buffer + w->held, bytes, len);
    w->held += len;
}

static inline void probe_write_byte(probe_writer *w, uint8_t b) {
    if (w->held == PROBE_WRITER_CAPACITY) {
        if (fwrite(w->buffer, 1, w->held, w->file) != w->held) w->failed = 1;
        w->held = 0;
    }
    w->buffer[w->held++] = b;
}

/* Flush; returns 0 when every byte reached the file. */
static inline int probe_writer_close(probe_writer *w) {
    if (w->held && fwrite(w->buffer, 1, w->held, w->file) != w->held) w->failed = 1;
    w->held = 0;
    if (fflush(w->file) != 0) w->failed = 1;
    return w->failed ? PROBE_EXIT_FAILED : PROBE_EXIT_OK;
}

/* ---- arguments ------------------------------------------------------ */

/* A count of bytes, plain or with a KiB/MiB/GiB/KB/MB/GB suffix. Returns 0
 * when the text is not a size. */
static inline int probe_parse_size(const char *text, uint64_t *out) {
    char *end;
    unsigned long long value;
    if (!text || !*text) return 0;
    value = strtoull(text, &end, 10);
    if (end == text) return 0;
    if (*end == '\0') {
        *out = (uint64_t)value;
        return 1;
    }
    if (strcmp(end, "KiB") == 0) value <<= 10;
    else if (strcmp(end, "MiB") == 0) value <<= 20;
    else if (strcmp(end, "GiB") == 0) value <<= 30;
    else if (strcmp(end, "KB") == 0) value *= 1000ULL;
    else if (strcmp(end, "MB") == 0) value *= 1000ULL * 1000ULL;
    else if (strcmp(end, "GB") == 0) value *= 1000ULL * 1000ULL * 1000ULL;
    else return 0;
    *out = (uint64_t)value;
    return 1;
}

/* Find `--name <value>` in argv from `first` on. Returns 1 and sets *value
 * when present, 0 when absent, -1 when the flag is there without a value. */
static inline int probe_option(int argc, char **argv, int first, const char *name, const char **value) {
    int i;
    for (i = first; i < argc; i++) {
        if (strcmp(argv[i], name) != 0) continue;
        if (i + 1 >= argc) return -1;
        *value = argv[i + 1];
        return 1;
    }
    return 0;
}

/* Every argument from `first` on is either a known `--name <value>` pair or
 * an error; this rejects anything else so a typo is never ignored. */
static inline int probe_only_options(int argc, char **argv, int first, const char *const *names, size_t count) {
    int i;
    for (i = first; i < argc;) {
        size_t k;
        int known = 0;
        for (k = 0; k < count; k++) {
            if (strcmp(argv[i], names[k]) == 0) { known = 1; break; }
        }
        if (!known || i + 1 >= argc) return 0;
        i += 2;
    }
    return 1;
}

#endif /* ADAPT_BENCH_PROBE_UTIL_H */
