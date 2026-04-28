// SPDX-License-Identifier: MIT

#include "bc_seek_cli_internal.h"
#include "bc_seek_strings_internal.h"

#include "bc_core.h"
#include "bc_core_io.h"
#include "bc_runtime.h"
#include "bc_runtime_cli.h"

#ifndef BC_SEEK_VERSION_STRING
#define BC_SEEK_VERSION_STRING "0.0.0-unversioned"
#endif

static void bc_seek_cli_emit_stderr(const char* prefix, const char* middle, const char* value, const char* suffix)
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
    if (value != NULL) {
        (void)bc_core_writer_write_cstring(&writer, value);
    }
    if (suffix != NULL) {
        (void)bc_core_writer_write_cstring(&writer, suffix);
    }
    (void)bc_core_writer_flush(&writer);
    (void)bc_core_writer_destroy(&writer);
}

static const char* const bc_seek_type_allowed_values[] = {"f", "d", "l", "file", "dir", "link", "symlink", NULL};

static const bc_runtime_cli_option_spec_t bc_seek_global_options[] = {
    {
        .long_name = "threads",
        .type = BC_RUNTIME_CLI_OPTION_STRING,
        .allowed_values = NULL,
        .default_value = "auto",
        .required = false,
        .value_placeholder = "mono|auto|io|N",
        .help_summary = "thread mode: mono (single-thread, alias 0), auto (CPU-bound, physical cores - 1, default), io (I/O-bound, logical processors - 1, oversubscribe), or N (1..logical_cpu_count)",
    },
};

static const bc_runtime_cli_option_spec_t bc_seek_find_options[] = {
    {
        .long_name = "name",
        .type = BC_RUNTIME_CLI_OPTION_STRING,
        .allowed_values = NULL,
        .default_value = NULL,
        .required = false,
        .value_placeholder = "GLOB",
        .help_summary = "match entries whose basename matches the shell glob",
    },
    {
        .long_name = "iname",
        .type = BC_RUNTIME_CLI_OPTION_STRING,
        .allowed_values = NULL,
        .default_value = NULL,
        .required = false,
        .value_placeholder = "GLOB",
        .help_summary = "like --name but case-insensitive",
    },
    {
        .long_name = "path",
        .type = BC_RUNTIME_CLI_OPTION_STRING,
        .allowed_values = NULL,
        .default_value = NULL,
        .required = false,
        .value_placeholder = "GLOB",
        .help_summary = "match entries whose full path matches the shell glob",
    },
    {
        .long_name = "type",
        .type = BC_RUNTIME_CLI_OPTION_ENUM,
        .allowed_values = bc_seek_type_allowed_values,
        .default_value = NULL,
        .required = false,
        .value_placeholder = "f|d|l",
        .help_summary = "filter by entry type",
    },
    {
        .long_name = "size",
        .type = BC_RUNTIME_CLI_OPTION_STRING,
        .allowed_values = NULL,
        .default_value = NULL,
        .required = false,
        .value_placeholder = "+N|-N|N[cwbkMGT]",
        .help_summary = "filter by size (find(1) syntax; default unit: 512-byte blocks)",
    },
    {
        .long_name = "mtime",
        .type = BC_RUNTIME_CLI_OPTION_STRING,
        .allowed_values = NULL,
        .default_value = NULL,
        .required = false,
        .value_placeholder = "+N|-N|N",
        .help_summary = "filter by modification age in 24h units (find(1) semantics)",
    },
    {
        .long_name = "newer",
        .type = BC_RUNTIME_CLI_OPTION_STRING,
        .allowed_values = NULL,
        .default_value = NULL,
        .required = false,
        .value_placeholder = "PATH",
        .help_summary = "match entries modified more recently than the reference file",
    },
    {
        .long_name = "perm",
        .type = BC_RUNTIME_CLI_OPTION_STRING,
        .allowed_values = NULL,
        .default_value = NULL,
        .required = false,
        .value_placeholder = "OCTAL",
        .help_summary = "match entries whose permission bits equal the octal mask",
    },
    {
        .long_name = "max-depth",
        .type = BC_RUNTIME_CLI_OPTION_INTEGER,
        .allowed_values = NULL,
        .default_value = NULL,
        .required = false,
        .value_placeholder = "N",
        .help_summary = "descend at most N levels below each root",
    },
    {
        .long_name = "min-depth",
        .type = BC_RUNTIME_CLI_OPTION_INTEGER,
        .allowed_values = NULL,
        .default_value = "0",
        .required = false,
        .value_placeholder = "N",
        .help_summary = "do not emit entries shallower than N",
    },
    {
        .long_name = "hidden",
        .type = BC_RUNTIME_CLI_OPTION_FLAG,
        .allowed_values = NULL,
        .default_value = NULL,
        .required = false,
        .value_placeholder = NULL,
        .help_summary = "include entries whose basename starts with a dot",
    },
    {
        .long_name = "no-ignore",
        .type = BC_RUNTIME_CLI_OPTION_FLAG,
        .allowed_values = NULL,
        .default_value = NULL,
        .required = false,
        .value_placeholder = NULL,
        .help_summary = "do not prune well-known directories (.git, node_modules, ...)",
    },
    {
        .long_name = "follow-symlinks",
        .type = BC_RUNTIME_CLI_OPTION_FLAG,
        .allowed_values = NULL,
        .default_value = NULL,
        .required = false,
        .value_placeholder = NULL,
        .help_summary = "follow symbolic links during descent",
    },
    {
        .long_name = "one-file-system",
        .type = BC_RUNTIME_CLI_OPTION_FLAG,
        .allowed_values = NULL,
        .default_value = NULL,
        .required = false,
        .value_placeholder = NULL,
        .help_summary = "do not cross filesystem boundaries",
    },
    {
        .long_name = "null",
        .type = BC_RUNTIME_CLI_OPTION_FLAG,
        .allowed_values = NULL,
        .default_value = NULL,
        .required = false,
        .value_placeholder = NULL,
        .help_summary = "separate matches with a NUL byte instead of a newline",
    },
    {
        .long_name = "output",
        .type = BC_RUNTIME_CLI_OPTION_STRING,
        .allowed_values = NULL,
        .default_value = "-",
        .required = false,
        .value_placeholder = "-|PATH",
        .help_summary = "output destination (- writes to stdout)",
    },
};

