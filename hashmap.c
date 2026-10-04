#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "hashmap.h"

//for map printing
#define C_RESET "\033[0m"
#define C_DIM   "\033[2m"
#define C_KEY   "\033[1;36m"
#define C_VAL   "\033[33m"
#define C_HEAD  "\033[1;35m"

//intialize the struct and return a map
map* createHashMap(void){
    //intialize the struct and return a map
    map *m = malloc(sizeof(map));
    m->bucketsPtr = calloc(8,sizeof(entry*));
    m->size = 8;
    m->count = 0;
    return m;
}

//if load is above 0.75, double the amount of buckets by creating a new array and put old values into it
map* map_resize(map *m){
    if (m == NULL) {
        return NULL; 
    }
    //create the new bucket and intialize it
    entry **newBucket = calloc(m->size*2, sizeof(entry*));
    entry **oldBucket = m->bucketsPtr;
    m->bucketsPtr = newBucket;
    size_t old_size = m->size;
    m->size *= 2;
    m->count = 0;

    //put old values in the new bucketsarr
    for(size_t i = 0; i< old_size; i++){
        entry* head= oldBucket[i];
        entry* tmp;
        while(head!=NULL){
            tmp = head;
            head = head->nxt;
            int index = calculateHash(tmp->key)%(m->size);
            tmp->nxt = m->bucketsPtr[index];
            m->bucketsPtr[index] = tmp;
            m->count++;                
        }
    }
    free(oldBucket);
    return m;
}

//put a value with a key
void map_put(map *m, const char *key, const char *value){
    //check load
    if ((float)m->count/m->size> 0.75 )
    {
        //if load is bad above 0.75, resize. (load = amountOfentries/amountofbuckets)
        m = map_resize(m);
    }
    
    //find index
    int index = calculateHash(key)%(m->size);

    //search if duplicate exists
    entry *tmp = m->bucketsPtr[index];
    while (tmp != NULL) {
        if (!strcmp(tmp->key,key))
        {
            char *newValue = strdup(value);
            free(tmp->value);
            tmp->value = newValue;
            return;
        }
        
        tmp = tmp->nxt;
    }
    
    //if not. put it first
    entry* new_entry = malloc(sizeof(entry));
    new_entry->key = strdup((char*)key);
    new_entry->value = strdup((char*)value);
    new_entry->nxt = m->bucketsPtr[index];
    m->bucketsPtr[index] = new_entry;
    m->count++;    
}

//get the value of a key in the map
char* map_get(map *m, const char *key){
    //get index
    int index = calculateHash(key)%(m->size);
    
    //found the index, so search the bucket's linkedlist
    entry *tmp = m->bucketsPtr[index];
    while (tmp!=NULL)
    {
        if (!strcmp(tmp->key,key))
        {
            return tmp->value;
        }
        tmp = tmp->nxt;
           
    }
    return NULL;
}

//remove a key,value pair with a key in the map, returns 1 if successful
int map_remove(map *m, const char *key){
    //find the bucket
    int index = calculateHash(key)%(m->size);
    
    entry *tmp = m->bucketsPtr[index];
    if (tmp ==NULL)
    {
        return 0;
    }
    
    //if key is the first value in the bucket's linkedlist
    if (tmp!=NULL && !strcmp(tmp->key,key))
    {
        m->bucketsPtr[index] = tmp->nxt;
        free(tmp->key);
        free(tmp->value);
        free(tmp);
        m->count--;
        return 1;
    }
    
    //if not walk through the linked list and remove it
    while (tmp->nxt!=NULL)
    {
        if (!strcmp(tmp->nxt->key,key))
        {
            entry *target = tmp->nxt;
            tmp->nxt = target->nxt;
            free(target->key);
            free(target->value);
            free(target);
            m->count--;
            return 1;
        }
        tmp = tmp->nxt;
           
    }
    return 0;
}

//free the map in the end of the execution
void map_free(map *m){
    if (m == NULL) {
        return; 
    }

    //free each bucket
    for(int i = 0; i< m->size ;i++){
        entry* head= m->bucketsPtr[i];
        entry* tmp;
        while(head!=NULL){
            tmp = head;
            head = head->nxt;

            free(tmp->key);
            free(tmp);
        }
    }

    //free the rest
    free(m->bucketsPtr);
    free(m);
}

//an algorithm to calculate the hash of a string, return a 32bit number
uint32_t calculateHash(const char *str){
    //32-bit FNV-1a
    //choose a start value and a prime number (both 32 bits)
    uint32_t startValue = 0x811C9DC5;
    uint32_t prime = 0x01000193;
    uint32_t h = startValue;

    //xor the startvalue with each char, then multiply the result by the prime number
    int i = 0;
    while (str[i] != '\0')
    {
        h = h ^ (unsigned char)str[i];
        h = h*prime;
        i++;
    }

    //mix to even the load
    h ^= h >> 16;
    h *= 0x85EBCA6B;
    h ^= h >> 13;
    h *= 0xC2B2AE35;
    h ^= h >> 16;
    return h;
    
}

//print the map beautifly. dont try to understand how this works, this part is AI generated.
void printMap(const map *m, void (*printValue)(const void *value))
{
    printf(C_HEAD "HashMap" C_RESET "  count %d, size %d, load %.2f\n",
           m->count, m->size, m->size ? (double)m->count / m->size : 0.0);

    printf("     ┌─────┐\n");
    for (int i = 0; i < m->size; i++) {
        entry *e = m->bucketsPtr[i];

        if (e == NULL) {
            printf(" %3d │  " C_DIM "∅" C_RESET "  │\n", i);
        } else {
            printf(" %3d │  ●  ├", i);
            for (; e != NULL; e = e->nxt) {
                printf("──▶ [" C_KEY "%s" C_RESET ": " C_VAL, e->key);
                if (printValue) printValue(e->value);
                else            printf("%p", e->value);
                printf(C_RESET "] ");
            }
            printf("\n");
        }

        if (i < m->size - 1) printf("     ├─────┤\n");
    }
    printf("     └─────┘\n");
}