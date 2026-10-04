#ifndef TEST_H
#define TEST_H

#include <stdio.h>
#include <string.h>

/* shared by every test file, defined once in main.c */
extern int passed, failed;

/* fflush so no line is lost if the program crashes right after */
#define CHECK(cond, msg) do { \
    if (cond) { passed++; printf("  \033[32mPASS\033[0m %s\n", msg); } \
    else      { failed++; printf("  \033[31mFAIL\033[0m %s (%s line %d)\n", msg, __FILE__, __LINE__); } \
    fflush(stdout); \
} while (0)

static inline int streq(const void *a, const char *b) {
    return a != NULL && strcmp((const char *)a, b) == 0;
}

/* one entry point per test suite */
void runMapTests(void);
void runPlainThreadTests(void);
void runCMapTests(void);
void runPersistTests(void);

#endif