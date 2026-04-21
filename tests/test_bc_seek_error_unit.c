// SPDX-License-Identifier: MIT

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>

#include <stdbool.h>
#include <string.h>

#include "bc_allocators.h"
#include "bc_seek_error_internal.h"

typedef struct error_fixture {
    bc_allocators_context_t* memory_context;
    bc_seek_error_collector_t* collector;
} error_fixture_t;

static int setup(void** state)
{
    error_fixture_t* fixture = test_calloc(1, sizeof(*fixture));
    bc_allocators_context_config_t config = {.tracking_enabled = true};
    if (!bc_allocators_context_create(&config, &fixture->memory_context)) {
        test_free(fixture);
        return -1;
    }
    if (!bc_seek_error_collector_create(fixture->memory_context, &fixture->collector)) {
        bc_allocators_context_destroy(fixture->memory_context);
        test_free(fixture);
        return -1;
    }
    *state = fixture;
    return 0;
}

static int teardown(void** state)
{
    error_fixture_t* fixture = *state;
    bc_seek_error_collector_destroy(fixture->memory_context, fixture->collector);
    bc_allocators_context_destroy(fixture->memory_context);
    test_free(fixture);
    return 0;
}

static void test_collector_starts_empty(void** state)
{
    error_fixture_t* fixture = *state;
    assert_int_equal(bc_seek_error_collector_count(fixture->collector), 0u);
}

static void test_collector_appends_records(void** state)
{
    error_fixture_t* fixture = *state;
    assert_true(bc_seek_error_collector_append(fixture->collector, "/tmp/a", 2));
    assert_true(bc_seek_error_collector_append(fixture->collector, "/tmp/b", 13));
    assert_int_equal(bc_seek_error_collector_count(fixture->collector), 2u);
}

static void test_collector_truncates_long_paths(void** state)
{
    error_fixture_t* fixture = *state;
    char long_path[8192];
    memset(long_path, 'x', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';
    assert_true(bc_seek_error_collector_append(fixture->collector, long_path, 1));
    assert_int_equal(bc_seek_error_collector_count(fixture->collector), 1u);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_collector_starts_empty, setup, teardown),
        cmocka_unit_test_setup_teardown(test_collector_appends_records, setup, teardown),
        cmocka_unit_test_setup_teardown(test_collector_truncates_long_paths, setup, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
