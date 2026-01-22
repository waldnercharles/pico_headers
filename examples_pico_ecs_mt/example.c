#define PICO_ECS_IMPLEMENTATION
#include "../pico_ecs_mt.h"

#include <stdio.h>

// Steps to create a multi-threaded ECS:
// 1. Create an MT ECS instance (using ecs_mt_new with task callbacks)
// 2. Define concrete components types (structs)
// 3. Assign each a unique, zero-based ID (using an enum is recommended)
// 4. Write multi-threaded system update callbacks
// 5. Define zero-based system IDs (using an enum is recommended)
// 6. Register components
// 7. Register systems
// 8. Associate components with systems (using ecs_require_component)

// Concrete component structs
typedef struct
{
    float x, y;
} pos_t;

typedef struct
{
    float vx, vy;
} vel_t;

typedef struct
{
    int x, y, w, h;
} rect_t;

// Component types
ecs_comp_t PosComp;
ecs_comp_t VelComp;
ecs_comp_t RectComp;

// System types
ecs_system_t System1;
ecs_system_t System2;
ecs_system_t System3;

// Placeholder task enqueue callback
// In a real implementation, this would submit the task to a thread pool
void* enqueue_task(ecs_mt_system_fn task, int start, int count, void* task_udata, void* udata)
{
    (void)start;
    (void)count;
    (void)udata;

    // For this example, we'll just execute the task immediately on the main thread
    ecs_mt_task_ctx_t* ctx = (ecs_mt_task_ctx_t*)task_udata;

    printf("    [Task %d] Processing %zu entities starting at index %d\n",
           ctx->ecs_mt.thread_id, ctx->entity_count, start);

    // Execute the task (in a real implementation, this would be done by a worker thread)
    task(&ctx->ecs_mt, ctx->entities, ctx->entity_count, ctx->user_udata);

    return (void*)(long)ctx->ecs_mt.thread_id;  // Return thread_id as handle
}

// Placeholder task finish callback
// In a real implementation, this would wait for the task to complete
void finish_task(void* user_task, void* udata)
{
    (void)udata;

    int thread_id = (int)(long)user_task;
    printf("    [Task %d] Completed\n", thread_id);
}

// Register components
void register_components(ecs_mt_t* ecs_mt)
{
    PosComp  = ecs_define_component(ecs_mt->ecs, sizeof(pos_t),  NULL, NULL);
    VelComp  = ecs_define_component(ecs_mt->ecs, sizeof(vel_t),  NULL, NULL);
    RectComp = ecs_define_component(ecs_mt->ecs, sizeof(rect_t), NULL, NULL);
}

// Multi-threaded system that prints the entity IDs of entities processed by this task
ecs_ret_t system_update(ecs_mt_t* ecs_mt,
                       ecs_entity_t* entities,
                       size_t entity_count,
                       void* udata)
{
    (void)ecs_mt;
    (void)udata;

    printf("        Thread %d: ", ecs_mt->thread_id);

    for (size_t i = 0; i < entity_count; i++)
    {
        printf("%lu ", entities[i].id);
    }

    printf("\n");

    return 0;
}

// Register all systems and required relationships
void register_systems(ecs_mt_t* ecs_mt)
{
    // Register systems
    System1 = ecs_mt_define_system(ecs_mt, 0, system_update, NULL, NULL, NULL);
    System2 = ecs_mt_define_system(ecs_mt, 0, system_update, NULL, NULL, NULL);
    System3 = ecs_mt_define_system(ecs_mt, 0, system_update, NULL, NULL, NULL);

    // System1 requires PosComp compnents
    ecs_require_component(ecs_mt->ecs, System1, PosComp);

    // System2 requires both PosComp and VelComp components
    ecs_require_component(ecs_mt->ecs, System2, PosComp);
    ecs_require_component(ecs_mt->ecs, System2, VelComp);

    // System3 requires the PosComp, VelComp, and RectComp components
    ecs_require_component(ecs_mt->ecs, System3, PosComp);
    ecs_require_component(ecs_mt->ecs, System3, VelComp);
    ecs_require_component(ecs_mt->ecs, System3, RectComp);
}


int main()
{
    // Creates concrete MT ECS instance with 4 parallel tasks
    ecs_mt_t* ecs_mt = ecs_mt_new(1024, enqueue_task, finish_task, 4, NULL);

    // Register components and systems
    register_components(ecs_mt);
    register_systems(ecs_mt);

    // Create entities for demonstration (more entities to show parallelism)
    printf("---------------------------------------------------------------\n");
    printf("Creating entities and adding components...\n");
    printf("---------------------------------------------------------------\n");

    ecs_entity_t entities[10];

    // Create entities with just PosComp
    for (int i = 0; i < 10; i++)
    {
        entities[i] = ecs_create(ecs_mt->ecs);
        ecs_add(ecs_mt->ecs, entities[i], PosComp, NULL);
    }

    // Add VelComp to half of them
    for (int i = 5; i < 10; i++)
    {
        ecs_add(ecs_mt->ecs, entities[i], VelComp, NULL);
    }

    // Add RectComp to a quarter of them
    for (int i = 7; i < 10; i++)
    {
        ecs_add(ecs_mt->ecs, entities[i], RectComp, NULL);
    }

    printf("---------------------------------------------------------------\n");

    // Manually execute the systems
    printf("Executing system 1 (entities with PosComp)\n");
    ecs_run_system(ecs_mt->ecs, System1, 0); // Should process all 10 entities across 4 tasks
    printf("\n");

    printf("Executing system 2 (entities with PosComp + VelComp)\n");
    ecs_run_system(ecs_mt->ecs, System2, 0); // Should process entities 5-9 across tasks
    printf("\n");

    printf("Executing system 3 (entities with PosComp + VelComp + RectComp)\n");
    ecs_run_system(ecs_mt->ecs, System3, 0); // Should process entities 7-9 across tasks
    printf("\n");

    printf("---------------------------------------------------------------\n");

    ecs_mt_free(ecs_mt);

    return 0;
}
