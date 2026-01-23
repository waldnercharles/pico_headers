#define PICO_TPOOL_IMPLEMENTATION
#include "../pico_tpool.h"

#define PICO_UNIT_IMPLEMENTATION
#include "../pico_unit.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// Global counters for testing
static pthread_mutex_t counter_mutex = PTHREAD_MUTEX_INITIALIZER;
static int task_counter = 0;
static int task_sum = 0;

void reset_counters(void)
{
    pthread_mutex_lock(&counter_mutex);
    task_counter = 0;
    task_sum = 0;
    pthread_mutex_unlock(&counter_mutex);
}

int increment_counter(void *arg)
{
    int value = *(int *)arg;
    usleep(3 * 1000);
    pthread_mutex_lock(&counter_mutex);
    task_counter++;
    task_sum += value;
    pthread_mutex_unlock(&counter_mutex);
    return 0;
}

int slow_task(void *arg)
{
    int sleep_ms = *(int *)arg;
    usleep(sleep_ms * 1000);
    pthread_mutex_lock(&counter_mutex);
    task_counter++;
    pthread_mutex_unlock(&counter_mutex);
    return 0;
}

// Test 1: Basic pool creation and destruction
TEST_CASE(test_pool_create_destroy)
{
    tpool_t *pool = tpool_create(4);
    REQUIRE(pool != NULL);
    tpool_destroy(pool);
    return true;
}

// Test 2: Single task execution
TEST_CASE(test_single_task)
{
    reset_counters();

    tpool_t *pool = tpool_create(2);
    int value = 42;

    tpool_add_work(pool, increment_counter, &value);
    tpool_wait(pool);

    pthread_mutex_lock(&counter_mutex);
    int counter = task_counter;
    int sum = task_sum;
    pthread_mutex_unlock(&counter_mutex);

    REQUIRE(counter == 1);
    REQUIRE(sum == 42);

    tpool_destroy(pool);
    return true;
}

// Test 3: Multiple tasks
TEST_CASE(test_multiple_tasks)
{
    reset_counters();

    tpool_t *pool = tpool_create(4);
    int values[10];
    int expected_sum = 0;

    for (int i = 0; i < 10; i++) {
        values[i] = i + 1;
        expected_sum += values[i];
        tpool_add_work(pool, increment_counter, &values[i]);
    }

    tpool_wait(pool);

    pthread_mutex_lock(&counter_mutex);
    int counter = task_counter;
    int sum = task_sum;
    pthread_mutex_unlock(&counter_mutex);

    REQUIRE(counter == 10);
    REQUIRE(sum == expected_sum);

    tpool_destroy(pool);
    return true;
}

// Test 4: Wait functionality with slow tasks
TEST_CASE(test_wait_functionality)
{
    reset_counters();

    tpool_t *pool = tpool_create(2);
    int sleep_times[5] = { 100, 200, 300, 400, 500 };

    for (int i = 0; i < 5; i++) {
        tpool_add_work(pool, slow_task, &sleep_times[i]);
    }

    tpool_wait(pool);

    pthread_mutex_lock(&counter_mutex);
    int counter = task_counter;
    pthread_mutex_unlock(&counter_mutex);

    REQUIRE(counter == 5);

    tpool_destroy(pool);
    return true;
}

// Test 5: Multiple wait calls
TEST_CASE(test_multiple_waits)
{
    reset_counters();

    tpool_t *pool = tpool_create(4);
    int values[3] = { 1, 2, 3 };

    // First batch
    for (int i = 0; i < 3; i++) {
        tpool_add_work(pool, increment_counter, &values[i]);
    }
    tpool_wait(pool);

    pthread_mutex_lock(&counter_mutex);
    int first_counter = task_counter;
    pthread_mutex_unlock(&counter_mutex);

    // Second batch
    for (int i = 0; i < 3; i++) {
        tpool_add_work(pool, increment_counter, &values[i]);
    }
    tpool_wait(pool);

    pthread_mutex_lock(&counter_mutex);
    int second_counter = task_counter;
    pthread_mutex_unlock(&counter_mutex);

    REQUIRE(first_counter == 3);
    REQUIRE(second_counter == 6);

    tpool_destroy(pool);
    return true;
}

// Test 6: Heavy load with many tasks
TEST_CASE(test_heavy_load)
{
    reset_counters();

    tpool_t *pool = tpool_create(4);
    int values[100];
    int expected_sum = 0;

    for (int i = 0; i < 100; i++) {
        values[i] = i + 1;
        expected_sum += values[i];
        tpool_add_work(pool, increment_counter, &values[i]);
    }

    tpool_wait(pool);

    pthread_mutex_lock(&counter_mutex);
    int counter = task_counter;
    int sum = task_sum;
    pthread_mutex_unlock(&counter_mutex);

    REQUIRE(counter == 100);
    REQUIRE(sum == expected_sum);

    tpool_destroy(pool);
    return true;
}

// Test 7: Tasks added after wait (edge case)
TEST_CASE(test_tasks_after_wait)
{
    reset_counters();

    tpool_t *pool = tpool_create(4);
    int value1 = 10;
    int value2 = 20;

    // First task
    tpool_add_work(pool, increment_counter, &value1);
    tpool_wait(pool);

    pthread_mutex_lock(&counter_mutex);
    int first_sum = task_sum;
    pthread_mutex_unlock(&counter_mutex);

    // Add another task after wait
    tpool_add_work(pool, increment_counter, &value2);
    tpool_wait(pool);

    pthread_mutex_lock(&counter_mutex);
    int second_sum = task_sum;
    pthread_mutex_unlock(&counter_mutex);

    REQUIRE(first_sum == 10);
    REQUIRE(second_sum == 30);

    tpool_destroy(pool);
    return true;
}

// Test 8: Race condition test - rapid add and wait
TEST_CASE(test_race_condition)
{
    reset_counters();

    tpool_t *pool = tpool_create(2);
    int sleep_time = 50;

    // Rapidly add tasks
    for (int i = 0; i < 20; i++) {
        tpool_add_work(pool, slow_task, &sleep_time);
    }

    // Immediately wait
    tpool_wait(pool);

    pthread_mutex_lock(&counter_mutex);
    int counter = task_counter;
    pthread_mutex_unlock(&counter_mutex);

    REQUIRE(counter == 20);

    tpool_destroy(pool);
    return true;
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    pu_display_colors(true);

    RUN_TEST_CASE(test_pool_create_destroy);
    RUN_TEST_CASE(test_single_task);
    RUN_TEST_CASE(test_multiple_tasks);
    RUN_TEST_CASE(test_wait_functionality);
    RUN_TEST_CASE(test_multiple_waits);
    RUN_TEST_CASE(test_heavy_load);
    RUN_TEST_CASE(test_tasks_after_wait);
    RUN_TEST_CASE(test_race_condition);

    pu_print_stats();

    pthread_mutex_destroy(&counter_mutex);
    return pu_test_failed();
}
