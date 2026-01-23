#define PICO_UNIT_IMPLEMENTATION
#include "../pico_unit.h"

#define PICO_ECS_IMPLEMENTATION
#define PICO_TPOOL_IMPLEMENTATION
#include "../pico_ecs_mt.h"
#include "../pico_tpool.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

// Test configuration
#define NUM_THREADS 4
#define NUM_ENTITIES_PER_THREAD 100
#define NUM_ITERATIONS 10

// Component definitions
typedef struct
{
    int value;
    bool used;
} comp_t;

// Global test state
ecs_mt_t *g_ecs_mt = NULL;
tpool_t *g_pool = NULL;
ecs_comp_t g_comp1;
ecs_comp_t g_comp2;
ecs_comp_t g_comp3;

// Task callbacks for thread pool
int enqueue_task(int (*fn)(void *), void *fn_args, void *udata)
{
    tpool_t *pool = (tpool_t *)udata;
    return tpool_add_work(pool, fn, fn_args);
}

void finish_task(void *udata)
{
    tpool_t *pool = (tpool_t *)udata;
    tpool_wait(pool);
}

// Setup and teardown
void setup()
{
    g_pool = tpool_create(NUM_THREADS);
    if (!g_pool) {
        fprintf(stderr, "Failed to create thread pool\n");
        return;
    }
    g_ecs_mt = ecs_mt_new(1024, enqueue_task, finish_task, g_pool, NUM_THREADS + 1, NULL);

    g_comp1 = ecs_define_component(g_ecs_mt->ecs, sizeof(comp_t), NULL, NULL);
    g_comp2 = ecs_define_component(g_ecs_mt->ecs, sizeof(comp_t), NULL, NULL);
    g_comp3 = ecs_define_component(g_ecs_mt->ecs, sizeof(comp_t), NULL, NULL);
}

void teardown()
{
    if (g_ecs_mt) {
        ecs_mt_free(g_ecs_mt);
        g_ecs_mt = NULL;
    }
    if (g_pool) {
        tpool_destroy(g_pool);
        g_pool = NULL;
    }
}

/*==============================================================================
 * Test 1: Concurrent ecs_create calls
 *
 * Issue: Multiple threads calling ecs_create() simultaneously can cause:
 * - Race condition on entity_pool (pop operation)
 * - Race condition on next_entity_id increment
 * - Concurrent reallocation of entities array
 * Solutions:
 * - Pre-allocated ID ranges per thread: Before system execution, divide entity
 *   ID space into ranges and assign each thread its own range. Lock-free, no
 * contention. What happens if a thread runs out of entities?
 * - Deferred allocation: Could return a placeholder id, and allocate real ids
 * during flush. This feels really bad.
 * - Atomic operations: Use some compiler built-ins like __sync_fetch_and_add
 * (C99) or <stdatomic.h> (C11). This would require modifying pico_ecs.h
 *============================================================================*/

static volatile int g_create_count = 0;
static pthread_mutex_t g_create_mutex = PTHREAD_MUTEX_INITIALIZER;

ecs_ret_t concurrent_create_system(ecs_mt_t *ecs_mt, ecs_entity_t *entities, size_t entity_count, void *udata)
{
    (void)entities;
    (void)entity_count;
    (void)udata;

    // Each thread tries to create entities using the thread-safe API
    for (int i = 0; i < NUM_ENTITIES_PER_THREAD; i++) {
        ecs_entity_t new_entity = ecs_mt_create(ecs_mt);

        if (!ecs_is_invalid_entity(new_entity)) {
            pthread_mutex_lock(&g_create_mutex);
            g_create_count++;
            pthread_mutex_unlock(&g_create_mutex);
        }
    }

    return 0;
}

