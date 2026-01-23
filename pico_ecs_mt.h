#ifndef PICO_ECS_MT_H
#define PICO_ECS_MT_H

#include "pico_ecs.h"

#ifndef PICO_ECS_MT_MAX_TASKS
#define PICO_ECS_MT_MAX_TASKS 64
#endif

#ifndef PICO_ECS_MT_CMD_BUFFER_INITIAL_SIZE
#define PICO_ECS_MT_CMD_BUFFER_INITIAL_SIZE 256
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

/**
 * @brief Thread-safe entity creation with command buffering
 *
 * Queues entity creation to be executed after all tasks complete.
 * Returns a valid entity ID immediately.
 *
 * @param ecs Multi-threaded ECS context
 * @return    New entity ID
 */
ecs_entity_t ecs_mt_create(ecs_mt_t *ecs);

/**
 * @brief Thread-safe entity destruction with command buffering
 *
 * Queues entity destruction to be executed after all tasks complete.
 *
 * @param ecs    Multi-threaded ECS context
 * @param entity Entity to destroy
 */
void ecs_mt_destroy(ecs_mt_t *ecs, ecs_entity_t entity);

/**
 * @brief Thread-safe component addition with command buffering
 *
 * Queues component addition to be executed after all tasks complete.
 * Component data is copied if size > 0.
 *
 * @param ecs       Multi-threaded ECS context
 * @param entity    Entity to add component to
 * @param component Component type
 * @param data      Component data to copy (can be NULL)
 * @param size      Size of data to copy (0 if data is NULL)
 * @return          0 on success, error code otherwise
 */
ecs_ret_t ecs_mt_add(ecs_mt_t *ecs, ecs_entity_t entity, ecs_comp_t component, void *data, size_t size);

/**
 * @brief Thread-safe component removal with command buffering
 *
 * Queues component removal to be executed after all tasks complete.
 *
 * @param ecs       Multi-threaded ECS context
 * @param entity    Entity to remove component from
 * @param component Component type to remove
 * @return          0 on success, error code otherwise
 */
ecs_ret_t ecs_mt_remove(ecs_mt_t *ecs, ecs_entity_t entity, ecs_comp_t component);

// Implementation

#ifdef PICO_ECS_IMPLEMENTATION

#include <stdlib.h> // malloc, realloc, free
#include <string.h> // memset, memcpy

/**
 * @brief Command type for deferred operations
 */
typedef enum
{
    ECS_MT_CMD_CREATE,
    ECS_MT_CMD_DESTROY,
    ECS_MT_CMD_ADD,
    ECS_MT_CMD_REMOVE
} ecs_mt_cmd_type_t;

/**
 * @brief Command for deferred ECS operations
 */
typedef struct
{
    ecs_mt_cmd_type_t type;
    union
    {
        struct
        {
            ecs_entity_t entity; /* Pre-allocated entity ID */
        } create;
        struct
        {
            ecs_entity_t entity;
        } destroy;
        struct
        {
            ecs_entity_t entity;
            ecs_comp_t component;
            void *data;       /* Copied component data (NULL if not provided) */
            size_t data_size; /* Size of copied data */
        } add;
        struct
        {
            ecs_entity_t entity;
            ecs_comp_t component;
        } remove;
    };
} ecs_mt_cmd_t;

/**
 * @brief Per-thread command buffer
 */
typedef struct
{
    ecs_mt_cmd_t *commands;
    size_t count;
    size_t capacity;

    void *mem_ctx;
} ecs_mt_cmd_buffer_t;

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
    void *mem_ctx;

    ecs_mt_system_ctx_t system_contexts[PICO_ECS_MAX_SYSTEMS];
    size_t system_count;

    /* Command buffers for deferred operations (ecs_mt_add, ecs_mt_remove, etc.) */
    /* One buffer per task/thread for lock-free command recording */
    ecs_mt_cmd_buffer_t *cmd_buffers; /* Array of task_count buffers */
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
 * @brief Ensures command buffer has enough capacity for one more command
 */
static void ecs_mt_cmd_buffer_ensure_capacity(ecs_mt_cmd_buffer_t *buf)
{
    if (buf->count >= buf->capacity) {
        size_t new_capacity = buf->capacity * 2;
        ecs_mt_cmd_t *new_commands = (ecs_mt_cmd_t *)
            ECS_REALLOC(buf->commands, sizeof(ecs_mt_cmd_t) * new_capacity, buf->mem_ctx);
        ECS_ASSERT(new_commands); /* TODO: Better error handling */
        buf->commands = new_commands;
        buf->capacity = new_capacity;
    }
}

