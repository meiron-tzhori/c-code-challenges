#include <stdio.h>
#include <string.h> // for memcpy()
#include <threads.h>

#define BUFLEN (1<<5)
#define DISKBUFS (1<<12)
#define DISKLEN (BUFLEN * DISKBUFS)
#define BUFNUM2ADDR(bn) ((void *)(long)((bn) * BUFLEN))
#define ADDR2BUFNUM(addr) ((long)(addr) / BUFLEN)

#define CACHENODES (1<<4)

// for test purposes
#define THREADS 2
#define ITERATIONS 2

// Buf is common to Disk and Cache

typedef struct {
	char buf[BUFLEN];
} Buf;

// Disk related types and methods

typedef struct {
	Buf bufs[DISKBUFS];
} Disk;

Disk my_disk;

// read from a "disk" buffer to "mem" buffer.
// addr is used as byte offset within the disk.
// return value: 0 on success, -1 on failure
int disk_read(void *addr, void *buf) {
	int bufnum = ADDR2BUFNUM(addr);
	if (bufnum < 0 || bufnum >= DISKBUFS) {
		return -1;
	}
	memcpy(buf, &my_disk.bufs[bufnum], BUFLEN);
	printf("disk_read: bufnum %d\n", bufnum);
	return 0;
}

int disk_write(void *addr, const void *buf) {
	int bufnum = ADDR2BUFNUM(addr);
	if (bufnum < 0 || bufnum >= DISKBUFS) {
		return -1;
	}
	memcpy(&my_disk.bufs[bufnum], buf, BUFLEN);
	printf("disk_write: bufnum %d\n", bufnum);
	return 0;
}

// Cache related types and methods

typedef struct _Node {
	int bufnum;
	int used; // boolean
	struct _Node *prev, *next;
	Buf buf;
} Node;

typedef struct {
	mtx_t mtx;
	int active; // boolean
	Node nodes[CACHENODES];
	Node *free_head, *used_head;
} Cache;

Cache my_cache;

void cache_init(Cache *c) {
	c->free_head = c->used_head = NULL;
	for (int i=0; i<CACHENODES; i++) {
		Node *n = &c->nodes[i];
		n->bufnum = i;
		n->used = 0;
		n->prev = NULL;
		n->next = c->free_head;
		if (c->free_head) {
			c->free_head->prev = n;
		}
		c->free_head = n;
	}
	printf("cache_init saved %d free nodes\n", CACHENODES);

	c->active = 1;
	mtx_init(&c->mtx, mtx_plain);
}

void cache_print(Cache *c) {
	mtx_lock(&c->mtx);
	for (int used=0; used<=1; used++) {
		Node *n = used ? c->used_head : c->free_head;
		if (n) {
			printf("%s list: ", used ? "Used" : "Free");
			do {
				int prev = n->prev ? n->prev->bufnum : -1;
				int curr = n->bufnum;
				int next = n->next ? n->next->bufnum : -1;
				printf("%d(%d,%d,%d) ", n->bufnum % CACHENODES, prev, curr, next);
				n = n->next;
			} while (n);
			printf("\n");
		}
	}
	mtx_unlock(&c->mtx);
}

void cache_pop(Cache *c, Node *n) {
	if (n->prev) {
		n->prev->next = n->next;
	}

	if (n->next) {
		n->next->prev = n->prev;
	}

	if (!n->prev) {
		if (n->used) {
			c->used_head = n->next;
		} else {
			c->free_head = n->next;
		}
	}
	n->prev = n->next = 0;
}

void cache_push(Cache *c, Node *n) {
	if (n->used) {
		if (c->used_head) {
			c->used_head->prev = n;
		}
		n->next = c->used_head;
		c->used_head = n;
	} else {
		if (c->free_head) {
			c->free_head->prev = n;
		}
		n->next = c->free_head;
		c->free_head = n;
	}
}

// Flush cache node to disk.
// return value: 0 on success, -1 on failure
int cache_flush(Cache *c, Node *n) {
	if (!n || !n->used) {
		return -1;
	}
	cache_pop(c, n);
	int ret = disk_write(BUFNUM2ADDR(n->bufnum), n->buf.buf);
	n->used = 0;
	cache_push(c, n);

	return ret;
}

int cache_write(Cache *c, int bufnum, void *buf) {
	int cache_node = bufnum % CACHENODES;
	Node *n = &c->nodes[cache_node];
	printf("cache_write: bufnum %d->%d node %d used %d data from: %s to: %s\n",
		   n->bufnum, bufnum, cache_node, n->used, n->buf.buf, buf);

	memcpy(n->buf.buf, buf, BUFLEN);

	if (n->used) {
		if (bufnum == n->bufnum) {
			printf("... updating used node\n");
		} else {
			printf("... WARNING: overriding used node\n");
			n->bufnum = bufnum;
		}
	} else {
		printf("... using free node\n");

		cache_pop(c, n);
		n->used = 1;
		n->bufnum = bufnum;
		cache_push(c, n);
	}

	return 0;
}

