#ifndef CONCURENTMAP_H
#define CONCURENTMAP_H

#include <pthread.h>
#include "hashmap.h"

//how much of the hash bits should go for choosing the segment (num segments is essentialy 2^segmentBits)
#define SEGMENT_BITS 4
#define NUM_SEGMENTS (1<<SEGMENT_BITS)

//fsync mode - always- everywrite, everysec - thread that fsync every second instead
typedef enum {
    FSYNC_ALWAYS,
    FSYNC_EVERYSEC
} FsyncMode;

//a segment in the concurent hashmap, a segment can be looked as just a mini map
typedef struct segment{
    map *segmentMap;
    pthread_mutex_t lock;
} segment;

//the map itself, containing also persistent info
typedef struct ConcurrentMap{
    segment* segments;
    int logfd;
    pthread_mutex_t logLock;
    FsyncMode mode;
    pthread_t flusher;
    int stopFlusher;
    pthread_mutex_t flushLock;
} ConcurrentMap;


//function declarations
ConcurrentMap* cmap_create(void);
ConcurrentMap* cmap_createPersistent(const char *path,FsyncMode mode);
void cmap_put(ConcurrentMap *cmap, const char *key, const char *value);
char* cmap_get(ConcurrentMap *cmap, const char *key);
int cmap_remove(ConcurrentMap *cmap, const char *key);
void cmap_free(ConcurrentMap *cmap);
int cmap_count(ConcurrentMap *cmap);
#endif