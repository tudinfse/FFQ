#ifndef FFQ_H
#define FFQ_H

#include <stdio.h>
#include <stdint.h>

#ifndef CACHELINE_SIZE
#define CACHELINE_SIZE 64
#endif

#ifdef RANDOMIZE_ADDRESSES
#define HASH_INDEX(idx, mask) (((idx) & ((mask) ^ 0xFF)) | (((idx) & 0x0F) << 4) | (((idx) & 0xF0) >> 4))
#else
#define HASH_INDEX(idx, mask) ((idx) & (mask))
#endif

struct cell_t {
    union {
        volatile unsigned __int128 rank_gap;
        struct {
            volatile long rank;
            volatile long gap;
        };
    };
    void *volatile data;
}
#ifdef CACHELINE_ALIGNED
__attribute__ ((aligned(CACHELINE_SIZE)))
#endif
;

struct ffq {
    volatile long head __attribute__ ((aligned(CACHELINE_SIZE)));
    volatile long tail __attribute__ ((aligned(CACHELINE_SIZE)));
    struct cell_t *buffer __attribute__ ((aligned(CACHELINE_SIZE)));
    struct cell_t *kbuffer;
    size_t buffer_mask;
} __attribute__((aligned(CACHELINE_SIZE)));

int new_ffq(struct ffq *q, size_t buffer_bytesize, void *buffer);

void print_ffq(struct ffq *q, FILE* f);
void print_queue_stats(FILE *f);

// Single producer, single consumer
int spsc_enqueue(volatile struct ffq *q, void *data);
int spsc_dequeue(volatile struct ffq *q, void **data);
int spsc_dequeue_backoff(volatile struct ffq *q, void **data);
int spsc_mdequeue(volatile struct ffq *qa, int n, int blocking, void **data);

// Single producer, multiple consumers
int spmc_enqueue(volatile struct ffq *q, void *data);
int spmc_dequeue(volatile struct ffq *q, void **data);
int spmc_dequeue_backoff(volatile struct ffq *q, void **data);

// Multiple producers, multiple consumers
int mpmc_enqueue(volatile struct ffq *q, void *data);
int mpmc_dequeue(volatile struct ffq *q, void **data);
int mpmc_dequeue_backoff(volatile struct ffq *q, void **data);

#endif /* FFQ_H */