static const bc_runtime_cli_command_spec_t bc_seek_commands[] = {
    {
        .name = "find",
        .summary = "enumerate files matching predicates",
        .options = bc_seek_find_options,
        .option_count = sizeof(bc_seek_find_options) / sizeof(bc_seek_find_options[0]),
        .positional_usage = "[path...]",
        .positional_min = 0,
        .positional_max = 0,
    },
};

static const bc_runtime_cli_program_spec_t bc_seek_program_spec_value = {
    .program_name = "bc-seek",
    .version = BC_SEEK_VERSION_STRING,
    .summary = "parallel hardware-saturating file search",
    .global_options = bc_seek_global_options,
    .global_option_count = sizeof(bc_seek_global_options) / sizeof(bc_seek_global_options[0]),
    .commands = bc_seek_commands,
    .command_count = sizeof(bc_seek_commands) / sizeof(bc_seek_commands[0]),
};

const bc_runtime_cli_program_spec_t* bc_seek_cli_program_spec(void)
{
    return &bc_seek_program_spec_value;
}

bool bc_seek_cli_bind_global_threads(const bc_runtime_config_store_t* store, bc_seek_threads_mode_t* out_mode, size_t* out_worker_count)
{
    const char* threads_value = NULL;
    if (!bc_runtime_config_store_get_string(store, "global.threads", &threads_value)) {
        bc_seek_cli_emit_stderr("bc-seek: internal error: missing global.threads\n", NULL, NULL, NULL);
        return false;
    }
    if (!bc_seek_cli_parse_threads(threads_value, out_mode, out_worker_count)) {
        bc_seek_cli_emit_stderr("bc-seek: invalid --threads value: '", NULL, threads_value, "'\n");
        return false;
    }
    return true;
}

