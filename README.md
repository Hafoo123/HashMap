# Concurrent, Persistent Hashmap in C

A small Redis-like key-value store written from scratch in C.

- **Concurrent:** many threads can read and write at the same time.
- **Persistent:** every change is saved to an append-only log file, and the map is rebuilt from it on startup.
- **Crash-safe:** a record cut off by a crash is repaired automatically. Real corruption is detected and the map refuses to start instead of guessing.

Built as a closing project for [OSTEP](https://pages.cs.wisc.edu/~remzi/OSTEP/), to practice its two big ideas in real code: **concurrency** (locks, threads, deadlock) and **persistence** (fsync, crash consistency, journaling).

---

## Quick start

```bash
gcc -Wall -Wextra -g -pthread -I. *.c tests/*.c -o hashmap
./hashmap
```

```c
#include "concurrentMap.h"

ConcurrentMap *m = cmap_createPersistent("data.aof", FSYNC_ALWAYS);

cmap_put(m, "name", "amit");

char *v = cmap_get(m, "name");   // "amit", a copy you own
free(v);

cmap_remove(m, "name");
cmap_free(m);                    // next cmap_createPersistent rebuilds the map from data.aof
```

---

## Files

| File | What it is |
|---|---|
| `hashmap.h` / `hashmap.c` | Single-threaded hashmap: hashing, chaining, resize |
| `concurrentMap.h` / `concurrentMap.c` | Thread-safe layer on top: segments, locks, log, replay |
| `tests/main.c` | Test runner, picks which test suites to run |
| `tests/test.h` | `CHECK` macro and shared test helpers |
| `tests/test_map.c` | Tests for the plain hashmap |
| `tests/test_cmap.c` | Multi-threaded tests (8 threads) |
| `tests/test_persist.c` | Log, restart and crash-recovery tests |

`-I.` in the build command lets the files in `tests/` find `hashmap.h` and `concurrentMap.h` in the project folder.

---

## API

```c
ConcurrentMap *cmap_create(void);
ConcurrentMap *cmap_createPersistent(const char *path, FsyncMode mode);

void  cmap_put(ConcurrentMap *m, const char *key, const char *value);
char *cmap_get(ConcurrentMap *m, const char *key);
int   cmap_remove(ConcurrentMap *m, const char *key);
int   cmap_count(ConcurrentMap *m);
void  cmap_free(ConcurrentMap *m);
```

| Function | Notes |
|---|---|
| `cmap_create` | Memory only. Nothing is ever written to disk. |
| `cmap_createPersistent` | Replays the log at `path` (if it exists), then logs every change. Returns `NULL` if the log is corrupt or can't be opened. |
| `cmap_put` | Inserts, or updates if the key exists. The map stores its **own copy** of key and value. |
| `cmap_get` | Returns a **copy** of the value. **The caller must `free()` it.** Returns `NULL` if the key is missing. |
| `cmap_remove` | Returns `1` if the key was there, `0` if not. |
| `cmap_count` | Number of keys, taken as an exact snapshot. |
| `cmap_free` | Stops the flush thread (if any), saves the log, frees everything. |

Fsync modes:

| Mode | Behavior |
|---|---|
| `FSYNC_ALWAYS` | `fsync` after every write. Slowest, loses nothing on power loss. |
| `FSYNC_EVERYSEC` | A background thread calls `fsync` once per second. Much faster, can lose up to about 1 second of writes on power loss. |

---

## Design

### 1. Hash function

**FNV-1a**, followed by the **fmix32** finalizer from MurmurHash3.

FNV-1a alone spreads keys very well in its **low** bits, but poorly in its **high** bits. The last character of a key only gets one multiply, which mostly lands just below the top 4 bits. The segment test caught this: with plain FNV-1a, 1600 keys landed between 45 and 146 per segment. With fmix32 added, they land between 77 and 115, close to random.

### 2. Hashmap

- An array of buckets. Each bucket is a linked list (**chaining**).
- New entries go to the **front** of the list.
- `put` means "insert or update": it searches the bucket first and only allocates if the key is new.
- When `count / size` goes above **0.75**, the array **doubles**. Resize **relinks** the existing nodes into the new array instead of copying them, so no memory is allocated or freed per entry.

### 3. Concurrency: segments

The concurrent map is **16 small hashmaps** ("segments"), each with its own mutex.

```
32-bit hash:  [ top 4 bits | ........ rest ........ ]
                  segment      bucket inside it (hash % size)
```

- The **top 4 bits** pick the segment. The bucket inside it comes from `hash % size`, which only uses the **bottom** bits. Using different parts of the hash keeps the two choices independent.
- Threads working on different segments never wait for each other.
- Each segment resizes on its own, under its own lock.
- `cmap_count` locks all 16 segments **in the same order** (0 to 15), so it can never deadlock with another multi-lock operation.
- `cmap_get` returns a **copy** made while the lock is held. Returning the map's own pointer would be unsafe: another thread could update or remove the key right after the unlock.

### 4. Persistence: the append-only log

Every change is written to a log file as one record:

```
SET <key length> <key> <value length> <value>\n
DEL <key length> <key>\n
```

Example:

```
SET 3 cat 1 5
SET 4 note 11 line1
line2
DEL 3 cat
```

- **Lengths come before every string**, so keys and values can contain anything, including spaces and newlines. The reader takes exactly that many bytes instead of searching for a separator.
- The file is opened with `O_APPEND`, and each record goes out in **one `write()` call**, so records from different threads never mix.
- **Write-ahead:** the record is logged **before** the change is applied in memory, the same rule filesystem journals use.
- The log write happens while the **segment lock** is held, so for every key, the order in the file matches the order of the real changes. Lock order is always **segment, then log**.
- `cmap_create` maps never log. Replay applies records with the plain map functions, so it never writes the log back into itself.

### 5. Recovery on startup

`cmap_createPersistent` reads the whole log into memory and replays it record by record. If a record can't be parsed, it decides **why**:

| Situation | Meaning | Action |
|---|---|---|
| The data **ran out** in the middle of a record | A crash interrupted the last write | Cut the file right before that record (`ftruncate`), then continue |
| The bytes are **wrong** while more data follows | Corruption in the middle of the file | Refuse to start, print the byte offset, leave the file untouched |

Cutting the broken tail matters: without it, new records would be appended **after** the garbage, and every later replay would stop there and lose them.

Refusing on corruption follows the same idea as Redis: never throw away good data by guessing.

---

## Tests

> **Note:** the test suites in `tests/` are AI-generated.

```bash
./hashmap              # map + cmap + persist
./hashmap map          # plain hashmap
./hashmap cmap         # 8-thread tests on the concurrent map
./hashmap persist      # log, restart, crash recovery
./hashmap plain        # the same threaded tests on the plain map with NO locks
```

Threaded tests run in a **child process** (`fork`), so a crash or a hang (killed after 10 s with `alarm`) is reported as a failure instead of killing the whole run. Results come back to the parent through a `pipe`.

The `plain` suite exists to **show race conditions**: without locks it gives wrong counts, crashes, and infinite loops, and the result changes every run.

Highlights:

- 8 threads, 80,000 puts, then every key checked
- 8 threads fighting over the same 100 keys, count must stay exactly 100
- A crash simulated at **every byte** of a record: the map always opens, always drops the half record, and a write after the crash always survives the next restart
- Corruption in the middle: refuses to open, file stays byte-for-byte the same

Checked with **valgrind**, **AddressSanitizer** and **ThreadSanitizer**: no leaks, no memory errors, no data races.

```bash
gcc -Wall -Wextra -g -pthread -fsanitize=address,undefined -I. *.c tests/*.c -o hashmap && ./hashmap
gcc -Wall -Wextra -g -pthread -fsanitize=thread -I. *.c tests/*.c -o hashmap && ./hashmap
```

---

## Known limits

- **No checksums.** If corruption hits a length number in the middle of the file, it can look like a crashed tail, and the records after it get truncated. Real systems (Postgres, SQLite, newer Redis) add a checksum to every record to tell these apart.
- **The log grows forever.** Setting the same key a million times leaves a million records. Redis fixes this by rewriting the log from the current map (or with snapshots).
- **Remove changes memory before logging.** If the log write fails, the key is gone in memory but comes back after a restart. Put does it in the safe order.
- **No directory `fsync`** when the log file is first created. After a power loss at exactly that moment, the file itself might not exist.
- **`cmap_free` can wait up to 1 second** in `FSYNC_EVERYSEC` mode, until the flush thread wakes up. `pthread_cond_timedwait` would fix this.
- **One `fsync` lock for all segments.** In `FSYNC_ALWAYS` mode every write waits in line for the disk, so the 16 segments help much less than in memory-only mode.
- **Values are C strings.** They can't contain the `'\0'` byte.