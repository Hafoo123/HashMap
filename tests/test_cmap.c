#define _POSIX_C_SOURCE 200809L   /* for strsignal, strdup, clock_gettime */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include "test.h"
#include "hashmap.h"
#include "concurrentMap.h"

#define THREADS          8
#define KEYS_PER_THREAD  10000
#define SHARED_KEYS      100
#define SHARED_ROUNDS    1000
#define TIMEOUT_SECONDS  10

/* =====================================================================
   The "ops" table: the tests only talk to a map through these pointers,
   so the same tests can run on any map implementation.

   get() ALWAYS returns a copy that the caller must free (or NULL).
   That's what cmap_get does, so the plain wrapper copies too.
   ===================================================================== */

typedef struct {
    const char *name;
    void *(*create)(void);
    void  (*put)(void *m, const char *key, const char *value);
    char *(*get)(void *m, const char *key);
    int   (*remove)(void *m, const char *key);
    int   (*count)(void *m);
    void  (*destroy)(void *m);
} mapOps;

/* --- plain map (no locks at all) --- */
static void *plainCreate(void)                                { return createHashMap(); }
static void  plainPut(void *m, const char *k, const char *v)  { map_put(m, k, v); }
static char *plainGet(void *m, const char *k) {
    char *v = map_get(m, k);
    return v ? strdup(v) : NULL;
}
static int   plainRemove(void *m, const char *k)              { return map_remove(m, k); }
static int   plainCount(void *m)                              { return ((map *)m)->count; }
static void  plainDestroy(void *m)                            { map_free(m); }

static const mapOps plainOps = {
    "plain map", plainCreate, plainPut, plainGet, plainRemove, plainCount, plainDestroy
};

/* --- concurrent map (16 segment locks) --- */
static void *cCreate(void)                                    { return cmap_create(); }
static void  cPut(void *m, const char *k, const char *v)      { cmap_put(m, k, v); }
static char *cGet(void *m, const char *k)                     { return cmap_get(m, k); }
static int   cRemove(void *m, const char *k)                  { return cmap_remove(m, k); }
static int   cCount(void *m)                                  { return cmap_count(m); }
static void  cDestroy(void *m)                                { cmap_free(m); }

static const mapOps cmapOps = {
    "cmap", cCreate, cPut, cGet, cRemove, cCount, cDestroy
};

/* ===== get helpers: compare, then free the copy ===== */

/* 1 if the key exists and its value equals expected */
static int getEquals(const mapOps *ops, void *m, const char *key, const char *expected) {
    char *v = ops->get(m, key);
    int ok = streq(v, expected);
    free(v);
    return ok;
}

/* 1 if the key is missing */
static int getMissing(const mapOps *ops, void *m, const char *key) {
    char *v = ops->get(m, key);
    int missing = (v == NULL);
    free(v);
    return missing;
}

/* ===== thread helpers ===== */

typedef struct {
    const mapOps *ops;
    void *m;
    int id;       /* 0..7 */
    int errors;   /* the thread counts its own problems, main checks them after join */
} worker;

static double nowSeconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* starts THREADS threads running fn, waits for all of them, returns seconds taken */
static double runWorkers(const mapOps *ops, void *m, void *(*fn)(void *), worker *ws) {
    pthread_t th[THREADS];
    double start = nowSeconds();

    for (int t = 0; t < THREADS; t++) {
        ws[t].ops = ops;
        ws[t].m = m;
        ws[t].id = t;
        ws[t].errors = 0;
        pthread_create(&th[t], NULL, fn, &ws[t]);
    }
    for (int t = 0; t < THREADS; t++) {
        pthread_join(th[t], NULL);
    }
    return nowSeconds() - start;
}

static int totalErrors(const worker *ws) {
    int total = 0;
    for (int t = 0; t < THREADS; t++) total += ws[t].errors;
    return total;
}

/* =====================================================================
   Run one test in a CHILD process (fork, from OSTEP ch 5).
   If the child crashes or hangs, only the child dies: the parent
   reports it and moves on. The child sends pass/fail back via a pipe.
   ===================================================================== */

typedef void (*testFn)(const mapOps *ops);

static void runIsolated(testFn test, const mapOps *ops) {
    int fd[2];
    if (pipe(fd) != 0) { perror("pipe"); return; }
    fflush(stdout);   /* or the child would print the parent's buffered text again */

    pid_t pid = fork();
    if (pid == 0) {
        /* ---- child ---- */
        close(fd[0]);
        alarm(TIMEOUT_SECONDS);           /* stuck in an endless loop? get killed */
        int p0 = passed, f0 = failed;
        test(ops);
        int result[2] = { passed - p0, failed - f0 };
        if (write(fd[1], result, sizeof result) != (ssize_t)sizeof result) _exit(1);
        fflush(stdout);
        _exit(0);
    }

    /* ---- parent ---- */
    close(fd[1]);
    int result[2];
    ssize_t n = read(fd[0], result, sizeof result);
    close(fd[0]);

    int status;
    waitpid(pid, &status, 0);

    if (n == (ssize_t)sizeof result && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        passed += result[0];
        failed += result[1];
    } else if (WIFSIGNALED(status) && WTERMSIG(status) == SIGALRM) {
        failed++;
        printf("  \033[31mHANG\033[0m  stuck for %d s, killed it (probably a loop in a broken list)\n",
               TIMEOUT_SECONDS);
    } else if (WIFSIGNALED(status)) {
        failed++;
        printf("  \033[31mCRASH\033[0m %s (signal %d)\n", strsignal(WTERMSIG(status)), WTERMSIG(status));
    } else {
        failed++;
        printf("  \033[31mFAIL\033[0m  child exited strangely\n");
    }
    fflush(stdout);
}

