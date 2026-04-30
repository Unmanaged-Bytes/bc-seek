// SPDX-License-Identifier: MIT

#include "bc_io_dirent_reader.h"
#include "bc_seek_discovery_internal.h"
#include "bc_seek_filter_internal.h"
#include "bc_seek_strings_internal.h"

#include "bc_concurrency_signal.h"
#include "bc_core.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define BC_SEEK_DISCOVERY_PATH_CAPACITY ((size_t)4096)

typedef struct bc_seek_walk_context {
    const bc_seek_predicate_t* predicate;
    bc_seek_output_t* output;
    bc_runtime_error_collector_t* errors;
    bc_allocators_context_t* memory_context;
    const bc_concurrency_signal_handler_t* signal_handler;
    bool follow_symlinks;
    bool one_file_system;
    dev_t root_device;
    bool root_device_resolved;
    bool stop_requested;
} bc_seek_walk_context_t;

static bool bc_seek_walk_should_stop(bc_seek_walk_context_t* context)
{
    if (context->stop_requested) {
        return true;
    }
    if (context->signal_handler != NULL) {
        bool should_stop = false;
        if (bc_concurrency_signal_handler_should_stop(context->signal_handler, &should_stop) && should_stop) {
            context->stop_requested = true;
            return true;
        }
    }
    return false;
}

static bool bc_seek_walk_append_path(char* buffer, size_t buffer_capacity, size_t* path_length, const char* name, size_t name_length)
{
    size_t current_length = *path_length;
    bool needs_separator = current_length > 0 && buffer[current_length - 1] != '/';
    size_t additional = (needs_separator ? 1u : 0u) + name_length;
    if (current_length + additional + 1u > buffer_capacity) {
        return false;
    }
    if (needs_separator) {
        buffer[current_length] = '/';
        current_length += 1u;
    }
    bc_core_copy(buffer + current_length, name, name_length);
    current_length += name_length;
    buffer[current_length] = '\0';
    *path_length = current_length;
    return true;
}

static void bc_seek_walk_truncate(char* buffer, size_t* path_length, size_t original_length)
{
    *path_length = original_length;
    buffer[original_length] = '\0';
}

static bool bc_seek_walk_is_hidden_name(const char* name)
{
    return name[0] == '.' && name[1] != '\0';
}

static bc_seek_entry_type_t bc_seek_walk_type_from_dtype(unsigned char d_type, bool* out_resolved)
{
    switch (d_type) {
        case DT_REG:
            *out_resolved = true;
            return BC_SEEK_ENTRY_TYPE_FILE;
        case DT_DIR:
            *out_resolved = true;
            return BC_SEEK_ENTRY_TYPE_DIRECTORY;
        case DT_LNK:
            *out_resolved = true;
            return BC_SEEK_ENTRY_TYPE_SYMLINK;
        case DT_UNKNOWN:
        default:
            *out_resolved = false;
            return BC_SEEK_ENTRY_TYPE_ANY;
    }
}

static bool bc_seek_walk_process_directory(bc_seek_walk_context_t* context, int parent_directory_fd, char* path_buffer, size_t path_length,
                                           size_t depth);

static bool bc_seek_walk_emit_if_match(bc_seek_walk_context_t* context, bc_seek_candidate_t* candidate)
{
    bc_seek_filter_decision_t decision = bc_seek_filter_evaluate(context->predicate, candidate);
    if (decision != BC_SEEK_FILTER_DECISION_ACCEPT) {
        return true;
    }
    return bc_seek_output_emit(context->output, candidate->path, candidate->path_length);
}

static bool bc_seek_walk_should_descend_directory(const bc_seek_walk_context_t* context, const char* name, size_t name_length, size_t child_depth)
{
    if (context->predicate->has_max_depth && child_depth > context->predicate->max_depth) {
        return false;
    }
    if (context->predicate->respect_ignore_defaults && bc_seek_filter_ignored_directory_name(name, name_length)) {
        return false;
    }
    if (!context->predicate->include_hidden && bc_seek_walk_is_hidden_name(name)) {
        return false;
    }
    return true;
}

static bool bc_seek_walk_open_subdir(const bc_seek_walk_context_t* context, int parent_directory_fd, const char* name, int* out_fd)
{
    int open_flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
    if (!context->follow_symlinks) {
        open_flags |= O_NOFOLLOW;
    }
    int fd = openat(parent_directory_fd, name, open_flags);
    if (fd < 0) {
        return false;
    }
    if (context->one_file_system && context->root_device_resolved) {
        struct stat sub_stat;
        if (fstat(fd, &sub_stat) != 0) {
            close(fd);
            return false;
        }
        if (sub_stat.st_dev != context->root_device) {
            close(fd);
            return false;
        }
    }
    *out_fd = fd;
    return true;
}

