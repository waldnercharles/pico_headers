#ifndef PICO_ECS_MT_H
#define PICO_ECS_MT_H

#include "pico_ecs.h"

#ifndef PICO_ECS_MT_MAX_TASKS
#define PICO_ECS_MT_MAX_TASKS 64
#endif

struct ecs_mt_s;
typedef struct ecs_mt_s ecs_mt_t;

/**
 * @brief Multi-threaded system callback
 *
 * @param ecs          Multi-threaded ECS context
 * @param entities     Array of entities for this task to process
 * @param entity_count Number of entities in the array
 * @param udata        User data associated with the system
 */
typedef ecs_ret_t (*ecs_mt_system_fn)(
    ecs_mt_t *ecs,
    ecs_entity_t *entities,
    size_t entity_count,
    void *udata
);

/**
 * @brief Callback to enqueue a task for execution
 *
 * @param fn       The system function to execute
 * @param fn_udata Context data for this task
 * @param udata    User data (Usually the user's threadpool)
 * @return         0 on success
 */
typedef int (*ecs_enqueue_task_fn)(int (*fn)(void *args), void *fn_args, void *udata);

/**
 * @brief Callback to wait for a task to complete
 *
 * @param udata     User data (Usually a pointer to the user's threadpool)
 */
typedef void (*ecs_wait_tasks_fn)(void *udata);

/**
 * @brief Creates a multi-threaded ECS context
 *
 * @param entity_count Initial number of entities to pre-allocate
 * @param enqueue_cb   Callback to enqueue tasks
 * @param finish_cb    Callback to wait for task completion
 * @param task_count   Number of parallel tasks to use
 * @param mem_ctx      Memory context for custom allocator
 * @return             Multi-threaded ECS context
 */
ecs_mt_t *ecs_mt_new(
    size_t entity_count,
    ecs_enqueue_task_fn enqueue_cb,
    ecs_wait_tasks_fn wait_cb,
    void *task_udata,
    int task_count,
    void *mem_ctx
);

/**
 * @brief Destroys a multi-threaded ECS context
 *
 * @param ecs Multi-threaded ECS context
 */
void ecs_mt_free(ecs_mt_t *ecs);

/**
 * @brief Defines a multi-threaded system
 *
 * @param ecs       Multi-threaded ECS context
 * @param mask      Bitmask for system categories
 * @param system_cb Multi-threaded system callback
 * @param add_cb    Called when entity is added (can be NULL)
 * @param remove_cb Called when entity is removed (can be NULL)
 * @param udata     User data passed to callbacks
 * @return          System handle
 */
ecs_system_t ecs_mt_define_system(
    ecs_mt_t *ecs,
    ecs_mask_t mask,
    ecs_mt_system_fn system_cb,
    ecs_added_fn add_cb,
    ecs_removed_fn remove_cb,
    void *udata
);

// Implementation

#ifdef PICO_ECS_IMPLEMENTATION

#include <string.h> // memset, memcpy

/**
 * @brief System context (internal)
 */
typedef struct
{
    ecs_mt_t *ecs_mt;
    ecs_mt_system_fn system_cb;
    void *udata;
} ecs_mt_system_ctx_t;

struct ecs_mt_s
{
    ecs_t *ecs;
    ecs_enqueue_task_fn enqueue_cb;
    ecs_wait_tasks_fn wait_cb;
    int task_count;
    void *task_udata;
    int thread_id;

    ecs_mt_system_ctx_t system_contexts[PICO_ECS_MAX_SYSTEMS];
    size_t system_count;

    // TODO: Command buffers for deferred operations (ecs_add, ecs_remove, etc.)
    // One buffer per task/thread for thread-safe command recording
};

/**
 * @brief Task context (internal)
 */
typedef struct
{
    ecs_mt_system_fn system_cb;
    ecs_mt_t ecs_mt;
    ecs_entity_t *entities;
    size_t entity_count;
    void *udata;
} ecs_mt_task_ctx_t;

/**
 * @brief Internal trampoline function to call the system_cb
 */