static bool bc_seek_cli_lookup_string(const bc_runtime_config_store_t* store, const char* key, const char** out_value)
{
    const char* value = NULL;
    if (!bc_runtime_config_store_get_string(store, key, &value)) {
        return false;
    }
    if (value == NULL || value[0] == '\0') {
        return false;
    }
    *out_value = value;
    return true;
}

static bool bc_seek_cli_lookup_integer(const bc_runtime_config_store_t* store, const char* key, long* out_value)
{
    return bc_runtime_config_store_get_integer(store, key, out_value);
}

static bool bc_seek_cli_lookup_boolean(const bc_runtime_config_store_t* store, const char* key, bool* out_value)
{
    return bc_runtime_config_store_get_boolean(store, key, out_value);
}

static bool bc_seek_cli_bind_output(const char* value, bc_seek_output_mode_t* out_mode, const char** out_path)
{
    if (bc_seek_strings_equal(value, "-")) {
        *out_mode = BC_SEEK_OUTPUT_MODE_STDOUT;
        *out_path = NULL;
        return true;
    }
    if (value[0] == '\0') {
        return false;
    }
    *out_mode = BC_SEEK_OUTPUT_MODE_FILE;
    *out_path = value;
    return true;
}

static bool bc_seek_cli_bind_newer_reference(const char* path, time_t* out_mtime);

