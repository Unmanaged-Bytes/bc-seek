// SPDX-License-Identifier: MIT

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "bc_allocators.h"
#include "bc_runtime.h"
#include "bc_runtime_cli.h"
#include "bc_seek_cli_internal.h"

typedef struct fixture {
    bc_allocators_context_t* memory_context;
    bc_runtime_config_store_t* store;
} fixture_t;

static int setup(void** state)
{
    fixture_t* fixture = test_calloc(1, sizeof(*fixture));
    bc_allocators_context_config_t config = {.tracking_enabled = true};
    if (!bc_allocators_context_create(&config, &fixture->memory_context)) {
        test_free(fixture);
        return -1;
    }
    if (!bc_runtime_config_store_create(fixture->memory_context, &fixture->store)) {
        bc_allocators_context_destroy(fixture->memory_context);
        test_free(fixture);
        return -1;
    }
    *state = fixture;
    return 0;
}

static int teardown(void** state)
{
    fixture_t* fixture = *state;
    bc_runtime_config_store_destroy(fixture->memory_context, fixture->store);
    bc_allocators_context_destroy(fixture->memory_context);
    test_free(fixture);
    return 0;
}

static bc_runtime_cli_parse_status_t parse_into(fixture_t* fixture, const char* const argv[], int argc, bc_runtime_cli_parsed_t* out_parsed)
{
    FILE* error_stream = fmemopen(NULL, 4096, "w");
    bc_runtime_cli_parse_status_t status = bc_runtime_cli_parse(bc_seek_cli_program_spec(), argc, argv, fixture->store, out_parsed, error_stream);
    if (error_stream != NULL) {
        fclose(error_stream);
    }
    return status;
}

static void test_bind_global_threads_auto(void** state)
{
    fixture_t* fixture = *state;
    const char* argv[] = {"bc-seek", "find", "."};
    bc_runtime_cli_parsed_t parsed;
    assert_int_equal(parse_into(fixture, argv, 3, &parsed), BC_RUNTIME_CLI_PARSE_OK);
    bc_seek_threads_mode_t mode = BC_SEEK_THREADS_MODE_EXPLICIT;
    size_t count = 99;
    assert_true(bc_seek_cli_bind_global_threads(fixture->store, &mode, &count));
    assert_int_equal(mode, BC_SEEK_THREADS_MODE_AUTO);
    assert_int_equal(count, 0);
}

static void test_bind_global_threads_mono(void** state)
{
    fixture_t* fixture = *state;
    const char* argv[] = {"bc-seek", "--threads=0", "find", "."};
    bc_runtime_cli_parsed_t parsed;
    assert_int_equal(parse_into(fixture, argv, 4, &parsed), BC_RUNTIME_CLI_PARSE_OK);
    bc_seek_threads_mode_t mode = BC_SEEK_THREADS_MODE_AUTO;
    size_t count = 99;
    assert_true(bc_seek_cli_bind_global_threads(fixture->store, &mode, &count));
    assert_int_equal(mode, BC_SEEK_THREADS_MODE_MONO);
}

static void test_bind_global_threads_explicit(void** state)
{
    fixture_t* fixture = *state;
    const char* argv[] = {"bc-seek", "--threads=4", "find", "."};
    bc_runtime_cli_parsed_t parsed;
    assert_int_equal(parse_into(fixture, argv, 4, &parsed), BC_RUNTIME_CLI_PARSE_OK);
    bc_seek_threads_mode_t mode = BC_SEEK_THREADS_MODE_AUTO;
    size_t count = 0;
    assert_true(bc_seek_cli_bind_global_threads(fixture->store, &mode, &count));
    assert_int_equal(mode, BC_SEEK_THREADS_MODE_EXPLICIT);
    assert_int_equal(count, 4);
}

static void test_bind_options_defaults(void** state)
{
    fixture_t* fixture = *state;
    const char* argv[] = {"bc-seek", "find"};
    bc_runtime_cli_parsed_t parsed;
    assert_int_equal(parse_into(fixture, argv, 2, &parsed), BC_RUNTIME_CLI_PARSE_OK);
    bc_seek_cli_options_t options;
    assert_true(bc_seek_cli_bind_options(fixture->store, &parsed, &options));
    assert_false(options.has_name_glob);
    assert_false(options.has_type_filter);
    assert_false(options.has_size_filter);
    assert_false(options.has_mtime_filter);
    assert_false(options.has_perm_filter);
    assert_false(options.has_max_depth);
    assert_int_equal(options.min_depth, 0u);
    assert_false(options.include_hidden);
    assert_true(options.respect_ignore_defaults);
    assert_false(options.follow_symlinks);
    assert_false(options.one_file_system);
    assert_false(options.null_terminated);
    assert_int_equal(options.output_mode, BC_SEEK_OUTPUT_MODE_STDOUT);
}

