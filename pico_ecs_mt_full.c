#include "pico_ecs_mt_full.h"

#include <stdatomic.h>
#include <stddef.h> // size_t
#include <stdint.h> // uint32_t, uint64_t
#include <stdlib.h> // malloc, realloc, free
#include <string.h> // memcpy, memset

#ifndef PICO_ECS_MAX_COMPONENTS
#define PICO_ECS_MAX_COMPONENTS 32
#endif

#ifndef PICO_ECS_MAX_SYSTEMS
#define PICO_ECS_MAX_SYSTEMS 16
#endif

#ifdef NDEBUG
#define PICO_ECS_ASSERT(expr) ((void)0)
#else
#ifndef PICO_ECS_ASSERT
#include <assert.h>
#define PICO_ECS_ASSERT(expr) (assert(expr))
#endif
#endif

#if !defined(PICO_ECS_MALLOC) || !defined(PICO_ECS_REALLOC) || !defined(PICO_ECS_FREE)
#include <stdlib.h>
#define PICO_ECS_MALLOC(size, ctx) (malloc(size))
#define PICO_ECS_REALLOC(ptr, size, ctx) (realloc(ptr, size))
#define PICO_ECS_FREE(ptr, ctx) (free(ptr))
#endif

/*=============================================================================
 *  Aliases
 *============================================================================*/

#define ECS_ASSERT PICO_ECS_ASSERT
#define ECS_MAX_COMPONENTS PICO_ECS_MAX_COMPONENTS
#define ECS_MAX_SYSTEMS PICO_ECS_MAX_SYSTEMS
#define ECS_MALLOC PICO_ECS_MALLOC
#define ECS_REALLOC PICO_ECS_REALLOC
#define ECS_FREE PICO_ECS_FREE
#define ECS_CORE(ecs) ((ecs)->core)

/*=============================================================================
 *  Data structures
 *============================================================================*/

#if ECS_MAX_COMPONENTS <= 32
typedef uint32_t ecs_bitset_t;
#elif ECS_MAX_COMPONENTS <= 64
typedef uint64_t ecs_bitset_t;
#else
#define ECS_BITSET_WIDTH 64
#define ECS_BITSET_SIZE (((ECS_MAX_COMPONENTS - 1) / ECS_BITSET_WIDTH) + 1)

typedef struct
{
    uint64_t array[ECS_BITSET_SIZE];
} ecs_bitset_t;

#endif // ECS_MAX_COMPONENTS

// Data-structure for a packed array implementation that provides O(1) functions
// for adding, removing, and accessing entity IDs
typedef struct
{
    size_t capacity;
    size_t size;
    size_t *sparse;
    ecs_entity_t *dense;
} ecs_sparse_set_t;

// A data-structure for providing O(1) operations for working with IDs
typedef struct
{
    size_t capacity;
    _Atomic size_t size; // array size
    ecs_id_t *data;
} ecs_id_array_t;

typedef struct
{
    size_t capacity;
    size_t size; // component size
    void *data;
} ecs_comp_array_t;

typedef struct
{
    ecs_bitset_t comp_bits;
    bool active;
    bool ready;
} ecs_entity_data_t;

typedef struct
{
    ecs_constructor_fn constructor;
    ecs_destructor_fn destructor;
} ecs_comp_data_t;

typedef struct
{
    bool active;
    ecs_sparse_set_t entity_ids;
    ecs_mask_t mask;
    ecs_system_fn system_cb;
    ecs_added_fn add_cb;
    ecs_removed_fn remove_cb;
    ecs_bitset_t require_bits;
    ecs_bitset_t exclude_bits;
    void *udata;
} ecs_sys_data_t;

typedef enum
{
    ECS_CMD_CREATE,
    ECS_CMD_ADD,
    ECS_CMD_REMOVE,
    ECS_CMD_DESTROY,
} ecs_cmd_type_t;

typedef struct
{
    ecs_cmd_type_t type;
    ecs_entity_t entity;
    ecs_comp_t component;
    void *data;
    size_t data_size;
} ecs_cmd_t;

typedef struct
{
    ecs_cmd_t *commands;
    size_t count;
    size_t capacity;
    void *mem_ctx;
} ecs_cmd_buffer_t;

typedef struct
{
    ecs_system_fn system_cb;
    ecs_t *ecs_view;
    ecs_entity_t *entities;
    size_t entity_count;
    void *udata;
    ecs_ret_t result;
} ecs_task_ctx_t;

struct ecs_internal_s
{
    ecs_id_array_t entity_pool;
    ecs_entity_data_t *entities;
    size_t entity_count;
    _Atomic ecs_id_t next_entity_id;
    ecs_comp_data_t comps[ECS_MAX_COMPONENTS];
    ecs_comp_array_t comp_arrays[ECS_MAX_COMPONENTS];
    size_t comp_count;
    ecs_sys_data_t systems[ECS_MAX_SYSTEMS];
    size_t system_count;
    ecs_enqueue_task_fn enqueue_cb;
    ecs_wait_tasks_fn wait_cb;
    void *task_udata;
    int task_count;
    ecs_cmd_buffer_t *cmd_buffers;
    void *mem_ctx;
};

struct ecs_s
{
    ecs_internal_t *core;
    uint32_t task_id;
};

/*=============================================================================
 * Handle constructors
 *============================================================================*/
static inline ecs_entity_t ecs_make_entity(ecs_id_t id);
static inline ecs_comp_t ecs_make_comp(ecs_id_t id);
static inline ecs_system_t ecs_make_system(ecs_id_t id);
static inline ecs_t ecs_make_view(ecs_internal_t *core, uint32_t task_id);
static inline bool ecs_is_task_view(ecs_t *ecs);

/*=============================================================================
 * Realloc wrapper
 *============================================================================*/
static void *ecs_realloc_zero(ecs_t *ecs, void *ptr, size_t old_size, size_t new_size);
static void ecs_ensure_entity_capacity(ecs_t *ecs, ecs_id_t entity_id);
static void ecs_cmd_buffer_ensure_capacity(ecs_cmd_buffer_t *buf);
static void ecs_cmd_buffer_clear(ecs_cmd_buffer_t *buf);
static void ecs_flush_commands(ecs_t *ecs);
static void *ecs_add_internal(ecs_t *ecs, ecs_entity_t entity, ecs_comp_t comp, void *args, bool run_constructor);

/*=============================================================================
 * Removes entity from ALL systems
 *============================================================================*/
static void ecs_remove_from_systems(ecs_t *ecs, ecs_entity_t entity);

/*=============================================================================
 * Calls destructors on all components of the entity
 *============================================================================*/
static void ecs_destruct(ecs_t *ecs, ecs_id_t entity);

/*=============================================================================
 * Tests if entity is active (created)
 *============================================================================*/
static inline bool ecs_is_active(ecs_t *ecs, ecs_id_t entity_id);

/*=============================================================================
 * Bitset functions
 *============================================================================*/