bool bc_seek_cli_bind_options(const bc_runtime_config_store_t* store, const bc_runtime_cli_parsed_t* parsed, bc_seek_cli_options_t* out_options)
{
    bc_core_zero(out_options, sizeof(*out_options));
    out_options->respect_ignore_defaults = true;

    if (!bc_seek_cli_bind_global_threads(store, &out_options->threads_mode, &out_options->explicit_worker_count)) {
        return false;
    }

    const char* name_value = NULL;
    const char* iname_value = NULL;
    bool has_name = bc_seek_cli_lookup_string(store, "find.name", &name_value);
    bool has_iname = bc_seek_cli_lookup_string(store, "find.iname", &iname_value);
    if (has_name && has_iname) {
        bc_seek_cli_emit_stderr("bc-seek: --name and --iname are mutually exclusive\n", NULL, NULL, NULL);
        return false;
    }
    if (has_name) {
        out_options->has_name_glob = true;
        out_options->name_glob = name_value;
        out_options->name_case_insensitive = false;
    } else if (has_iname) {
        out_options->has_name_glob = true;
        out_options->name_glob = iname_value;
        out_options->name_case_insensitive = true;
    }

    const char* path_value = NULL;
    if (bc_seek_cli_lookup_string(store, "find.path", &path_value)) {
        out_options->has_path_glob = true;
        out_options->path_glob = path_value;
    }

    const char* type_value = NULL;
    if (bc_seek_cli_lookup_string(store, "find.type", &type_value)) {
        if (!bc_seek_cli_parse_type_filter(type_value, &out_options->type_filter)) {
            bc_seek_cli_emit_stderr("bc-seek: invalid --type: '", NULL, type_value, "'\n");
            return false;
        }
        out_options->has_type_filter = true;
    }

    const char* size_value = NULL;
    if (bc_seek_cli_lookup_string(store, "find.size", &size_value)) {
        if (!bc_seek_cli_parse_size_filter(size_value, &out_options->size_op, &out_options->size_threshold_bytes)) {
            bc_seek_cli_emit_stderr("bc-seek: invalid --size: '", NULL, size_value, "'\n");
            return false;
        }
        out_options->has_size_filter = true;
    }

    const char* mtime_value = NULL;
    if (bc_seek_cli_lookup_string(store, "find.mtime", &mtime_value)) {
        int64_t days = 0;
        if (!bc_seek_cli_parse_mtime_filter(mtime_value, &out_options->mtime_op, &days)) {
            bc_seek_cli_emit_stderr("bc-seek: invalid --mtime: '", NULL, mtime_value, "'\n");
            return false;
        }
        if (days > (int64_t)(INT64_MAX / 86400)) {
            bc_seek_cli_emit_stderr("bc-seek: --mtime value out of range: '", NULL, mtime_value, "'\n");
            return false;
        }
        out_options->mtime_threshold_seconds_ago = days * 86400;
        out_options->has_mtime_filter = true;
    }

    const char* newer_value = NULL;
    if (bc_seek_cli_lookup_string(store, "find.newer", &newer_value)) {
        time_t reference_mtime = 0;
        if (!bc_seek_cli_bind_newer_reference(newer_value, &reference_mtime)) {
            bc_seek_cli_emit_stderr("bc-seek: cannot stat --newer reference: '", NULL, newer_value, "'\n");
            return false;
        }
        out_options->has_newer_reference = true;
        out_options->newer_reference_mtime = reference_mtime;
    }

    const char* perm_value = NULL;
    if (bc_seek_cli_lookup_string(store, "find.perm", &perm_value)) {
        if (!bc_seek_cli_parse_perm_filter(perm_value, &out_options->perm_mask)) {
            bc_seek_cli_emit_stderr("bc-seek: invalid --perm: '", NULL, perm_value, "'\n");
            return false;
        }
        out_options->has_perm_filter = true;
    }

    long max_depth_value = -1;
    if (bc_seek_cli_lookup_integer(store, "find.max-depth", &max_depth_value)) {
        if (max_depth_value < 0) {
            bc_seek_cli_emit_stderr("bc-seek: --max-depth must be non-negative\n", NULL, NULL, NULL);
            return false;
        }
        out_options->has_max_depth = true;
        out_options->max_depth = (size_t)max_depth_value;
    }

    long min_depth_value = 0;
    if (bc_seek_cli_lookup_integer(store, "find.min-depth", &min_depth_value)) {
        if (min_depth_value < 0) {
            bc_seek_cli_emit_stderr("bc-seek: --min-depth must be non-negative\n", NULL, NULL, NULL);
            return false;
        }
        out_options->min_depth = (size_t)min_depth_value;
    }

    if (out_options->has_max_depth && out_options->min_depth > out_options->max_depth) {
        bc_seek_cli_emit_stderr("bc-seek: --min-depth cannot exceed --max-depth\n", NULL, NULL, NULL);
        return false;
    }

    bool hidden_flag = false;
    if (bc_seek_cli_lookup_boolean(store, "find.hidden", &hidden_flag)) {
        out_options->include_hidden = hidden_flag;
    }

    bool no_ignore_flag = false;
    if (bc_seek_cli_lookup_boolean(store, "find.no-ignore", &no_ignore_flag)) {
        out_options->respect_ignore_defaults = !no_ignore_flag;
    }

    bool follow_flag = false;
    if (bc_seek_cli_lookup_boolean(store, "find.follow-symlinks", &follow_flag)) {
        out_options->follow_symlinks = follow_flag;
    }

    bool one_fs_flag = false;
    if (bc_seek_cli_lookup_boolean(store, "find.one-file-system", &one_fs_flag)) {
        out_options->one_file_system = one_fs_flag;
    }

    bool null_flag = false;
    if (bc_seek_cli_lookup_boolean(store, "find.null", &null_flag)) {
        out_options->null_terminated = null_flag;
    }

    const char* output_value = NULL;
    if (!bc_runtime_config_store_get_string(store, "find.output", &output_value)) {
        bc_seek_cli_emit_stderr("bc-seek: internal error: missing find.output\n", NULL, NULL, NULL);
        return false;
    }
    if (!bc_seek_cli_bind_output(output_value, &out_options->output_mode, &out_options->output_path)) {
        bc_seek_cli_emit_stderr("bc-seek: invalid --output: '", NULL, output_value, "'\n");
        return false;
    }

    out_options->positional_argument_count = (int)parsed->positional_count;
    out_options->positional_argument_values = parsed->positional_values;
    return true;
}

#include <sys/stat.h>

static bool bc_seek_cli_bind_newer_reference(const char* path, time_t* out_mtime)
{
    struct stat reference_stat;
    if (stat(path, &reference_stat) != 0) {
        return false;
    }
    *out_mtime = reference_stat.st_mtime;
    return true;
}
