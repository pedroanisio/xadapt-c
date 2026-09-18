/* What the examples share: a check that names what failed and exits 1, so
 * `make check` treats each example as a test of the interface it shows.
 *
 * Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-17. */
#ifndef ADAPT_EXAMPLE_H
#define ADAPT_EXAMPLE_H
#include "../adapt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "%s:%d: expected %s\n", __FILE__, __LINE__, #cond); \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

/* A status with its message, for a failure the example did not expect. */
#define EXPECT_OK(status, error)                                                                          \
    do {                                                                                                  \
        adapt_status s_ = (status);                                                                       \
        if (s_ != ADAPT_OK) {                                                                             \
            char m_[160];                                                                                 \
            adapt_error_message((error), m_, sizeof(m_));                                                 \
            fprintf(stderr, "%s:%d: status %d: %s\n", __FILE__, __LINE__, (int)s_, m_);                    \
            exit(1);                                                                                      \
        }                                                                                                 \
    } while (0)

/* A sample input: text with structure enough for the model to learn.
 * `static inline` so an example that does not call it compiles clean. */
static inline uint8_t *example_input(size_t *len) {
    static const char *words[] = {"the", "codec", "reads", "bytes", "and", "writes", "fewer", "of", "them", "\n"};
    size_t n = 200000, at = 0;
    uint8_t *buf = malloc(n + 16);
    unsigned seed = 12345;
    if (buf == NULL) exit(1);
    while (at < n) {
        const char *w;
        seed = seed * 1103515245u + 12345u;
        w = words[(seed >> 16) % 10];
        at += (size_t)sprintf((char *)buf + at, "%s ", w);
    }
    *len = at;
    return buf;
}
#endif