static bool bc_seek_walk_iterate_entries(bc_seek_walk_context_t* context, int directory_fd, char* path_buffer, size_t path_length, size_t depth)
{
    bc_io_dirent_reader_t* reader = NULL;
    if (!bc_io_dirent_reader_create(context->memory_context, directory_fd, &reader)) {
        bc_runtime_error_collector_append(context->errors, context->memory_context, path_buffer, NULL, ENOMEM);
        return false;
    }

    bool ok = true;
    for (;;) {
        if (bc_seek_walk_should_stop(context)) {
            ok = false;
            break;
        }
        bc_io_dirent_entry_t entry;
        bool has_entry = false;
        if (!bc_io_dirent_reader_next(reader, &entry, &has_entry)) {
            int reader_errno = 0;
            bc_io_dirent_reader_last_errno(reader, &reader_errno);
            bc_runtime_error_collector_append(context->errors, context->memory_context, path_buffer, NULL, reader_errno);
            ok = false;
            break;
        }
        if (!has_entry) {
            break;
        }
        if (!context->predicate->include_hidden && bc_seek_walk_is_hidden_name(entry.name)) {
            continue;
        }

        size_t saved_path_length = path_length;
        if (!bc_seek_walk_append_path(path_buffer, BC_SEEK_DISCOVERY_PATH_CAPACITY, &path_length, entry.name, entry.name_length)) {
            bc_seek_walk_truncate(path_buffer, &path_length, saved_path_length);
            continue;
        }

        bool type_resolved = false;
        bc_seek_entry_type_t entry_type = bc_seek_walk_type_from_dtype(entry.d_type, &type_resolved);
        const char* basename_pointer = path_buffer + path_length - entry.name_length;

        bc_seek_candidate_t candidate;
        bc_core_zero(&candidate, sizeof(candidate));
        candidate.path = path_buffer;
        candidate.path_length = path_length;
        candidate.basename = basename_pointer;
        candidate.basename_length = entry.name_length;
        candidate.depth = depth + 1u;
        candidate.entry_type = entry_type;
        candidate.type_resolved = type_resolved;
        candidate.follow_symlinks_enabled = context->follow_symlinks;
        candidate.parent_directory_fd = directory_fd;

        if (!bc_seek_walk_emit_if_match(context, &candidate)) {
            ok = false;
            break;
        }

        bool is_directory = candidate.type_resolved && candidate.entry_type == BC_SEEK_ENTRY_TYPE_DIRECTORY;
        if (!candidate.type_resolved || (context->follow_symlinks && candidate.entry_type == BC_SEEK_ENTRY_TYPE_SYMLINK)) {
            if (bc_seek_filter_populate_stat(&candidate)) {
                is_directory = candidate.entry_type == BC_SEEK_ENTRY_TYPE_DIRECTORY;
            }
        }

        if (is_directory && bc_seek_walk_should_descend_directory(context, entry.name, entry.name_length, candidate.depth)) {
            int subdir_fd = -1;
            if (!bc_seek_walk_open_subdir(context, directory_fd, entry.name, &subdir_fd)) {
                bc_runtime_error_collector_append(context->errors, context->memory_context, path_buffer, NULL, errno);
            } else {
                bool recurse_ok = bc_seek_walk_process_directory(context, subdir_fd, path_buffer, path_length, candidate.depth);
                close(subdir_fd);
                if (!recurse_ok && bc_seek_walk_should_stop(context)) {
                    ok = false;
                    bc_seek_walk_truncate(path_buffer, &path_length, saved_path_length);
                    break;
                }
            }
        }

        bc_seek_walk_truncate(path_buffer, &path_length, saved_path_length);
    }

    bc_io_dirent_reader_destroy(context->memory_context, reader);
    return ok;
}

static bool bc_seek_walk_process_directory(bc_seek_walk_context_t* context, int parent_directory_fd, char* path_buffer, size_t path_length,
                                           size_t depth)
{
    return bc_seek_walk_iterate_entries(context, parent_directory_fd, path_buffer, path_length, depth);
}

