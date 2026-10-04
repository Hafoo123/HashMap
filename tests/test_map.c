#include <stdio.h>
#include <string.h>
#include "test.h"
#include "hashmap.h"

/* ===== value printer ===== */

static void printStr(const void *v) {
    printf("%s", (const char *)v);
}

/* ===== helpers that look inside the map ===== */

static int chainLength(const entry *e) {
    int n = 0;
    for (; e != NULL; e = e->nxt) n++;
    return n;
}

/* walks every bucket and counts real entries (should always equal m->count) */
static int countEntries(const map *m) {
    int total = 0;
    for (int i = 0; i < m->size; i++) total += chainLength(m->bucketsPtr[i]);
    return total;
}

static int longestChain(const map *m) {
    int best = 0;
    for (int i = 0; i < m->size; i++) {
        int len = chainLength(m->bucketsPtr[i]);
        if (len > best) best = len;
    }
    return best;
}

/* ===== tests ===== */

static void testBasics(void) {
    printf("\n== basics ==\n");
    map *m = createHashMap();

    CHECK(m->count == 0, "new map is empty");
    CHECK(map_get(m, "nothing") == NULL, "get on empty map returns NULL");
    CHECK(map_remove(m, "nothing") == 0, "remove on empty map returns 0");

    map_put(m, "apple", "red");
    map_put(m, "eat", "five");
    map_put(m, "mahh", "three");

    CHECK(m->count == 3, "count is 3 after 3 puts");
    CHECK(streq(map_get(m, "apple"), "red"), "get apple");
    CHECK(streq(map_get(m, "eat"), "five"), "get eat");
    CHECK(streq(map_get(m, "mahh"), "three"), "get mahh");
    CHECK(map_get(m, "banana") == NULL, "missing key returns NULL");

    char buf[16] = "temp";
    map_put(m, buf, "copied");
    strcpy(buf, "XXXX");
    CHECK(streq(map_get(m, "temp"), "copied"), "key was copied, not borrowed");

    printMap(m, printStr);
    map_free(m);
}

static void testValueOwnership(void) {
    printf("\n== the map owns its values ==\n");
    map *m = createHashMap();

    /* the map must copy the value, not keep my pointer */
    char val[16] = "original";
    map_put(m, "k", val);
    strcpy(val, "CHANGED");
    CHECK(streq(map_get(m, "k"), "original"), "value was copied, not borrowed");
    CHECK(map_get(m, "k") != val, "map holds its own copy (different address)");

    /* updating must replace the copy (valgrind checks the old one got freed) */
    map_put(m, "k", "second");
    CHECK(streq(map_get(m, "k"), "second"), "update stores the new value");
    map_put(m, "k", "third");
    CHECK(streq(map_get(m, "k"), "third"), "second update works too");
    CHECK(m->count == 1, "updates never add entries");

    /* values can contain anything, including spaces */
    map_put(m, "with space", "hello world");
    CHECK(streq(map_get(m, "with space"), "hello world"), "spaces in key and value");

    /* empty string is a real value, different from 'missing' */
    map_put(m, "empty", "");
    CHECK(streq(map_get(m, "empty"), ""), "empty string is stored");
    CHECK(map_get(m, "empty") != NULL, "empty value is not the same as missing");

    CHECK(map_remove(m, "k") == 1, "remove returns 1 when found");
    CHECK(map_remove(m, "k") == 0, "remove returns 0 the second time");

    map_free(m);
}

static void testUpdate(void) {
    printf("\n== update existing key ==\n");
    map *m = createHashMap();

    map_put(m, "apple", "red");
    map_put(m, "apple", "green");

    CHECK(streq(map_get(m, "apple"), "green"), "second put replaces value");
    CHECK(m->count == 1, "count stays 1 after updating");

    printMap(m, printStr);
    map_free(m);
}