static inline void ecs_bitset_flip(ecs_bitset_t *set, int bit, bool on);
static inline bool ecs_bitset_is_zero(ecs_bitset_t *set);
static inline bool ecs_bitset_test(ecs_bitset_t *set, int bit);
static inline ecs_bitset_t ecs_bitset_and(ecs_bitset_t *set1, ecs_bitset_t *set2);
static inline ecs_bitset_t ecs_bitset_or(ecs_bitset_t *set1, ecs_bitset_t *set2);
static inline ecs_bitset_t ecs_bitset_not(ecs_bitset_t *set);
static inline bool ecs_bitset_equal(ecs_bitset_t *set1, ecs_bitset_t *set2);
static inline bool ecs_bitset_true(ecs_bitset_t *set);

/*=============================================================================
 * Sparse set functions
 *============================================================================*/
static void ecs_sparse_set_init(ecs_t *ecs, ecs_sparse_set_t *set, size_t capacity);
static void ecs_sparse_set_free(ecs_t *ecs, ecs_sparse_set_t *set);
static bool ecs_sparse_set_add(ecs_t *ecs, ecs_sparse_set_t *set, ecs_id_t id);
static bool ecs_sparse_set_find(ecs_sparse_set_t *set, ecs_id_t id, size_t *found);
static bool ecs_sparse_set_remove(ecs_sparse_set_t *set, ecs_id_t id);

/*=============================================================================
 * System entity add/remove functions
 *============================================================================*/
static bool ecs_entity_system_test(
    ecs_bitset_t *require_bits,
    ecs_bitset_t *exclude_bits,
    ecs_bitset_t *entity_bits
);

/*=============================================================================
 * ID array functions
 *============================================================================*/
static void ecs_id_array_init(ecs_t *ecs, ecs_id_array_t *pool, int capacity);
static void ecs_id_array_free(ecs_t *ecs, ecs_id_array_t *pool);
static void ecs_id_array_push(ecs_t *ecs, ecs_id_array_t *pool, ecs_id_t id);
static ecs_id_t ecs_id_array_pop(ecs_id_array_t *pool);
static bool ecs_id_array_try_pop(ecs_id_array_t *pool, ecs_id_t *out_id);
static int ecs_id_array_size(ecs_id_array_t *pool);

/*=============================================================================
 * Component array functions
 *============================================================================*/
static void ecs_comp_array_init(ecs_t *ecs, ecs_comp_array_t *array, size_t size, size_t capacity);
static void ecs_comp_array_free(ecs_t *ecs, ecs_comp_array_t *array);
static void ecs_comp_array_resize(ecs_t *ecs, ecs_comp_array_t *array, size_t capacity);

/*=============================================================================
 * Validation functions
 *============================================================================*/
#ifndef NDEBUG
static bool ecs_is_not_null(void *ptr);
static bool ecs_is_valid_component_id(ecs_id_t id);
static bool ecs_is_valid_system_id(ecs_id_t id);
static bool ecs_is_entity_ready(ecs_t *ecs, ecs_id_t entity_id);
static bool ecs_is_component_ready(ecs_t *ecs, ecs_id_t comp_id);
static bool ecs_is_system_ready(ecs_t *ecs, ecs_id_t sys_id);
#endif // NDEBUG

/*=============================================================================
 * Public API implementation
 *============================================================================*/

bool ecs_is_invalid_entity(ecs_entity_t entity)
{
    return 0 == entity.id;
}

ecs_entity_t ecs_invalid_entity()
{
    ecs_entity_t invalid = { 0 };
    return invalid;
}

ecs_t *ecs_new(size_t entity_count, void *mem_ctx)
{
    ECS_ASSERT(entity_count > 0);

    ecs_t *ecs = (ecs_t *)ECS_MALLOC(sizeof(ecs_t), mem_ctx);

    if (NULL == ecs) return NULL;

    memset(ecs, 0, sizeof(ecs_t));

    ecs_internal_t *core = (ecs_internal_t *)ECS_MALLOC(sizeof(ecs_internal_t), mem_ctx);

    if (NULL == core) {
        ECS_FREE(ecs, mem_ctx);
        return NULL;
    }

    memset(core, 0, sizeof(ecs_internal_t));

    ecs->core = core;
    ecs->task_id = 0;

    core->entity_count = (entity_count > 0) ? entity_count : 1;
    atomic_init(&core->next_entity_id, 1);
    core->mem_ctx = mem_ctx;
    core->task_count = 0;
    core->cmd_buffers = NULL;

    // Initialize entity pool and queues
    ecs_id_array_init(ecs, &core->entity_pool, core->entity_count);

    // Allocate entity array
    core->entities = (ecs_entity_data_t *)
        ECS_MALLOC(core->entity_count * sizeof(ecs_entity_data_t), core->mem_ctx);

    // Zero entity array
    memset(core->entities, 0, core->entity_count * sizeof(ecs_entity_data_t));

    return ecs;
}

void ecs_free(ecs_t *ecs)
{
    ECS_ASSERT(ecs_is_not_null(ecs));

    ecs_internal_t *core = ECS_CORE(ecs);

    for (ecs_id_t entity_id = 0; entity_id < core->entity_count; entity_id++) {
        if (core->entities[entity_id].active) ecs_destruct(ecs, entity_id);
    }

    ecs_id_array_free(ecs, &core->entity_pool);

    for (ecs_id_t comp_id = 0; comp_id < core->comp_count; comp_id++) {
        ecs_comp_array_t *comp_array = &core->comp_arrays[comp_id];
        ecs_comp_array_free(ecs, comp_array);
    }

    for (ecs_id_t sys_id = 0; sys_id < core->system_count; sys_id++) {
        ecs_sys_data_t *sys = &core->systems[sys_id];
        ecs_sparse_set_free(ecs, &sys->entity_ids);
    }

    if (core->cmd_buffers) {
        for (int i = 0; i < core->task_count; i++) {
            ecs_cmd_buffer_t *buf = &core->cmd_buffers[i];
            for (size_t j = 0; j < buf->count; j++) {
                if (buf->commands[j].type == ECS_CMD_ADD && buf->commands[j].data) {
                    ECS_FREE(buf->commands[j].data, buf->mem_ctx);
                }
            }
            ECS_FREE(buf->commands, core->mem_ctx);
        }
        ECS_FREE(core->cmd_buffers, core->mem_ctx);
    }

    ECS_FREE(core->entities, core->mem_ctx);
    ECS_FREE(core, core->mem_ctx);
    ECS_FREE(ecs, core->mem_ctx);
}

void ecs_reset(ecs_t *ecs)
{
    ECS_ASSERT(ecs_is_not_null(ecs));

    for (ecs_id_t entity_id = 0; entity_id < ECS_CORE(ecs)->entity_count; entity_id++) {
        if (ECS_CORE(ecs)->entities[entity_id].active)
            ecs_destruct(ecs, entity_id);
    }

    atomic_store_explicit(&ECS_CORE(ecs)->entity_pool.size, 0, memory_order_release);

    memset(ECS_CORE(ecs)->entities, 0, ECS_CORE(ecs)->entity_count * sizeof(ecs_entity_data_t));

    atomic_store(&ECS_CORE(ecs)->next_entity_id, 1);

    for (ecs_id_t sys_id = 0; sys_id < ECS_CORE(ecs)->system_count; sys_id++) {
        ECS_CORE(ecs)->systems[sys_id].entity_ids.size = 0;
    }

    if (ECS_CORE(ecs)->cmd_buffers) {
        for (int i = 0; i < ECS_CORE(ecs)->task_count; i++) {
            ecs_cmd_buffer_clear(&ECS_CORE(ecs)->cmd_buffers[i]);
        }
    }
}

