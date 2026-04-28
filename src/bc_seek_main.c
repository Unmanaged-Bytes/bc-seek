// SPDX-License-Identifier: MIT

#include "bc_seek_cli_internal.h"
#include "bc_seek_discovery_internal.h"
#include "bc_runtime_error_collector.h"
#include "bc_seek_filter_internal.h"
#include "bc_seek_output_internal.h"
#include "bc_seek_types_internal.h"

#include "bc_allocators.h"
#include "bc_concurrency.h"
#include "bc_core.h"
#include "bc_core_io.h"
#include "bc_runtime.h"
#include "bc_runtime_cli.h"

#include <stdio.h>

static void bc_seek_emit_stderr_message(const char* prefix, const char* middle, const char* path, const char* suffix)
{
    char buffer[1024];
    bc_core_writer_t writer;
    if (!bc_core_writer_init_standard_error(&writer, buffer, sizeof(buffer))) {
        return;
    }
    if (prefix != NULL) {
        (void)bc_core_writer_write_cstring(&writer, prefix);
    }
    if (middle != NULL) {
        (void)bc_core_writer_write_cstring(&writer, middle);
    }
    if (path != NULL) {
        (void)bc_core_writer_write_cstring(&writer, path);
    }
    if (suffix != NULL) {
        (void)bc_core_writer_write_cstring(&writer, suffix);
    }
    (void)bc_core_writer_flush(&writer);
    (void)bc_core_writer_destroy(&writer);
}

typedef struct bc_seek_application_state {
    bc_seek_cli_options_t cli_options;
    bc_seek_predicate_t predicate;
    bc_seek_output_t output;
    bool output_opened;
    bc_runtime_error_collector_t* errors;
    int exit_code;
} bc_seek_application_state_t;

static bool bc_seek_application_init(const bc_runtime_t* application, void* user_data)
{
    bc_seek_application_state_t* state = (bc_seek_application_state_t*)user_data;

    bc_allocators_context_t* memory_context = NULL;
    if (!bc_runtime_memory_context(application, &memory_context)) {
        state->exit_code = 1;
        return false;
    }

    if (!bc_runtime_error_collector_create(memory_context, &state->errors)) {
        state->exit_code = 1;
        return false;
    }

    if (!bc_seek_predicate_from_options(&state->cli_options, &state->predicate)) {
        state->exit_code = 1;
        return false;
    }

    if (state->cli_options.output_mode == BC_SEEK_OUTPUT_MODE_STDOUT) {
        if (!bc_seek_output_open_stdout(state->cli_options.null_terminated, &state->output)) {
            bc_seek_emit_stderr_message("bc-seek: cannot configure stdout output\n", NULL, NULL, NULL);
            state->exit_code = 1;
            return false;
        }
    } else {
        if (!bc_seek_output_open_file(state->cli_options.output_path, state->cli_options.null_terminated, &state->output)) {
            bc_seek_emit_stderr_message("bc-seek: cannot open output '", NULL, state->cli_options.output_path, "'\n");
            state->exit_code = 1;
            return false;
        }
    }
    state->output_opened = true;
    return true;
}

static bool bc_seek_application_run(const bc_runtime_t* application, void* user_data)
{
    bc_seek_application_state_t* state = (bc_seek_application_state_t*)user_data;

    bc_allocators_context_t* memory_context = NULL;
    if (!bc_runtime_memory_context(application, &memory_context)) {
        state->exit_code = 1;
        return false;
    }

    bc_concurrency_context_t* concurrency_context = NULL;
    if (!bc_runtime_parallel_context(application, &concurrency_context)) {
        state->exit_code = 1;
        return false;
    }

    bc_concurrency_signal_handler_t* signal_handler = NULL;
    bc_runtime_signal_handler(application, &signal_handler);

    size_t root_count = (size_t)state->cli_options.positional_argument_count;
    const char* const* root_paths = state->cli_options.positional_argument_values;

    bool walk_ok;
    size_t effective_workers = bc_concurrency_effective_worker_count(concurrency_context);
    if (effective_workers >= 2 && state->cli_options.threads_mode != BC_SEEK_THREADS_MODE_MONO) {
        walk_ok = bc_seek_discovery_walk_parallel(memory_context, concurrency_context, &state->predicate, root_paths, root_count,
                                                  state->cli_options.follow_symlinks, state->cli_options.one_file_system, &state->output,
                                                  state->errors, signal_handler);
    } else {
        walk_ok = bc_seek_discovery_walk_sequential(memory_context, &state->predicate, root_paths, root_count,
                                                    state->cli_options.follow_symlinks, state->cli_options.one_file_system, &state->output,
                                                    state->errors, signal_handler);
    }

    bool interrupted = false;
    bc_runtime_should_stop(application, &interrupted);
    if (interrupted) {
        state->exit_code = 130;
        bc_runtime_error_collector_flush_to_stderr(state->errors, "bc-seek");
        return false;
    }

    bc_runtime_error_collector_flush_to_stderr(state->errors, "bc-seek");

    if (!walk_ok && bc_runtime_error_collector_count(state->errors) == 0) {
        state->exit_code = 1;
        return false;
    }
    state->exit_code = bc_runtime_error_collector_count(state->errors) == 0 ? 0 : 1;
    return true;
}

static void bc_seek_application_cleanup(const bc_runtime_t* application, void* user_data)
{
    bc_seek_application_state_t* state = (bc_seek_application_state_t*)user_data;

    if (state->output_opened) {
        bc_seek_output_close(&state->output);
        state->output_opened = false;
    }

    bc_allocators_context_t* memory_context = NULL;
    if (!bc_runtime_memory_context(application, &memory_context)) {
        return;
    }
    if (state->errors != NULL) {
        bc_runtime_error_collector_destroy(memory_context, state->errors);
        state->errors = NULL;
    }
}

