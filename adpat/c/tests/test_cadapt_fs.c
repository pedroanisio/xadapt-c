/* The filesystem layer's metadata reading and checking, on a struct stat
 * made by hand: what a real filesystem cannot be asked to produce on
 * every platform.
 *
 *  - A time before 1970 that the platform reports as a negative fraction
 *    (Darwin: tv_sec truncated toward zero, tv_nsec negative) is read as
 *    the same instant with the fraction in [0, 1e9), which the container's
 *    metadata record requires, and checks equal to that record.
 *  - A symbolic link's mode is checked only where the platform can set it.
 *
 * Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-18,
 * with Claude Opus 5 (Claude Code). */
#include "../cadapt_fs.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
#define EXPECT(cond)                                                            \
    do {                                                                        \
        if (!(cond)) {                                                          \
            fprintf(stderr, "%s:%d: expected %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                         \
        }                                                                       \
    } while (0)

static void set_times(struct stat *st, time_t atime_sec, long atime_nsec, time_t mtime_sec, long mtime_nsec) {
#ifdef __APPLE__
    st->st_atimespec.tv_sec = atime_sec;
    st->st_atimespec.tv_nsec = atime_nsec;
    st->st_mtimespec.tv_sec = mtime_sec;
    st->st_mtimespec.tv_nsec = mtime_nsec;
#else
    st->st_atim.tv_sec = atime_sec;
    st->st_atim.tv_nsec = atime_nsec;
    st->st_mtim.tv_sec = mtime_sec;
    st->st_mtim.tv_nsec = mtime_nsec;
#endif
}

int main(void) {
    struct stat st;
    adapt_metadata m;

    /* a plain time reads as it is */
    memset(&st, 0, sizeof(st));
    st.st_mode = S_IFREG | 0751;
    set_times(&st, 1500000000, 123456789, 1700000000, 5);
    metadata_from_stat(&st, &m);
    EXPECT(m.mode == 0751 && m.atime_seconds == 1500000000 && m.atime_nanoseconds == 123456789);
    EXPECT(m.mtime_seconds == 1700000000 && m.mtime_nanoseconds == 5);
    EXPECT(metadata_check_stat(&st, &m, 1) == 0);

    /* -100,000,000.987654321 s reported as (-100000000, -987654321) */
    set_times(&st, -100000000, -987654321, -100000000, -987654321);
    metadata_from_stat(&st, &m);
    EXPECT(m.mtime_seconds == -100000001 && m.mtime_nanoseconds == 12345679);
    EXPECT(m.atime_seconds == -100000001 && m.atime_nanoseconds == 12345679);
    EXPECT(m.mtime_nanoseconds < 1000000000u && m.atime_nanoseconds < 1000000000u);
    EXPECT(metadata_check_stat(&st, &m, 1) == 0);
    /* the same instant in the normalized form checks equal too */
    set_times(&st, -100000001, 12345679, -100000001, 12345679);
    EXPECT(metadata_check_stat(&st, &m, 1) == 0);
    /* a different instant does not */
    set_times(&st, -100000001, 12345678, -100000001, 12345679);
    EXPECT(metadata_check_stat(&st, &m, 1) == ENOTSUP);

    /* the mode: checked, or left to the platform */
    set_times(&st, -100000001, 12345679, -100000001, 12345679);
    st.st_mode = S_IFLNK | 0755;
    EXPECT(metadata_check_stat(&st, &m, 1) == ENOTSUP);
    EXPECT(metadata_check_stat(&st, &m, 0) == 0);

    if (failures == 0) printf("metadata from a stat: ok\n");
    return failures != 0;
}