void ecs_set_task_callbacks(
    ecs_t *ecs,
    ecs_enqueue_task_fn enqueue_cb,
    ecs_wait_tasks_fn wait_cb,
    void *task_udata,
    int task_count
)
{
    ECS_ASSERT(ecs_is_not_null(ecs));
    ECS_ASSERT(task_count >= 0);
    ECS_ASSERT(task_count <= PICO_ECS_MT_MAX_TASKS);

    ecs_internal_t *core = ECS_CORE(ecs);

    if (core->cmd_buffers) {
        for (int i = 0; i < core->task_count; i++) {
            ecs_cmd_buffer_clear(&core->cmd_buffers[i]);
            ECS_FREE(core->cmd_buffers[i].commands, core->mem_ctx);
        }
        ECS_FREE(core->cmd_buffers, core->mem_ctx);
        core->cmd_buffers = NULL;
    }

    core->enqueue_cb = enqueue_cb;
    core->wait_cb = wait_cb;
    core->task_udata = task_udata;
    core->task_count = (enqueue_cb && wait_cb && task_count > 0) ? task_count : 0;

    if (core->task_count == 0) return;

    core->cmd_buffers = (ecs_cmd_buffer_t *)
        ECS_MALLOC(sizeof(ecs_cmd_buffer_t) * core->task_count, core->mem_ctx);

    ECS_ASSERT(core->cmd_buffers != NULL);

    memset(core->cmd_buffers, 0, sizeof(ecs_cmd_buffer_t) * core->task_count);

    for (int i = 0; i < core->task_count; i++) {
        ecs_cmd_buffer_t *buf = &core->cmd_buffers[i];
        buf->mem_ctx = core->mem_ctx;
        buf->capacity = PICO_ECS_MT_CMD_BUFFER_INITIAL_SIZE;
        buf->commands = (ecs_cmd_t *)
            ECS_MALLOC(sizeof(ecs_cmd_t) * buf->capacity, core->mem_ctx);
        ECS_ASSERT(buf->commands != NULL);
    }
}

ecs_comp_t ecs_define_component(ecs_t *ecs, size_t size, ecs_constructor_fn constructor, ecs_destructor_fn destructor)
{
    ECS_ASSERT(ecs_is_not_null(ecs));
    ECS_ASSERT(ECS_CORE(ecs)->comp_count < ECS_MAX_COMPONENTS);
    ECS_ASSERT(size > 0);

    ecs_comp_t comp = ecs_make_comp(ECS_CORE(ecs)->comp_count);

    ecs_comp_array_t *comp_array = &ECS_CORE(ecs)->comp_arrays[comp.id];
    ecs_comp_array_init(ecs, comp_array, size, ECS_CORE(ecs)->entity_count);

    ECS_CORE(ecs)->comps[comp.id].constructor = constructor;
    ECS_CORE(ecs)->comps[comp.id].destructor = destructor;

    ECS_CORE(ecs)->comp_count++;

    return comp;
}

ecs_system_t ecs_define_system(
    ecs_t *ecs,
    ecs_mask_t mask,
    ecs_system_fn system_cb,
    ecs_added_fn add_cb,
    ecs_removed_fn remove_cb,
    void *udata
)
{
    ECS_ASSERT(ecs_is_not_null(ecs));
    ECS_ASSERT(ECS_CORE(ecs)->system_count < ECS_MAX_SYSTEMS);
    ECS_ASSERT(NULL != system_cb);

    ecs_system_t sys = ecs_make_system(ECS_CORE(ecs)->system_count);
    ecs_sys_data_t *sys_data = &ECS_CORE(ecs)->systems[sys.id];

    ecs_sparse_set_init(ecs, &sys_data->entity_ids, ECS_CORE(ecs)->entity_count);

    sys_data->active = true;
    sys_data->mask = mask;
    sys_data->system_cb = system_cb;
    sys_data->add_cb = add_cb;
    sys_data->remove_cb = remove_cb;
    sys_data->udata = udata;

    ECS_CORE(ecs)->system_count++;

    return sys;
}

void ecs_require_component(ecs_t *ecs, ecs_system_t sys, ecs_comp_t comp)
{
    ECS_ASSERT(ecs_is_not_null(ecs));
    ECS_ASSERT(ecs_is_valid_system_id(sys.id));
    ECS_ASSERT(ecs_is_valid_component_id(comp.id));
    ECS_ASSERT(ecs_is_system_ready(ecs, sys.id));
    ECS_ASSERT(ecs_is_component_ready(ecs, comp.id));

    // Set system component bit for the specified component
    ecs_sys_data_t *sys_data = &ECS_CORE(ecs)->systems[sys.id];
    ecs_bitset_flip(&sys_data->require_bits, comp.id, true);
}

void ecs_exclude_component(ecs_t *ecs, ecs_system_t sys, ecs_comp_t comp)
{
    ECS_ASSERT(ecs_is_not_null(ecs));
    ECS_ASSERT(ecs_is_valid_system_id(sys.id));
    ECS_ASSERT(ecs_is_valid_component_id(comp.id));
    ECS_ASSERT(ecs_is_system_ready(ecs, sys.id));
    ECS_ASSERT(ecs_is_component_ready(ecs, comp.id));

    // Set system component bit for the specified component
    ecs_sys_data_t *sys_data = &ECS_CORE(ecs)->systems[sys.id];
    ecs_bitset_flip(&sys_data->exclude_bits, comp.id, true);
}

void ecs_enable_system(ecs_t *ecs, ecs_system_t sys)
{
    ECS_ASSERT(ecs_is_not_null(ecs));
    ECS_ASSERT(ecs_is_valid_system_id(sys.id));
    ECS_ASSERT(ecs_is_system_ready(ecs, sys.id));

    ecs_sys_data_t *sys_data = &ECS_CORE(ecs)->systems[sys.id];
    sys_data->active = true;
}

void ecs_disable_system(ecs_t *ecs, ecs_system_t sys)
{
    ECS_ASSERT(ecs_is_not_null(ecs));
    ECS_ASSERT(ecs_is_valid_system_id(sys.id));
    ECS_ASSERT(ecs_is_system_ready(ecs, sys.id));

    ecs_sys_data_t *sys_data = &ECS_CORE(ecs)->systems[sys.id];
    sys_data->active = false;
}

void ecs_set_system_callbacks(
    ecs_t *ecs,
    ecs_system_t sys,
    ecs_system_fn system_cb,
    ecs_added_fn add_cb,
    ecs_removed_fn remove_cb
)
{
    ECS_ASSERT(ecs_is_not_null(ecs));
    ECS_ASSERT(ecs_is_valid_system_id(sys.id));
    ECS_ASSERT(ecs_is_system_ready(ecs, sys.id));
    ECS_ASSERT(NULL != system_cb);

    ecs_sys_data_t *sys_data = &ECS_CORE(ecs)->systems[sys.id];
    sys_data->system_cb = system_cb;
    sys_data->add_cb = add_cb;
    sys_data->remove_cb = remove_cb;
}

