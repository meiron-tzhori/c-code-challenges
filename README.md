# C Systems Programming & Technical Challenges

A collection of low-level C implementations focusing on data structures, memory management, and multi-threaded caching.

## Included Modules

- **`pairs.c`**: In-place pairwise node switching in a singly-linked list using pointer-to-pointer manipulation.
- **`cache.c`**: Thread-safe in-memory buffer cache simulator backed by disk I/O, utilizing C11 threads (`<threads.h>`) and mutex locks.
- **`malloc.c`**: Custom memory allocator (`my_malloc` / `my_free`) featuring size-class binning, pool splitting, and thread safety.
