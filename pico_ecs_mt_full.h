#ifndef PICO_ECS_H
#define PICO_ECS_H

#include <stdbool.h> // bool, true, false
#include <stddef.h>  // size_t
#include <stdint.h>  // uint32_t

/**
 * @brief ECS context
 */
struct ecs_internal_s;
typedef struct ecs_internal_s ecs_internal_t;

struct ecs_s;
typedef struct ecs_s ecs_t;

/**
 * @brief Determine ID type
 */
#ifndef ECS_ID_TYPE
#define ECS_ID_TYPE uint64_t
#endif

/**
 * @brief ID used for entity and components
 */
typedef ECS_ID_TYPE ecs_id_t;

/**
 * @brief Determine mask type
 */
#ifndef ECS_MASK_TYPE
#define ECS_MASK_TYPE uint64_t
#endif

/**
 * @brief Type for value used in system matching
 */
typedef ECS_MASK_TYPE ecs_mask_t;

/**
 * @brief Return code for system callback and calling functions
 */
typedef int32_t ecs_ret_t;

/**
 * @brief An entity handle
 */
typedef struct ecs_entity_t
{
    ecs_id_t id;
} ecs_entity_t;

/**
 * @brief A component handle
 */
typedef struct ecs_comp_t
{
    ecs_id_t id;
} ecs_comp_t;

/**
 * @brief A system handle
 */
typedef struct ecs_system_t
{
    ecs_id_t id;
} ecs_system_t;

/**
 * @brief Returns true if the entity is invalid and false otherwise
 */
bool ecs_is_invalid_entity(ecs_entity_t entity);

/**
 * @brief Returns an invalid entity
 */
ecs_entity_t ecs_invalid_entity();

/**
 * @brief Creates an ECS context.
 *
 * @param entity_count The inital number of entities to pre-allocated
 * @param mem_ctx A context for a custom allocator
 *
 * @returns An ECS context or NULL if out of memory
 */
ecs_t *ecs_new(size_t entity_count, void *mem_ctx);

/**
 * @brief Destroys an ECS context
 *
 * @param ecs The ECS context
 */
void ecs_free(ecs_t *ecs);

/**
 * @brief Removes all entities from the ECS, preserving systems and components.
 */
void ecs_reset(ecs_t *ecs);

/**
 * @brief Called when a component is created (via ecs_add)
 *
 * @param ecs    The ECS context
 * @param entity The entity being constructed
 * @param ptr    The pointer to the component
 */
typedef void (*ecs_constructor_fn)(ecs_t *ecs, ecs_entity_t entity, void *comp_ptr, void *args);

/**
 * @brief Called when a component is destroyed (via ecs_remove or ecs_destroy)
 *
 * @param ecs    The ECS context
 * @param entity The entity being destoryed
 * @param ptr    The pointer to the component
 */
typedef void (*ecs_destructor_fn)(ecs_t *ecs, ecs_entity_t entity, void *comp_ptr);

/**
 * @brief Defines a component
 *
 * Defines a component with the specfied size in bytes. Components define the
 * game state (usually contained within structs) and are manipulated by systems.
 *
 * @param ecs         The ECS context
 * @param size        The number of bytes to allocate for each component instance
 * @param constructor Called when a component is created (disabled if NULL)
 * @param destructor  Called when a component is destroyed (disabled if NULL)
 * @param udata       Data passed to callbacks (can be NULL)
 * @returns           A component handle
 */
ecs_comp_t ecs_define_component(ecs_t *ecs, size_t size, ecs_constructor_fn constructor, ecs_destructor_fn destructor);

/**
 * @brief System callback
 *
 * Systems implement the core logic of an ECS by manipulating entities
 * and components.
 *
 * @param ecs          The ECS context
 * @param entities     An array of entities managed by the system
 * @param entity_count The number of entities in the array
 * @param udata        The user data associated with the system
 */
typedef ecs_ret_t (*ecs_system_fn)(ecs_t *ecs, ecs_entity_t *entities, size_t entity_count, void *udata);

#ifndef PICO_ECS_MT_MAX_TASKS
#define PICO_ECS_MT_MAX_TASKS 64
#endif