void ecs_set_system_udata(ecs_t *ecs, ecs_system_t sys, void *udata)
{
    ECS_ASSERT(ecs_is_not_null(ecs));
    ECS_ASSERT(ecs_is_valid_system_id(sys.id));
    ECS_ASSERT(ecs_is_system_ready(ecs, sys.id));

    ECS_CORE(ecs)->systems[sys.id].udata = udata;
}

void *ecs_get_system_udata(ecs_t *ecs, ecs_system_t sys)
{
    ECS_ASSERT(ecs_is_not_null(ecs));
    ECS_ASSERT(ecs_is_valid_system_id(sys.id));
    ECS_ASSERT(ecs_is_system_ready(ecs, sys.id));

    return ECS_CORE(ecs)->systems[sys.id].udata;
}

void ecs_set_system_mask(ecs_t *ecs, ecs_system_t sys, ecs_mask_t mask)
{
    ECS_ASSERT(ecs_is_not_null(ecs));
    ECS_ASSERT(ecs_is_valid_system_id(sys.id));
    ECS_ASSERT(ecs_is_system_ready(ecs, sys.id));

    ECS_CORE(ecs)->systems[sys.id].mask = mask;
}

ecs_mask_t ecs_get_system_mask(ecs_t *ecs, ecs_system_t sys)
{
    ECS_ASSERT(ecs_is_not_null(ecs));
    ECS_ASSERT(ecs_is_valid_system_id(sys.id));
    ECS_ASSERT(ecs_is_system_ready(ecs, sys.id));

    return ECS_CORE(ecs)->systems[sys.id].mask;
}

size_t ecs_get_system_entity_count(ecs_t *ecs, ecs_system_t sys)
{
    return ECS_CORE(ecs)->systems[sys.id].entity_ids.size;
}

ecs_entity_t ecs_create(ecs_t *ecs)
{
    ECS_ASSERT(ecs_is_not_null(ecs));

    ecs_internal_t *core = ECS_CORE(ecs);
    ecs_id_t entity_id = 0;

    if (ecs_is_task_view(ecs)) {
        if (!ecs_id_array_try_pop(&core->entity_pool, &entity_id)) {
            entity_id = atomic_fetch_add_explicit(&core->next_entity_id, 1, memory_order_relaxed);
        }

        ecs_entity_t entity = ecs_make_entity(entity_id);
        ecs_cmd_buffer_t *buf = &core->cmd_buffers[ecs->task_id - 1];

        ecs_cmd_buffer_ensure_capacity(buf);

        ecs_cmd_t *cmd = &buf->commands[buf->count++];
        cmd->type = ECS_CMD_CREATE;
        cmd->entity = entity;
        cmd->component = (ecs_comp_t){ 0 };
        cmd->data = NULL;
        cmd->data_size = 0;

        return entity;
    }

    // If there is an ID in the pool, pop it
    if (!ecs_id_array_try_pop(&core->entity_pool, &entity_id)) {
        // Otherwise, issue a fresh ID
        entity_id = atomic_fetch_add_explicit(&core->next_entity_id, 1, memory_order_relaxed);

        ecs_ensure_entity_capacity(ecs, entity_id);
    }

    // Activate the entity and return a handle
    core->entities[entity_id].active = true;
    core->entities[entity_id].ready = true;

    return ecs_make_entity(entity_id);
}

bool ecs_is_ready(ecs_t *ecs, ecs_entity_t entity)
{
    ECS_ASSERT(ecs_is_not_null(ecs));

    return ECS_CORE(ecs)->entities[entity.id].ready;
}

void ecs_destroy(ecs_t *ecs, ecs_entity_t entity)
{
    ECS_ASSERT(ecs_is_not_null(ecs));

    if (ecs_is_task_view(ecs)) {
        ecs_internal_t *core = ECS_CORE(ecs);
        ecs_cmd_buffer_t *buf = &core->cmd_buffers[ecs->task_id - 1];

        ecs_cmd_buffer_ensure_capacity(buf);

        ecs_cmd_t *cmd = &buf->commands[buf->count++];
        cmd->type = ECS_CMD_DESTROY;
        cmd->entity = entity;
        cmd->component = ecs_make_comp(0);
        cmd->data = NULL;
        cmd->data_size = 0;
        return;
    }

    ECS_ASSERT(ecs_is_active(ecs, entity.id));

    // Load entity data
    ecs_entity_data_t *entity_data = &ECS_CORE(ecs)->entities[entity.id];

    // Remove entity from systems
    if (ecs_is_ready(ecs, entity)) { ecs_remove_from_systems(ecs, entity); }

    // Call destructors on entity components
    ecs_destruct(ecs, entity.id);

    // Push entity ID into pool
    ecs_id_array_t *pool = &ECS_CORE(ecs)->entity_pool;
    ecs_id_array_push(ecs, pool, entity.id);

    // Reset entity (sets bitset to 0 and, active and ready to false)
    memset(entity_data, 0, sizeof(ecs_entity_data_t));
}

bool ecs_has(ecs_t *ecs, ecs_entity_t entity, ecs_comp_t comp)
{
    ECS_ASSERT(ecs_is_not_null(ecs));
    ECS_ASSERT(ecs_is_valid_component_id(comp.id));
    ECS_ASSERT(ecs_is_entity_ready(ecs, entity.id));

    // Load entity data
    ecs_entity_data_t *entity_data = &ECS_CORE(ecs)->entities[entity.id];

    // Return true if the component belongs to the entity
    return ecs_bitset_test(&entity_data->comp_bits, comp.id);
}

void *ecs_get(ecs_t *ecs, ecs_entity_t entity, ecs_comp_t comp)
{
    ECS_ASSERT(ecs_is_not_null(ecs));
    ECS_ASSERT(ecs_is_valid_component_id(comp.id));
    ECS_ASSERT(ecs_is_component_ready(ecs, comp.id));
    ECS_ASSERT(ecs_is_entity_ready(ecs, entity.id));

    // Return pointer to component
    //  eid0,  eid1   eid2, ...
    // [comp0, comp1, comp2, ...]
    ecs_comp_array_t *comp_array = &ECS_CORE(ecs)->comp_arrays[comp.id];
    return (char *)comp_array->data + (comp_array->size * entity.id);
}