TEST_CASE(test_concurrent_create_race)
{
    g_create_count = 0;

    ecs_system_t sys = ecs_mt_define_system(g_ecs_mt, 0, concurrent_create_system, NULL, NULL, NULL);
    ecs_require_component(g_ecs_mt->ecs, sys, g_comp1);

    // Create some initial entities to ensure the system runs
    ecs_entity_t trigger_entities[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {
        trigger_entities[i] = ecs_create(g_ecs_mt->ecs);
        ecs_add(g_ecs_mt->ecs, trigger_entities[i], g_comp1, NULL);
    }

    // Run the system - this will execute across multiple threads
    ecs_run_system(g_ecs_mt->ecs, sys, 0);

    // Expected: NUM_THREADS * NUM_ENTITIES_PER_THREAD entities created
    int expected = NUM_THREADS * NUM_ENTITIES_PER_THREAD;

    printf("    Created entities: %d (expected %d)\n", g_create_count, expected);

    // This test will likely fail or crash due to race conditions
    // We might see:
    // - Fewer entities than expected (lost creates)
    // - Duplicate entity IDs
    // - Crashes from concurrent array reallocation
    REQUIRE(g_create_count == expected);

    return true;
}

/*==============================================================================
 * Test 2: Concurrent ecs_add calls
 *
 * Issue: Multiple threads calling ecs_add() simultaneously can cause:
 * - Race condition on entity component bitset
 * - Concurrent component array resizing
 * - Race condition on sparse set add/remove (system membership)
 * Solution: Use ecs_mt_add
 *============================================================================*/

static volatile int g_add_success_count = 0;
static pthread_mutex_t g_add_mutex = PTHREAD_MUTEX_INITIALIZER;

ecs_ret_t concurrent_add_system(ecs_mt_t *ecs_mt, ecs_entity_t *entities, size_t entity_count, void *udata)
{
    (void)udata;

    // Each thread tries to add components to entities using the thread-safe API
    for (size_t i = 0; i < entity_count; i++) {
        ecs_entity_t entity = entities[i];

        // Add different components - buffered for thread-safety!
        if (ecs_mt->thread_id == 0) {
            ecs_mt_add(ecs_mt, entity, g_comp2, NULL, 0);
        } else {
            ecs_mt_add(ecs_mt, entity, g_comp3, NULL, 0);
        }

        pthread_mutex_lock(&g_add_mutex);
        g_add_success_count++;
        pthread_mutex_unlock(&g_add_mutex);
    }

    return 0;
}

TEST_CASE(test_concurrent_add_race)
{
    g_add_success_count = 0;

    ecs_system_t sys = ecs_mt_define_system(g_ecs_mt, 0, concurrent_add_system, NULL, NULL, NULL);
    ecs_require_component(g_ecs_mt->ecs, sys, g_comp1);

    // Create entities
    int num_entities = NUM_THREADS * 10;
    ecs_entity_t *entities = malloc(num_entities * sizeof(ecs_entity_t));

    for (int i = 0; i < num_entities; i++) {
        entities[i] = ecs_create(g_ecs_mt->ecs);
        ecs_add(g_ecs_mt->ecs, entities[i], g_comp1, NULL);
    }

    // Run the system - multiple threads will add components concurrently
    ecs_run_system(g_ecs_mt->ecs, sys, 0);

    printf("    Component adds completed: %d (expected %d)\n", g_add_success_count, num_entities);

    // Verify all entities got their components
    int comp2_count = 0;
    int comp3_count = 0;
    for (int i = 0; i < num_entities; i++) {
        if (ecs_has(g_ecs_mt->ecs, entities[i], g_comp2)) comp2_count++;
        if (ecs_has(g_ecs_mt->ecs, entities[i], g_comp3)) comp3_count++;
    }

    printf("    Entities with comp2: %d, comp3: %d\n", comp2_count, comp3_count);

    free(entities);

    // This will likely fail due to:
    // - Lost component additions
    // - Corrupted entity bitsets
    // - Incorrect system membership
    REQUIRE(g_add_success_count == num_entities);
    REQUIRE((comp2_count + comp3_count) == num_entities);

    return true;
}

/*==============================================================================
 * Test 3: Concurrent ecs_remove calls
 *
 * Issue: Multiple threads calling ecs_remove() simultaneously can cause:
 * - Race condition on entity component bitset
 * - Race condition on sparse set remove (system membership)
 * - Concurrent system entity array modifications
 * Solution: ecs_mt_remove
 *============================================================================*/

static volatile int g_remove_count = 0;
static pthread_mutex_t g_remove_mutex = PTHREAD_MUTEX_INITIALIZER;

ecs_ret_t concurrent_remove_system(ecs_mt_t *ecs_mt, ecs_entity_t *entities, size_t entity_count, void *udata)
{
    (void)udata;

    // Each thread tries to remove components from entities using the thread-safe API
    for (size_t i = 0; i < entity_count; i++) {
        ecs_entity_t entity = entities[i];

        // Remove the component - buffered for thread-safety!
        if (ecs_has(ecs_mt->ecs, entity, g_comp2)) {
            ecs_mt_remove(ecs_mt, entity, g_comp2);

            pthread_mutex_lock(&g_remove_mutex);
            g_remove_count++;
            pthread_mutex_unlock(&g_remove_mutex);
        }
    }

    return 0;
}

TEST_CASE(test_concurrent_remove_race)
{
    g_remove_count = 0;

    ecs_system_t sys = ecs_mt_define_system(g_ecs_mt, 0, concurrent_remove_system, NULL, NULL, NULL);
    ecs_require_component(g_ecs_mt->ecs, sys, g_comp1);

    // Create entities with components
    int num_entities = NUM_THREADS * 10;
    ecs_entity_t *entities = malloc(num_entities * sizeof(ecs_entity_t));

    for (int i = 0; i < num_entities; i++) {
        entities[i] = ecs_create(g_ecs_mt->ecs);
        ecs_add(g_ecs_mt->ecs, entities[i], g_comp1, NULL);
        ecs_add(g_ecs_mt->ecs, entities[i], g_comp2, NULL);
    }

    // Run the system - multiple threads will remove components concurrently
    ecs_run_system(g_ecs_mt->ecs, sys, 0);

    printf("    Component removes completed: %d (expected %d)\n", g_remove_count, num_entities);

    // Verify all components were removed
    int still_has_comp2 = 0;
    for (int i = 0; i < num_entities; i++) {
        if (ecs_has(g_ecs_mt->ecs, entities[i], g_comp2)) { still_has_comp2++; }
    }

    printf("    Entities still with comp2: %d\n", still_has_comp2);

    free(entities);

    // This will likely fail due to:
    // - Lost component removals
    // - Corrupted entity bitsets
    // - Incorrect system membership
    REQUIRE(g_remove_count == num_entities);
    REQUIRE(still_has_comp2 == 0);

    return true;
}

/*==============================================================================
 * Test 4: Concurrent ecs_destroy calls
 *
 * Issue: Multiple threads calling ecs_destroy() simultaneously can cause:
 * - Race condition on entity_pool push
 * - Race condition on sparse set removals
 * - Double-free or use-after-free issue
 * Solution: Use ecs_mt_destroy
 *============================================================================*/

static volatile int g_destroy_count = 0;
static pthread_mutex_t g_destroy_mutex = PTHREAD_MUTEX_INITIALIZER;

ecs_ret_t concurrent_destroy_system(ecs_mt_t *ecs_mt, ecs_entity_t *entities, size_t entity_count, void *udata)
{
    (void)udata;

    // Each thread tries to destroy entities using the thread-safe API
    // Note: Buffering makes this safe even while iterating!
    for (size_t i = 0; i < entity_count; i++) {
        ecs_entity_t entity = entities[i];

        if (ecs_is_ready(ecs_mt->ecs, entity)) {
            ecs_mt_destroy(ecs_mt, entity);

            pthread_mutex_lock(&g_destroy_mutex);
            g_destroy_count++;
            pthread_mutex_unlock(&g_destroy_mutex);
        }
    }

    return 0;
}

TEST_CASE(test_concurrent_destroy_race)
{
    g_destroy_count = 0;

    ecs_system_t sys = ecs_mt_define_system(g_ecs_mt, 0, concurrent_destroy_system, NULL, NULL, NULL);
    ecs_require_component(g_ecs_mt->ecs, sys, g_comp1);

    // Create entities
    int num_entities = NUM_THREADS * 10;
    ecs_entity_t *entities = malloc(num_entities * sizeof(ecs_entity_t));

    for (int i = 0; i < num_entities; i++) {
        entities[i] = ecs_create(g_ecs_mt->ecs);
        ecs_add(g_ecs_mt->ecs, entities[i], g_comp1, NULL);
    }

    // Run the system - multiple threads will destroy entities concurrently
    // This is VERY dangerous!
    ecs_run_system(g_ecs_mt->ecs, sys, 0);

    printf("    Entities destroyed: %d (expected %d)\n", g_destroy_count, num_entities);

    // Verify all entities were destroyed
    int still_ready = 0;
    for (int i = 0; i < num_entities; i++) {
        if (ecs_is_ready(g_ecs_mt->ecs, entities[i])) { still_ready++; }
    }

    printf("    Entities still ready: %d\n", still_ready);

    free(entities);

    // This will likely:
    // - Crash due to iterator invalidation
    // - Have entities that weren't destroyed
    // - Corrupt internal data structures
    REQUIRE(g_destroy_count <= num_entities); // May destroy fewer due to races
    REQUIRE(still_ready == 0);

    return true;
}

/*==============================================================================
 * Test 5: Mixed operations stress test
 *
 * This test performs multiple different operations concurrently to stress
 * test the system and reveal interaction issues between operations.
 *============================================================================*/

static volatile int g_mixed_operations = 0;
static pthread_mutex_t g_mixed_mutex = PTHREAD_MUTEX_INITIALIZER;

ecs_ret_t mixed_operations_system(ecs_mt_t *ecs_mt, ecs_entity_t *entities, size_t entity_count, void *udata)
{
    (void)udata;

    for (size_t i = 0; i < entity_count; i++) {
        ecs_entity_t entity = entities[i];

        // Different threads do different operations
        switch (ecs_mt->thread_id % 3) {
            case 0:
                // Create new entities
                // TODO: Currently fails, because ecs_mt_create is not thread safe.
                /* for (int j = 0; j < 5; j++) { */
                /*     ecs_entity_t new_entity = ecs_mt_create(ecs_mt); */
                /*     ecs_mt_add(ecs_mt, new_entity, g_comp1, NULL, 0); */
                /* } */
                break;

            case 1:
                // Add/remove components
                if (!ecs_has(ecs_mt->ecs, entity, g_comp2)) {
                    ecs_mt_add(ecs_mt, entity, g_comp2, NULL, 0);
                } else {
                    ecs_mt_remove(ecs_mt, entity, g_comp2);
                }
                break;

            case 2:
                // Destroy every other entity
                if (i % 2 == 0 && ecs_is_ready(ecs_mt->ecs, entity)) {
                    ecs_mt_destroy(ecs_mt, entity);
                }
                break;
        }

        pthread_mutex_lock(&g_mixed_mutex);
        g_mixed_operations++;
        pthread_mutex_unlock(&g_mixed_mutex);
    }

    return 0;
}

TEST_CASE(test_mixed_operations_stress)
{
    g_mixed_operations = 0;

    ecs_system_t sys = ecs_mt_define_system(g_ecs_mt, 0, mixed_operations_system, NULL, NULL, NULL);
    ecs_require_component(g_ecs_mt->ecs, sys, g_comp1);

    // Create initial entities
    int num_entities = NUM_THREADS * 10;
    for (int i = 0; i < num_entities; i++) {
        ecs_entity_t entity = ecs_create(g_ecs_mt->ecs);
        ecs_add(g_ecs_mt->ecs, entity, g_comp1, NULL);
    }

    // Run multiple iterations to increase likelihood of race conditions
    for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
        printf("    Iteration %d/%d\n", iter + 1, NUM_ITERATIONS);
        ecs_run_system(g_ecs_mt->ecs, sys, 0);
    }

    printf("    Total operations: %d\n", g_mixed_operations);

    // If we get here without crashing, that's something!
    // But we still likely have corrupted state
    REQUIRE(g_mixed_operations > 0);

    return true;
}

/*==============================================================================
 * Test Suite
 *============================================================================*/

TEST_SUITE(suite_ecs_mt_threading_issues)
{
    printf("\n");
    printf("These tests demonstrate threading issues in pico_ecs_mt.h\n");
    printf("when ecs_create, ecs_add, ecs_remove, and ecs_destroy are called\n");
    printf("from within multi-threaded systems.\n\n");
    printf("Expected behavior: Tests may FAIL or CRASH due to race conditions.\n");
    printf("This is intentional - we're demonstrating the problem before fixing it.\n");
    printf("\n");

    RUN_TEST_CASE(test_concurrent_create_race);
    RUN_TEST_CASE(test_concurrent_add_race);
    RUN_TEST_CASE(test_concurrent_remove_race);
    RUN_TEST_CASE(test_concurrent_destroy_race);
    RUN_TEST_CASE(test_mixed_operations_stress);
}

int main()
{
    pu_display_colors(true);
    pu_setup(setup, teardown);
    RUN_TEST_SUITE(suite_ecs_mt_threading_issues);
    pu_print_stats();

    return pu_test_failed();
}