/* ===== test 1: one thread, basic operations ===== */

static void testBasics(const mapOps *ops) {
    printf("\n== %s: basics (1 thread) ==\n", ops->name);
    fflush(stdout);
    void *m = ops->create();

    CHECK(ops->count(m) == 0, "new map is empty");
    CHECK(getMissing(ops, m, "nothing"), "get on empty map returns NULL");
    CHECK(ops->remove(m, "nothing") == 0, "remove on empty map returns 0");

    ops->put(m, "apple", "red");
    ops->put(m, "eat", "five");
    CHECK(ops->count(m) == 2, "count is 2 after 2 puts");
    CHECK(getEquals(ops, m, "apple", "red"), "get apple");

    ops->put(m, "apple", "green");
    CHECK(getEquals(ops, m, "apple", "green"), "update replaces value");
    CHECK(ops->count(m) == 2, "update doesn't add an entry");

    /* get returns a COPY: changing it must not change the map */
    char *copy = ops->get(m, "apple");
    if (copy) copy[0] = 'X';
    free(copy);
    CHECK(getEquals(ops, m, "apple", "green"), "get returns a copy, map is untouched");

    CHECK(ops->remove(m, "eat") == 1, "remove returns 1 when found");
    CHECK(getMissing(ops, m, "eat"), "removed key is gone");
    CHECK(ops->count(m) == 1, "count is 1 after remove");

    ops->destroy(m);
}

/* ===== test 2: 8 threads, each puts only its own keys ===== */

static void *putWorker(void *arg) {
    worker *w = arg;
    char key[32], val[32];
    for (int i = 0; i < KEYS_PER_THREAD; i++) {
        snprintf(key, sizeof key, "t%d_k%d", w->id, i);
        snprintf(val, sizeof val, "t%d_v%d", w->id, i);
        w->ops->put(w->m, key, val);
    }
    return NULL;
}

static void testParallelPut(const mapOps *ops) {
    printf("\n== %s: %d threads, each puts its own %d keys ==\n", ops->name, THREADS, KEYS_PER_THREAD);
    fflush(stdout);
    void *m = ops->create();
    worker ws[THREADS];

    double secs = runWorkers(ops, m, putWorker, ws);
    printf("  %d puts in %.3f s\n", THREADS * KEYS_PER_THREAD, secs);

    int count = ops->count(m);
    if (count != THREADS * KEYS_PER_THREAD)
        printf("  count is %d, expected %d\n", count, THREADS * KEYS_PER_THREAD);
    CHECK(count == THREADS * KEYS_PER_THREAD, "count is 80000");

    int missing = 0;
    char key[32], val[32];
    for (int t = 0; t < THREADS; t++) {
        for (int i = 0; i < KEYS_PER_THREAD; i++) {
            snprintf(key, sizeof key, "t%d_k%d", t, i);
            snprintf(val, sizeof val, "t%d_v%d", t, i);
            if (!getEquals(ops, m, key, val)) missing++;
        }
    }
    if (missing) printf("  %d keys missing or wrong\n", missing);
    CHECK(missing == 0, "every key from every thread is there with the right value");

    ops->destroy(m);
}

/* ===== test 3: put + get + remove all mixed together ===== */

static void *mixedWorker(void *arg) {
    worker *w = arg;
    char key[32], val[32];

    /* put all my keys */
    for (int i = 0; i < KEYS_PER_THREAD; i++) {
        snprintf(key, sizeof key, "t%d_k%d", w->id, i);
        snprintf(val, sizeof val, "t%d_v%d", w->id, i);
        w->ops->put(w->m, key, val);
    }
    /* read them all back */
    for (int i = 0; i < KEYS_PER_THREAD; i++) {
        snprintf(key, sizeof key, "t%d_k%d", w->id, i);
        snprintf(val, sizeof val, "t%d_v%d", w->id, i);
        if (!getEquals(w->ops, w->m, key, val)) w->errors++;
    }
    /* remove the even ones */
    for (int i = 0; i < KEYS_PER_THREAD; i += 2) {
        snprintf(key, sizeof key, "t%d_k%d", w->id, i);
        if (w->ops->remove(w->m, key) != 1) w->errors++;
    }
    /* evens must be gone, odds must still be there */
    for (int i = 0; i < KEYS_PER_THREAD; i++) {
        snprintf(key, sizeof key, "t%d_k%d", w->id, i);
        snprintf(val, sizeof val, "t%d_v%d", w->id, i);
        if (i % 2 == 0 && !getMissing(w->ops, w->m, key)) w->errors++;
        if (i % 2 == 1 && !getEquals(w->ops, w->m, key, val)) w->errors++;
    }
    return NULL;
}