void *ecs_add(ecs_t *ecs, ecs_entity_t entity, ecs_comp_t comp, void *args)
{
    ECS_ASSERT(ecs_is_not_null(ecs));
    ECS_ASSERT(ecs_is_valid_component_id(comp.id));
    ECS_ASSERT(ecs_is_component_ready(ecs, comp.id));

    if (ecs_is_task_view(ecs)) {
        ecs_internal_t *core = ECS_CORE(ecs);
        ecs_cmd_buffer_t *buf = &core->cmd_buffers[ecs->task_id - 1];

        ecs_cmd_buffer_ensure_capacity(buf);

        ecs_cmd_t *cmd = &buf->commands[buf->count++];
        cmd->type = ECS_CMD_ADD;
        cmd->entity = entity;
        cmd->component = comp;
        cmd->data_size = core->comp_arrays[comp.id].size;
        cmd->data = NULL;

        if (cmd->data_size > 0) {
            cmd->data = ECS_MALLOC(cmd->data_size, core->mem_ctx);
            ECS_ASSERT(cmd->data != NULL);
            memset(cmd->data, 0, cmd->data_size);

            ecs_comp_data_t *comp_data = &core->comps[comp.id];

            if (comp_data->constructor) {
                comp_data->constructor(ecs, entity, cmd->data, args);
            } else if (args) {
                memcpy(cmd->data, args, cmd->data_size);
            }
        }

        return cmd->data;
    }

    ECS_ASSERT(ecs_is_entity_ready(ecs, entity.id));

    return ecs_add_internal(ecs, entity, comp, args, true);
}

static void *ecs_add_internal(ecs_t *ecs, ecs_entity_t entity, ecs_comp_t comp, void *args, bool run_constructor)
{
    ECS_ASSERT(ecs_is_not_null(ecs));
    ECS_ASSERT(ecs_is_valid_component_id(comp.id));
    ECS_ASSERT(ecs_is_component_ready(ecs, comp.id));
    ECS_ASSERT(ecs_is_entity_ready(ecs, entity.id));

    // Load entity data
    ecs_entity_data_t *entity_data = &ECS_CORE(ecs)->entities[entity.id];

    // Load component
    ecs_comp_array_t *comp_array = &ECS_CORE(ecs)->comp_arrays[comp.id];
    ecs_comp_data_t *comp_data = &ECS_CORE(ecs)->comps[comp.id];

    // Grow the component array
    ecs_comp_array_resize(ecs, comp_array, entity.id);

    // Get pointer to component
    void *comp_ptr = ecs_get(ecs, entity, comp);

    // Zero component
    memset(comp_ptr, 0, comp_array->size);

    if (run_constructor) {
        if (comp_data->constructor)
            comp_data->constructor(ecs, entity, comp_ptr, args);
    } else if (args) {
        memcpy(comp_ptr, args, comp_array->size);
    }

    // Set entity component bit that determines which systems this entity
    // belongs to
    ecs_bitset_flip(&entity_data->comp_bits, comp.id, true);

    // Add or remove entity from systems
    for (ecs_id_t sys_id = 0; sys_id < ECS_CORE(ecs)->system_count; sys_id++) {
        ecs_sys_data_t *sys_data = &ECS_CORE(ecs)->systems[sys_id];

        if (ecs_entity_system_test(
                &sys_data->require_bits,
                &sys_data->exclude_bits,
                &entity_data->comp_bits
            )) {
            if (ecs_sparse_set_add(ecs, &sys_data->entity_ids, entity.id)) {
                if (sys_data->add_cb)
                    sys_data->add_cb(ecs, entity, sys_data->udata);
            }
        } else // Just remove the entity if its components no longer match for whatever reason.
        {
            if (!ecs_bitset_is_zero(&sys_data->exclude_bits) &&
                ecs_sparse_set_remove(&sys_data->entity_ids, entity.id)) {
                if (sys_data->remove_cb)
                    sys_data->remove_cb(ecs, entity, sys_data->udata);
            }
        }
    }

    // Return component
    return comp_ptr;
}

void ecs_remove(ecs_t *ecs, ecs_entity_t entity, ecs_comp_t comp)
{
    ECS_ASSERT(ecs_is_not_null(ecs));
    ECS_ASSERT(ecs_is_valid_component_id(comp.id));
    ECS_ASSERT(ecs_is_component_ready(ecs, comp.id));

    if (ecs_is_task_view(ecs)) {
        ecs_internal_t *core = ECS_CORE(ecs);
        ecs_cmd_buffer_t *buf = &core->cmd_buffers[ecs->task_id - 1];

        ecs_cmd_buffer_ensure_capacity(buf);

        ecs_cmd_t *cmd = &buf->commands[buf->count++];
        cmd->type = ECS_CMD_REMOVE;
        cmd->entity = entity;
        cmd->component = comp;
        cmd->data = NULL;
        cmd->data_size = 0;
        return;
    }

    ECS_ASSERT(ecs_is_entity_ready(ecs, entity.id));

    // Load entity data
    ecs_entity_data_t *entity_data = &ECS_CORE(ecs)->entities[entity.id];

    // Create bit mask with comp bit flipped on
    ecs_bitset_t comp_bit;

    memset(&comp_bit, 0, sizeof(ecs_bitset_t));
    ecs_bitset_flip(&comp_bit, comp.id, true);

    for (ecs_id_t sys_id = 0; sys_id < ECS_CORE(ecs)->system_count; sys_id++) {
        ecs_sys_data_t *sys_data = &ECS_CORE(ecs)->systems[sys_id];

        if (ecs_entity_system_test(&sys_data->require_bits, &sys_data->exclude_bits, &comp_bit)) {
            if (ecs_sparse_set_remove(&sys_data->entity_ids, entity.id)) {
                if (sys_data->remove_cb)
                    sys_data->remove_cb(ecs, entity, sys_data->udata);
            }
        } else {
            if (!ecs_bitset_is_zero(&sys_data->exclude_bits) &&
                ecs_sparse_set_add(ecs, &sys_data->entity_ids, entity.id)) {
                if (sys_data->add_cb)
                    sys_data->add_cb(ecs, entity, sys_data->udata);
            }
        }
    }

    ecs_comp_data_t *comp_data = &ECS_CORE(ecs)->comps[comp.id];

    if (comp_data->destructor) {
        void *comp_ptr = ecs_get(ecs, entity, comp);
        comp_data->destructor(ecs, entity, comp_ptr);
    }

    // Reset the relevant component mask bit
    ecs_bitset_flip(&entity_data->comp_bits, comp.id, false);
}

static int ecs_task_entry(void *args)
{
    ecs_task_ctx_t *ctx = (ecs_task_ctx_t *)args;
    ctx->result = ctx->system_cb(ctx->ecs_view, ctx->entities, ctx->entity_count, ctx->udata);
    return 0;
}

