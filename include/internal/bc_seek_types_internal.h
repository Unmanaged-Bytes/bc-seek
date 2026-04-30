// SPDX-License-Identifier: MIT

#ifndef BC_SEEK_TYPES_INTERNAL_H
#define BC_SEEK_TYPES_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

typedef enum {
    BC_SEEK_ENTRY_TYPE_ANY = 0,
    BC_SEEK_ENTRY_TYPE_FILE = 1,
    BC_SEEK_ENTRY_TYPE_DIRECTORY = 2,
    BC_SEEK_ENTRY_TYPE_SYMLINK = 3,
} bc_seek_entry_type_t;

typedef enum {
    BC_SEEK_COMPARE_EQUAL = 0,
    BC_SEEK_COMPARE_GREATER = 1,
    BC_SEEK_COMPARE_LESS = 2,
} bc_seek_compare_op_t;

typedef enum {
    BC_SEEK_THREADS_MODE_AUTO = 0,
    BC_SEEK_THREADS_MODE_MONO = 1,
    BC_SEEK_THREADS_MODE_EXPLICIT = 2,
    BC_SEEK_THREADS_MODE_IO = 3,
} bc_seek_threads_mode_t;

typedef enum {
    BC_SEEK_OUTPUT_MODE_STDOUT = 0,
    BC_SEEK_OUTPUT_MODE_FILE = 1,
} bc_seek_output_mode_t;

typedef struct bc_seek_cli_options {
    bc_seek_threads_mode_t threads_mode;
    size_t explicit_worker_count;

    bool has_name_glob;
    const char* name_glob;
    bool name_case_insensitive;

    bool has_path_glob;
    const char* path_glob;

    bool has_type_filter;
    bc_seek_entry_type_t type_filter;

    bool has_size_filter;
    bc_seek_compare_op_t size_op;
    uint64_t size_threshold_bytes;

    bool has_mtime_filter;
    bc_seek_compare_op_t mtime_op;
    int64_t mtime_threshold_seconds_ago;

    bool has_newer_reference;
    time_t newer_reference_mtime;

    bool has_perm_filter;
    unsigned int perm_mask;

    bool has_max_depth;
    size_t max_depth;
    size_t min_depth;

    bool include_hidden;
    bool respect_ignore_defaults;
    bool follow_symlinks;
    bool one_file_system;
    bool null_terminated;

    bc_seek_output_mode_t output_mode;
    const char* output_path;

    int positional_argument_count;
    const char* const* positional_argument_values;
} bc_seek_cli_options_t;

#endif /* BC_SEEK_TYPES_INTERNAL_H */
