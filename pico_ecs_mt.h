#ifndef PICO_ECS_MT_H
#define PICO_ECS_MT_H

#include "pico_ecs.h"

#ifndef PICO_ECS_MT_MAX_TASKS
#define PICO_ECS_MT_MAX_TASKS 16
#endif

struct ecs_mt_s;
typedef struct ecs_mt_s ecs_mt_t;

/**
 * @brief Multi-threaded system callback
 *
 * @param ecs          The MT ECS context (with thread-specific thread_id)
 * @param entities     Array of entities for this task to process
 * @param entity_count Number of entities in the array
 * @param udata        User data associated with the system
 */
typedef ecs_ret_t (*ecs_mt_system_fn)(ecs_mt_t *ecs,
                                   ecs_entity_t *entities,
                                   size_t entity_count,
                                   void *udata);

/**
 * @brief Callback to enqueue a task for execution
 *
 * @param task       The system function to execute
 * @param start      Starting index in the entity array
 * @param count      Number of entities for this task
 * @param task_udata Context data for this task
 * @param udata      User data for the threading system
 * @return           Handle to the enqueued task
 */
typedef void *(*ecs_enqueue_task_fn)(ecs_mt_system_fn task,
                                     int start,
                                     int count,
                                     void *task_udata,
                                     void *udata);

/**
 * @brief Callback to wait for a task to complete
 *
 * @param user_task Handle to the task (returned from enqueue_cb)
 * @param udata     User data for the threading system
 */
typedef void (*ecs_finish_task_fn)(void *user_task, void *udata);

/**
 * @brief Creates a multi-threaded ECS context
 *
 * @param entity_count Initial number of entities to pre-allocate
 * @param enqueue_cb   Callback to enqueue tasks
 * @param finish_cb    Callback to wait for task completion
 * @param task_count   Number of parallel tasks to use
 * @param mem_ctx      Memory context for custom allocator
 * @return             MT ECS context or NULL if out of memory
 */
ecs_mt_t *ecs_mt_new(size_t entity_count,
                     ecs_enqueue_task_fn enqueue_cb,
                     ecs_finish_task_fn finish_cb,
                     int task_count,
                     void *mem_ctx);

/**
 * @brief Destroys a multi-threaded ECS context
 *
 * @param ecs The MT ECS context
 */
void ecs_mt_free(ecs_mt_t *ecs);

/**
 * @brief Defines a multi-threaded system
 *
 * @param ecs       The MT ECS context
 * @param mask      Bitmask for system categories
 * @param system_cb Multi-threaded system callback
 * @param add_cb    Called when entity is added (can be NULL)
 * @param remove_cb Called when entity is removed (can be NULL)
 * @param udata     User data passed to callbacks
 * @return          System handle
 */
ecs_system_t ecs_mt_define_system(ecs_mt_t *ecs,
                                  ecs_mask_t mask,
                                  ecs_mt_system_fn system_cb,
                                  ecs_added_fn add_cb,
                                  ecs_removed_fn remove_cb,
                                  void *udata);

// Implementation

#ifdef PICO_ECS_IMPLEMENTATION

#include <string.h>  // memset, memcpy

#if defined(_MSC_VER)
    #include <malloc.h>  // _alloca
    #define ecs_alloca _alloca
#elif defined(__GNUC__) || defined(__clang__)
    #include <alloca.h>
    #define ecs_alloca alloca
#else
    #error "alloca not supported on this compiler"
#endif

/**
 * @brief System context (internal)
 */
typedef struct {
    ecs_mt_system_fn user_callback;
    void* user_udata;
    ecs_mt_t* ecs_mt;
} ecs_mt_system_ctx_t;

struct ecs_mt_s {
    ecs_t *ecs;
    ecs_enqueue_task_fn enqueue_cb;
    ecs_finish_task_fn finish_cb;
    int task_count;
    int thread_id;

    // System contexts (shared across all threads)
    ecs_mt_system_ctx_t system_contexts[PICO_ECS_MAX_SYSTEMS];
    size_t system_count;

    // TODO: Command buffers for deferred operations (ecs_add, ecs_remove, etc.)
    // One buffer per task/thread for thread-safe command recording
};

/**
 * @brief Task context (internal)
 */
typedef struct {
    ecs_mt_t ecs_mt;           // Copy with thread-specific thread_id
    ecs_entity_t* entities;
    size_t entity_count;
    void* user_udata;
} ecs_mt_task_ctx_t;



/**
 * @brief Internal wrapper system that divides entities across tasks
 */