ecs_ret_t ecs_run_system(ecs_t *ecs, ecs_system_t sys, ecs_mask_t mask)
{
    ECS_ASSERT(ecs_is_not_null(ecs));
    ECS_ASSERT(ecs_is_valid_system_id(sys.id));
    ECS_ASSERT(ecs_is_system_ready(ecs, sys.id));
    ECS_ASSERT(!ecs_is_task_view(ecs));

    ecs_internal_t *core = ECS_CORE(ecs);
    ecs_sys_data_t *sys_data = &core->systems[sys.id];

    if (!sys_data->active) return 0;

    if (0 != sys_data->mask && !(sys_data->mask & mask)) return 0;

    if (0 == sys_data->entity_ids.size) return 0;

    if (core->task_count > 0 && core->enqueue_cb && core->wait_cb && core->cmd_buffers) {
        size_t total = sys_data->entity_ids.size;
        size_t per_task = (total + (size_t)core->task_count - 1) / (size_t)core->task_count;
        ecs_task_ctx_t task_ctxs[PICO_ECS_MT_MAX_TASKS];
        int task_count = 0;

        for (size_t start = 0; start < total; start += per_task) {
            size_t count = per_task;
            if (start + count > total) count = total - start;

            ecs_task_ctx_t *ctx = &task_ctxs[task_count];
            *ctx = (ecs_task_ctx_t){ sys_data->system_cb,
                                     &(ecs_t){ .core = core, .task_id = task_count + 1 },
                                     &sys_data->entity_ids.dense[start],
                                     count,
                                     sys_data->udata,
                                     0 };

            core->enqueue_cb(ecs_task_entry, ctx, core->task_udata);
            task_count++;
        }

        core->wait_cb(core->task_udata);

        ecs_flush_commands(ecs);

        ecs_ret_t code = 0;
        for (int i = 0; i < task_count; i++) {
            if (task_ctxs[i].result != 0) {
                code = task_ctxs[i].result;
                break;
            }
        }

        return code;
    }

    ecs_ret_t code = sys_data->system_cb(
        ecs,
        sys_data->entity_ids.dense,
        sys_data->entity_ids.size,
        sys_data->udata
    );

    return code;
}

ecs_ret_t ecs_run_systems(ecs_t *ecs, ecs_mask_t mask)
{
    ECS_ASSERT(ecs_is_not_null(ecs));

    for (ecs_id_t sys_id = 0; sys_id < ECS_CORE(ecs)->system_count; sys_id++) {
        ecs_system_t sys = ecs_make_system(sys_id);
        ecs_ret_t code = ecs_run_system(ecs, sys, mask);

        if (0 != code) return code;
    }

    return 0;
}

/*=============================================================================
 * Handle constructors
 *============================================================================*/
static inline ecs_entity_t ecs_make_entity(ecs_id_t id)
{
    ecs_entity_t entity = { id };
    return entity;
}

static inline ecs_comp_t ecs_make_comp(ecs_id_t id)
{
    ecs_comp_t comp = { id };
    return comp;
}

static inline ecs_system_t ecs_make_system(ecs_id_t id)
{
    ecs_system_t sys = { id };
    return sys;
}

static inline ecs_t ecs_make_view(ecs_internal_t *core, uint32_t task_id)
{
    ecs_t view;
    view.core = core;
    view.task_id = task_id;
    return view;
}

static inline bool ecs_is_task_view(ecs_t *ecs)
{
    return ecs->task_id != 0;
}

/*=============================================================================
 * Realloc wrapper
 *============================================================================*/
static void *ecs_realloc_zero(ecs_t *ecs, void *ptr, size_t old_size, size_t new_size)
{
    (void)ecs;

    ptr = ECS_REALLOC(ptr, new_size, ECS_CORE(ecs)->mem_ctx);

    if (new_size > old_size && ptr) {
        size_t diff = new_size - old_size;
        void *start = ((char *)ptr) + old_size;
        memset(start, 0, diff);
    }

    return ptr;
}

static void ecs_ensure_entity_capacity(ecs_t *ecs, ecs_id_t entity_id)
{
    if (entity_id < ECS_CORE(ecs)->entity_count) return;

    size_t old_count = ECS_CORE(ecs)->entity_count;
    size_t new_count = (old_count > 0) ? old_count : 1;

    while (entity_id >= new_count) { new_count *= 2; }

    ECS_CORE(ecs)->entities = (ecs_entity_data_t *)ecs_realloc_zero(
        ecs,
        ECS_CORE(ecs)->entities,
        old_count * sizeof(ecs_entity_data_t),
        new_count * sizeof(ecs_entity_data_t)
    );

    ECS_CORE(ecs)->entity_count = new_count;
}

static void ecs_cmd_buffer_ensure_capacity(ecs_cmd_buffer_t *buf)
{
    if (buf->count < buf->capacity) return;

    size_t new_capacity = buf->capacity ? buf->capacity * 2
                                        : PICO_ECS_MT_CMD_BUFFER_INITIAL_SIZE;
    buf->commands = (ecs_cmd_t *)
        ECS_REALLOC(buf->commands, new_capacity * sizeof(ecs_cmd_t), buf->mem_ctx);
    ECS_ASSERT(buf->commands != NULL);
    buf->capacity = new_capacity;
}

static void ecs_cmd_buffer_clear(ecs_cmd_buffer_t *buf)
{
    for (size_t i = 0; i < buf->count; i++) {
        if (buf->commands[i].type == ECS_CMD_ADD && buf->commands[i].data) {
            ECS_FREE(buf->commands[i].data, buf->mem_ctx);
        }
    }

    buf->count = 0;
}

static void ecs_flush_commands(ecs_t *ecs)
{
    ecs_internal_t *core = ECS_CORE(ecs);

    if (!core->cmd_buffers || core->task_count == 0) return;

    for (int i = 0; i < core->task_count; i++) {
        ecs_cmd_buffer_t *buf = &core->cmd_buffers[i];

        for (size_t j = 0; j < buf->count; j++) {
            ecs_cmd_t *cmd = &buf->commands[j];

            ecs_ensure_entity_capacity(ecs, cmd->entity.id);

            if (cmd->type != ECS_CMD_CREATE) continue;

            ecs_entity_data_t *entity_data = &core->entities[cmd->entity.id];
            entity_data->active = true;
            entity_data->ready = true;
        }
    }

    for (int i = 0; i < core->task_count; i++) {
        ecs_cmd_buffer_t *buf = &core->cmd_buffers[i];

        for (size_t j = 0; j < buf->count; j++) {
            ecs_cmd_t *cmd = &buf->commands[j];

            switch (cmd->type) {
                case ECS_CMD_CREATE: break;
                case ECS_CMD_ADD: {
                    ecs_add_internal(ecs, cmd->entity, cmd->component, cmd->data, false);
                    break;
                }
                case ECS_CMD_REMOVE: {
                    ecs_remove(ecs, cmd->entity, cmd->component);
                    break;
                }
                case ECS_CMD_DESTROY: {
                    ecs_destroy(ecs, cmd->entity);
                    break;
                }
            }
        }
    }

    for (int i = 0; i < core->task_count; i++) {
        ecs_cmd_buffer_clear(&core->cmd_buffers[i]);
    }
}

/*=============================================================================
 * Tests if entity is active (created)
 *============================================================================*/
static inline bool ecs_is_active(ecs_t *ecs, ecs_id_t entity_id)
{
    ECS_ASSERT(ecs_is_not_null(ecs));
    return ECS_CORE(ecs)->entities[entity_id].active;
}

/*=============================================================================
 * Calls destructors on all components of the entity
 *============================================================================*/
