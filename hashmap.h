#ifndef HASHMAP_H
#define HASHMAP_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

//a entry struct (entry in the bucket's linked list)
typedef struct entry{
    char *key;
    char *value;
    struct entry *nxt;
} entry;

//the map itself
typedef struct{
    entry **bucketsPtr;
    int size;
    int count;
} map;

//function declarations
uint32_t calculateHash(const char *str);
map* createHashMap();
void map_put(map *m, const char *key, const char *value);
char* map_get(map *m, const char *key);
void printMap(const map *m, void (*printValue)(const void *value));
int map_remove(map *m, const char *key);
void map_free(map *m);
map* map_resize(map *m);

#endif
