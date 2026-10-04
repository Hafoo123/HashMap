#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hashmap.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include "concurrentMap.h"

//indication codes of ok vs crashed vs corrupt journal
typedef enum {
    REPLAY_OK,
    REPLAY_INCOMPLETE,
    REPLAY_CORRUPT
} ReplayStatus;

//declaration of static functions
static ReplayStatus cmap_buildFromReplay(ConcurrentMap *cmap, char *buff, size_t size, size_t *goodbytes);

static void *flush(void *arg);

//create a concurrent map, (not persistent)
ConcurrentMap* cmap_create(void){
    //allocating the structs
    ConcurrentMap *cmap = malloc(sizeof(ConcurrentMap));
    cmap->segments = calloc(NUM_SEGMENTS,sizeof(segment));

    //creating cmap data (persistent purposes)
    cmap->mode = FSYNC_ALWAYS;
    cmap->stopFlusher = 0;
    pthread_mutex_init(&cmap->flushLock,NULL);     
    cmap->logfd = -1;
    pthread_mutex_init(&cmap->logLock,NULL);

    //creating the minimap per segment
    for (int i = 0; i < NUM_SEGMENTS; i++)
    {
        cmap->segments[i].segmentMap = createHashMap();
        pthread_mutex_init(&cmap->segments[i].lock,NULL);
    
    }
    return cmap;

}

//build the hashmap from the journal
static ReplayStatus cmap_buildFromReplay(ConcurrentMap *cmap, char *buff, size_t size, size_t *goodbytes){
    char *p = buff;
    char *end = buff + size;
    //build start and end pointers with the file (journal file was transitioned to memory so its buff)
    while (p<end)
    {
        //get the instruction (set/del)
        char *start = p;
        char *value = NULL;
        char *key = NULL;
        if (end - p < 4) goto incomplete;
        int isSet = (memcmp(p, "SET ", 4) == 0);
        int isDel = (memcmp(p, "DEL ", 4) == 0);
        if (!isSet && !isDel) goto corrupt;
        p += 4;
        
        //get the key length
        char *after;
        unsigned long klen = strtoul(p, &after, 10);
        if (after == p){
            if (p==end)
            {
                goto incomplete;
            }
            if(p<end){
                goto corrupt;
            }
        }
        p = after;
        if (p >= end) goto incomplete;
        if (*p != ' ') goto corrupt;
        
        //get the key
        p++;
        if ((unsigned long)(end-p)<klen) goto incomplete;
        key = malloc(klen+1);
        memcpy(key,p,klen);
        key[klen] = '\0';
        p+=klen;

        //if its set there is also value:
        unsigned long vlen = 0;

        if (isSet)
        {
            //find value length
            if (p >= end) goto incomplete;
            if (*p != ' ') goto corrupt;
            p++;
            char *aftervalue;
            vlen = strtoul(p, &aftervalue, 10);
            if (aftervalue == p){
                if (p==end)
                {
                    goto incomplete;
                }
                if(p<end){
                    goto corrupt;
                }
            }
            p = aftervalue;
            if (p >= end) goto incomplete;
            if (*p != ' ') goto corrupt;
            p++;
            
            //find the value it self
            if ((unsigned long)(end-p)<vlen) goto incomplete;
            value = malloc(vlen+1);
            memcpy(value,p,vlen);
            value[vlen] = '\0';
            p+=vlen;
        }
        if (p >= end) goto incomplete;
        if (*p != '\n') goto corrupt;
        p++;

        //do the actual instruction
        if (isSet)
        {
            int seg = calculateHash(key)>>(32-SEGMENT_BITS);
            pthread_mutex_lock(&cmap->segments[seg].lock);
            map_put(cmap->segments[seg].segmentMap, key , value);
            pthread_mutex_unlock(&cmap->segments[seg].lock);
        }
        if (isDel)
        {
            int seg = calculateHash(key)>>(32-SEGMENT_BITS);
            pthread_mutex_lock(&cmap->segments[seg].lock);
            map_remove(cmap->segments[seg].segmentMap, key);
            pthread_mutex_unlock(&cmap->segments[seg].lock);
        }

        //free and do the next line
        free(key);
        free(value);
        continue;
        //handle corrupt or incomplete file (incomplete means the something crashed, thus it stopped in the middle of writing a line)
        corrupt: free(key); free(value); *goodbytes = start-buff;  return REPLAY_CORRUPT; 
        incomplete: free(key); free(value); *goodbytes = start-buff; return REPLAY_INCOMPLETE;
    }
    //good bytes is the amount of bytes of valid lines in the file
    *goodbytes = size;
    return REPLAY_OK;
}