static void ecs_destruct(ecs_t *ecs, ecs_id_t entity_id)
{
    // Load entity
    ecs_entity_data_t *entity = &ECS_CORE(ecs)->entities[entity_id];

    // Loop through components and call the destructors
    for (ecs_id_t comp_id = 0; comp_id < ECS_CORE(ecs)->comp_count; comp_id++) {
        if (ecs_bitset_test(&entity->comp_bits, comp_id)) {
            ecs_comp_data_t *comp = &ECS_CORE(ecs)->comps[comp_id];

            if (comp->destructor) {
                // Get component pointer directly without ecs_get to avoid
                // ready assertion, since entity may be queued for destruction
                ecs_comp_array_t *comp_array = &ECS_CORE(ecs)->comp_arrays[comp_id];
                void *comp_ptr = (char *)comp_array->data +
                                 (comp_array->size * entity_id);
                ecs_entity_t entity = ecs_make_entity(entity_id);

                comp->destructor(ecs, entity, comp_ptr);
            }
        }
    }
}

/*=============================================================================
 * Removes entity from ALL systems
 *============================================================================*/
static void ecs_remove_from_systems(ecs_t *ecs, ecs_entity_t entity)
{
    // Load entity
    ecs_entity_data_t *entity_data = &ECS_CORE(ecs)->entities[entity.id];

    for (ecs_id_t sys_id = 0; sys_id < ECS_CORE(ecs)->system_count; sys_id++) {
        ecs_sys_data_t *sys_data = &ECS_CORE(ecs)->systems[sys_id];

        if (ecs_entity_system_test(
                &sys_data->require_bits,
                &sys_data->exclude_bits,
                &entity_data->comp_bits
            ) &&
            ecs_sparse_set_remove(&sys_data->entity_ids, entity.id)) {
            if (sys_data->remove_cb)
                sys_data->remove_cb(ecs, entity, sys_data->udata);
        }
    }
}

/*=============================================================================
 * Bitset functions
 *============================================================================*/

#if ECS_MAX_COMPONENTS <= 64

static inline bool ecs_bitset_is_zero(ecs_bitset_t *set)
{
    return *set == 0;
}

static inline void ecs_bitset_flip(ecs_bitset_t *set, int bit, bool on)
{
    if (on) *set |= ((uint64_t)1 << bit);
    else *set &= ~((uint64_t)1 << bit);
}

static inline bool ecs_bitset_test(ecs_bitset_t *set, int bit)
{
    return *set & ((uint64_t)1 << bit);
}

static inline ecs_bitset_t ecs_bitset_and(ecs_bitset_t *set1, ecs_bitset_t *set2)
{
    return *set1 & *set2;
}

static inline ecs_bitset_t ecs_bitset_or(ecs_bitset_t *set1, ecs_bitset_t *set2)
{
    return *set1 | *set2;
}

static inline ecs_bitset_t ecs_bitset_not(ecs_bitset_t *set)
{
    return ~(*set);
}

static inline bool ecs_bitset_equal(ecs_bitset_t *set1, ecs_bitset_t *set2)
{
    return *set1 == *set2;
}

static inline bool ecs_bitset_true(ecs_bitset_t *set)
{
    return *set;
}

#else // ECS_MAX_COMPONENTS

static inline bool ecs_bitset_is_zero(ecs_bitset_t *set)
{
    for (int i = 0; i < ECS_BITSET_SIZE; i++) {
        if (set->array[i] != 0) return false;
    }

    return true;
}

static inline void ecs_bitset_flip(ecs_bitset_t *set, int bit, bool on)
{
    int index = bit / ECS_BITSET_WIDTH;

    if (on) set->array[index] |= ((uint64_t)1 << bit % ECS_BITSET_WIDTH);
    else set->array[index] &= ~((uint64_t)1 << bit % ECS_BITSET_WIDTH);
}

static inline bool ecs_bitset_test(ecs_bitset_t *set, int bit)
{
    int index = bit / ECS_BITSET_WIDTH;
    return set->array[index] & ((uint64_t)1 << bit % ECS_BITSET_WIDTH);
}

static inline ecs_bitset_t ecs_bitset_and(ecs_bitset_t *set1, ecs_bitset_t *set2)
{
    ecs_bitset_t set;

    for (int i = 0; i < ECS_BITSET_SIZE; i++) {
        set.array[i] = set1->array[i] & set2->array[i];
    }

    return set;
}

static inline ecs_bitset_t ecs_bitset_or(ecs_bitset_t *set1, ecs_bitset_t *set2)
{
    ecs_bitset_t set;

    for (int i = 0; i < ECS_BITSET_SIZE; i++) {
        set.array[i] = set1->array[i] | set2->array[i];
    }

    return set;
}

static inline ecs_bitset_t ecs_bitset_not(ecs_bitset_t *set)
{
    ecs_bitset_t out;

    for (int i = 0; i < ECS_BITSET_SIZE; i++) { out.array[i] = ~set->array[i]; }

    return out;
}

static inline bool ecs_bitset_equal(ecs_bitset_t *set1, ecs_bitset_t *set2)
{
    for (int i = 0; i < ECS_BITSET_SIZE; i++) {
        if (set1->array[i] != set2->array[i]) { return false; }
    }

    return true;
}

static inline bool ecs_bitset_true(ecs_bitset_t *set)
{
    for (int i = 0; i < ECS_BITSET_SIZE; i++) {
        if (set->array[i]) return true;
    }

    return false;
}

#endif // ECS_MAX_COMPONENTS

/*=============================================================================
 * Sparse set functions
 *============================================================================*/

static void ecs_sparse_set_init(ecs_t *ecs, ecs_sparse_set_t *set, size_t capacity)
{
    ECS_ASSERT(ecs_is_not_null(ecs));
    ECS_ASSERT(ecs_is_not_null(set));
    ECS_ASSERT(capacity > 0);

    (void)ecs;

    set->capacity = capacity;
    set->size = 0;

    set->dense = (ecs_entity_t *)
        ECS_MALLOC(capacity * sizeof(ecs_id_t), ECS_CORE(ecs)->mem_ctx);
    set->sparse = (size_t *)ECS_MALLOC(capacity * sizeof(size_t), ECS_CORE(ecs)->mem_ctx);

    memset(set->sparse, 0, capacity * sizeof(size_t));
}

static void ecs_sparse_set_free(ecs_t *ecs, ecs_sparse_set_t *set)
{
    ECS_ASSERT(ecs_is_not_null(ecs));
    ECS_ASSERT(ecs_is_not_null(set));

    (void)ecs;

    ECS_FREE(set->dense, ECS_CORE(ecs)->mem_ctx);
    ECS_FREE(set->sparse, ECS_CORE(ecs)->mem_ctx);
}

static bool ecs_sparse_set_add(ecs_t *ecs, ecs_sparse_set_t *set, ecs_id_t id)
{
    ECS_ASSERT(ecs_is_not_null(ecs));
    ECS_ASSERT(ecs_is_not_null(set));

    (void)ecs;

    // Grow sparse set if necessary
    if (id >= set->capacity) {
        size_t old_capacity = set->capacity;
        size_t new_capacity = old_capacity;

        // Calculate new capacity
        new_capacity *= 2;

        // Grow dense array
        set->dense = (ecs_entity_t *)ECS_REALLOC(
            set->dense,
            new_capacity * sizeof(ecs_id_t),
            ECS_CORE(ecs)->mem_ctx
        );

        // Grow sparse array and zero it
        set->sparse = (size_t *)ecs_realloc_zero(
            ecs,
            set->sparse,
            old_capacity * sizeof(size_t),
            new_capacity * sizeof(size_t)
        );

        // Set the new capacity
        set->capacity = new_capacity;
    }

    // Check if ID exists within the set
    if (ecs_sparse_set_find(set, id, NULL)) return false;

    // Add ID to set
    set->dense[set->size].id = id;
    set->sparse[id] = set->size;

    set->size++;

    return true;
}

