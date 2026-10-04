#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include "test.h"
#include "concurrentMap.h"

#define LOG_PATH "test_persist.aof"

/* ===== helpers ===== */

/* 1 if key has exactly this value (expected NULL = key must be missing). frees the copy */
static int has(ConcurrentMap *m, const char *key, const char *expected) {
    char *got = cmap_get(m, key);
    int ok = expected ? streq(got, expected) : (got == NULL);
    free(got);
    return ok;
}

static long fileSize(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;   /* -1 = file doesn't exist */
    return (long)st.st_size;
}

/* reads the whole file into a malloc'd string (caller frees), NULL if missing */
static char *readFile(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(n + 1);
    size_t got = fread(buf, 1, n, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

static void writeFile(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    fputs(text, f);
    fclose(f);
}

static void appendFile(const char *path, const char *text) {
    FILE *f = fopen(path, "ab");
    fputs(text, f);
    fclose(f);
}

/* ===== tests ===== */

static void testFreshStart(void) {
    printf("\n== fresh start ==\n");
    remove(LOG_PATH);

    ConcurrentMap *m = cmap_createPersistent(LOG_PATH, FSYNC_ALWAYS);
    CHECK(m != NULL, "open works when the file doesn't exist yet");
    if (!m) return;
    CHECK(cmap_count(m) == 0, "new map is empty");
    CHECK(fileSize(LOG_PATH) == 0, "empty log file was created");
    cmap_free(m);
}

static void testLogFormat(void) {
    printf("\n== what gets written to the log ==\n");
    remove(LOG_PATH);

    ConcurrentMap *m = cmap_createPersistent(LOG_PATH, FSYNC_ALWAYS);
    if (!m) { CHECK(0, "open"); return; }

    cmap_put(m, "cat", "5");
    char *text = readFile(LOG_PATH);
    CHECK(streq(text, "SET 3 cat 1 5\n"), "put writes exactly one SET record");
    free(text);

    cmap_remove(m, "nope");
    CHECK(fileSize(LOG_PATH) == (long)strlen("SET 3 cat 1 5\n"),
          "removing a missing key writes nothing");

    cmap_remove(m, "cat");
    text = readFile(LOG_PATH);
    CHECK(streq(text, "SET 3 cat 1 5\nDEL 3 cat\n"), "remove writes a DEL record");
    free(text);

    cmap_free(m);
}

static void testInMemoryWritesNothing(void) {
    printf("\n== cmap_create never touches the disk ==\n");
    remove(LOG_PATH);

    ConcurrentMap *m = cmap_create();
    cmap_put(m, "cat", "5");
    cmap_remove(m, "cat");
    cmap_put(m, "dog", "7");
    CHECK(fileSize(LOG_PATH) == -1, "no log file appears");
    CHECK(has(m, "dog", "7"), "the map still works in memory");
    cmap_free(m);
}

static void testRestart(void) {
    printf("\n== data survives a restart ==\n");
    remove(LOG_PATH);

    ConcurrentMap *m = cmap_createPersistent(LOG_PATH, FSYNC_ALWAYS);
    if (!m) { CHECK(0, "open"); return; }
    cmap_put(m, "cat", "5");
    cmap_put(m, "dog", "7");
    cmap_put(m, "cat", "9");                       /* update */
    cmap_remove(m, "dog");                         /* remove */
    cmap_put(m, "note", "line1\nline2");           /* newline inside */
    cmap_put(m, "with space", "hello world");      /* spaces in key and value */
    cmap_put(m, "empty", "");                      /* empty value */
    cmap_free(m);                                  /* "program dies" */

    m = cmap_createPersistent(LOG_PATH, FSYNC_ALWAYS);                       /* "program restarts" */
    CHECK(m != NULL, "reopen works");
    if (!m) return;
    CHECK(has(m, "cat", "9"), "updated value is the latest one");
    CHECK(has(m, "dog", NULL), "removed key stays removed");
    CHECK(has(m, "note", "line1\nline2"), "value with a newline survives");
    CHECK(has(m, "with space", "hello world"), "spaces in key and value survive");
    CHECK(has(m, "empty", ""), "empty value survives (and is not 'missing')");
    CHECK(cmap_count(m) == 4, "count is 4 after restart");
    cmap_free(m);
}

static void testReplayDoesNotRelog(void) {
    printf("\n== replay doesn't write the log again ==\n");
    /* uses the file from testRestart */
    long before = fileSize(LOG_PATH);

    for (int i = 0; i < 3; i++) {
        ConcurrentMap *m = cmap_createPersistent(LOG_PATH, FSYNC_ALWAYS);
        if (m) cmap_free(m);
    }
    CHECK(fileSize(LOG_PATH) == before, "3 open/close cycles leave the file the same size");
}

static void testManyKeys(void) {
    printf("\n== 500 keys survive a restart ==\n");
    remove(LOG_PATH);

    enum { N = 500 };
    char key[32], val[32];

    ConcurrentMap *m = cmap_createPersistent(LOG_PATH, FSYNC_ALWAYS);
    if (!m) { CHECK(0, "open"); return; }
    for (int i = 0; i < N; i++) {
        snprintf(key, sizeof key, "key%d", i);
        snprintf(val, sizeof val, "val%d", i);
        cmap_put(m, key, val);
    }
    for (int i = 0; i < N; i += 5) {               /* remove every 5th */
        snprintf(key, sizeof key, "key%d", i);
        cmap_remove(m, key);
    }
    cmap_free(m);

    m = cmap_createPersistent(LOG_PATH, FSYNC_ALWAYS);
    if (!m) { CHECK(0, "reopen"); return; }
    int wrong = 0;
    for (int i = 0; i < N; i++) {
        snprintf(key, sizeof key, "key%d", i);
        snprintf(val, sizeof val, "val%d", i);
        if (!has(m, key, i % 5 == 0 ? NULL : val)) wrong++;
    }
    if (wrong) printf("  %d keys wrong after restart\n", wrong);
    CHECK(wrong == 0, "every key has the right value (or is gone)");
    CHECK(cmap_count(m) == N - N / 5, "count is 400");
    cmap_free(m);
}

/* ---- threads writing through the log at the same time ---- */

#define P_THREADS 4
#define P_KEYS    100

typedef struct { ConcurrentMap *m; int id; } pworker;

static void *persistWorker(void *arg) {
    pworker *w = arg;
    char key[32], val[32];
    for (int i = 0; i < P_KEYS; i++) {
        snprintf(key, sizeof key, "t%d_k%d", w->id, i);
        snprintf(val, sizeof val, "t%d_v%d", w->id, i);
        cmap_put(w->m, key, val);
    }
    return NULL;
}

static void testThreadsWithLog(void) {
    printf("\n== %d threads writing through the log ==\n", P_THREADS);
    remove(LOG_PATH);

    ConcurrentMap *m = cmap_createPersistent(LOG_PATH, FSYNC_ALWAYS);
    if (!m) { CHECK(0, "open"); return; }

    pthread_t th[P_THREADS];
    pworker ws[P_THREADS];
    for (int t = 0; t < P_THREADS; t++) {
        ws[t].m = m;
        ws[t].id = t;
        pthread_create(&th[t], NULL, persistWorker, &ws[t]);
    }
    for (int t = 0; t < P_THREADS; t++) pthread_join(th[t], NULL);
    cmap_free(m);

    /* if two records ever got mixed together, replay would stop early */
    m = cmap_createPersistent(LOG_PATH, FSYNC_ALWAYS);
    CHECK(m != NULL, "log written by 4 threads replays cleanly");
    if (!m) return;

    int wrong = 0;
    char key[32], val[32];
    for (int t = 0; t < P_THREADS; t++) {
        for (int i = 0; i < P_KEYS; i++) {
            snprintf(key, sizeof key, "t%d_k%d", t, i);
            snprintf(val, sizeof val, "t%d_v%d", t, i);
            if (!has(m, key, val)) wrong++;
        }
    }
    if (wrong) printf("  %d keys wrong after restart\n", wrong);
    CHECK(wrong == 0, "all 400 keys from all threads survived");
    CHECK(cmap_count(m) == P_THREADS * P_KEYS, "count is 400");
    cmap_free(m);
}

/* ---- crash in the middle of a write ---- */

static void testCrashTail(void) {
    printf("\n== crash mid-write: cut at every possible byte ==\n");

    const char *good    = "SET 3 cat 1 5\n";
    const char *partial = "SET 3 owl 1 9\n";   /* the record the "crash" interrupts */
    int len = (int)strlen(partial);

    int opened = 0, cut = 0, kept = 0, survived = 0;

    /* try every prefix: "S", "SE", "SET", ... up to one byte short of the full record */
    for (int n = 1; n < len; n++) {
        writeFile(LOG_PATH, good);
        char piece[32];
        memcpy(piece, partial, n);
        piece[n] = '\0';
        appendFile(LOG_PATH, piece);

        ConcurrentMap *m = cmap_createPersistent(LOG_PATH, FSYNC_ALWAYS);
        if (!m) continue;
        opened++;
        if (fileSize(LOG_PATH) == (long)strlen(good)) cut++;
        if (has(m, "cat", "5") && has(m, "owl", NULL)) kept++;

        cmap_put(m, "fox", "2");                  /* write after the crash */
        cmap_free(m);

        m = cmap_createPersistent(LOG_PATH, FSYNC_ALWAYS);
        if (m && has(m, "fox", "2") && has(m, "cat", "5")) survived++;
        if (m) cmap_free(m);
    }

    int cases = len - 1;
    printf("  tried %d cut points\n", cases);
    CHECK(opened == cases, "opens no matter where the record was cut");
    CHECK(cut == cases, "half record is always removed from the file");
    CHECK(kept == cases, "older records stay, half record is never applied");
    CHECK(survived == cases, "a write after the crash survives the next restart");
}

static void testCorruptMiddle(void) {
    printf("\n== corruption in the middle ==\n");

    const char *text = "SET 1 a 1 1\nXET 1 b 1 2\nSET 1 c 1 3\n";
    writeFile(LOG_PATH, text);
    long size = fileSize(LOG_PATH);

    ConcurrentMap *m = cmap_createPersistent(LOG_PATH, FSYNC_ALWAYS);
    printf("\n");
    CHECK(m == NULL, "refuses to open");
    CHECK(fileSize(LOG_PATH) == size, "file is not touched");
    char *after = readFile(LOG_PATH);
    CHECK(streq(after, text), "content is exactly the same");
    free(after);
    if (m) cmap_free(m);
}


/* ---- fsync every second ---- */

static double nowSec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static double timePuts(FsyncMode mode, int n) {
    remove(LOG_PATH);
    ConcurrentMap *m = cmap_createPersistent(LOG_PATH, mode);
    if (!m) return -1;
    char key[32];
    double start = nowSec();
    for (int i = 0; i < n; i++) {
        snprintf(key, sizeof key, "k%d", i);
        cmap_put(m, key, "v");
    }
    double secs = nowSec() - start;
    cmap_free(m);
    return secs;
}

static void testEverySecond(void) {
    printf("\n== fsync every second ==\n");
    remove(LOG_PATH);

    ConcurrentMap *m = cmap_createPersistent(LOG_PATH, FSYNC_EVERYSEC);
    CHECK(m != NULL, "opens in EVERYSEC mode");
    if (!m) return;
    cmap_put(m, "cat", "5");
    cmap_put(m, "dog", "7");
    cmap_remove(m, "dog");

    double start = nowSec();
    cmap_free(m);                      /* must stop the flush thread */
    double waited = nowSec() - start;
    printf("  cmap_free waited %.2f s for the flush thread\n", waited);
    CHECK(waited < 1.5, "cmap_free stops the flush thread (no hang)");

    m = cmap_createPersistent(LOG_PATH, FSYNC_ALWAYS);
    CHECK(m && has(m, "cat", "5") && has(m, "dog", NULL), "data written in EVERYSEC mode survives a restart");
    if (m) cmap_free(m);

    enum { N = 300 };
    double always = timePuts(FSYNC_ALWAYS, N);
    double everysec = timePuts(FSYNC_EVERYSEC, N);
    printf("  %d puts:  ALWAYS %.3f s,  EVERYSEC %.3f s\n",
           N, always, everysec);
}

void runPersistTests(void) {
    testFreshStart();
    testLogFormat();
    testInMemoryWritesNothing();
    testRestart();
    testReplayDoesNotRelog();
    testManyKeys();
    testThreadsWithLog();
    testCrashTail();
    testCorruptMiddle();
    testEverySecond();
    remove(LOG_PATH);
}