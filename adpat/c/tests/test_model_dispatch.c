/* Model dispatch must belong to the model, including concurrent creation.
 * Source: OpenAI GPT-6 via Codex, P2 review regression, 2026-09-15. */
#define _POSIX_C_SOURCE 200809L
#include "../adapt_model.h"
#include <stdio.h>
#ifdef ADAPT_THREADS
#include <pthread.h>
#endif

static adapt_sizes small_sizes(void) {
    adapt_sizes s = {0};
    s.table_log = TABLE_LOG_MIN;
    s.match_log = MATCH_LOG_MIN;
    s.window_log = WINDOW_LOG_MIN;
    return s;
}

static uint32_t exercise(void) {
    model *m = model_new(small_sizes());
    uint32_t sum = 0;
    size_t i;
    if (m == NULL) return 0;
    {
        ADAPT_PROFILE(m, predict_p, update_p);
        for (i = 0; i < 2048; i++) {
            sum = sum * 33u + predict_p(m);
            update_p(m, (uint32_t)((i * 17u + i / 7u) & 1u));
        }
    }
    model_free(m);
    return sum;
}

#ifdef ADAPT_THREADS
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int ready, go;
static void *worker(void *context) {
    uint32_t *sum = context;
    pthread_mutex_lock(&lock);
    ready++;
    pthread_cond_broadcast(&changed);
    while (!go) pthread_cond_wait(&changed, &lock);
    pthread_mutex_unlock(&lock);
    *sum = exercise();
    return NULL;
}
#endif

int main(void) {
    uint32_t expected;
#if ADAPT_X86_V3 && !defined(_WIN32)
    model *first, *second;
    unsetenv("ADAPT_ISA");
    first = model_new(small_sizes());
    if (first == NULL) return 2;
    {
        ADAPT_PROFILE(first, before_predict, before_update);
        setenv("ADAPT_ISA", "baseline", 1);
        second = model_new(small_sizes());
        if (second == NULL) { model_free(first); return 2; }
        {
            ADAPT_PROFILE(first, after_predict, after_update);
            ADAPT_PROFILE(second, baseline_predict, baseline_update);
            if (before_predict != after_predict || before_update != after_update ||
                baseline_predict != predict || baseline_update != update) {
                fputs("creating another model changed an existing model's dispatch\n", stderr);
                model_free(first);
                model_free(second);
                return 1;
            }
        }
    }
    model_free(first);
    model_free(second);
    unsetenv("ADAPT_ISA");
#endif
    expected = exercise();
    if (expected == 0) return 2;
#ifdef ADAPT_THREADS
    {
        pthread_t threads[4];
        uint32_t sums[4] = {0};
        size_t i;
        for (i = 0; i < 4; i++) {
            if (pthread_create(&threads[i], NULL, worker, &sums[i]) != 0) return 2;
        }
        pthread_mutex_lock(&lock);
        while (ready != 4) pthread_cond_wait(&changed, &lock);
        go = 1;
        pthread_cond_broadcast(&changed);
        pthread_mutex_unlock(&lock);
        for (i = 0; i < 4; i++) {
            pthread_join(threads[i], NULL);
            if (sums[i] != expected) return 1;
        }
        pthread_cond_destroy(&changed);
        pthread_mutex_destroy(&lock);
    }
#endif
    puts("model dispatch: independent selection and concurrent predictions passed");
    return 0;
}
