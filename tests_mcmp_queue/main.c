#include "../mcmp_queue.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define NUM_THREADS 14
#define NUM_ITEMS 50000
#define NUM_ITERATIONS 100

typedef struct
{
    int value;
} item_t;

mcmpq_t *queue;

void *producer(void *arg)
{
    int thread_id = *(int *)arg;
    item_t item_t;
    int count = NUM_ITEMS / NUM_THREADS;
    for (int i = 0; i < count; ++i) {
        item_t.value = thread_id * count + i;
        mcmpq_enqueue(queue, &item_t);
    }
    return NULL;
}

void *consumer(void *arg)
{
    (void)arg;
    item_t item_t;
    int count = NUM_ITEMS / NUM_THREADS;
    for (int i = 0; i < count; ++i) { mcmpq_dequeue(queue, &item_t); }
    return NULL;
}

int test(void)
{
    pthread_t producers[NUM_THREADS];
    pthread_t consumers[NUM_THREADS];
    int thread_ids[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; ++i) {
        thread_ids[i] = i;
        pthread_create(&producers[i], NULL, producer, &thread_ids[i]);
        pthread_create(&consumers[i], NULL, consumer, &thread_ids[i]);
    }

    for (int i = 0; i < NUM_THREADS; ++i) {
        pthread_join(producers[i], NULL);
        pthread_join(consumers[i], NULL);
    }

    assert(HEAD(queue) - TAIL(queue) == 0);
    return 0;
}

int main(void)
{
    queue = mcmpq_new(NUM_ITEMS / NUM_THREADS, sizeof(item_t));
    for (int i = 0; i < NUM_ITERATIONS; ++i) { test(); }
    printf("\nTest completed successfully. Queue is empty.\n\n\t Iterations: %i\n\n", (NUM_ITEMS * NUM_ITERATIONS));
    mcmpq_free(queue);
    return 0;
}
