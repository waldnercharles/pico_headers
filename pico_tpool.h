/**
    @file pico_tpool.h
    @brief A simple thread pool implementation using pthreads in C99.

    --------
    Summary:
    --------

    This library implements a high-performance thread pool for executing tasks
    in parallel. Tasks are submitted to a pre-allocated ring buffer queue and
    executed by worker threads. When the queue is full, tasks are executed
    synchronously on the calling thread, providing natural backpressure. The
    pool supports waiting for all tasks to complete and graceful shutdown.

    The ring buffer design eliminates malloc/free calls in the hot path,
    providing predictable performance and better cache locality.

    Basic usage:

        tpool_t *pool = tpool_create(4, 256);        // Create pool with 4 threads and queue size of 256
        tpool_add_work(pool, my_function, my_data);  // Add work
        tpool_wait(pool);                            // Wait for all tasks to complete
        tpool_destroy(pool);                         // Destroy the pool

    Note: The work queue uses a pre-allocated ring buffer.
    If the queue is full when adding work, the task will be executed
    synchronously on the calling thread.

    To use this library, do this in *one* C file:
        #define PICO_TPOOL_IMPLEMENTATION
        #include "pico_tpool.h"
*/

#ifndef PICO_TPOOL_H
#define PICO_TPOOL_H

#include <stddef.h>

struct tpool_s;
typedef struct tpool_s tpool_t;

/**
 * @brief Task function type
 *
 * @param arg User data passed to the task
 * @return    Return value (currently unused)
 */
typedef int (*tpool_task_fn)(void *arg);

/**
 * @brief Creates a new thread pool
 *
 * @param num_threads Number of worker threads to create (defaults to 1)
 * @return            Thread pool handle, or NULL on failure
 */
tpool_t *tpool_create(size_t num_threads);

/**
 * @brief Destroys a thread pool
 *
 * Waits for all queued tasks to complete before destroying the pool.
 *
 * @param pool Thread pool to destroy
 */
void tpool_destroy(tpool_t *pool);

/**
 * @brief Adds a task to the thread pool
 *
 * If the queue is full, the task will be executed synchronously on the
 * calling thread.
 *
 * @param pool Thread pool
 * @param func Task function to execute
 * @param arg  User data to pass to the task function
 * @return     0 on success, -1 on failure
 */
int tpool_add_work(tpool_t *pool, tpool_task_fn func, void *arg);

/**
 * @brief Waits for all tasks to complete
 *
 * Blocks until all currently queued tasks have been executed.
 *
 * @param pool Thread pool
 */
void tpool_wait(tpool_t *pool);

#endif /* PICO_TPOOL_H */

/* Implementation */

#ifdef PICO_TPOOL_IMPLEMENTATION

#include <pthread.h>
#include <stdlib.h>

#ifndef PICO_TPOOL_QUEUE_SIZE
#define PICO_TPOOL_QUEUE_SIZE 1024
#endif

struct tpool_s
{
    pthread_mutex_t work_mutex;
    pthread_cond_t work_cond;
    pthread_cond_t working_cond;

    tpool_task_fn work_funcs[PICO_TPOOL_QUEUE_SIZE]; /* Array of function pointers */
    void *work_args[PICO_TPOOL_QUEUE_SIZE]; /* Array of function arguments */

    size_t queue_head;  /* Index where workers dequeue from */
    size_t queue_tail;  /* Index where new work is enqueued */
    size_t queue_count; /* Current number of items in queue */
    size_t working_cnt; /* Number of threads currently executing tasks */
    size_t thread_cnt;  /* Number of worker threads */
    int stop;           /* Shutdown flag */
    pthread_t *threads; /* Array of worker thread IDs */
};

/* Check if ring buffer queue is full */
static int tpool_queue_is_full(tpool_t *pool)
{
    return pool->queue_count >= PICO_TPOOL_QUEUE_SIZE;
}

/* Check if ring buffer queue is empty */
static int tpool_queue_is_empty(tpool_t *pool)
{
    return pool->queue_count == 0;
}

/* Dequeue a work item from the ring buffer (must hold work_mutex) */
static int tpool_work_get(tpool_t *pool, tpool_task_fn *func, void **arg)
{
    if (pool == NULL || tpool_queue_is_empty(pool)) { return 0; }

    *func = pool->work_funcs[pool->queue_head];
    *arg = pool->work_args[pool->queue_head];

    pool->queue_head = (pool->queue_head + 1) % PICO_TPOOL_QUEUE_SIZE;
    pool->queue_count--;

    return 1;
}