int cache_delete(Cache *c, int bufnum) {
	int cache_node = bufnum % CACHENODES;
	Node *n = &c->nodes[cache_node];
	printf("cache_delete: bufnum %d->%d node %d used %d data: %s\n",
		   n->bufnum, bufnum, cache_node, n->used, n->buf.buf);

	if (n->used && n->bufnum == bufnum) {
		printf("... freeing the node\n");

		cache_pop(c, n);
		n->used = 0;
		cache_push(c, n);
		return 0;
	}

	return -1;
}

int cache_read(Cache *c, int bufnum, void *buf) {
	int cache_node = bufnum % CACHENODES;
	Node *n = &c->nodes[cache_node];
	printf("cache_read: bufnum %d->%d node %d used %d data: %s\n",
		   n->bufnum, bufnum, cache_node, n->used, n->buf.buf);

	if (n->used && n->bufnum == bufnum) {
		memcpy(buf, n->buf.buf, BUFLEN);
		return 0;
	}

	disk_read(BUFNUM2ADDR(bufnum), buf);

	return cache_write(c, bufnum, buf);
}

// To be run by a dedicated thread: flush cache to disk
int cache_flush_all(void *arg) {
	Cache *c = &my_cache;
	while (c->active) {
		thrd_sleep(&(struct timespec) {
			.tv_sec=1
		}, NULL); // sleep 1 sec
		printf("cache_flush_all\n");
		while (c->used_head) {
			mtx_lock(&c->mtx);
			cache_flush(c, c->used_head);
			mtx_unlock(&c->mtx);
		}
	}
}

// read from a "disk" or "cache" buffer to "mem" buffer.
// addr is used as byte offset within the disk.
// return value: 0 on success, -1 on failure
int my_read(void *addr, void *buf) {
	int bufnum = ADDR2BUFNUM(addr);
	if (bufnum < 0 || bufnum >= DISKBUFS) {
		return -1;
	}

	Cache *c = &my_cache;
	mtx_lock(&c->mtx);
	int ret = cache_read(c, bufnum, buf);
	mtx_unlock(&c->mtx);

	return ret;
}

int my_write(void *addr, void *buf) {
	int bufnum = ADDR2BUFNUM(addr);
	if (bufnum < 0 || bufnum >= DISKBUFS) {
		return -1;
	}

	Cache *c = &my_cache;
	mtx_lock(&c->mtx);
	int ret = cache_write(c, bufnum, buf);
	mtx_unlock(&c->mtx);

	return ret;
}

int cache_test(void *arg) {
	char buf[BUFLEN];
	Cache *c = &my_cache;

	my_write(BUFNUM2ADDR(200), "Written to buf 200");
	// my_write(BUFNUM2ADDR(200), "Written again to buf 200");
	cache_print(c);

	// test override case (since I've not fully implemented hash map)
	// sprintf(buf, "Written to buf %d", 200 + CACHENODES);
	// my_write(BUFNUM2ADDR(200 + CACHENODES), buf);
	// cache_print(c);


	my_read(BUFNUM2ADDR(3), buf);
	printf("Read from buf 3: %s\n", buf);
	// my_read(BUFNUM2ADDR(3 + CACHENODES), buf);
	// printf("Read from buf %d: %s\n", 3 + CACHENODES, buf);

	cache_print(c);

	thrd_sleep(&(struct timespec) {
		.tv_sec=1
	}, NULL); // sleep 1 sec

	return 0;
}

int main()
{
	printf("\nTest direct disk operations (NOT thread safe)\n\n");

	disk_write(BUFNUM2ADDR(3), "Written to disk buf 3");
	disk_write(BUFNUM2ADDR(5), "Written to disk buf 5");
	disk_write(BUFNUM2ADDR(7), "Written to disk buf 7");

	char buf[BUFLEN];
	disk_read(BUFNUM2ADDR(3), buf);
	printf("Read from disk buf 3: %s\n", buf);
	disk_read(BUFNUM2ADDR(5), buf);
	printf("Read from disk buf 5: %s\n", buf);
	disk_read(BUFNUM2ADDR(7), buf);
	printf("Read from disk buf 7: %s\n", buf);

	printf("\nTest direct cache operations (NOT thread safe)\n\n");

	Cache *c = &my_cache;

	cache_init(c);
	cache_print(c);
	cache_write(c, 100, "Written to cache buf 100");
	cache_write(c, 100, "Written again to cache buf 100");
	cache_print(c);

	// test override case (since I've not fully implemented hash map)
	// sprintf(buf, "Written to cache buf %d", 100 + CACHENODES);
	// cache_write(c, 100 + CACHENODES, buf);
	// cache_print(c);

	cache_delete(c, 100);
	cache_print(c);

	printf("\nTest cache operations (thread safe)\n\n");

	thrd_t t[THREADS], flush;
	thrd_create(&flush, cache_flush_all, NULL);

	for (int j=0; j<ITERATIONS; j++) {
		printf("\nITERATION %d\n\n", j);
		for (int i=0; i<THREADS; i++) {
			thrd_create(&t[i], cache_test, NULL);
		}

		for (int i=0; i<THREADS; i++) {
			thrd_join(t[i], 0);
		}

		cache_print(c);
	}

	my_cache.active = 0;
	thrd_join(flush, 0);

	return 0;
}

