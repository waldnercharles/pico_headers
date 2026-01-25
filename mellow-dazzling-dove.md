# Multithreaded ECS Fork: Lock-Free Implementation Plan

## Overview

Convert `pico_ecs_mt_full.h/c` from a wrapper around pico_ecs.h into a standalone lock-free multithreaded ECS using C11/C23 atomics. The design maintains the command buffer approach while making entity operations thread-safe through atomic operations and deferred execution.

## Design Principles

1. **Lock-free core operations** - Entity ID allocation and state management use C11/C23 atomics
2. **Deferred structural changes** - Command buffers (per-thread) queue add/remove/destroy operations
3. **Two-phase execution model** - Parallel work phase (read-only) + Serial flush phase (structural changes)
4. **User-controlled parallelism** - Both `ecs_define_system` (sequential) and `ecs_mt_define_system` (parallel)
5. **Threadpool abstraction** - User supplies callbacks, avoiding pthread.h dependency

## Critical Race Conditions Addressed

From test analysis (`tests_pico_ecs_mt/main.c`):

1. **Entity ID allocation** (`next_entity_id++`) - Use `atomic_fetch_add`
2. **Entity pool operations** - Lock-free stack with CAS
3. **Entity array reallocation** - Deferred reclamation with epoch-based grace period
4. **Sparse set modifications** - Serial phase only (system membership changes)
5. **Component array resizing** - Atomic pointer swapping with deferred free
6. **Entity state flags** - `_Atomic(bool)` for active/ready
7. **Command buffer flush** - Memory fences for visibility guarantees

## Implementation Approach

### 1. Lock-Free Entity ID Allocation

**Current Problem (pico_ecs_mt_full.c:480):**
```c
entity_id = ecs->next_entity_id++;  // RACE CONDITION
```

**Solution:**
```c
// In ecs_s struct
_Atomic(ecs_id_t) next_entity_id;

// In ecs_mt_create
ecs_id_t entity_id = atomic_fetch_add_explicit(
    &ecs->next_entity_id,
    1,
    memory_order_relaxed  // No synchronization needed, just atomicity
);
```

### 2. Lock-Free Entity Pool (ID Recycling)

**Replace ecs_id_array_t with lock-free stack:**

```c
typedef struct {
    _Atomic(void*) head;       // Lock-free stack head (ABA-safe tagged pointer)
    _Atomic(uint64_t) aba_counter;  // Prevents ABA problem
    void* mem_ctx;
} ecs_lf_stack_t;

// Stack node
typedef struct ecs_lf_node_s {
    ecs_id_t value;
    _Atomic(struct ecs_lf_node_s*) next;
} ecs_lf_node_t;
```

**Push/Pop using CAS:**
- Push: CAS loop with `memory_order_release` to publish node
- Pop: CAS loop with `memory_order_acquire` to see node data
- ABA counter prevents stack corruption during concurrent operations

### 3. Atomic Entity State

**Update ecs_entity_data_t:**
```c
typedef struct {
    ecs_bitset_t comp_bits;      // Modified only in serial phase
    _Atomic(bool) active;         // Thread-safe read/write
    _Atomic(bool) ready;          // Thread-safe read/write
} ecs_entity_data_t;
```

**Access patterns:**
- Parallel phase: `atomic_load_explicit(..., memory_order_acquire)` to check ready state
- Serial phase: `atomic_store_explicit(..., memory_order_release)` after modifications

### 4. Component Array Pointer Safety

**Update ecs_comp_array_t:**
```c
typedef struct {
    size_t capacity;
    size_t size;  // Component size in bytes
    _Atomic(void*) data;   // Atomic pointer for safe concurrent reads
    void* old_data;        // For deferred free
} ecs_comp_array_t;
```

**Resize strategy:**
- Allocate new array, copy data
- `atomic_store_explicit(&array->data, new_data, memory_order_release)`
- Defer free of old_data using epoch-based reclamation (see below)

### 5. Epoch-Based Memory Reclamation

Prevent use-after-free when resizing arrays during concurrent access:

```c
typedef struct {
    _Atomic(uint64_t) current_epoch;
    struct {
        void* ptr;
        uint64_t epoch;
    } pending_frees[256];
    size_t pending_count;
} ecs_reclaim_t;
```

**Timeline:**
1. Epoch N: Resize array, defer free old pointer
2. Epoch N+1: Parallel phase (threads may still access old array)
3. Epoch N+2: Safe to free - 2 epochs elapsed, all threads see new array
4. Advance epoch after each parallel→serial transition

### 6. Two-Phase Execution Model

```
┌────────────────────────────────────────────────────┐
│                  FRAME TIMELINE                    │
├────────────────────────────────────────────────────┤
│  [Serial Phase - Single Threaded]                 │
│  • Process deferred frees (epoch-based)            │
│  • Flush all command buffers in order:            │
│    1. CREATE commands                              │
│    2. ADD commands (updates sparse sets)           │
│    3. REMOVE commands                              │
│    4. DESTROY commands                             │
│  • atomic_thread_fence(memory_order_release)       │
│                                                    │
│  [Parallel Phase - Multi-Threaded]                │
│  • atomic_thread_fence(memory_order_acquire)       │
│  • Systems execute across worker threads           │
│  • Read entity/component data (atomic loads)       │
│  • Write to thread-local command buffers           │
│  • Barrier: wait for all tasks to complete         │
│                                                    │
│  [Advance Epoch]                                   │
│  • Increment epoch counter for next frame          │
└────────────────────────────────────────────────────┘
```

