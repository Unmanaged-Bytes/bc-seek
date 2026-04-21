// SPDX-License-Identifier: MIT

#include "bc_seek_filter_internal.h"
#include "bc_seek_strings_internal.h"

#include "bc_core.h"

#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static const char* const bc_seek_ignored_directory_names[] = {
    ".git",
    ".hg",
    ".svn",
    ".bzr",
    "CVS",
    ".cache",
    "node_modules",
    "target",
    "__pycache__",
    ".venv",
    ".tox",
    ".mypy_cache",
    ".pytest_cache",
    ".ruff_cache",
    ".ccls-cache",
    NULL,
};

bool bc_seek_filter_ignored_directory_name(const char* name, size_t name_length)
{
    for (size_t index = 0; bc_seek_ignored_directory_names[index] != NULL; index++) {
        const char* candidate = bc_seek_ignored_directory_names[index];
        size_t candidate_length = bc_seek_strings_length(candidate);
        if (candidate_length != name_length) {
            continue;
        }
        if (memcmp(candidate, name, name_length) == 0) {
            return true;
        }
    }
    return false;
}

bool bc_seek_filter_glob_matches(const char* pattern, const char* value, bool case_insensitive)
{
    int flags = 0;
    if (case_insensitive) {
        flags |= FNM_CASEFOLD;
    }
    return fnmatch(pattern, value, flags) == 0;
}

static bool bc_seek_filter_compare(uint64_t left, uint64_t right, bc_seek_compare_op_t op)
{
    switch (op) {
        case BC_SEEK_COMPARE_GREATER:
            return left > right;
        case BC_SEEK_COMPARE_LESS:
            return left < right;
        case BC_SEEK_COMPARE_EQUAL:
        default:
            return left == right;
    }
}

static bool bc_seek_filter_compare_signed(int64_t left, int64_t right, bc_seek_compare_op_t op)
{
    switch (op) {
        case BC_SEEK_COMPARE_GREATER:
            return left > right;
        case BC_SEEK_COMPARE_LESS:
            return left < right;
        case BC_SEEK_COMPARE_EQUAL:
        default:
            return left == right;
    }
}

bool bc_seek_predicate_from_options(const bc_seek_cli_options_t* options, bc_seek_predicate_t* out_predicate)
{
    bc_core_zero(out_predicate, sizeof(*out_predicate));

    out_predicate->has_name_glob = options->has_name_glob;
    out_predicate->name_glob = options->name_glob;
    out_predicate->name_case_insensitive = options->name_case_insensitive;

    out_predicate->has_path_glob = options->has_path_glob;
    out_predicate->path_glob = options->path_glob;

    out_predicate->has_type_filter = options->has_type_filter;
    out_predicate->type_filter = options->type_filter;

    out_predicate->has_size_filter = options->has_size_filter;
    out_predicate->size_op = options->size_op;
    out_predicate->size_threshold_bytes = options->size_threshold_bytes;

    out_predicate->has_mtime_filter = options->has_mtime_filter;
    out_predicate->mtime_op = options->mtime_op;
    out_predicate->mtime_threshold_seconds_ago = options->mtime_threshold_seconds_ago;

    out_predicate->has_newer_reference = options->has_newer_reference;
    out_predicate->newer_reference_mtime = options->newer_reference_mtime;

    out_predicate->has_perm_filter = options->has_perm_filter;
    out_predicate->perm_mask = options->perm_mask;

    out_predicate->has_max_depth = options->has_max_depth;
    out_predicate->max_depth = options->max_depth;
    out_predicate->min_depth = options->min_depth;

    out_predicate->include_hidden = options->include_hidden;
    out_predicate->respect_ignore_defaults = options->respect_ignore_defaults;
    out_predicate->follow_symlinks = options->follow_symlinks;

    out_predicate->require_stat =
        options->has_size_filter || options->has_mtime_filter || options->has_newer_reference || options->has_perm_filter;

    out_predicate->evaluation_reference_time = time(NULL);
    return true;
}