//flush the write using fsync every second, USED ONLY IF FSYNC_EVERYSEC IS CHOOSEN
static void *flush(void *arg){
    ConcurrentMap *cmap = arg;
    while (1)
    {
        sleep(1);
        int shouldStop = 0;
        pthread_mutex_lock(&cmap->flushLock);
        shouldStop = cmap->stopFlusher;
        pthread_mutex_unlock(&cmap->flushLock);
        if (shouldStop) break;
        fsync(cmap->logfd);
    }
    return NULL;
}

//create a concurrent persistent map
ConcurrentMap* cmap_createPersistent(const char *path,FsyncMode mode){
    //create the map with the non persistent creation
    ConcurrentMap *cmap = cmap_create();

    //checking if the journal exists
    int fdForReplay = open(path, O_RDONLY);
    int shouldTruncate = 0;
    size_t goodSize;
    //if it exists but cant open due to other reasons (permissions for example). return
    if (fdForReplay == -1 && errno != ENOENT) {
        perror("open log for replay");
        cmap_free(cmap);
        return NULL;
    }
    //if exists, handle replay
    else if (fdForReplay != -1)  {
        //find the size of the file
        struct stat sb;
        if(fstat(fdForReplay, &sb) ==-1){
            cmap_free(cmap);
            close(fdForReplay);
            return NULL;
        }
        size_t size = sb.st_size;

        // put the file into memory
        // read() may return fewer bytes than asked, so keep reading until we have them all
        char *buffer = malloc(size+1);
        size_t total = 0;
        while (total<size)
        {
            ssize_t lineSize = read(fdForReplay, buffer+total, size-total);
            if (lineSize<=0)
            {
                perror("error");
                free(buffer);
                close(fdForReplay);
                cmap_free(cmap);
                return NULL;
            }
            total+=lineSize;
        }
        buffer[size] = '\0';
        close(fdForReplay);

        //handle the replay itself from the buffer
        ReplayStatus rt = cmap_buildFromReplay(cmap,buffer,size,&goodSize);
        //check the indication result codem, if corrupt, return, if incomplete, remove the incomplete line
        switch (rt)
        {
            case REPLAY_OK:
                break;
            case REPLAY_CORRUPT:
                printf("corrupted bytes, stopped at %zu\n", goodSize);
                free(buffer);
                cmap_free(cmap);
                return NULL;
            case REPLAY_INCOMPLETE:
                shouldTruncate = 1;
        }
        free(buffer);
    }

    //create the actual journal file
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd ==-1)
    {
        cmap_free(cmap);
        perror("open failed");
        return NULL;
    }

    //if incomplete, remove the bad line and fix the file
    if (shouldTruncate)
    {
        if(ftruncate(fd,goodSize)){
            perror("error");
            close(fd);
            cmap_free(cmap);
            return NULL;
        }   
    }
    
    //link the file
    cmap->logfd = fd;
    cmap->mode = mode;
    fsync(fd);

    //if flushmode is everysecond create the thread to handle it
    if (mode == FSYNC_EVERYSEC)
    {
        if(pthread_create(&cmap->flusher,NULL,flush,cmap)) cmap->mode = FSYNC_ALWAYS;
    }
    return cmap;
}

//append the line into the file
static int appendLog(ConcurrentMap *cmap, char* buff, int len){
    //take the log writing lock and write to the file
    // lock order is always segment and then log, never the reverse, to avoid deadlock
    pthread_mutex_lock(&cmap->logLock);
    ssize_t result = write(cmap->logfd, buff,len);
    if (result!=(ssize_t)len)
    {
        perror("write bug");
        pthread_mutex_unlock(&cmap->logLock);
        return 0;
    }
    if (cmap->mode == FSYNC_ALWAYS) fsync(cmap->logfd); //if mode is to always fsync after every write, fsync.
    pthread_mutex_unlock(&cmap->logLock);
    return 1;
} 