static bool bc_seek_walk_visit_root(bc_seek_walk_context_t* context, const char* root_path)
{
    struct stat root_stat;
    int open_flags = O_RDONLY | O_CLOEXEC;
    int stat_flags = context->follow_symlinks ? 0 : AT_SYMLINK_NOFOLLOW;
    if (fstatat(AT_FDCWD, root_path, &root_stat, stat_flags) != 0) {
        bc_runtime_error_collector_append(context->errors, context->memory_context, root_path, NULL, errno);
        return false;
    }

    bc_seek_entry_type_t root_type = BC_SEEK_ENTRY_TYPE_ANY;
    if (S_ISREG(root_stat.st_mode)) {
        root_type = BC_SEEK_ENTRY_TYPE_FILE;
    } else if (S_ISDIR(root_stat.st_mode)) {
        root_type = BC_SEEK_ENTRY_TYPE_DIRECTORY;
    } else if (S_ISLNK(root_stat.st_mode)) {
        root_type = BC_SEEK_ENTRY_TYPE_SYMLINK;
    }

    if (context->one_file_system && !context->root_device_resolved) {
        context->root_device = root_stat.st_dev;
        context->root_device_resolved = true;
    }

    char path_buffer[BC_SEEK_DISCOVERY_PATH_CAPACITY];
    size_t path_length = 0;
    if (!bc_core_length(root_path, '\0', &path_length)) {
        bc_runtime_error_collector_append(context->errors, context->memory_context, root_path, NULL, EINVAL);
        return false;
    }
    if (path_length >= sizeof(path_buffer)) {
        bc_runtime_error_collector_append(context->errors, context->memory_context, root_path, NULL, ENAMETOOLONG);
        return false;
    }
    bc_core_copy(path_buffer, root_path, path_length);
    path_buffer[path_length] = '\0';

    const char* basename_pointer = path_buffer;
    size_t slash_offset = 0;
    if (bc_core_find_last_byte(path_buffer, path_length, '/', &slash_offset)) {
        basename_pointer = path_buffer + slash_offset + 1;
    }
    size_t basename_length = path_length - (size_t)(basename_pointer - path_buffer);

    bc_seek_candidate_t root_candidate;
    bc_core_zero(&root_candidate, sizeof(root_candidate));
    root_candidate.path = path_buffer;
    root_candidate.path_length = path_length;
    root_candidate.basename = basename_pointer;
    root_candidate.basename_length = basename_length;
    root_candidate.depth = 0;
    root_candidate.entry_type = root_type;
    root_candidate.type_resolved = root_type != BC_SEEK_ENTRY_TYPE_ANY;
    root_candidate.size_bytes = root_stat.st_size >= 0 ? (uint64_t)root_stat.st_size : 0u;
    root_candidate.modification_time = root_stat.st_mtime;
    root_candidate.permission_mask = (unsigned int)(root_stat.st_mode & 07777);
    root_candidate.stat_populated = true;
    root_candidate.parent_directory_fd = AT_FDCWD;

    if (!bc_seek_walk_emit_if_match(context, &root_candidate)) {
        return false;
    }

    if (root_type != BC_SEEK_ENTRY_TYPE_DIRECTORY) {
        return true;
    }

    if (context->predicate->has_max_depth && context->predicate->max_depth == 0) {
        return true;
    }

    if (!context->follow_symlinks) {
        open_flags |= O_NOFOLLOW;
    }
    open_flags |= O_DIRECTORY;
    int root_fd = openat(AT_FDCWD, root_path, open_flags);
    if (root_fd < 0) {
        bc_runtime_error_collector_append(context->errors, context->memory_context, root_path, NULL, errno);
        return false;
    }
    bool walk_ok = bc_seek_walk_process_directory(context, root_fd, path_buffer, path_length, 0);
    close(root_fd);
    return walk_ok;
}

bool bc_seek_discovery_walk_sequential(bc_allocators_context_t* memory_context, const bc_seek_predicate_t* predicate,
                                       const char* const* root_paths, size_t root_count, bool follow_symlinks, bool one_file_system,
                                       bc_seek_output_t* output, bc_runtime_error_collector_t* errors,
                                       const bc_concurrency_signal_handler_t* signal_handler)
{
    bc_seek_walk_context_t context;
    bc_core_zero(&context, sizeof(context));
    context.predicate = predicate;
    context.memory_context = memory_context;
    context.output = output;
    context.errors = errors;
    context.signal_handler = signal_handler;
    context.follow_symlinks = follow_symlinks;
    context.one_file_system = one_file_system;
    context.root_device_resolved = false;

    if (root_count == 0) {
        const char* default_root = ".";
        return bc_seek_walk_visit_root(&context, default_root);
    }

    bool any_ok = true;
    for (size_t index = 0; index < root_count; index++) {
        if (bc_seek_walk_should_stop(&context)) {
            break;
        }
        if (!bc_seek_walk_visit_root(&context, root_paths[index])) {
            any_ok = false;
        }
    }
    return any_ok;
}