bool bc_seek_filter_populate_stat(bc_seek_candidate_t* candidate)
{
    struct stat entry_stat;
    int flags = candidate->follow_symlinks_enabled ? 0 : AT_SYMLINK_NOFOLLOW;
    int rc = fstatat(candidate->parent_directory_fd, candidate->basename, &entry_stat, flags);
    if (rc != 0) {
        return false;
    }
    if (!candidate->type_resolved || candidate->follow_symlinks_enabled) {
        if (S_ISREG(entry_stat.st_mode)) {
            candidate->entry_type = BC_SEEK_ENTRY_TYPE_FILE;
        } else if (S_ISDIR(entry_stat.st_mode)) {
            candidate->entry_type = BC_SEEK_ENTRY_TYPE_DIRECTORY;
        } else if (S_ISLNK(entry_stat.st_mode)) {
            candidate->entry_type = BC_SEEK_ENTRY_TYPE_SYMLINK;
        } else {
            candidate->entry_type = BC_SEEK_ENTRY_TYPE_ANY;
        }
        candidate->type_resolved = true;
    }
    candidate->size_bytes = entry_stat.st_size >= 0 ? (uint64_t)entry_stat.st_size : 0u;
    candidate->modification_time = entry_stat.st_mtime;
    candidate->permission_mask = (unsigned int)(entry_stat.st_mode & 07777);
    candidate->stat_populated = true;
    return true;
}

static bc_seek_filter_decision_t bc_seek_filter_check_depth(const bc_seek_predicate_t* predicate, const bc_seek_candidate_t* candidate)
{
    if (candidate->depth < predicate->min_depth) {
        return BC_SEEK_FILTER_DECISION_REJECT;
    }
    if (predicate->has_max_depth && candidate->depth > predicate->max_depth) {
        return BC_SEEK_FILTER_DECISION_REJECT;
    }
    return BC_SEEK_FILTER_DECISION_ACCEPT;
}

static bc_seek_filter_decision_t bc_seek_filter_check_type(const bc_seek_predicate_t* predicate, const bc_seek_candidate_t* candidate)
{
    if (!predicate->has_type_filter) {
        return BC_SEEK_FILTER_DECISION_ACCEPT;
    }
    if (!candidate->type_resolved) {
        return BC_SEEK_FILTER_DECISION_STAT_REQUIRED;
    }
    return candidate->entry_type == predicate->type_filter ? BC_SEEK_FILTER_DECISION_ACCEPT : BC_SEEK_FILTER_DECISION_REJECT;
}

static bc_seek_filter_decision_t bc_seek_filter_check_name(const bc_seek_predicate_t* predicate, const bc_seek_candidate_t* candidate)
{
    if (!predicate->has_name_glob) {
        return BC_SEEK_FILTER_DECISION_ACCEPT;
    }
    return bc_seek_filter_glob_matches(predicate->name_glob, candidate->basename, predicate->name_case_insensitive)
               ? BC_SEEK_FILTER_DECISION_ACCEPT
               : BC_SEEK_FILTER_DECISION_REJECT;
}

static bc_seek_filter_decision_t bc_seek_filter_check_path(const bc_seek_predicate_t* predicate, const bc_seek_candidate_t* candidate)
{
    if (!predicate->has_path_glob) {
        return BC_SEEK_FILTER_DECISION_ACCEPT;
    }
    return bc_seek_filter_glob_matches(predicate->path_glob, candidate->path, false)
               ? BC_SEEK_FILTER_DECISION_ACCEPT
               : BC_SEEK_FILTER_DECISION_REJECT;
}

static bc_seek_filter_decision_t bc_seek_filter_check_size(const bc_seek_predicate_t* predicate, const bc_seek_candidate_t* candidate)
{
    if (!predicate->has_size_filter) {
        return BC_SEEK_FILTER_DECISION_ACCEPT;
    }
    if (!candidate->stat_populated) {
        return BC_SEEK_FILTER_DECISION_STAT_REQUIRED;
    }
    return bc_seek_filter_compare(candidate->size_bytes, predicate->size_threshold_bytes, predicate->size_op)
               ? BC_SEEK_FILTER_DECISION_ACCEPT
               : BC_SEEK_FILTER_DECISION_REJECT;
}

