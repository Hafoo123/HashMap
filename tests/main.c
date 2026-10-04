#include <stdio.h>
#include <string.h>
#include "test.h"

int passed = 0, failed = 0;

typedef struct {
    const char *name;
    void (*run)(void);
    const char *about;
    int byDefault;   /* 1 = runs when you give no arguments */
} suite;
static const suite suites[] = {
    { "map",   runMapTests,         "single-threaded hashmap (stage 1)",              1 },
    { "cmap",  runCMapTests,        "threaded tests on the concurrent map (stage 2)", 1 },
    { "persist", runPersistTests,   "log file, restart, crash recovery (stage 3)",    1 },
    { "plain", runPlainThreadTests, "threaded tests on the plain map, NO locks",      0 },
};

#define NUM_SUITES ((int)(sizeof suites / sizeof suites[0]))

static void usage(const char *prog) {
    printf("usage: %s [suite ...]\n\n", prog);
    printf("  (no args)   run the default suites (map, cmap, persist)\n");
    for (int i = 0; i < NUM_SUITES; i++)
        printf("  %-10s  %s\n", suites[i].name, suites[i].about);
    printf("\nexample: %s plain cmap\n", prog);
}

static const suite *findSuite(const char *name) {
    for (int i = 0; i < NUM_SUITES; i++)
        if (strcmp(suites[i].name, name) == 0) return &suites[i];
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        for (int i = 0; i < NUM_SUITES; i++) {
            if (!suites[i].byDefault) continue;
            printf("\n######## %s ########\n", suites[i].name);
            suites[i].run();
        }
    } else {
        /* check every name first, so a typo doesn't run half the suites */
        for (int a = 1; a < argc; a++) {
            if (findSuite(argv[a]) == NULL) {
                printf("unknown suite: %s\n\n", argv[a]);
                usage(argv[0]);
                return 2;
            }
        }
        for (int a = 1; a < argc; a++) {
            printf("\n######## %s ########\n", argv[a]);
            findSuite(argv[a])->run();
        }
    }

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}