static bool ecs_sparse_set_find(ecs_sparse_set_t *set, ecs_id_t id, size_t *found)
{
    ECS_ASSERT(ecs_is_not_null(set));

    if (set->sparse[id] < set->size && set->dense[set->sparse[id]].id == id) {
        if (found) *found = set->sparse[id];
        return true;
    } else {
        if (found) *found = 0;
        return false;
    }
}

static bool ecs_sparse_set_remove(ecs_sparse_set_t *set, ecs_id_t id)
{
    ECS_ASSERT(ecs_is_not_null(set));

    if (!ecs_sparse_set_find(set, id, NULL)) return false;

    // Swap and remove (changes order of array)
    ecs_id_t tmp = set->dense[set->size - 1].id;
    set->dense[set->sparse[id]].id = tmp;
    set->sparse[tmp] = set->sparse[id];

    set->size--;

    return true;
}

/*=============================================================================
 * System entity add/remove functions
 *============================================================================*/

inline static bool ecs_entity_system_test(
    ecs_bitset_t *require_bits,
    ecs_bitset_t *exclude_bits,
    ecs_bitset_t *entity_bits
)
{
    if (!ecs_bitset_is_zero(exclude_bits)) {
        ecs_bitset_t overlap = ecs_bitset_and(entity_bits, exclude_bits);

        if (ecs_bitset_true(&overlap)) { return false; }
    }

    ecs_bitset_t entity_and_require = ecs_bitset_and(entity_bits, require_bits);
    return ecs_bitset_equal(&entity_and_require, require_bits);
}

/*=============================================================================
 * ID array functions
 *============================================================================*/

inline static void ecs_id_array_init(ecs_t *ecs, ecs_id_array_t *array, int capacity)
{
    ECS_ASSERT(ecs_is_not_null(ecs));
    ECS_ASSERT(ecs_is_not_null(array));
    ECS_ASSERT(capacity > 0);

    (void)ecs;

    atomic_init(&array->size, 0);
    array->capacity = capacity;
    array->data = (ecs_id_t *)
        ECS_MALLOC(capacity * sizeof(ecs_id_t), ECS_CORE(ecs)->mem_ctx);
}

inline static void ecs_id_array_free(ecs_t *ecs, ecs_id_array_t *array)
{
    ECS_ASSERT(ecs_is_not_null(ecs));
    ECS_ASSERT(ecs_is_not_null(array));

    (void)ecs;

    ECS_FREE(array->data, ECS_CORE(ecs)->mem_ctx);
}

inline static void ecs_id_array_push(ecs_t *ecs, ecs_id_array_t *array, ecs_id_t id)
{
    ECS_ASSERT(ecs_is_not_null(ecs));
    ECS_ASSERT(ecs_is_not_null(array));
    ECS_ASSERT(array->capacity > 0);

    (void)ecs;

    size_t size = atomic_load_explicit(&array->size, memory_order_relaxed);

    if (size == array->capacity) {
        array->capacity *= 2;
        array->data = (ecs_id_t *)ECS_REALLOC(
            array->data,
            array->capacity * sizeof(ecs_id_t),
            ECS_CORE(ecs)->mem_ctx
        );
    }

    array->data[size] = id;
    atomic_store_explicit(&array->size, size + 1, memory_order_release);
}

inline static ecs_id_t ecs_id_array_pop(ecs_id_array_t *array)
{
    ECS_ASSERT(ecs_is_not_null(array));
    size_t size = atomic_load_explicit(&array->size, memory_order_acquire);
    ECS_ASSERT(size > 0);
    size_t new_size = size - 1;
    atomic_store_explicit(&array->size, new_size, memory_order_release);
    return array->data[new_size];
}

static bool ecs_id_array_try_pop(ecs_id_array_t *array, ecs_id_t *out_id)
{
    ECS_ASSERT(ecs_is_not_null(array));
    ECS_ASSERT(ecs_is_not_null(out_id));

    size_t size = atomic_load_explicit(&array->size, memory_order_acquire);

    while (size > 0) {
        if (atomic_compare_exchange_weak_explicit(&array->size, &size, size - 1, memory_order_acq_rel, memory_order_acquire)) {
            *out_id = array->data[size - 1];
            return true;
        }
    }

    return false;
}

inline static int ecs_id_array_size(ecs_id_array_t *array)
{
    return (int)atomic_load_explicit(&array->size, memory_order_acquire);
}

static void ecs_comp_array_init(ecs_t *ecs, ecs_comp_array_t *array, size_t size, size_t capacity)
{
    ECS_ASSERT(ecs_is_not_null(ecs));
    ECS_ASSERT(ecs_is_not_null(array));

    (void)ecs;

    memset(array, 0, sizeof(ecs_comp_array_t));

    array->capacity = capacity;
    array->size = size;
    array->data = ECS_MALLOC(size * capacity, ECS_CORE(ecs)->mem_ctx);
}

static void ecs_comp_array_free(ecs_t *ecs, ecs_comp_array_t *array)
{
    ECS_ASSERT(ecs_is_not_null(ecs));
    ECS_ASSERT(ecs_is_not_null(array));

    (void)ecs;

    ECS_FREE(array->data, ECS_CORE(ecs)->mem_ctx);
}

static void ecs_comp_array_resize(ecs_t *ecs, ecs_comp_array_t *array, size_t capacity)
{
    ECS_ASSERT(ecs_is_not_null(ecs));
    ECS_ASSERT(ecs_is_not_null(array));

    (void)ecs;

    if (capacity >= array->capacity) {
        array->capacity *= 2;
        array->data = ECS_REALLOC(
            array->data,
            array->capacity * array->size,
            ECS_CORE(ecs)->mem_ctx
        );
    }
}

/*=============================================================================
 * Validation functions
 *============================================================================*/
#ifndef NDEBUG
static bool ecs_is_not_null(void *ptr)
{
    return NULL != ptr;
}

static bool ecs_is_valid_component_id(ecs_id_t id)
{
    return id < ECS_MAX_COMPONENTS;
}

static bool ecs_is_valid_system_id(ecs_id_t id)
{
    return id < ECS_MAX_SYSTEMS;
}

static bool ecs_is_entity_ready(ecs_t *ecs, ecs_id_t entity_id)
{
    return ECS_CORE(ecs)->entities[entity_id].ready;
}

static bool ecs_is_component_ready(ecs_t *ecs, ecs_id_t comp_id)
{
    return comp_id < ECS_CORE(ecs)->comp_count;
}

static bool ecs_is_system_ready(ecs_t *ecs, ecs_id_t sys_id)
{
    return sys_id < ECS_CORE(ecs)->system_count;
}

#endif // NDEBUG