static ecs_ret_t ecs_mt_system(ecs_t *ecs,
                               ecs_entity_t *entities,
                               size_t entity_count,
                               void *udata)
{
    ecs_mt_system_ctx_t* ctx = (ecs_mt_system_ctx_t*)udata;
    ecs_mt_t* ecs_mt = ctx->ecs_mt;

    // Early return if no entities
    if (entity_count == 0)
        return 0;

    // Calculate entities per task
    size_t entities_per_task = (entity_count + ecs_mt->task_count - 1) / ecs_mt->task_count;

    // Allocate task contexts and handles on stack
    ecs_mt_task_ctx_t* task_contexts = (ecs_mt_task_ctx_t*)ecs_alloca(
        ecs_mt->task_count * sizeof(ecs_mt_task_ctx_t));
    void** task_handles = (void**)ecs_alloca(
        ecs_mt->task_count * sizeof(void*));

    int num_tasks = 0;

    // Divide entities into tasks
    for (size_t i = 0; i < entity_count; i += entities_per_task) {
        size_t count = (i + entities_per_task > entity_count)
                       ? (entity_count - i)
                       : entities_per_task;

        // Create a copy of ecs_mt with thread-specific thread_id
        task_contexts[num_tasks].ecs_mt = *ecs_mt;
        task_contexts[num_tasks].ecs_mt.thread_id = num_tasks + 1;  // 0 is main thread
        task_contexts[num_tasks].entities = entities + i;
        task_contexts[num_tasks].entity_count = count;
        task_contexts[num_tasks].user_udata = ctx->user_udata;

        // Enqueue task
        task_handles[num_tasks] = ecs_mt->enqueue_cb(
            ctx->user_callback,
            (int)i,
            (int)count,
            &task_contexts[num_tasks],
            NULL  // No additional udata for threading system
        );

        num_tasks++;
    }

    // Wait for all tasks to complete
    for (int i = 0; i < num_tasks; i++) {
        ecs_mt->finish_cb(task_handles[i], NULL);
    }

    // TODO: Flush command buffers from all threads

    return 0;
}

ecs_mt_t *ecs_mt_new(size_t entity_count,
                     ecs_enqueue_task_fn enqueue_cb,
                     ecs_finish_task_fn finish_cb,
                     int task_count,
                     void *mem_ctx)
{
    ECS_ASSERT(entity_count > 0);
    ECS_ASSERT(enqueue_cb != NULL);
    ECS_ASSERT(finish_cb != NULL);
    ECS_ASSERT(task_count > 0 && task_count <= PICO_ECS_MT_MAX_TASKS);

    ecs_mt_t *ecs_mt = (ecs_mt_t*)ECS_MALLOC(sizeof(ecs_mt_t), mem_ctx);
    if (!ecs_mt)
        return NULL;

    memset(ecs_mt, 0, sizeof(ecs_mt_t));

    ecs_mt->ecs = ecs_new(entity_count, mem_ctx);
    if (!ecs_mt->ecs) {
        ECS_FREE(ecs_mt, mem_ctx);
        return NULL;
    }

    ecs_mt->enqueue_cb = enqueue_cb;
    ecs_mt->finish_cb = finish_cb;
    ecs_mt->task_count = task_count;
    ecs_mt->thread_id = 0;  // Main thread
    ecs_mt->system_count = 0;

    return ecs_mt;
}

void ecs_mt_free(ecs_mt_t *ecs_mt)
{
    if (!ecs_mt)
        return;

    void* mem_ctx = ecs_mt->ecs->mem_ctx;
    ecs_free(ecs_mt->ecs);
    ECS_FREE(ecs_mt, mem_ctx);
}

ecs_system_t ecs_mt_define_system(ecs_mt_t *ecs_mt,
                                  ecs_mask_t mask,
                                  ecs_mt_system_fn system_cb,
                                  ecs_added_fn add_cb,
                                  ecs_removed_fn remove_cb,
                                  void *udata)
{
    ECS_ASSERT(ecs_mt != NULL);
    ECS_ASSERT(system_cb != NULL);
    ECS_ASSERT(ecs_mt->system_count < PICO_ECS_MAX_SYSTEMS);

    // Get next available system context
    ecs_mt_system_ctx_t* ctx = &ecs_mt->system_contexts[ecs_mt->system_count++];
    ctx->user_callback = system_cb;
    ctx->user_udata = udata;
    ctx->ecs_mt = ecs_mt;

    // Define system on underlying ECS, using ecs_mt_system as the wrapper
    return ecs_define_system(ecs_mt->ecs, mask, ecs_mt_system, add_cb, remove_cb, ctx);
}

#endif // PICO_ECS_IMPLEMENTATION

#endif // PICO_ECS_MT_H