/**
 * @brief Resets a command buffer for reuse (doesn't free memory)
 */
static void ecs_mt_cmd_buffer_clear(ecs_mt_cmd_buffer_t *buf)
{
    /* Free any allocated component data */
    for (size_t i = 0; i < buf->count; i++) {
        if (buf->commands[i].type == ECS_MT_CMD_ADD && buf->commands[i].add.data) {
            ECS_FREE(buf->commands[i].add.data, buf->mem_ctx);
            buf->commands[i].add.data = NULL;
        }
    }
    buf->count = 0;
}

/**
 * @brief Flushes all command buffers and applies them to the ECS
 */
static void ecs_mt_flush_commands(ecs_mt_t *ecs_mt)
{
    /* TODO: Should commands be processed in a specific order? */
    for (int i = 0; i < ecs_mt->task_count; i++) {
        ecs_mt_cmd_buffer_t *buf = &ecs_mt->cmd_buffers[i];
        for (size_t j = 0; j < buf->count; j++) {
            ecs_mt_cmd_t *cmd = &buf->commands[j];
            switch (cmd->type) {
                case ECS_MT_CMD_CREATE:
                    /* Entity ID was pre-allocated, just mark as ready */
                    /* The entity should already exist from pre-allocation */
                    break;
                case ECS_MT_CMD_ADD:
                    ecs_add(
                        ecs_mt->ecs,
                        cmd->add.entity,
                        cmd->add.component,
                        cmd->add.data
                    );
                    break;
                case ECS_MT_CMD_REMOVE:
                    ecs_remove(ecs_mt->ecs, cmd->remove.entity, cmd->remove.component);
                    break;
                case ECS_MT_CMD_DESTROY:
                    ecs_destroy(ecs_mt->ecs, cmd->destroy.entity);
                    break;
            }
        }
    }

    /* Clear all command buffers for reuse */
    for (int i = 0; i < ecs_mt->task_count; i++) {
        ecs_mt_cmd_buffer_clear(&ecs_mt->cmd_buffers[i]);
    }
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

    // Flush all command buffers after tasks complete
    ecs_mt_flush_commands(ecs_mt);

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
    ecs_mt->mem_ctx = mem_ctx;

    /* Allocate command buffers (one per thread) */
    ecs_mt->cmd_buffers = (ecs_mt_cmd_buffer_t *)
        ECS_MALLOC(sizeof(ecs_mt_cmd_buffer_t) * task_count, mem_ctx);
    if (!ecs_mt->cmd_buffers) {
        ecs_free(ecs_mt->ecs);
        ECS_FREE(ecs_mt, mem_ctx);
        return NULL;
    }

    /* Initialize each command buffer */
    for (int i = 0; i < task_count; i++) {
        ecs_mt->cmd_buffers[i].commands = (ecs_mt_cmd_t *)
            ECS_MALLOC(sizeof(ecs_mt_cmd_t) * PICO_ECS_MT_CMD_BUFFER_INITIAL_SIZE, mem_ctx);
        if (!ecs_mt->cmd_buffers[i].commands) {
            /* Cleanup on failure */
            for (int j = 0; j < i; j++) {
                ECS_FREE(ecs_mt->cmd_buffers[j].commands, mem_ctx);
            }
            ECS_FREE(ecs_mt->cmd_buffers, mem_ctx);
            ecs_free(ecs_mt->ecs);
            ECS_FREE(ecs_mt, mem_ctx);
            return NULL;
        }
        ecs_mt->cmd_buffers[i].count = 0;
        ecs_mt->cmd_buffers[i].capacity = PICO_ECS_MT_CMD_BUFFER_INITIAL_SIZE;
        ecs_mt->cmd_buffers[i].mem_ctx = mem_ctx;
    }

    return ecs_mt;
}