static void testParallelMixed(const mapOps *ops) {
    printf("\n== %s: %d threads, put + get + remove mixed ==\n", ops->name, THREADS);
    fflush(stdout);
    void *m = ops->create();
    worker ws[THREADS];

    double secs = runWorkers(ops, m, mixedWorker, ws);
    printf("  finished in %.3f s\n", secs);

    int errors = totalErrors(ws);
    if (errors) printf("  threads saw %d wrong results\n", errors);
    CHECK(errors == 0, "no thread saw a wrong value");

    int count = ops->count(m);
    if (count != THREADS * KEYS_PER_THREAD / 2)
        printf("  count is %d, expected %d\n", count, THREADS * KEYS_PER_THREAD / 2);
    CHECK(count == THREADS * KEYS_PER_THREAD / 2, "count is 40000 (half removed)");

    ops->destroy(m);
}

/* ===== test 4: 8 threads fighting over the SAME 100 keys ===== */

static void *sharedWorker(void *arg) {
    worker *w = arg;
    char key[32], val[32];
    for (int r = 0; r < SHARED_ROUNDS; r++) {
        for (int k = 0; k < SHARED_KEYS; k++) {
            snprintf(key, sizeof key, "shared%d", k);
            snprintf(val, sizeof val, "t%d_s%d", w->id, k);
            w->ops->put(w->m, key, val);
            /* nobody removes, so after my own put the key must exist */
            if (getMissing(w->ops, w->m, key)) w->errors++;
        }
    }
    return NULL;
}

static void testSharedKeys(const mapOps *ops) {
    printf("\n== %s: %d threads updating the same %d keys ==\n", ops->name, THREADS, SHARED_KEYS);
    fflush(stdout);
    void *m = ops->create();
    worker ws[THREADS];

    double secs = runWorkers(ops, m, sharedWorker, ws);
    printf("  %d puts in %.3f s\n", THREADS * SHARED_ROUNDS * SHARED_KEYS, secs);

    int errors = totalErrors(ws);
    if (errors) printf("  a key vanished %d times\n", errors);
    CHECK(errors == 0, "a key never vanished right after being put");

    int count = ops->count(m);
    if (count != SHARED_KEYS) printf("  count is %d, expected %d\n", count, SHARED_KEYS);
    CHECK(count == SHARED_KEYS, "count is exactly 100 (updates never duplicate)");

    /* the final value of each key must be one that some thread really wrote */
    int allValid = 1;
    char key[32], val[32];
    for (int k = 0; k < SHARED_KEYS; k++) {
        snprintf(key, sizeof key, "shared%d", k);
        char *got = ops->get(m, key);
        int ok = 0;
        for (int t = 0; t < THREADS; t++) {
            snprintf(val, sizeof val, "t%d_s%d", t, k);
            if (streq(got, val)) ok = 1;
        }
        if (!ok) allValid = 0;
        free(got);
    }
    CHECK(allValid, "every final value came from one of the threads");

    ops->destroy(m);
}

/* ===== cmap only: do keys spread evenly over the segments? ===== */

static void testSegmentSpread(const mapOps *ops) {
    (void)ops;
    printf("\n== cmap: keys spread over segments ==\n");
    ConcurrentMap *cm = cmap_create();

    enum { N = 1600 };
    char key[32];
    for (int i = 0; i < N; i++) {
        snprintf(key, sizeof key, "spread%d", i);
        cmap_put(cm, key, "x");
    }

    int empty = 0, min = N, max = 0;
    printf("  keys per segment:");
    for (int s = 0; s < NUM_SEGMENTS; s++) {
        int c = cm->segments[s].segmentMap->count;
        printf(" %d", c);
        if (c == 0) empty++;
        if (c < min) min = c;
        if (c > max) max = c;
    }
    printf("\n");

    CHECK(cmap_count(cm) == N, "count is 1600");
    CHECK(empty == 0, "every segment got some keys");
    CHECK(max <= 2 * min, "no segment is badly overloaded");

    cmap_free(cm);
}

/* ===== suites ===== */

static void runThreadTests(const mapOps *ops) {
    runIsolated(testBasics, ops);
    runIsolated(testParallelPut, ops);
    runIsolated(testParallelMixed, ops);
    runIsolated(testSharedKeys, ops);
}

void runPlainThreadTests(void) {
    printf("\n(no locks: expect wrong counts, crashes or hangs. Run it a few times,\n"
           " the result changes every run. That randomness IS the race condition.)\n");
    runThreadTests(&plainOps);
}

void runCMapTests(void) {
    runIsolated(testSegmentSpread, &cmapOps);
    runThreadTests(&cmapOps);
}