//put a value into the map, in a concurrent way
void cmap_put(ConcurrentMap *cmap, const char *key, const char *value){
    //take the segment's lock
    int seg = calculateHash(key)>>(32-SEGMENT_BITS);
    pthread_mutex_lock(&cmap->segments[seg].lock);
    //if a journal exists, put the action in the journal
    // write-ahead: log first, then change memory. A crash between the
    // two still leaves the change in the log, so replay restores it.
    if (cmap->logfd != -1)
    {
        int len = snprintf(NULL, 0, "SET %zu %s %zu %s\n", strlen(key), key, strlen(value), value);
        char* buff = malloc(len+1);
        snprintf(buff, len+1, "SET %zu %s %zu %s\n", strlen(key), key, strlen(value), value);
        if (appendLog(cmap,buff, len)==0){
            free(buff);
            pthread_mutex_unlock(&cmap->segments[seg].lock);
            return;
        }
        free(buff);
    }
    //put the key value pair
    map_put(cmap->segments[seg].segmentMap, key , value);
    //release the lock
    pthread_mutex_unlock(&cmap->segments[seg].lock);
}

//get the value of a key in the map, in a concurrent way
char* cmap_get(ConcurrentMap *cmap, const char *key){
    //take the segment's lock
    int seg = calculateHash(key)>>(32-SEGMENT_BITS);
    pthread_mutex_lock(&cmap->segments[seg].lock);
    //get the value, if it exists, unlock the lock and return a mallocated string.
    char* returnValue =NULL;
    char* value = map_get(cmap->segments[seg].segmentMap, key);
    if (value)
    {
        //return a copy: after unlock, another thread may free the map's value
        returnValue =strdup(value);
    }
    
    pthread_mutex_unlock(&cmap->segments[seg].lock);
    return returnValue;
}

//remove the entry of a key in the map, in a concurrent way, returns 1 if successful
int cmap_remove(ConcurrentMap *cmap, const char *key){
    //take the segment's lock
    int seg = calculateHash(key)>>(32-SEGMENT_BITS);
    pthread_mutex_lock(&cmap->segments[seg].lock);
    //remove the key value pair
    int result = map_remove(cmap->segments[seg].segmentMap, key);
    //if a journal exists, put the action in the journal
    if (result == 1 && cmap->logfd != -1)
    {
        int len = snprintf(NULL, 0, "DEL %zu %s\n", strlen(key), key);
        char* buff = malloc(len+1);
        snprintf(buff, len+1, "DEL %zu %s\n", strlen(key), key);
        appendLog(cmap,buff, len);
        free(buff);
    }
    //release the lock
    pthread_mutex_unlock(&cmap->segments[seg].lock);
    return result;
}

//free the map in the end of the execution
void cmap_free(ConcurrentMap *cmap){
    //stop the fsync everysecond thread 
    if (cmap->mode == FSYNC_EVERYSEC && cmap->logfd != -1)
    {
        pthread_mutex_lock(&cmap->flushLock);
        cmap->stopFlusher = 1;
        pthread_mutex_unlock(&cmap->flushLock);
        pthread_join(cmap->flusher,NULL);
    }
    //close the log file
    if(cmap->logfd!=-1){
        //save the last second of writes on a clean shutdown (EVERYSEC mode)
        fsync(cmap->logfd);
        close(cmap->logfd);
    }
    //free each segment and destroy its lock
    for (int i = 0; i < NUM_SEGMENTS; i++)
    {
        map_free(cmap->segments[i].segmentMap);
        pthread_mutex_destroy(&cmap->segments[i].lock);
    }
    //free all the rest
    free(cmap->segments);
    pthread_mutex_destroy(&cmap->logLock);
    pthread_mutex_destroy(&cmap->flushLock);
    free(cmap);   
}

//count the amount of entries in a map
int cmap_count(ConcurrentMap *cmap){
    int entriesCount = 0;
    //take all the locks, to give an accurate snapshot
    for (int seg = 0; seg < NUM_SEGMENTS; seg++)
    {
        pthread_mutex_lock(&cmap->segments[seg].lock);
    }
    //counts
    for (int seg = 0; seg < NUM_SEGMENTS; seg++)
    {
        entriesCount += cmap->segments[seg].segmentMap->count;
    }
    //release all the locks
    for (int seg = 0; seg < NUM_SEGMENTS; seg++)
    {
        pthread_mutex_unlock(&cmap->segments[seg].lock);
    }
    //returns count
    return entriesCount;
}