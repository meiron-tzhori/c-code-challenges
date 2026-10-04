#include <stdio.h>
#include <threads.h>

#define MINSHIFT 5
#define MAXSHIFT 18
#define MEMLEN (1<<MAXSHIFT)
#define NUMPOOLS (MAXSHIFT - MINSHIFT + 1)

// for test purposes
#define THREADS 2
#define ITERATIONS 2
#define PRINTPOOLS 1

typedef struct _MemHdr {
	int len;
	int used; // boolean
	struct _MemHdr *prev, *next;
	char data[0];
} MemHdr;

typedef struct {
	mtx_t mtx;
	char buf[MEMLEN];
	MemHdr *free_pools[NUMPOOLS];
	MemHdr *used_pools[NUMPOOLS];
	int max_free_pool_index;
} MemPool;

MemPool myMemPool;

int get_pool_index(int len) {
	int shift = MINSHIFT;
	int buflen = 1 << MINSHIFT;
	for (; shift <= MAXSHIFT; shift++, buflen *= 2) {
		if (len <= buflen) {
			break;
		}
	}
	if (shift > MAXSHIFT) {
		return -1; // requested len is too big
	}

	return shift - MINSHIFT;
}

// init a memory pool.
// return value: 0 on success, -1 on failure.
int mem_pool_init(MemPool *mp) {
	for (int i=0; i<NUMPOOLS; i++) {
		mp->free_pools[i] = mp->used_pools[i] = NULL;
	}

	int pool_index = get_pool_index(MEMLEN);
	if (pool_index < 0) {
		return -1; // error
	}

	MemHdr *hdr = (MemHdr *)mp->buf;
	hdr->len = MEMLEN;
	hdr->used = 0;
	hdr->prev = hdr->next = NULL;
	mp->free_pools[pool_index] = hdr;
	mp->max_free_pool_index = pool_index;

	mtx_init(&mp->mtx, mtx_plain);

	return 0;
}

void print_pools(MemPool *mp) {
	MemHdr *hdr;
	if (!PRINTPOOLS) return;
	mtx_lock(&mp->mtx);
	for (int used=0; used<=1; used++) {
		for (int i=0; i<NUMPOOLS; i++) {
			hdr = used ? mp->used_pools[i] : mp->free_pools[i];
			if (hdr) {
				printf("%s list pool %d: ", used ? "Used" : "Free", i);
				do {
					int prev = hdr->prev ? (char *)hdr->prev - mp->buf : -1;
					int curr = (char *)hdr - mp->buf;
					int next = hdr->next ? (char *)hdr->next - mp->buf : -1;
					printf("%p (%d/%d/%d len %d used %d) ",
						   hdr, prev, curr, next, hdr->len, hdr->used);
					hdr = hdr->next;
				} while (hdr);
				printf("\n");
			}
		}
	}
	mtx_unlock(&mp->mtx);
}

// take a free pool with requested index and split it into 2 halves.
// return value: 1 on success, 0 on failure.
int split_pool_index(MemPool *mp, int pool_index) {
	if (pool_index < 1 || pool_index >= NUMPOOLS) {
		return 0;
	}
	MemHdr *hdr = mp->free_pools[pool_index];
	if (!hdr) {
		return 0;
	}

	if (hdr->prev) {
		hdr->prev->next = hdr->next;
	}
	if (hdr->next) {
		hdr->next->prev = hdr->prev;
	}

	if (!hdr->prev) {
		mp->free_pools[pool_index] = hdr->next;
		if (!hdr->next) {
			// pool became empty
			if (pool_index == mp->max_free_pool_index) {
				--mp->max_free_pool_index;
			}
		}
	}

	hdr->prev = NULL;
	hdr->len /= 2;
	hdr->used = 0;
	char *chdr = (char *)hdr;
	MemHdr *hdr2 = (MemHdr *)(chdr + hdr->len);
	hdr2->prev = hdr;
	hdr2->len = hdr->len;
	hdr2->used = 0;
	hdr->next = hdr2;
	hdr2->next = mp->free_pools[pool_index - 1]; // probably NULL
	mp->free_pools[pool_index - 1] = hdr;

	return 1;
}

