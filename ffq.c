
#ifdef THREAD_AFFINITY
#define _GNU_SOURCE             /* See feature_test_macros(7) */
#include <pthread.h>
#endif       

#ifdef __KERNEL__
#include <linux/kernel.h>
#include <asm/bug.h>
#define assert BUG_ON
typedef long intptr_t;
#define IN_KERNEL 1
#else
#include <stdlib.h>
#include <inttypes.h>
#include <assert.h>
#define IN_KERNEL 0
#endif

#include "ffq.h"
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <execinfo.h>
#include <stdlib.h>
#include <stdbool.h>

/*
 * int sleep_priority()
 *
 * returns a number between 0 and number of threads-1. 0 means shortest time to sleep!
 */
static volatile long no_sleeping = 0;

static inline int sleep_priority() {
    return __atomic_fetch_add(&no_sleeping, 1, __ATOMIC_RELAXED);
}

static inline void done_sleeping() {
    __atomic_fetch_add(&no_sleeping, -1, __ATOMIC_RELAXED);
}

/*
 Queue statistics
*/
#define QST_MAX_RETRY_ENTRIES 12
#define QST_SPSC_RETRIES      1
#define QST_SPMC_RETRIES      2
#define QST_MPMC_RETRIES      3

typedef struct  {
    unsigned long spsc_enqueue_delayed;
    unsigned long spmc_enqueue_skipped;
    unsigned long mpmc_enqueue_skipped;
    unsigned long spsc_enqueue_retries[QST_MAX_RETRY_ENTRIES];
    unsigned long spmc_enqueue_retries[QST_MAX_RETRY_ENTRIES];
    unsigned long mpmc_enqueue_retries[QST_MAX_RETRY_ENTRIES];
    unsigned long errors;
} queue_stats_t;

static queue_stats_t qst = { 0 };

void print_queue_stats(FILE *f) {
    fprintf(f, "Single Producer / Single Consumer Queues Stats:\n");
    fprintf(f, "  Slots delayed: %lu (slot was still busy - should be: 0)\n", qst.spsc_enqueue_delayed);
    for (unsigned long i = 0, m = 1; i < QST_MAX_RETRY_ENTRIES ; ++i, m *= 10) {
        if (qst.spsc_enqueue_retries[i] > 0) {
            fprintf(f, "  retries (>= %lu): %lu\n", m, qst.spsc_enqueue_retries[i]);
        }
    }
    fprintf(f, "Single Producer / Multiple Consumer Queues Stats:\n");
    fprintf(f, "  Slots skipped: %lu (slot was still busy - should be: 0)\n", qst.spmc_enqueue_skipped);
    for (unsigned long i = 0, m = 1; i < QST_MAX_RETRY_ENTRIES ; ++i, m *= 10) {
        if (qst.spmc_enqueue_retries[i] > 0) {
            fprintf(f, "  retries (>= %lu): %lu\n", m, qst.spmc_enqueue_retries[i]);
        }
    }
    fprintf(f, "Multiple Producers / Multiple Consumers Queues Stats:\n");
    fprintf(f, "  Slots skipped: %lu (slot was still busy - should be: 0)\n", qst.mpmc_enqueue_skipped);
    for (unsigned long i = 0, m = 1; i < QST_MAX_RETRY_ENTRIES ; ++i, m *= 10) {
        if (qst.mpmc_enqueue_retries[i] > 0) {
            fprintf(f, "  retries (>= %lu): %lu\n", m, qst.mpmc_enqueue_retries[i]);
        }
    }
    fprintf(f, "Errors: %lu (should be: 0)\n", qst.errors);
}

void print_ffq(struct ffq *q, FILE *f) {
    fprintf(f, "struct ffq (%p) = { enqueue_pos(%u)=%ld; dequeue_pos(%u)=%ld; buffer(%u); }\n",
        q, (unsigned)((char *)&q->tail - (char *)q), q->tail,
        (unsigned) ((char *)&q->head - (char *)q), q->head,
        (unsigned)((char *)&q->buffer - (char *)q));
}

static void inc_retry(int retry_type, unsigned long r) {
    if (retry_type == QST_SPSC_RETRIES) {
        for (int i = 0, m = 1; i < QST_MAX_RETRY_ENTRIES; ++i, m *= 10) {
            if (r == m) {
                __atomic_fetch_add((volatile long *)&qst.spsc_enqueue_retries[i], 1, __ATOMIC_RELAXED);
                return;
            }
        }
    } else if (retry_type == QST_SPMC_RETRIES) {
        for (int i = 0, m = 1; i < QST_MAX_RETRY_ENTRIES; ++i, m *= 10) {
            if (r == m) {
                __atomic_fetch_add((volatile long *)&qst.spmc_enqueue_retries[i], 1, __ATOMIC_RELAXED);
                return;
            }
        }
    } else if (retry_type == QST_MPMC_RETRIES) {
        for (int i = 0, m = 1; i < QST_MAX_RETRY_ENTRIES; ++i, m *= 10) {
            if (r == m) {
                __atomic_fetch_add((volatile long *)&qst.mpmc_enqueue_retries[i], 1, __ATOMIC_RELAXED);
                return;
            }
        }
    } else {
        __atomic_fetch_add((volatile long *)&qst.errors, 1, __ATOMIC_RELAXED);
    }
}