static void *tpool_worker(void *arg)
{
    tpool_t *pool = (tpool_t *)arg;
    tpool_task_fn func;
    void *task_arg;

    while (1) {
        pthread_mutex_lock(&pool->work_mutex);

        while (tpool_queue_is_empty(pool) && !pool->stop) {
            pthread_cond_wait(&pool->work_cond, &pool->work_mutex);
        }

        if (pool->stop) { break; }

        if (tpool_work_get(pool, &func, &task_arg)) {
            pool->working_cnt++;
            pthread_mutex_unlock(&pool->work_mutex);

            func(task_arg);

            pthread_mutex_lock(&pool->work_mutex);
            pool->working_cnt--;
            if (!pool->stop && pool->working_cnt == 0 && tpool_queue_is_empty(pool)) {
                pthread_cond_signal(&pool->working_cond);
            }
            pthread_mutex_unlock(&pool->work_mutex);
        } else {
            pthread_mutex_unlock(&pool->work_mutex);
        }
    }

    pool->thread_cnt--;
    pthread_cond_signal(&pool->working_cond);
    pthread_mutex_unlock(&pool->work_mutex);
    return NULL;
}

tpool_t *tpool_create(size_t num_threads)
{
    tpool_t *pool;
    size_t i;

    if (num_threads == 0) { num_threads = 1; }

    pool = (tpool_t *)malloc(sizeof(*pool));
    if (pool == NULL) { return NULL; }

    pool->thread_cnt = num_threads;
    pool->working_cnt = 0;
    pool->stop = 0;
    pool->queue_head = 0;
    pool->queue_tail = 0;
    pool->queue_count = 0;

    pthread_mutex_init(&pool->work_mutex, NULL);
    pthread_cond_init(&pool->work_cond, NULL);
    pthread_cond_init(&pool->working_cond, NULL);

    pool->threads = (pthread_t *)malloc(sizeof(pthread_t) * num_threads);
    if (pool->threads == NULL) {
        free(pool);
        return NULL;
    }

    for (i = 0; i < num_threads; i++) {
        if (pthread_create(&pool->threads[i], NULL, tpool_worker, pool) != 0) {
            tpool_destroy(pool);
            return NULL;
        }
        pthread_detach(pool->threads[i]);
    }

    return pool;
}

void tpool_destroy(tpool_t *pool)
{
    if (pool == NULL) { return; }

    pthread_mutex_lock(&pool->work_mutex);
    pool->stop = 1;
    pthread_cond_broadcast(&pool->work_cond);
    pthread_mutex_unlock(&pool->work_mutex);

    tpool_wait(pool);

    pthread_mutex_destroy(&pool->work_mutex);
    pthread_cond_destroy(&pool->work_cond);
    pthread_cond_destroy(&pool->working_cond);

    free(pool->threads);
    free(pool);
}

int tpool_add_work(tpool_t *pool, tpool_task_fn func, void *arg)
{
    if (pool == NULL) { return -1; }

    pthread_mutex_lock(&pool->work_mutex);

    /* If queue is full, execute synchronously on calling thread */
    if (tpool_queue_is_full(pool)) {
        pthread_mutex_unlock(&pool->work_mutex);
        func(arg);
        return 0;
    }

    /* Add to ring buffer */
    pool->work_funcs[pool->queue_tail] = func;
    pool->work_args[pool->queue_tail] = arg;
    pool->queue_tail = (pool->queue_tail + 1) % PICO_TPOOL_QUEUE_SIZE;
    pool->queue_count++;

    pthread_cond_signal(&pool->work_cond);
    pthread_mutex_unlock(&pool->work_mutex);

    return 0;
}

void tpool_wait(tpool_t *pool)
{
    if (pool == NULL) { return; }

    pthread_mutex_lock(&pool->work_mutex);
    while (1) {
        if ((!pool->stop && (pool->working_cnt != 0 || !tpool_queue_is_empty(pool))) ||
            (pool->stop && pool->thread_cnt != 0)) {
            pthread_cond_wait(&pool->working_cond, &pool->work_mutex);
        } else {
            break;
        }
    }
    pthread_mutex_unlock(&pool->work_mutex);
}

#endif /* PICO_TPOOL_IMPLEMENTATION */