void ecs_mt_free(ecs_mt_t *ecs_mt)
{
    if (!ecs_mt) return;

    /* Free command buffers */
    if (ecs_mt->cmd_buffers) {
        for (int i = 0; i < ecs_mt->task_count; i++) {
            ecs_mt_cmd_buffer_t *buf = &ecs_mt->cmd_buffers[i];
            /* Free any allocated component data in add commands */
            for (size_t j = 0; j < buf->count; j++) {
                if (buf->commands[j].type == ECS_MT_CMD_ADD &&
                    buf->commands[j].add.data) {
                    ECS_FREE(buf->commands[j].add.data, buf->mem_ctx);
                }
            }
            ECS_FREE(buf->commands, buf->mem_ctx);
        }
        ECS_FREE(ecs_mt->cmd_buffers, ecs_mt->mem_ctx);
    }

    ecs_free(ecs_mt->ecs);
    ECS_FREE(ecs_mt, ecs_mt->mem_ctx);
}

/**
 * @brief Thread-safe entity creation using atomic operations
 */
ecs_entity_t ecs_mt_create(ecs_mt_t *ecs_mt)
{
    ECS_ASSERT(ecs_mt);
    ECS_ASSERT(ecs_mt->thread_id > 0 && ecs_mt->thread_id <= ecs_mt->task_count);

    /* For now, we use ecs_create directly - this needs to be made thread-safe */
    /* TODO: Add atomic operations for thread-safe entity ID allocation? */
    ecs_entity_t entity = ecs_create(ecs_mt->ecs);

    /* Queue the creation command */
    ecs_mt_cmd_buffer_t *buf = &ecs_mt->cmd_buffers[ecs_mt->thread_id - 1];
    ecs_mt_cmd_buffer_ensure_capacity(buf);

    ecs_mt_cmd_t *cmd = &buf->commands[buf->count++];
    cmd->type = ECS_MT_CMD_CREATE;
    cmd->create.entity = entity;

    return entity;
}

/**
 * @brief Thread-safe entity destruction
 */
void ecs_mt_destroy(ecs_mt_t *ecs_mt, ecs_entity_t entity)
{
    ECS_ASSERT(ecs_mt);
    ECS_ASSERT(ecs_mt->thread_id > 0 && ecs_mt->thread_id <= ecs_mt->task_count);

    /* Queue the destroy command */
    ecs_mt_cmd_buffer_t *buf = &ecs_mt->cmd_buffers[ecs_mt->thread_id - 1];
    ecs_mt_cmd_buffer_ensure_capacity(buf);

    ecs_mt_cmd_t *cmd = &buf->commands[buf->count++];
    cmd->type = ECS_MT_CMD_DESTROY;
    cmd->destroy.entity = entity;
}

/**
 * @brief Thread-safe component addition
 */
ecs_ret_t ecs_mt_add(ecs_mt_t *ecs_mt, ecs_entity_t entity, ecs_comp_t component, void *data, size_t size)
{
    ECS_ASSERT(ecs_mt);
    ECS_ASSERT(ecs_mt->thread_id > 0 && ecs_mt->thread_id <= ecs_mt->task_count);

    /* Queue the add command */
    ecs_mt_cmd_buffer_t *buf = &ecs_mt->cmd_buffers[ecs_mt->thread_id - 1];
    ecs_mt_cmd_buffer_ensure_capacity(buf);

    ecs_mt_cmd_t *cmd = &buf->commands[buf->count++];
    cmd->type = ECS_MT_CMD_ADD;
    cmd->add.entity = entity;
    cmd->add.component = component;

    /* Copy component data if provided */
    if (data && size > 0) {
        void *data_copy = ECS_MALLOC(size, buf->mem_ctx);
        ECS_ASSERT(data_copy); /* TODO: Better error handling */
        memcpy(data_copy, data, size);

        cmd->add.data = data_copy;
        cmd->add.data_size = size;
    } else {
        cmd->add.data = NULL;
        cmd->add.data_size = 0;
    }

    return 0;
}

/**
 * @brief Thread-safe component removal
 */
ecs_ret_t ecs_mt_remove(ecs_mt_t *ecs_mt, ecs_entity_t entity, ecs_comp_t component)
{
    ECS_ASSERT(ecs_mt);
    ECS_ASSERT(ecs_mt->thread_id > 0 && ecs_mt->thread_id <= ecs_mt->task_count);

    /* Queue the remove command */
    ecs_mt_cmd_buffer_t *buf = &ecs_mt->cmd_buffers[ecs_mt->thread_id - 1];
    ecs_mt_cmd_buffer_ensure_capacity(buf);

    ecs_mt_cmd_t *cmd = &buf->commands[buf->count++];
    cmd->type = ECS_MT_CMD_REMOVE;
    cmd->remove.entity = entity;
    cmd->remove.component = component;

    return 0;
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
