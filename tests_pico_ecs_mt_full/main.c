#define PICO_UNIT_IMPLEMENTATION

#include "../pico_unit.h"
#include "../pico_ecs_mt_full.h"
#include <stdatomic.h>

typedef struct
{
    int value;
} ecs_test_comp_t;

static void ecs_test_comp_ctor(ecs_t *ecs, ecs_entity_t entity, void *comp, void *args)
{
    (void)ecs;
    (void)entity;

    ecs_test_comp_t *dst = (ecs_test_comp_t *)comp;

    if (args) {
        ecs_test_comp_t *src = (ecs_test_comp_t *)args;
        *dst = *src;
    }
}

static int ecs_test_enqueue(int (*fn)(void *args), void *fn_args, void *udata)
{
    (void)udata;
    return fn(fn_args);
}

static void ecs_test_wait(void *udata)
{
    (void)udata;
}

static ecs_ret_t ecs_test_add_system(
    ecs_t *ecs,
    ecs_entity_t *entities,
    size_t entity_count,
    void *udata
)
{
    ecs_comp_t comp = *(ecs_comp_t *)udata;
    ecs_test_comp_t init = { 7 };

    for (size_t i = 0; i < entity_count; i++) {
        ecs_test_comp_t *data = (ecs_test_comp_t *)ecs_add(ecs, entities[i], comp, &init);
        REQUIRE(data != NULL);
        REQUIRE(data->value == init.value);
    }

    return 0;
}

typedef struct
{
    _Atomic size_t count;
    ecs_entity_t created[4];
} ecs_create_capture_t;

static ecs_ret_t ecs_test_create_system(
    ecs_t *ecs,
    ecs_entity_t *entities,
    size_t entity_count,
    void *udata
)
{
    ecs_create_capture_t *capture = (ecs_create_capture_t *)udata;

    for (size_t i = 0; i < entity_count; i++) {
        (void)entities[i];
        size_t index = atomic_fetch_add_explicit(&capture->count, 1, memory_order_relaxed);
        ecs_entity_t created = ecs_create(ecs);

        if (index < (sizeof(capture->created) / sizeof(capture->created[0]))) {
            capture->created[index] = created;
        }
    }

    return 0;
}

TEST_CASE(test_mt_full_task_view_add)
{
    ecs_t *ecs = ecs_new(16, NULL);
    REQUIRE(ecs != NULL);

    ecs_comp_t tag = ecs_define_component(ecs, sizeof(unsigned char), NULL, NULL);
    ecs_comp_t test = ecs_define_component(ecs, sizeof(ecs_test_comp_t), ecs_test_comp_ctor, NULL);
    ecs_system_t sys = ecs_define_system(ecs, 0, ecs_test_add_system, NULL, NULL, &test);

    ecs_require_component(ecs, sys, tag);

    ecs_entity_t entities[4];
    for (size_t i = 0; i < 4; i++) {
        entities[i] = ecs_create(ecs);
        ecs_add(ecs, entities[i], tag, NULL);
    }

    ecs_enable_system(ecs, sys);
    ecs_set_task_callbacks(ecs, ecs_test_enqueue, ecs_test_wait, NULL, 2);
    ecs_run_system(ecs, sys, 0);

    for (size_t i = 0; i < 4; i++) {
        REQUIRE(ecs_has(ecs, entities[i], test));
        ecs_test_comp_t *data = (ecs_test_comp_t *)ecs_get(ecs, entities[i], test);
        REQUIRE(data->value == 7);
    }

    ecs_free(ecs);

    return true;
}

TEST_CASE(test_mt_full_task_view_pool_reuse)
{
    ecs_t *ecs = ecs_new(16, NULL);
    REQUIRE(ecs != NULL);

    ecs_comp_t tag = ecs_define_component(ecs, sizeof(unsigned char), NULL, NULL);

    ecs_create_capture_t capture;
    atomic_init(&capture.count, 0);
    for (size_t i = 0; i < 4; i++) {
        capture.created[i] = ecs_invalid_entity();
    }

    ecs_system_t sys = ecs_define_system(ecs, 0, ecs_test_create_system, NULL, NULL, &capture);
    ecs_require_component(ecs, sys, tag);

    ecs_enable_system(ecs, sys);
    ecs_set_task_callbacks(ecs, ecs_test_enqueue, ecs_test_wait, NULL, 2);

    ecs_entity_t entities[2];
    for (size_t i = 0; i < 2; i++) {
        entities[i] = ecs_create(ecs);
        ecs_add(ecs, entities[i], tag, NULL);
    }

    ecs_entity_t recycled[2];
    for (size_t i = 0; i < 2; i++) {
        recycled[i] = ecs_create(ecs);
    }

    for (size_t i = 0; i < 2; i++) {
        ecs_destroy(ecs, recycled[i]);
    }

    ecs_run_system(ecs, sys, 0);

    size_t created_count = atomic_load_explicit(&capture.count, memory_order_acquire);
    REQUIRE(created_count == 2);

    bool found_first = false;
    bool found_second = false;

    for (size_t i = 0; i < 2; i++) {
        ecs_entity_t created = capture.created[i];
        REQUIRE(!ecs_is_invalid_entity(created));

        if (created.id == recycled[0].id) {
            found_first = true;
        } else if (created.id == recycled[1].id) {
            found_second = true;
        } else {
            REQUIRE(false);
        }
    }

    REQUIRE(found_first && found_second);

    ecs_free(ecs);

    return true;
}

TEST_SUITE(suite_pico_ecs_mt_full)
{
    RUN_TEST_CASE(test_mt_full_task_view_add);
    RUN_TEST_CASE(test_mt_full_task_view_pool_reuse);
}

int main(void)
{
    pu_display_colors(true);

    RUN_TEST_SUITE(suite_pico_ecs_mt_full);

    pu_print_stats();

    return pu_test_failed();
}