### 7. Command Buffer System (Keep Current Design)

**Already thread-safe:**
- One buffer per thread (from `pico_ecs_mt.h`)
- Lock-free writes (each thread owns its buffer)
- Serial flush (single-threaded after barrier)

**Enhancement needed:**
```c
void ecs_mt_flush_commands(ecs_mt_t* ecs_mt) {
    atomic_thread_fence(memory_order_acquire);  // See all command writes

    // Execute in deterministic order (avoid races)
    for (phase = CREATE; phase <= DESTROY; phase++) {
        for each buffer {
            for each command of type phase {
                execute_command(cmd);
            }
        }
    }

    atomic_thread_fence(memory_order_release);  // Publish all changes

    // Clear buffers
    for each buffer {
        ecs_mt_cmd_buffer_clear(buf);
    }
}
```

### 8. API Design: Mixed Sequential/Parallel Systems

**Sequential System (unchanged from pico_ecs.h):**
```c
ecs_system_t physics_sys = ecs_define_system(
    ecs_mt->ecs,          // Access underlying ecs_t
    PHYSICS_MASK,
    physics_update,       // ecs_system_fn (single-threaded callback)
    NULL, NULL, NULL
);
```

**Parallel System (from pico_ecs_mt.h):**
```c
ecs_system_t render_sys = ecs_mt_define_system(
    ecs_mt,               // ecs_mt_t context
    RENDER_MASK,
    render_update,        // ecs_mt_system_fn (multi-threaded callback)
    NULL, NULL, NULL
);
```

**Key difference:**
- Sequential systems: Full write access, no parallelism
- Parallel systems: Read-only entity/component access, write via command buffers

### 9. Thread-Safe Entity Creation

**Fix ecs_mt_create (currently unsafe at line 464):**

```c
ecs_entity_t ecs_mt_create(ecs_mt_t* ecs_mt) {
    // 1. Atomically allocate ID
    ecs_id_t entity_id = atomic_fetch_add_explicit(
        &ecs_mt->ecs->next_entity_id,
        1,
        memory_order_relaxed
    );

    // 2. Grow entities array if needed (use current_epoch for safety)
    // This is deferred to serial phase via command

    // 3. Queue creation command
    ecs_mt_cmd_buffer_t* buf = &ecs_mt->cmd_buffers[ecs_mt->thread_id - 1];
    ecs_mt_cmd_buffer_ensure_capacity(buf);

    ecs_mt_cmd_t* cmd = &buf->commands[buf->count++];
    cmd->type = ECS_MT_CMD_CREATE;
    cmd->create.entity = ecs_make_entity(entity_id);

    return cmd->create.entity;
}
```

## C11/C23 Atomic Primitives Used

| Type | Usage |
|------|-------|
| `_Atomic(ecs_id_t)` | next_entity_id counter |
| `_Atomic(bool)` | Entity active/ready flags |
| `_Atomic(void*)` | Component array data pointers |
| `_Atomic(uint64_t)` | Epoch counter, ABA counter |
| `_Atomic(struct node*)` | Lock-free stack nodes |

| Operation | Memory Order | Rationale |
|-----------|--------------|-----------|
| `atomic_fetch_add` | `relaxed` | ID allocation (no sync needed) |
| Lock-free stack CAS | `acq_rel` | Synchronize node visibility |
| Array pointer store | `release` | Publish new array |
| Array pointer load | `acquire` | See latest array |
| Entity flags store | `release` | Publish state change |
| Entity flags load | `acquire` | See latest state |
| Phase fence | `acq_rel` | Full barrier between phases |

## Files to Modify

### Core Implementation
1. **pico_ecs_mt_full.h** - Add atomic type annotations, lock-free stack, epoch reclamation
2. **pico_ecs_mt_full.c** - Implement lock-free algorithms, atomic operations, command flush

### API Surface
3. **pico_ecs_mt.h** (optional reference) - Shows user-facing wrapper patterns

### Examples/Tests
4. **examples_pico_ecs_mt/example.c** - Update to demonstrate mixed sequential/parallel systems
5. **tests_pico_ecs_mt/main.c** - Validate thread safety of all operations

## Implementation Steps

### Phase 1: Core Atomics (Foundation)
- [ ] Add `_Atomic` annotations to ecs_s struct (next_entity_id, epoch)
- [ ] Update ecs_entity_data_t with atomic bool flags
- [ ] Update ecs_comp_array_t with atomic void* data
- [ ] Implement atomic entity ID allocation in ecs_create
- [ ] Add epoch tracking infrastructure

