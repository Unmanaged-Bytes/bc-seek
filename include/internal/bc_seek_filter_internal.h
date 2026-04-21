// SPDX-License-Identifier: MIT

#ifndef BC_SEEK_FILTER_INTERNAL_H
#define BC_SEEK_FILTER_INTERNAL_H

#include "bc_seek_types_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <sys/stat.h>

typedef struct bc_seek_predicate {
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

    bool require_stat;
    time_t evaluation_reference_time;
} bc_seek_predicate_t;

typedef struct bc_seek_candidate {
    const char* path;
    size_t path_length;
    const char* basename;
    size_t basename_length;
    size_t depth;
    bc_seek_entry_type_t entry_type;
    bool type_resolved;
    uint64_t size_bytes;
    time_t modification_time;
    unsigned int permission_mask;
    bool stat_populated;
    int parent_directory_fd;
} bc_seek_candidate_t;

typedef enum {
    BC_SEEK_FILTER_DECISION_REJECT = 0,
    BC_SEEK_FILTER_DECISION_ACCEPT = 1,
    BC_SEEK_FILTER_DECISION_STAT_REQUIRED = 2,
} bc_seek_filter_decision_t;

bool bc_seek_predicate_from_options(const bc_seek_cli_options_t* options, bc_seek_predicate_t* out_predicate);

bc_seek_filter_decision_t bc_seek_filter_evaluate(const bc_seek_predicate_t* predicate, bc_seek_candidate_t* candidate);

bool bc_seek_filter_ignored_directory_name(const char* name, size_t name_length);

bool bc_seek_filter_glob_matches(const char* pattern, const char* value, bool case_insensitive);

bool bc_seek_filter_populate_stat(bc_seek_candidate_t* candidate);

#endif /* BC_SEEK_FILTER_INTERNAL_H */