static void test_bind_options_all_predicates(void** state)
{
    fixture_t* fixture = *state;
    const char* argv[] = {
        "bc-seek",
        "find",
        "--name=*.c",
        "--type=f",
        "--size=+1k",
        "--mtime=-7",
        "--perm=755",
        "--max-depth=3",
        "--min-depth=1",
        "--hidden",
        "--no-ignore",
        "--follow-symlinks",
        "--one-file-system",
        "--null",
        "--output=-",
        "/tmp",
    };
    bc_runtime_cli_parsed_t parsed;
    assert_int_equal(parse_into(fixture, argv, sizeof(argv) / sizeof(argv[0]), &parsed), BC_RUNTIME_CLI_PARSE_OK);
    bc_seek_cli_options_t options;
    assert_true(bc_seek_cli_bind_options(fixture->store, &parsed, &options));
    assert_true(options.has_name_glob);
    assert_string_equal(options.name_glob, "*.c");
    assert_false(options.name_case_insensitive);
    assert_true(options.has_type_filter);
    assert_int_equal(options.type_filter, BC_SEEK_ENTRY_TYPE_FILE);
    assert_true(options.has_size_filter);
    assert_int_equal(options.size_op, BC_SEEK_COMPARE_GREATER);
    assert_int_equal(options.size_threshold_bytes, 1024u);
    assert_true(options.has_mtime_filter);
    assert_int_equal(options.mtime_op, BC_SEEK_COMPARE_LESS);
    assert_int_equal(options.mtime_threshold_seconds_ago, 7 * 86400);
    assert_true(options.has_perm_filter);
    assert_int_equal(options.perm_mask, 0755u);
    assert_true(options.has_max_depth);
    assert_int_equal(options.max_depth, 3u);
    assert_int_equal(options.min_depth, 1u);
    assert_true(options.include_hidden);
    assert_false(options.respect_ignore_defaults);
    assert_true(options.follow_symlinks);
    assert_true(options.one_file_system);
    assert_true(options.null_terminated);
    assert_int_equal(options.output_mode, BC_SEEK_OUTPUT_MODE_STDOUT);
    assert_int_equal(options.positional_argument_count, 1);
}

static void test_bind_options_iname_case_insensitive(void** state)
{
    fixture_t* fixture = *state;
    const char* argv[] = {"bc-seek", "find", "--iname=FOO*", "/tmp"};
    bc_runtime_cli_parsed_t parsed;
    assert_int_equal(parse_into(fixture, argv, 4, &parsed), BC_RUNTIME_CLI_PARSE_OK);
    bc_seek_cli_options_t options;
    assert_true(bc_seek_cli_bind_options(fixture->store, &parsed, &options));
    assert_true(options.has_name_glob);
    assert_true(options.name_case_insensitive);
    assert_string_equal(options.name_glob, "FOO*");
}

static void test_bind_options_name_and_iname_rejected(void** state)
{
    fixture_t* fixture = *state;
    const char* argv[] = {"bc-seek", "find", "--name=a", "--iname=b", "/tmp"};
    bc_runtime_cli_parsed_t parsed;
    assert_int_equal(parse_into(fixture, argv, 5, &parsed), BC_RUNTIME_CLI_PARSE_OK);
    bc_seek_cli_options_t options;
    assert_false(bc_seek_cli_bind_options(fixture->store, &parsed, &options));
}

static void test_bind_options_invalid_type(void** state)
{
    fixture_t* fixture = *state;
    const char* argv[] = {"bc-seek", "find", "--type=b", "/tmp"};
    bc_runtime_cli_parsed_t parsed;
    bc_runtime_cli_parse_status_t status = parse_into(fixture, argv, 4, &parsed);
    assert_int_equal(status, BC_RUNTIME_CLI_PARSE_ERROR);
}

static void test_bind_options_min_exceeds_max(void** state)
{
    fixture_t* fixture = *state;
    const char* argv[] = {"bc-seek", "find", "--max-depth=2", "--min-depth=5", "/tmp"};
    bc_runtime_cli_parsed_t parsed;
    assert_int_equal(parse_into(fixture, argv, 5, &parsed), BC_RUNTIME_CLI_PARSE_OK);
    bc_seek_cli_options_t options;
    assert_false(bc_seek_cli_bind_options(fixture->store, &parsed, &options));
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_bind_global_threads_auto, setup, teardown),
        cmocka_unit_test_setup_teardown(test_bind_global_threads_mono, setup, teardown),
        cmocka_unit_test_setup_teardown(test_bind_global_threads_explicit, setup, teardown),
        cmocka_unit_test_setup_teardown(test_bind_options_defaults, setup, teardown),
        cmocka_unit_test_setup_teardown(test_bind_options_all_predicates, setup, teardown),
        cmocka_unit_test_setup_teardown(test_bind_options_iname_case_insensitive, setup, teardown),
        cmocka_unit_test_setup_teardown(test_bind_options_name_and_iname_rejected, setup, teardown),
        cmocka_unit_test_setup_teardown(test_bind_options_invalid_type, setup, teardown),
        cmocka_unit_test_setup_teardown(test_bind_options_min_exceeds_max, setup, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