#ifndef PICO_ECS_MT_CMD_BUFFER_INITIAL_SIZE
#define PICO_ECS_MT_CMD_BUFFER_INITIAL_SIZE 256
#endif

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
 * @brief Callback to wait for task completion
 *
 * @param udata User data (Usually a pointer to the user's threadpool)
 */
typedef void (*ecs_wait_tasks_fn)(void *udata);

/**
 * @brief Configures task callbacks for parallel system execution
 *
 * @param ecs        The ECS context
 * @param enqueue_cb Callback to enqueue tasks
 * @param wait_cb    Callback to wait for task completion
 * @param task_udata User data passed to callbacks
 * @param task_count Number of parallel tasks to use (max PICO_ECS_MT_MAX_TASKS)
 */
void ecs_set_task_callbacks(
    ecs_t *ecs,
    ecs_enqueue_task_fn enqueue_cb,
    ecs_wait_tasks_fn wait_cb,
    void *task_udata,
    int task_count
);

/**
 * @brief Called when an entity is added to a system
 *
 * @param ecs    The ECS context
 * @param entity The entity being added
 * @param udata  The user data passed to the callback
 */
typedef void (*ecs_added_fn)(ecs_t *ecs, ecs_entity_t entity, void *udata);

/**
 * @brief Called when an entity is removed from a system
 *
 * @param ecs    The ECS context
 * @param entity The enitty being removed
 * @param udata  The user data passed to the callback
 */
typedef void (*ecs_removed_fn)(ecs_t *ecs, ecs_entity_t entity, void *udata);

/**
 * @brief Defines a system
 *
 * Defines a system with the specified parameters. Systems contain the
 * core logic of a game by manipulating game state as defined by components.
 *
 * @param ecs       The ECS context
 * @param mask      Bitmask that determines which categories the system belongs
                    to. A value of 0 matches all categories
 * @param system_cb Callback that is fired every update
 * @param add_cb    Called when an entity is added to the system (can be NULL)
 * @param remove_cb Called when an entity is removed from the system (can be NULL)
 * @param udata     The user data passed to the callbacks
 * @returns         A system handle
 */
ecs_system_t ecs_define_system(
    ecs_t *ecs,
    ecs_mask_t mask,
    ecs_system_fn system_cb,
    ecs_added_fn add_cb,
    ecs_removed_fn remove_cb,
    void *udata
);
/**
 * @brief Entities are processed by the target system if they have all of the
 * the components required by the system
 *
 * @param ecs  The ECS context
 * @param sys  The target system
 * @param comp A component to require
 */
void ecs_require_component(ecs_t *ecs, ecs_system_t sys, ecs_comp_t comp);

/**
 * @brief Excludes entities having the specified component from being added to
 * the target system.
 *
 * @param ecs  The ECS context
 * @param sys  The target system
 * @param comp A component to exclude
 */
void ecs_exclude_component(ecs_t *ecs, ecs_system_t sys, ecs_comp_t comp);

/**
 * @brief Enables a system
 *
 * @param ecs    The ECS context
 * @param sys_id The specified system
 */
void ecs_enable_system(ecs_t *ecs, ecs_system_t sys);

/**
 * @brief Disables a system
 *
 * @param ecs The ECS context
 * @param sys The specified system
 */
void ecs_disable_system(ecs_t *ecs, ecs_system_t sys);

/**
 * @brief Updates the callbacks for an existing system
 *
 * @param ecs       The ECS context
 * @param sys       The system
 * @param system_cb Callback that is fired every update
 * @param add_cb    Called when an entity is added to the system (can be NULL)
 * @param remove_cb Called when an entity is removed from the system (can be NULL)
 */
void ecs_set_system_callbacks(
    ecs_t *ecs,
    ecs_system_t sys,
    ecs_system_fn system_cb,
    ecs_added_fn add_cb,
    ecs_removed_fn remove_cb
);

/**
 * @brief Sets the user data for a system
 *
 * @param ecs   The ECS context
 * @param sys   The system
 * @param udata The user data to set
 */
void ecs_set_system_udata(ecs_t *ecs, ecs_system_t sys, void *udata);

/**
 * @brief Gets the user data from a system
 *
 * @param ecs The ECS context
 * @param sys The system
 * @return    The system's user data
 */
void *ecs_get_system_udata(ecs_t *ecs, ecs_system_t sys);

/**
 * @brief Sets the system's mask
 *
 * @param ecs  The ECS context
 * @param sys  The system
 * @param mask The mask to set
 */
void ecs_set_system_mask(ecs_t *ecs, ecs_system_t sys, ecs_mask_t mask);

/**
 * @brief Returns the system mask
 *
 * @param ecs The ECS context
 * @param sys The system
 * @return    The system's mask
 */
ecs_mask_t ecs_get_system_mask(ecs_t *ecs, ecs_system_t sys);

/**
 * @brief Returns the number of entities assigned to the specified system
 */
size_t ecs_get_system_entity_count(ecs_t *ecs, ecs_system_t sys);

/**
 * @brief Creates an entity
 *
 * @param ecs The ECS context
 *
 * @returns The new entity
 */
ecs_entity_t ecs_create(ecs_t *ecs);

/**
 * @brief Returns true if the entity is currently active and has not been queued
 * for destruction
 *
 * @param ecs The ECS context
 * @param entity The target entity
 */
bool ecs_is_ready(ecs_t *ecs, ecs_entity_t entity);

/**
 * @brief Test if entity has the specified component
 *
 * @param ecs    The ECS context
 * @param entity The entity
 * @param comp   The component
 *
 * @returns True if the entity has the component
 */
bool ecs_has(ecs_t *ecs, ecs_entity_t entity, ecs_comp_t comp);

/**
 * @brief Adds a component instance to an entity
 *
 * @param ecs    The ECS context
 * @param entity The entity
 * @param comp   The component
 *
 * @returns The component data
 */
void *ecs_add(ecs_t *ecs, ecs_entity_t entity, ecs_comp_t comp, void *args);

/**
 * @brief Gets a component instance associated with an entity
 *
 * @param ecs    The ECS context
 * @param entity The entity
 * @param comp   The component
 *
 * @returns The component data
 */
void *ecs_get(ecs_t *ecs, ecs_entity_t entity, ecs_comp_t comp);

/**
 * @brief Destroys an entity
 *
 * Destroys an entity, releasing resources and returning it to the pool.
 *
 * WARNING: This function may change the order of a system's entity array. It
 * should be used with caution. A better option in most circumstances is to use
 * the {@link ecs_queue_destroy} function, which destroys the entity after the
 * system has finished executing.
 *
 * @param ecs    The ECS context
 * @param entity The entity to destroy
 */
void ecs_destroy(ecs_t *ecs, ecs_entity_t entity);

/**
 * @brief Removes a component instance from an entity
 *
 * WARNING: This function may change the order of a system's entity array. It
 * should be used with caution. A better option in most circumstances is to use
 * the {@link ecs_queue_remove} function, which removes the component after the
 * system has finished executing.
 *
 * @param ecs    The ECS context
 * @param entity The entity
 * @param comp   The component
 */
void ecs_remove(ecs_t *ecs, ecs_entity_t entity, ecs_comp_t comp);

/**
 * @brief Queues an entity for destruction after the current system returns
 *
 * Queued entities are destroyed after the curent iteration.
 *
 * @param ecs    The ECS context
 * @param entity The entity to destroy
 */
void ecs_queue_destroy(ecs_t *ecs, ecs_entity_t entity);

/**
 * @brief Queues a component for removal from the specified entity
 *
 * Queued entity/component pairs that will be deleted after the current system
 * returns.
 *
 * @param ecs    The ECS context
 * @param entity The entity that has the component
 * @param comp   The component to remove
 */
void ecs_queue_remove(ecs_t *ecs, ecs_entity_t entity, ecs_comp_t comp);

/**
 * @brief Update an individual system
 *
 * Calls system logic on required components, but not excluded ones.
 *
 * @param ecs The ECS context
 * @param sys The system to update
 * @param mask Bitmask that determines which systems run based on category.
 */
ecs_ret_t ecs_run_system(ecs_t *ecs, ecs_system_t sys, ecs_mask_t mask);

/**
 * @brief Updates all systems
 *
 * Calls {@link ecs_run_system} on all components in order of system
 * definition. In many cases it is better to call {@link ecs_run_system} as
 * needed.
 *
 * @param ecs The ECS context
 * @param mask Bitmask that determines which systems run based on category.
 */
ecs_ret_t ecs_run_systems(ecs_t *ecs, ecs_mask_t mask);

#endif // PICO_ECS_H