/*
 Queue backoff time
*/
static unsigned syscall_interarrivaltime_ns = 100;
static unsigned max_wait_periods = 10000;
static unsigned backoff = 10;

static inline void backoff_queue(int retry_type, long n) {
    inc_retry(retry_type, n);

#ifdef THREAD_AFFINITY
    pthread_yield();
#else
    if (n < 2) {
#ifdef __i386__
        __asm__ __volatile__("pause" : : : "memory");
#endif
#ifdef __powerpc__
        __asm__ __volatile__("or 27,27,27"); /* Yield */
#endif
    } else {
        if (n >= max_wait_periods)
            n = max_wait_periods;
        struct timespec ts = { 0, 0 };
        if (retry_type == QST_SPMC_RETRIES)
            ts.tv_nsec = syscall_interarrivaltime_ns * n + sleep_priority() * backoff;
        else
            ts.tv_nsec = syscall_interarrivaltime_ns * n;
        nanosleep(&ts, NULL);
        done_sleeping();
    }
#endif
}

#ifndef __KERNEL__
int new_ffq(struct ffq *q,  size_t buffer_size, void *buffer) {
    size_t i;
    buffer_size /= sizeof(*q->buffer);
    q->buffer = (buffer != NULL ? buffer : calloc(sizeof(struct cell_t), buffer_size));
    q->kbuffer = NULL;
    q->buffer_mask = (buffer_size - 1);
    assert(q->buffer != NULL && buffer_size >= 2 && (buffer_size & (buffer_size - 1)) == 0);
#ifdef RANDOMIZE_ADDRESSES
    assert(buffer_size >= 256);
#endif
    for (i = 0; i != buffer_size; i += 1) {
        q->buffer[i].rank = -1;
        q->buffer[i].data = NULL;
        q->buffer[i].gap = -1;
    }
    q->tail = 0;
    q->head = 0;
    return 1;
}
#endif

/*
 Single producer, single consumer
*/
static int spsc_element_available(volatile struct ffq *q) {
    volatile struct cell_t *cell;
    long rank = q->head;
    if (IN_KERNEL) {
        cell = &q->kbuffer[HASH_INDEX(rank, q->buffer_mask)];
    } else {
        cell = &q->buffer[HASH_INDEX(rank, q->buffer_mask)];
    }
    return (cell->rank == rank);
}

int spsc_enqueue(volatile struct ffq *q, void *data) {
    volatile struct cell_t* cell;
    long tail = q->tail++;
    int r = 0;

    if (IN_KERNEL) {
        cell = &q->kbuffer[HASH_INDEX(tail, q->buffer_mask)];
    } else {
        cell = &q->buffer[HASH_INDEX(tail, q->buffer_mask)];
    }
    if (cell->rank != -1) {
        qst.spsc_enqueue_delayed++;
        do {
            backoff_queue(QST_SPSC_RETRIES, r++);
        } while (cell->rank != -1);
    }
    cell->data = data;
    __asm__ __volatile__("" : : : "memory");
    cell->rank = tail;
    __asm__ __volatile__("" : : : "memory");
    return r + 1;
}

int spsc_dequeue_backoff(volatile struct ffq *q, void **data) {
    volatile struct cell_t* cell;
    long rank;
    int r = 0;

    rank = q->head++;
    if (IN_KERNEL) {
        cell = &q->kbuffer[HASH_INDEX(rank, q->buffer_mask)];
    } else {
        cell = &q->buffer[HASH_INDEX(rank, q->buffer_mask)];
    }
    while (cell->rank != rank)
        backoff_queue(QST_SPSC_RETRIES, r++);
    *data = cell->data;
    __asm__ __volatile__("" : : : "memory");
    cell->rank = -1;
    return r + 1;
}

int spsc_dequeue(volatile struct ffq *q, void **data) {
    if (!spsc_element_available(q))
        return 0;
    return spsc_dequeue_backoff(q, data);
}

