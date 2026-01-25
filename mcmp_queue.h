#ifndef MCMP_QUEUE_H
#define MCMP_QUEUE_H

#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifndef CACHE_LINE
#define CACHE_LINE 64
#endif

#include <assert.h>
_Static_assert((CACHE_LINE & (CACHE_LINE - 1)) == 0, "CACHE_LINE must be a power-of-two");

#define HEAD(q) atomic_load_explicit(&(q)->head, memory_order_acquire)
#define TAIL(q) atomic_load_explicit(&(q)->tail, memory_order_acquire)

typedef struct
{
    alignas(CACHE_LINE) _Atomic size_t turn;
    unsigned char data[];
} slot_t;

typedef struct
{
    alignas(CACHE_LINE) _Atomic size_t head;
    alignas(CACHE_LINE) _Atomic size_t tail;
    alignas(CACHE_LINE) size_t num_slots;
    size_t slot_size;
    size_t slot_stride;
    unsigned char *slots_data;
} mcmpq_t;

static inline slot_t *get_slot(mcmpq_t *queue, size_t index)
{
    return (slot_t *)(queue->slots_data + (index % queue->num_slots) * queue->slot_stride);
}

static inline mcmpq_t *mcmpq_new(size_t num_slots, size_t slot_size)
{
    if (num_slots == 0 || slot_size == 0) { return NULL; }

    mcmpq_t *queue = (mcmpq_t *)malloc(sizeof(mcmpq_t));
    if (!queue) { return NULL; }

    queue->num_slots = num_slots;
    queue->slot_size = slot_size;

    size_t slot_header_size = offsetof(slot_t, data);
    size_t total_slot_size = slot_header_size + slot_size;
    queue->slot_stride = (total_slot_size + CACHE_LINE - 1) & ~(CACHE_LINE - 1);

    queue->slots_data = (unsigned char *)aligned_alloc(CACHE_LINE, queue->slot_stride * num_slots);
    if (!queue->slots_data) {
        free(queue);
        return NULL;
    }

    memset(queue->slots_data, 0, queue->slot_stride * num_slots);

    for (size_t i = 0; i < num_slots; i++) {
        slot_t *slot = get_slot(queue, i);
        atomic_init(&slot->turn, 0);
    }

    atomic_init(&queue->head, 0);
    atomic_init(&queue->tail, 0);

    return queue;
}

static inline void mcmpq_free(mcmpq_t *queue)
{
    free(queue->slots_data);
    free(queue);
}

static inline void mcmpq_enqueue(mcmpq_t *queue, const void *item)
{
    size_t head = atomic_fetch_add_explicit(&queue->head, 1, memory_order_acq_rel);
    slot_t *slot = get_slot(queue, head);
    while ((head / queue->num_slots) * 2 !=
           atomic_load_explicit(&slot->turn, memory_order_acquire)) { /* busy-wait */
    }
    memcpy(slot->data, item, queue->slot_size);
    atomic_store_explicit(&slot->turn, (head / queue->num_slots) * 2 + 1, memory_order_release);
}

static inline void mcmpq_dequeue(mcmpq_t *queue, void *item)
{
    size_t tail = atomic_fetch_add_explicit(&queue->tail, 1, memory_order_acq_rel);
    slot_t *slot = get_slot(queue, tail);
    while ((tail / queue->num_slots) * 2 + 1 !=
           atomic_load_explicit(&slot->turn, memory_order_acquire)) { /* busy-wait */
    }
    memcpy(item, slot->data, queue->slot_size);
    atomic_store_explicit(&slot->turn, (tail / queue->num_slots) * 2 + 2, memory_order_release);
}

static inline bool mcmpq_try_enqueue(mcmpq_t *queue, const void *item)
{
    size_t head = atomic_load_explicit(&queue->head, memory_order_acquire);
    for (;;) {
        slot_t *slot = get_slot(queue, head);
        if ((head / queue->num_slots) * 2 ==
            atomic_load_explicit(&slot->turn, memory_order_acquire)) {
            if (atomic_compare_exchange_strong_explicit(&queue->head, &head, head + 1, memory_order_acq_rel, memory_order_acquire)) {
                memcpy(slot->data, item, queue->slot_size);
                atomic_store_explicit(&slot->turn, (head / queue->num_slots) * 2 + 1, memory_order_release);
                return true;
            }
        } else {
            size_t prev_head = head;
            head = atomic_load_explicit(&queue->head, memory_order_acquire);
            if (head == prev_head) { return false; }
        }
    }
}

static inline bool mcmpq_try_dequeue(mcmpq_t *queue, void *item)
{
    size_t tail = atomic_load_explicit(&queue->tail, memory_order_acquire);
    for (;;) {
        slot_t *slot = get_slot(queue, tail);
        if ((tail / queue->num_slots) * 2 + 1 ==
            atomic_load_explicit(&slot->turn, memory_order_acquire)) {
            if (atomic_compare_exchange_strong_explicit(&queue->tail, &tail, tail + 1, memory_order_acq_rel, memory_order_acquire)) {
                memcpy(item, slot->data, queue->slot_size);
                atomic_store_explicit(&slot->turn, (tail / queue->num_slots) * 2 + 2, memory_order_release);
                return true;
            }
        } else {
            size_t prev_tail = tail;
            tail = atomic_load_explicit(&queue->tail, memory_order_acquire);
            if (tail == prev_tail) { return false; }
        }
    }
}
#endif