// Recursive func:
// verify having a free pool with requested index.
// if missing, look for a free pool with higher index and split it.
// return value: 1 on success, 0 on failure.
int get_free_pool_index(MemPool *mp, int pool_index) {
	if (mp->free_pools[pool_index]) {
		return 1;
	}

	if (pool_index >= mp->max_free_pool_index) {
		return 0;
	}

	return get_free_pool_index(mp, pool_index + 1) && split_pool_index(mp, pool_index + 1);
}

void *my_malloc(int len) {
	int pool_index;
	if (len < 1 || len > MEMLEN) {
		pool_index = -2;
	} else {
		pool_index = get_pool_index(len + sizeof(MemHdr));
	}

	printf("my_malloc: len %d pool %d\n", len, pool_index);
	if (pool_index < 0) {
		return NULL;
	}

	MemPool *mp = &myMemPool;
	mtx_lock(&mp->mtx);

	if (!get_free_pool_index(mp, pool_index)) {
		mtx_unlock(&mp->mtx);
		return NULL;
	}
	
	MemHdr *hdr = mp->free_pools[pool_index];

	if (hdr->next) {
		hdr->next->prev = NULL;
	}
	
	hdr->used = 1;
	mp->free_pools[pool_index] = hdr->next;
	hdr->prev = NULL;
	hdr->next = mp->used_pools[pool_index];
	if (mp->used_pools[pool_index]) {
		mp->used_pools[pool_index]->prev = hdr;
	}
	mp->used_pools[pool_index] = hdr;

	mtx_unlock(&mp->mtx);

	printf("... return %p\n", hdr->data);

	return (void *)(hdr->data);
}

// free a previously allocated memory pointer.
// return value: 0 on success, -1 on failure.
int my_free(void *p) {
	if (!p) {
		return -1;
	}
	MemHdr *hdr = (MemHdr *)(p - sizeof(MemHdr));
	int pool_index = get_pool_index(hdr->len);
	printf("my_free called with p %p -> hdr %p len %d used %d pool %d\n",
		   p, hdr, hdr->len, hdr->used, pool_index);

	if (!hdr->used) {
		printf("... WARNING: header is NOT marked as used\n");
		return -1;
	}

	MemPool *mp = &myMemPool;
	mtx_lock(&mp->mtx);

	if (hdr->prev) {
		hdr->prev->next = hdr->next;
	} else if (hdr == mp->used_pools[pool_index]) {
		mp->used_pools[pool_index] = hdr->next;
	} else {
		mtx_unlock(&mp->mtx);
		printf("... Internal error: cannot free\n");
		return -1;
	}

	hdr->used = 0;

	if (hdr->next) {
		hdr->next->prev = hdr->prev;
	}

	hdr->prev = NULL;
	hdr->next = mp->free_pools[pool_index];
	if (mp->free_pools[pool_index]) {
		mp->free_pools[pool_index]->prev = hdr;
	}
	mp->free_pools[pool_index] = hdr;

	mtx_unlock(&mp->mtx);

	return 0;
}

int test(void *arg) {
	void *p[NUMPOOLS + 10];
	int pcount = 0;
	p[pcount++] = my_malloc(0);

	for (int len=1; len <= MEMLEN * 2; len *= 2) {
		p[pcount++] = my_malloc(len);

		if (pcount >= 4) {
			my_free(p[pcount - 4]);
		}
	}

	// try free an already free pointer
	my_free(p[5]);
}

int main()
{
	if (mem_pool_init(&myMemPool) < 0) {
		printf("mem_pool_init failed\n");
		return 1;
	}

	print_pools(&myMemPool);

	thrd_t t[THREADS];

	for (int j=0; j<ITERATIONS; j++) {
		printf("\nITERATION %d\n\n", j);
		for (int i=0; i<THREADS; i++) {
			thrd_create(&t[i], test, NULL);
		}

		for (int i=0; i<THREADS; i++) {
			thrd_join(t[i], 0);
		}

		print_pools(&myMemPool);
	}

	return 0;
}