int spsc_mdequeue(volatile struct ffq *qa, int n, int blocking, void **data) {
    volatile struct ffq *q;
    volatile struct cell_t* cell;
    long rank;
    int r = 0;

    while (1) {
        for (int i = 0 ; i < n ; ++i) {
            q = &qa[i];
            rank = q->head;
            if (IN_KERNEL) {
                cell = &q->kbuffer[HASH_INDEX(rank, q->buffer_mask)];
            } else {
                cell = &q->buffer[HASH_INDEX(rank, q->buffer_mask)];
            }
            if (cell->rank == rank) {
                *data = cell->data;
                __asm__ __volatile__("" : : : "memory");
                cell->rank = -1;
                q->head = rank + 1;
                return r + 1;
            }
        }
        if (blocking == 0)
            break;
        backoff_queue(QST_SPSC_RETRIES, r++);
    }
    return 0;
}

/*
 Single producer, multiple consumers
*/
static int spmc_element_available(volatile struct ffq *q) {
    volatile struct cell_t *cell;
    long rank = q->head;
    if (IN_KERNEL) {
        cell = &q->kbuffer[HASH_INDEX(rank, q->buffer_mask)];
    } else {
        cell = &q->buffer[HASH_INDEX(rank, q->buffer_mask)];
    }
    return (cell->rank == rank) || (cell->gap >= rank);
}

int spmc_enqueue(volatile struct ffq *q, void *data) {
    volatile struct cell_t *cell;
    long tail = q->tail;

    while (1) {
        if (IN_KERNEL) {
            cell = &q->kbuffer[HASH_INDEX(tail, q->buffer_mask)];
        } else {
            cell = &q->buffer[HASH_INDEX(tail, q->buffer_mask)];
        }
        if (cell->rank < 0) {
            cell->data = data;
            __asm__ __volatile__("" : : : "memory");
            cell->rank = tail;
            break;
        } else {
            cell->gap = tail++;
            __atomic_fetch_add((volatile long *)&qst.spmc_enqueue_skipped, 1, __ATOMIC_RELAXED);
        }
    }
    q->tail = tail + 1;
    return 1;
}

int spmc_dequeue_backoff(volatile struct ffq *q, void **data) {
    volatile struct cell_t *cell;
    long rank;
    int r = 0;
retry:
    rank = __atomic_fetch_add(&q->head, 1, __ATOMIC_RELAXED);
    if (IN_KERNEL) {
        cell = &q->kbuffer[HASH_INDEX(rank, q->buffer_mask)];
    } else {
        cell = &q->buffer[HASH_INDEX(rank, q->buffer_mask)];
    }
    while (cell->rank != rank) {
        if (cell->gap >= rank)
            if (cell->rank != rank) /* Re-check rank */
                goto retry;
        backoff_queue(QST_SPMC_RETRIES, r++);
    }
    *data = cell->data;
    __asm__ __volatile__("" : : : "memory");
    cell->rank = -1;
    return r + 1;
}

int spmc_dequeue(volatile struct ffq *q, void **data) {
    if (!spmc_element_available(q))
        return 0;
    return spmc_dequeue_backoff(q, data);
}

/*
 Multiple producers, multiple consumers
*/
int mpmc_enqueue(volatile struct ffq *q, void *data) {
    volatile struct cell_t *cell;
    long rank;
    struct cell_t c, exp;

    while (1) {
        rank = __atomic_fetch_add(&q->tail, 1, __ATOMIC_RELAXED);
        if (IN_KERNEL) {
            cell = &q->kbuffer[HASH_INDEX(rank, q->buffer_mask)];
        } else {
            cell = &q->buffer[HASH_INDEX(rank, q->buffer_mask)];
        }
        while (1) {
            exp = cell->rank_gap;
            if (exp.gap >= rank) break;

            if (exp.rank >= 0) {
                c.rank = exp.rank;
                c.gap = rank;
                __atomic_compare_exchange_n(&cell->rank_gap, (unsigned __int128*)&exp.rank_gap,
                                            c.rank_gap, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
            } else {
                exp.rank = -1;
                c.rank = -2;
                c.gap = exp.gap;
                if (__atomic_compare_exchange_n(&cell->rank_gap, (unsigned __int128 *)&exp.rank_gap,
                                                c.rank_gap, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
                    cell->data = data;
                    cell->rank = rank;
                    goto success;
                }
            }
        }
        __atomic_fetch_add((volatile long *)&qst.mpmc_enqueue_skipped, 1, __ATOMIC_RELAXED);
    }
success:
    return 1;
}

int mpmc_dequeue(volatile struct ffq *q, void **data)
{
    return spmc_dequeue(q, data);
}

int mpmc_dequeue_backoff(volatile struct ffq *q, void **data)
{
    return spmc_dequeue_backoff(q, data);
}