static bc_seek_filter_decision_t bc_seek_filter_check_mtime(const bc_seek_predicate_t* predicate, const bc_seek_candidate_t* candidate)
{
    if (!predicate->has_mtime_filter) {
        return BC_SEEK_FILTER_DECISION_ACCEPT;
    }
    if (!candidate->stat_populated) {
        return BC_SEEK_FILTER_DECISION_STAT_REQUIRED;
    }
    int64_t seconds_ago = (int64_t)predicate->evaluation_reference_time - (int64_t)candidate->modification_time;
    int64_t threshold = predicate->mtime_threshold_seconds_ago;
    return bc_seek_filter_compare_signed(seconds_ago, threshold, predicate->mtime_op) ? BC_SEEK_FILTER_DECISION_ACCEPT
                                                                                      : BC_SEEK_FILTER_DECISION_REJECT;
}

static bc_seek_filter_decision_t bc_seek_filter_check_newer(const bc_seek_predicate_t* predicate, const bc_seek_candidate_t* candidate)
{
    if (!predicate->has_newer_reference) {
        return BC_SEEK_FILTER_DECISION_ACCEPT;
    }
    if (!candidate->stat_populated) {
        return BC_SEEK_FILTER_DECISION_STAT_REQUIRED;
    }
    return candidate->modification_time > predicate->newer_reference_mtime ? BC_SEEK_FILTER_DECISION_ACCEPT
                                                                           : BC_SEEK_FILTER_DECISION_REJECT;
}

static bc_seek_filter_decision_t bc_seek_filter_check_perm(const bc_seek_predicate_t* predicate, const bc_seek_candidate_t* candidate)
{
    if (!predicate->has_perm_filter) {
        return BC_SEEK_FILTER_DECISION_ACCEPT;
    }
    if (!candidate->stat_populated) {
        return BC_SEEK_FILTER_DECISION_STAT_REQUIRED;
    }
    return (candidate->permission_mask & 07777u) == (predicate->perm_mask & 07777u) ? BC_SEEK_FILTER_DECISION_ACCEPT
                                                                                    : BC_SEEK_FILTER_DECISION_REJECT;
}

bc_seek_filter_decision_t bc_seek_filter_evaluate(const bc_seek_predicate_t* predicate, bc_seek_candidate_t* candidate)
{
    bc_seek_filter_decision_t decision = bc_seek_filter_check_depth(predicate, candidate);
    if (decision != BC_SEEK_FILTER_DECISION_ACCEPT) {
        return decision;
    }

    decision = bc_seek_filter_check_type(predicate, candidate);
    if (decision == BC_SEEK_FILTER_DECISION_REJECT) {
        return decision;
    }
    if (decision == BC_SEEK_FILTER_DECISION_STAT_REQUIRED) {
        if (!bc_seek_filter_populate_stat(candidate)) {
            return BC_SEEK_FILTER_DECISION_REJECT;
        }
        decision = bc_seek_filter_check_type(predicate, candidate);
        if (decision != BC_SEEK_FILTER_DECISION_ACCEPT) {
            return decision;
        }
    }

    decision = bc_seek_filter_check_name(predicate, candidate);
    if (decision != BC_SEEK_FILTER_DECISION_ACCEPT) {
        return decision;
    }

    decision = bc_seek_filter_check_path(predicate, candidate);
    if (decision != BC_SEEK_FILTER_DECISION_ACCEPT) {
        return decision;
    }

    if (predicate->require_stat && !candidate->stat_populated) {
        if (!bc_seek_filter_populate_stat(candidate)) {
            return BC_SEEK_FILTER_DECISION_REJECT;
        }
    }

    decision = bc_seek_filter_check_size(predicate, candidate);
    if (decision != BC_SEEK_FILTER_DECISION_ACCEPT) {
        return decision;
    }
    decision = bc_seek_filter_check_mtime(predicate, candidate);
    if (decision != BC_SEEK_FILTER_DECISION_ACCEPT) {
        return decision;
    }
    decision = bc_seek_filter_check_newer(predicate, candidate);
    if (decision != BC_SEEK_FILTER_DECISION_ACCEPT) {
        return decision;
    }
    decision = bc_seek_filter_check_perm(predicate, candidate);
    if (decision != BC_SEEK_FILTER_DECISION_ACCEPT) {
        return decision;
    }

    return BC_SEEK_FILTER_DECISION_ACCEPT;
}