int main(int argument_count, char** argument_values)
{
    const bc_runtime_cli_program_spec_t* spec = bc_seek_cli_program_spec();

    bc_allocators_context_config_t cli_memory_config = {.max_pool_memory = 0,
                                                        .tracking_enabled = true,
                                                        .leak_callback = NULL,
                                                        .leak_callback_argument = NULL};
    bc_allocators_context_t* cli_memory_context = NULL;
    if (!bc_allocators_context_create(&cli_memory_config, &cli_memory_context)) {
        bc_seek_emit_stderr_message("bc-seek: failed to initialize CLI memory context\n", NULL, NULL, NULL);
        return 1;
    }

    bc_runtime_config_store_t* cli_store = NULL;
    if (!bc_runtime_config_store_create(cli_memory_context, &cli_store)) {
        bc_seek_emit_stderr_message("bc-seek: failed to initialize CLI config store\n", NULL, NULL, NULL);
        bc_allocators_context_destroy(cli_memory_context);
        return 1;
    }

    bc_runtime_cli_parsed_t parsed;
    bc_runtime_cli_parse_status_t parse_status =
        bc_runtime_cli_parse(spec, argument_count, (const char* const*)argument_values, cli_store, &parsed, stderr);

    if (parse_status == BC_RUNTIME_CLI_PARSE_HELP_GLOBAL) {
        bc_runtime_cli_print_help_global(spec, stdout);
        bc_runtime_config_store_destroy(cli_memory_context, cli_store);
        bc_allocators_context_destroy(cli_memory_context);
        return 0;
    }
    if (parse_status == BC_RUNTIME_CLI_PARSE_HELP_COMMAND) {
        bc_runtime_cli_print_help_command(spec, parsed.command, stdout);
        bc_runtime_config_store_destroy(cli_memory_context, cli_store);
        bc_allocators_context_destroy(cli_memory_context);
        return 0;
    }
    if (parse_status == BC_RUNTIME_CLI_PARSE_VERSION) {
        bc_runtime_cli_print_version(spec, stdout);
        bc_runtime_config_store_destroy(cli_memory_context, cli_store);
        bc_allocators_context_destroy(cli_memory_context);
        return 0;
    }
    if (parse_status == BC_RUNTIME_CLI_PARSE_ERROR) {
        bc_runtime_config_store_destroy(cli_memory_context, cli_store);
        bc_allocators_context_destroy(cli_memory_context);
        return 2;
    }

    bc_seek_application_state_t state;
    bc_core_zero(&state, sizeof(state));
    if (!bc_seek_cli_bind_options(cli_store, &parsed, &state.cli_options)) {
        bc_runtime_config_store_destroy(cli_memory_context, cli_store);
        bc_allocators_context_destroy(cli_memory_context);
        return 2;
    }

    bc_concurrency_config_t parallel_config;
    bc_core_zero(&parallel_config, sizeof(parallel_config));
    if (state.cli_options.threads_mode == BC_SEEK_THREADS_MODE_MONO) {
        parallel_config.worker_count_explicit = true;
        parallel_config.worker_count = 0;
    } else if (state.cli_options.threads_mode == BC_SEEK_THREADS_MODE_EXPLICIT) {
        parallel_config.worker_count_explicit = true;
        parallel_config.worker_count = state.cli_options.explicit_worker_count > 0 ? state.cli_options.explicit_worker_count - 1 : 0;
        size_t logical_processor_count = bc_concurrency_logical_processor_count();
        if (state.cli_options.explicit_worker_count > logical_processor_count) {
            bc_seek_emit_stderr_message("bc-seek: --threads exceeds online logical processors\n", NULL, NULL, NULL);
            bc_runtime_config_store_destroy(cli_memory_context, cli_store);
            bc_allocators_context_destroy(cli_memory_context);
            return 2;
        }
        if (state.cli_options.explicit_worker_count > bc_concurrency_physical_core_count()) {
            parallel_config.allow_oversubscribe = true;
        }
    } else if (state.cli_options.threads_mode == BC_SEEK_THREADS_MODE_AUTO_IO) {
        size_t logical_processor_count = bc_concurrency_logical_processor_count();
        parallel_config.allow_oversubscribe = true;
        parallel_config.worker_count_explicit = true;
        parallel_config.worker_count = logical_processor_count >= 2 ? logical_processor_count - 1 : 0;
    }

    bc_runtime_config_t runtime_config = {
        .max_pool_memory = 0,
        .memory_tracking_enabled = true,
        .log_level = BC_RUNTIME_LOG_LEVEL_WARN,
        .config_file_path = NULL,
        .argument_count = 0,
        .argument_values = NULL,
        .parallel_config = &parallel_config,
    };
    bc_runtime_callbacks_t runtime_callbacks = {
        .init = bc_seek_application_init,
        .cleanup = bc_seek_application_cleanup,
        .run = bc_seek_application_run,
    };

    bc_runtime_t* runtime = NULL;
    if (!bc_runtime_create(&runtime_config, &runtime_callbacks, &state, &runtime)) {
        bc_seek_emit_stderr_message("bc-seek: failed to initialize runtime\n", NULL, NULL, NULL);
        bc_runtime_config_store_destroy(cli_memory_context, cli_store);
        bc_allocators_context_destroy(cli_memory_context);
        return 1;
    }

    bc_runtime_run(runtime);
    bc_runtime_destroy(runtime);

    bc_runtime_config_store_destroy(cli_memory_context, cli_store);
    bc_allocators_context_destroy(cli_memory_context);

    return state.exit_code;
}