### Phase 2: Lock-Free Entity Pool
- [ ] Implement ecs_lf_stack_t (lock-free stack)
- [ ] Implement ecs_lf_stack_push (CAS-based)
- [ ] Implement ecs_lf_stack_pop (CAS-based with ABA protection)
- [ ] Replace ecs_id_array_t entity_pool with ecs_lf_stack_t
- [ ] Update ecs_create to use lock-free pop
- [ ] Update ecs_destroy to use lock-free push

### Phase 3: Memory Reclamation
- [ ] Add ecs_reclaim_t to ecs_s struct
- [ ] Implement ecs_defer_free (add to pending list)
- [ ] Implement ecs_process_deferred_frees (free after 2+ epochs)
- [ ] Implement ecs_advance_epoch (increment after parallel phase)
- [ ] Update ecs_comp_array_resize to defer old array free
- [ ] Update entity array resize to defer old array free

### Phase 4: Command Buffer Enhancement
- [ ] Add memory fences to ecs_mt_flush_commands
- [ ] Implement deterministic command ordering (CREATE→ADD→REMOVE→DESTROY)
- [ ] Add epoch advancement after flush
- [ ] Ensure atomic stores use release semantics in flush

### Phase 5: Thread-Safe Entity Operations
- [ ] Fix ecs_mt_create to use atomic_fetch_add
- [ ] Update ecs_mt_add to safely copy component data
- [ ] Update ecs_mt_remove to properly queue removal
- [ ] Update ecs_mt_destroy to mark ready=false atomically

### Phase 6: Component Access Safety
- [ ] Update ecs_get to use atomic_load for component array pointer
- [ ] Ensure component reads use acquire semantics
- [ ] Add assertions for serial-phase-only operations (sparse sets, bitsets)

### Phase 7: Testing & Validation
- [ ] Run all tests in tests_pico_ecs_mt/main.c
- [ ] Verify no ThreadSanitizer (TSan) warnings
- [ ] Stress test with high thread count (16+ threads)
- [ ] Validate memory reclamation (no leaks with Valgrind/ASan)
- [ ] Benchmark parallel vs sequential performance

## Verification Plan

### Correctness Tests
1. **test_concurrent_create_race** - Should pass (atomic ID allocation)
2. **test_concurrent_add_race** - Should pass (command buffering)
3. **test_concurrent_remove_race** - Should pass (command buffering)
4. **test_concurrent_destroy_race** - Should pass (command buffering + atomic flags)
5. **test_mixed_operations_stress** - Should pass (all operations safe)

### Tools
- **ThreadSanitizer (TSan)**: `gcc -fsanitize=thread` - Detect data races
- **AddressSanitizer (ASan)**: `gcc -fsanitize=address` - Detect memory errors
- **Valgrind**: Check for memory leaks in epoch reclamation

### Success Criteria
- All tests pass without failures
- No TSan warnings
- No memory leaks (Valgrind clean)
- Performance scales with thread count (2-16 threads)

## Known Limitations

1. **Sparse sets remain serial** - System membership changes only during flush phase (acceptable trade-off)
2. **Bitset operations serial** - Component bitsets modified only in serial phase
3. **Entity array growth** - Pre-allocation recommended to avoid frequent resizes
4. **Epoch reclamation overhead** - 2-epoch delay before free (minimal memory impact)

## Migration from Current Wrapper

**Before (pico_ecs_mt.h wrapper):**
```c
ecs_mt_t *ecs_mt = ecs_mt_new(1024, enqueue_task, finish_task, pool, 4, NULL);
ecs_system_t sys = ecs_mt_define_system(ecs_mt, 0, system_cb, NULL, NULL, NULL);
ecs_run_system(ecs_mt->ecs, sys, 0);  // Access underlying ecs_t
```

**After (pico_ecs_mt_full.h fork):**
```c
ecs_mt_t *ecs_mt = ecs_mt_new(1024, enqueue_task, finish_task, pool, 4, NULL);
ecs_system_t sys = ecs_mt_define_system(ecs_mt, 0, system_cb, NULL, NULL, NULL);
ecs_run_system(ecs_mt->ecs, sys, 0);  // Same API, but thread-safe internally
```

**Key difference:** Internal implementation uses atomics instead of locks, but API remains similar.

## Performance Characteristics

### Read Path (Hot Path)
- Component access: 1 atomic load (array pointer) + pointer arithmetic
- Entity state check: 1 atomic load (ready flag)
- **No atomics on actual component data** (read-only during parallel phase)

### Write Path (Command Buffer)
- Entity creation: 1 atomic fetch-add + buffer append
- Component add/remove: Buffer append + optional memcpy
- **Lock-free, per-thread buffer** (no contention)

### Synchronization Points
- Frame start: Process deferred frees + flush commands (serial)
- Frame end: Barrier (wait for tasks) + advance epoch
- **Only 2 memory fences per frame** (acquire at start, release at end)

## Expected Scalability
- **1-4 threads**: Near-linear speedup for read-heavy workloads
- **8+ threads**: Good scaling if entity count >> thread count
- **Contention**: Minimal (only on entity pool CAS, which is rare)
