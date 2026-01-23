#define PICO_TPOOL_IMPLEMENTATION
#include "../pico_tpool.h"

#include <stdio.h>
#include <unistd.h>

typedef struct
{
    int id;
    int sleep_ms;
} task_data_t;

int task_function(void *arg)
{
    task_data_t *data = (task_data_t *)arg;
    printf("Task %d started (sleeping for %dms)\n", data->id, data->sleep_ms);
    usleep(data->sleep_ms * 1000);
    printf("Task %d completed\n", data->id);
    return 0;
}

int main(void)
{
    printf("Creating thread pool with 4 worker threads\n");
    tpool_t *pool = tpool_create(4);
    if (!pool) {
        fprintf(stderr, "Failed to create thread pool\n");
        return 1;
    }

    printf("Adding 10 tasks to the pool\n");
    task_data_t tasks[10];
    for (int i = 0; i < 10; i++) {
        tasks[i].id = i;
        tasks[i].sleep_ms = 100 + (i * 50);
        tpool_add_work(pool, task_function, &tasks[i]);
    }

    printf("Waiting for all tasks to complete...\n");
    tpool_wait(pool);

    printf("All tasks completed!\n");
    printf("Destroying thread pool\n");
    tpool_destroy(pool);

    return 0;
}