static void testCollisionsAndRemove(void) {
    printf("\n== collisions + remove ==\n");
    map *m = createHashMap();

    char key[16], val[16];
    for (int i = 0; i < 100; i++) {
        snprintf(key, sizeof key, "k%d", i);
        snprintf(val, sizeof val, "v%d", i * 10);
        map_put(m, key, val);
    }
    CHECK(m->count == 100, "count is 100");

    int allFound = 1;
    for (int i = 0; i < 100; i++) {
        snprintf(key, sizeof key, "k%d", i);
        snprintf(val, sizeof val, "v%d", i * 10);
        if (!streq(map_get(m, key), val)) allFound = 0;
    }
    CHECK(allFound, "all 100 keys found with right values");

    /* resize keeps chains short, so pick the LONGEST chain instead of assuming 3+ */
    entry *chain = NULL;
    int best = 0;
    for (int i = 0; i < m->size; i++) {
        int len = chainLength(m->bucketsPtr[i]);
        if (len > best) { best = len; chain = m->bucketsPtr[i]; }
    }
    printf("  longest chain has %d entries\n", best);
    CHECK(best >= 2, "found a bucket with 2+ entries");
    if (best < 2) { map_free(m); return; }

    /* copy the keys now, because remove frees them */
    char head[16], mid[16] = "", tail[16];
    strcpy(head, chain->key);
    if (best >= 3) strcpy(mid, chain->nxt->key);
    entry *last = chain;
    while (last->nxt) last = last->nxt;
    strcpy(tail, last->key);
    printf("  testing chain: head=%s middle=%s tail=%s\n",
           head, best >= 3 ? mid : "(none)", tail);

    int removed = 0;

    if (best >= 3) {
        CHECK(map_remove(m, mid) == 1, "remove middle of chain");
        CHECK(map_get(m, mid) == NULL, "middle is gone");
        CHECK(map_get(m, head) && map_get(m, tail), "head and tail still there");
        removed++;
    } else {
        printf("  (no chain of 3, skipping middle test)\n");
    }

    CHECK(map_remove(m, tail) == 1, "remove tail of chain");
    CHECK(map_get(m, tail) == NULL, "tail is gone");
    CHECK(map_get(m, head) != NULL, "head still there");
    removed++;

    CHECK(map_remove(m, head) == 1, "remove head of chain");
    CHECK(map_get(m, head) == NULL, "head is gone");
    removed++;

    CHECK(map_remove(m, head) == 0, "removing twice returns 0");
    CHECK(map_remove(m, "nope") == 0, "removing missing key returns 0");
    CHECK(m->count == 100 - removed, "count dropped by the number removed");
    CHECK(countEntries(m) == m->count, "count matches real number of entries");

    map_free(m);
}

static void testResize(void) {
    printf("\n== resize ==\n");

    /* small one you can look at */
    map *m = createHashMap();
    int startSize = m->size;
    char key[16], val[16];
    for (int i = 0; i < 12; i++) {
        snprintf(key, sizeof key, "item%d", i);
        snprintf(val, sizeof val, "%d", i);
        map_put(m, key, val);
    }
    CHECK(m->size > startSize, "map grew after 12 inserts");
    CHECK(countEntries(m) == 12, "all 12 entries survived the resize");
    printMap(m, printStr);
    map_free(m);

    /* big one */
    enum { N = 10000 };
    m = createHashMap();
    for (int i = 0; i < N; i++) {
        snprintf(key, sizeof key, "key%d", i);
        snprintf(val, sizeof val, "val%d", i);
        map_put(m, key, val);
    }

    printf("  after %d inserts: size %d, load %.2f, longest chain %d\n",
           N, m->size, (double)m->count / m->size, longestChain(m));

    CHECK(m->count == N, "count is 10000");
    CHECK(countEntries(m) == N, "no entries lost or duplicated while rehashing");
    CHECK(m->count <= m->size, "load factor stays at or below 1");
    CHECK(longestChain(m) <= 10, "chains stay short");

    int allFound = 1;
    for (int i = 0; i < N; i++) {
        snprintf(key, sizeof key, "key%d", i);
        snprintf(val, sizeof val, "val%d", i);
        if (!streq(map_get(m, key), val)) allFound = 0;
    }
    CHECK(allFound, "all 10000 keys found with right values after resizing");

    /* update after resize must not add a new entry */
    int sizeBefore = m->size;
    map_put(m, "key5000", "updated");
    CHECK(streq(map_get(m, "key5000"), "updated"), "update works after resize");
    CHECK(m->count == N, "update after resize doesn't change count");
    CHECK(m->size == sizeBefore, "update doesn't trigger a resize");

    /* remove everything */
    int allRemoved = 1;
    for (int i = 0; i < N; i++) {
        snprintf(key, sizeof key, "key%d", i);
        if (map_remove(m, key) != 1) allRemoved = 0;
    }
    CHECK(allRemoved, "every key could be removed");
    CHECK(m->count == 0, "count is 0 after removing everything");
    CHECK(countEntries(m) == 0, "no entries left in any bucket");

    map_free(m);
}

void runMapTests(void) {
    testBasics();
    testValueOwnership();
    testUpdate();
    testCollisionsAndRemove();
    testResize();
}