static int ecs_mt_task(void *args)
{
    ecs_mt_task_ctx_t *ctx = (ecs_mt_task_ctx_t *)args;
    ctx->system_cb(&ctx->ecs_mt, ctx->entities, ctx->entity_count, ctx->udata);
    return 0;
}

/**
 * @brief Internal wrapper system that divides entities across tasks
 */
static ecs_ret_t ecs_mt_system(ecs_t *ecs, ecs_entity_t *entities, size_t entity_count, void *udata)
{
    (void)ecs;
    ecs_mt_system_ctx_t *ctx = (ecs_mt_system_ctx_t *)udata;
    ecs_mt_t *ecs_mt = ctx->ecs_mt;

    // Early return if no entities
    if (entity_count == 0) return 0;

    // Calculate entities per task
    size_t entities_per_task = (entity_count + ecs_mt->task_count - 1) / ecs_mt->task_count;

    // Allocate task contexts and handles on stack
    ecs_mt_task_ctx_t task_contexts[PICO_ECS_MT_MAX_TASKS];

    // Divide entities into tasks
    int num_tasks = 0;

    for (size_t i = 0; i < entity_count; i += entities_per_task) {
        size_t count = (i + entities_per_task > entity_count) ? (entity_count - i)
                                                              : entities_per_task;

        // Create a copy of ecs_mt with thread-specific thread_id
        task_contexts[num_tasks].system_cb = ctx->system_cb;
        task_contexts[num_tasks].ecs_mt = *ecs_mt;
        task_contexts[num_tasks].ecs_mt.thread_id = num_tasks + 1;
        task_contexts[num_tasks].entities = entities + i;
        task_contexts[num_tasks].entity_count = count;
        task_contexts[num_tasks].udata = ctx->udata;

        // Enqueue task
        // TODO: What do we do when there is a return value? How should they be aggregated?
        ecs_mt->enqueue_cb(ecs_mt_task, task_contexts + num_tasks, ecs_mt->task_udata);
        num_tasks++;
    }

    // Wait for all tasks to complete
    ecs_mt->wait_cb(ecs_mt->task_udata);

    return 0;
}

ecs_mt_t *ecs_mt_new(
    size_t entity_count,
    ecs_enqueue_task_fn enqueue_cb,
    ecs_wait_tasks_fn wait_cb,
    void *task_udata,
    int task_count,
    void *mem_ctx
)
{
    ecs_mt_t *ecs_mt = (ecs_mt_t *)ECS_MALLOC(sizeof(ecs_mt_t), mem_ctx);
    if (!ecs_mt) return NULL;

    memset(ecs_mt, 0, sizeof(ecs_mt_t));

    ecs_mt->ecs = ecs_new(entity_count, mem_ctx);
    if (!ecs_mt->ecs) {
        ECS_FREE(ecs_mt, mem_ctx);
        return NULL;
    }
    ecs_mt->enqueue_cb = enqueue_cb;
    ecs_mt->wait_cb = wait_cb;
    ecs_mt->task_udata = task_udata;
    ecs_mt->task_count = task_count;
    ecs_mt->thread_id = 0;
    ecs_mt->system_count = 0;

    return ecs_mt;
}

void ecs_mt_free(ecs_mt_t *ecs_mt)
{
    ecs_free(ecs_mt->ecs);
    ECS_ASSERT(ecs_mt);
}

ecs_system_t ecs_mt_define_system(
    ecs_mt_t *ecs_mt,
    ecs_mask_t mask,
    ecs_mt_system_fn system_cb,
    ecs_added_fn add_cb,
    ecs_removed_fn remove_cb,
    void *udata
)
{
    ECS_ASSERT(ecs_mt);
    ECS_ASSERT(system_cb);
    ECS_ASSERT(ecs_mt->system_count < PICO_ECS_MAX_SYSTEMS);

    ecs_mt_system_ctx_t *ctx = &ecs_mt->system_contexts[ecs_mt->system_count++];
    ctx->ecs_mt = ecs_mt;
    ctx->system_cb = system_cb;
    ctx->udata = udata;

    return ecs_define_system(ecs_mt->ecs, mask, ecs_mt_system, add_cb, remove_cb, ctx);
}

#endif // PICO_ECS_IMPLEMENTATION

#endif // PICO_ECS_MT_H
