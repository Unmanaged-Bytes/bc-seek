// SPDX-License-Identifier: MIT

#include "bc_seek_discovery_internal.h"
#include "bc_seek_filter_internal.h"

#include "bc_allocators.h"
#include "bc_allocators_pool.h"
#include "bc_allocators_typed_array.h"
#include "bc_concurrency.h"
#include "bc_concurrency_signal.h"
#include "bc_core.h"
#include "bc_io_walk.h"

BC_TYPED_ARRAY_DEFINE(char, bc_seek_output_bytes)

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#define BC_SEEK_PARALLEL_OUTPUT_INITIAL_CAPACITY ((size_t)(64 * 1024))
#define BC_SEEK_PARALLEL_OUTPUT_MAX_CAPACITY ((size_t)1U << 30)
#define BC_SEEK_PARALLEL_MAX_IOVEC ((size_t)256)

typedef struct bc_seek_parallel_worker_slot {
    bc_seek_output_bytes_t output_bytes;
    size_t emitted_count;
    bool initialized;
    BC_PAD_TO_CACHE_LINE(sizeof(bc_seek_output_bytes_t) + sizeof(size_t) + sizeof(bool));
} bc_seek_parallel_worker_slot_t;

typedef struct bc_seek_parallel_context {
    size_t worker_slot_index;
    bc_allocators_context_t* main_memory_context;
    bc_runtime_error_collector_t* errors;
    const bc_seek_predicate_t* predicate;
    char output_separator;
    bool follow_symlinks;
    bool one_file_system;
    _Atomic unsigned char root_device_initialized;
    _Atomic dev_t root_device;
} bc_seek_parallel_context_t;

static bool bc_seek_parallel_append_match(bc_allocators_context_t* memory_context, bc_seek_output_bytes_t* buffer, const char* path,
                                          size_t path_length, char separator)
{
    if (!bc_seek_output_bytes_append_bulk(memory_context, buffer, path, path_length)) {
        return false;
    }
    if (!bc_seek_output_bytes_push(memory_context, buffer, separator)) {
        return false;
    }
    return true;
}

static bool bc_seek_parallel_ensure_worker_slot(const bc_seek_parallel_context_t* context, bc_allocators_context_t* worker_memory,
                                                bc_seek_parallel_worker_slot_t** out_slot)
{
    bc_seek_parallel_worker_slot_t* slot =
        (bc_seek_parallel_worker_slot_t*)bc_concurrency_worker_slot(context->worker_slot_index);
    if (slot == NULL) {
        return false;
    }
    if (!slot->initialized) {
        bc_core_zero(&slot->output_bytes, sizeof(slot->output_bytes));
        if (!bc_seek_output_bytes_reserve(worker_memory, &slot->output_bytes, BC_SEEK_PARALLEL_OUTPUT_INITIAL_CAPACITY)) {
            return false;
        }
        slot->emitted_count = 0u;
        slot->initialized = true;
    }
    *out_slot = slot;
    return true;
}

static void bc_seek_parallel_record_error(bc_runtime_error_collector_t* errors, bc_allocators_context_t* memory_context, const char* path,
                                          int errno_value)
{
    (void)bc_runtime_error_collector_append(errors, memory_context, path, NULL, errno_value);
}

static bool bc_seek_parallel_is_hidden_name(const char* name)
{
    return name != NULL && name[0] == '.';
}

static bc_seek_entry_type_t bc_seek_parallel_type_from_walk_kind(bc_io_walk_entry_kind_t kind)
{
    switch (kind) {
    case BC_IO_WALK_ENTRY_FILE:
        return BC_SEEK_ENTRY_TYPE_FILE;
    case BC_IO_WALK_ENTRY_DIRECTORY:
        return BC_SEEK_ENTRY_TYPE_DIRECTORY;
    case BC_IO_WALK_ENTRY_SYMLINK:
        return BC_SEEK_ENTRY_TYPE_SYMLINK;
    case BC_IO_WALK_ENTRY_OTHER:
    default:
        return BC_SEEK_ENTRY_TYPE_ANY;
    }
}

static const char* bc_seek_parallel_basename(const char* path, size_t path_length, size_t* out_length)
{
    size_t last_slash_offset = 0;
    if (bc_core_find_last_byte(path, path_length, '/', &last_slash_offset)) {
        const char* basename = path + last_slash_offset + 1;
        *out_length = path_length - (size_t)(basename - path);
        return basename;
    }
    *out_length = path_length;
    return path;
}

/* cppcheck-suppress constParameterCallback; signature fixed by bc_io_walk_visit_fn */
static bool bc_seek_parallel_visit(const bc_io_walk_entry_t* entry, void* user_data)
{
    bc_seek_parallel_context_t* context = (bc_seek_parallel_context_t*)user_data;

    if (context->one_file_system) {
        dev_t expected_root = atomic_load_explicit(&context->root_device, memory_order_acquire);
        if (atomic_load_explicit(&context->root_device_initialized, memory_order_acquire) == 1u
            && entry->device_id != expected_root) {
            return true;
        }
    }

    bc_allocators_context_t* worker_memory = bc_concurrency_worker_memory();
    if (worker_memory == NULL) {
        worker_memory = context->main_memory_context;
    }

    bc_seek_parallel_worker_slot_t* slot = NULL;
    if (!bc_seek_parallel_ensure_worker_slot(context, worker_memory, &slot)) {
        return false;
    }

    size_t basename_length = 0;
    const char* basename = bc_seek_parallel_basename(entry->absolute_path, entry->absolute_path_length, &basename_length);

    bc_seek_candidate_t candidate;
    bc_core_zero(&candidate, sizeof(candidate));
    candidate.path = entry->absolute_path;
    candidate.path_length = entry->absolute_path_length;
    candidate.basename = basename;
    candidate.basename_length = basename_length;
    candidate.depth = entry->depth;
    candidate.entry_type = bc_seek_parallel_type_from_walk_kind(entry->kind);
    candidate.type_resolved = candidate.entry_type != BC_SEEK_ENTRY_TYPE_ANY;
    candidate.size_bytes = (uint64_t)entry->file_size;
    candidate.modification_time = (time_t)entry->modification_time_seconds;
    candidate.permission_mask = entry->permission_mask;
    candidate.stat_populated = entry->stat_populated;
    candidate.follow_symlinks_enabled = context->follow_symlinks;
    candidate.parent_directory_fd = AT_FDCWD;

    bc_seek_filter_decision_t decision = bc_seek_filter_evaluate(context->predicate, &candidate);
    if (decision != BC_SEEK_FILTER_DECISION_ACCEPT) {
        return true;
    }

    if (!bc_seek_parallel_append_match(worker_memory, &slot->output_bytes, entry->absolute_path, entry->absolute_path_length,
                                       context->output_separator)) {
        bc_seek_parallel_record_error(context->errors, context->main_memory_context, entry->absolute_path, ENOMEM);
        return true;
    }
    slot->emitted_count += 1u;
    return true;
}

/* cppcheck-suppress constParameterCallback; signature fixed by bc_io_walk_should_descend_fn */
static bool bc_seek_parallel_should_descend(const bc_io_walk_entry_t* entry, void* user_data)
{
    const bc_seek_parallel_context_t* context = (const bc_seek_parallel_context_t*)user_data;

    if (context->predicate->has_max_depth && entry->depth >= context->predicate->max_depth) {
        return false;
    }

    size_t basename_length = 0;
    const char* basename = bc_seek_parallel_basename(entry->absolute_path, entry->absolute_path_length, &basename_length);

    if (context->predicate->respect_ignore_defaults && bc_seek_filter_ignored_directory_name(basename, basename_length)) {
        return false;
    }
    if (!context->predicate->include_hidden && bc_seek_parallel_is_hidden_name(basename)) {
        return false;
    }

    if (context->one_file_system
        && atomic_load_explicit(&context->root_device_initialized, memory_order_acquire) == 1u
        && entry->device_id != atomic_load_explicit(&context->root_device, memory_order_acquire)) {
        return false;
    }
    return true;
}

/* cppcheck-suppress constParameterCallback; signature fixed by bc_io_walk_error_fn */
static void bc_seek_parallel_on_error(const char* path, const char* stage, int errno_value, void* user_data)
{
    (void)stage;
    const bc_seek_parallel_context_t* context = (const bc_seek_parallel_context_t*)user_data;
    bc_seek_parallel_record_error(context->errors, context->main_memory_context, path, errno_value);
}

typedef struct bc_seek_parallel_iovec_builder {
    struct iovec iov[BC_SEEK_PARALLEL_MAX_IOVEC];
    size_t count;
    size_t total_emitted;
} bc_seek_parallel_iovec_builder_t;

/* cppcheck-suppress constParameterCallback; signature fixed by bc_concurrency_foreach_slot */
static void bc_seek_parallel_collect_slot_iovec(void* slot_data, size_t worker_index, void* arg)
{
    (void)worker_index;
    const bc_seek_parallel_worker_slot_t* slot = (const bc_seek_parallel_worker_slot_t*)slot_data;
    bc_seek_parallel_iovec_builder_t* builder = (bc_seek_parallel_iovec_builder_t*)arg;
    if (!slot->initialized || slot->output_bytes.length == 0u || builder->count >= BC_SEEK_PARALLEL_MAX_IOVEC) {
        return;
    }
    builder->iov[builder->count].iov_base = slot->output_bytes.data;
    builder->iov[builder->count].iov_len = slot->output_bytes.length;
    builder->count += 1u;
    builder->total_emitted += slot->emitted_count;
}

static bool bc_seek_parallel_write_all_iovec(int fd, struct iovec* iov, size_t count)
{
    while (count > 0u) {
        int clamped_count = count > (size_t)IOV_MAX ? (int)IOV_MAX : (int)count;
        ssize_t written = writev(fd, iov, clamped_count);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        size_t remaining = (size_t)written;
        while (remaining > 0u && count > 0u) {
            if (remaining >= iov[0].iov_len) {
                remaining -= iov[0].iov_len;
                iov += 1;
                count -= 1u;
            } else {
                iov[0].iov_base = (char*)iov[0].iov_base + remaining;
                iov[0].iov_len -= remaining;
                remaining = 0u;
            }
        }
    }
    return true;
}

typedef struct bc_seek_parallel_root_result {
    bc_seek_output_bytes_t main_output_buffer;
    size_t main_emitted_count;
} bc_seek_parallel_root_result_t;

static bool bc_seek_parallel_handle_root(bc_seek_parallel_context_t* context, bc_allocators_context_t* memory_context,
                                         bc_concurrency_context_t* concurrency_context,
                                         bc_concurrency_signal_handler_t* signal_handler,
                                         bc_seek_parallel_root_result_t* root_result,
                                         const char* root_path)
{
    struct stat root_stat;
    int stat_flags = context->follow_symlinks ? 0 : AT_SYMLINK_NOFOLLOW;
    if (fstatat(AT_FDCWD, root_path, &root_stat, stat_flags) != 0) {
        bc_seek_parallel_record_error(context->errors, memory_context, root_path, errno);
        return false;
    }

    size_t root_path_length = 0;
    if (!bc_core_length(root_path, '\0', &root_path_length)) {
        bc_seek_parallel_record_error(context->errors, memory_context, root_path, EINVAL);
        return false;
    }
    while (root_path_length > 1 && root_path[root_path_length - 1] == '/') {
        root_path_length -= 1;
    }

    bc_seek_entry_type_t root_type = BC_SEEK_ENTRY_TYPE_ANY;
    if (S_ISREG(root_stat.st_mode)) {
        root_type = BC_SEEK_ENTRY_TYPE_FILE;
    } else if (S_ISDIR(root_stat.st_mode)) {
        root_type = BC_SEEK_ENTRY_TYPE_DIRECTORY;
    } else if (S_ISLNK(root_stat.st_mode)) {
        root_type = BC_SEEK_ENTRY_TYPE_SYMLINK;
    }

    if (context->one_file_system) {
        unsigned char expected = 0u;
        if (atomic_compare_exchange_strong_explicit(&context->root_device_initialized, &expected, 1u, memory_order_acq_rel,
                                                    memory_order_acquire)) {
            atomic_store_explicit(&context->root_device, root_stat.st_dev, memory_order_release);
        }
    }

    size_t basename_length = 0;
    const char* basename = bc_seek_parallel_basename(root_path, root_path_length, &basename_length);

    bc_seek_candidate_t root_candidate;
    bc_core_zero(&root_candidate, sizeof(root_candidate));
    root_candidate.path = root_path;
    root_candidate.path_length = root_path_length;
    root_candidate.basename = basename;
    root_candidate.basename_length = basename_length;
    root_candidate.depth = 0u;
    root_candidate.entry_type = root_type;
    root_candidate.type_resolved = root_type != BC_SEEK_ENTRY_TYPE_ANY;
    root_candidate.size_bytes = root_stat.st_size >= 0 ? (uint64_t)root_stat.st_size : 0u;
    root_candidate.modification_time = root_stat.st_mtime;
    root_candidate.permission_mask = (unsigned int)(root_stat.st_mode & 07777);
    root_candidate.stat_populated = true;
    root_candidate.parent_directory_fd = AT_FDCWD;

    bc_seek_filter_decision_t decision = bc_seek_filter_evaluate(context->predicate, &root_candidate);
    if (decision == BC_SEEK_FILTER_DECISION_ACCEPT) {
        if (!bc_seek_parallel_append_match(memory_context, &root_result->main_output_buffer, root_path, root_path_length,
                                           context->output_separator)) {
            bc_seek_parallel_record_error(context->errors, memory_context, root_path, ENOMEM);
        } else {
            root_result->main_emitted_count += 1u;
        }
    }

    if (root_type != BC_SEEK_ENTRY_TYPE_DIRECTORY) {
        return true;
    }
    if (context->predicate->has_max_depth && context->predicate->max_depth == 0u) {
        return true;
    }

    bc_io_walk_config_t walk_config = {
        .root = root_path,
        .root_length = root_path_length,
        .main_memory_context = memory_context,
        .concurrency_context = concurrency_context,
        .signal_handler = signal_handler,
        .queue_capacity = 0,
        .follow_symlinks = context->follow_symlinks,
        .include_hidden = context->predicate->include_hidden,
        .filter = NULL,
        .filter_user_data = NULL,
        .should_descend = bc_seek_parallel_should_descend,
        .should_descend_user_data = context,
        .visit = bc_seek_parallel_visit,
        .visit_user_data = context,
        .on_error = bc_seek_parallel_on_error,
        .error_user_data = context,
    };
    bc_io_walk_stats_t stats;
    (void)bc_io_walk_parallel(&walk_config, &stats);
    return true;
}

bool bc_seek_discovery_walk_parallel(bc_allocators_context_t* memory_context, bc_concurrency_context_t* concurrency_context,
                                     const bc_seek_predicate_t* predicate, const char* const* root_paths, size_t root_count,
                                     bool follow_symlinks, bool one_file_system, bc_seek_output_t* output,
                                     bc_runtime_error_collector_t* errors, const bc_concurrency_signal_handler_t* signal_handler)
{
    if (concurrency_context == NULL || bc_concurrency_effective_worker_count(concurrency_context) < 2u) {
        return bc_seek_discovery_walk_sequential(memory_context, predicate, root_paths, root_count, follow_symlinks, one_file_system, output,
                                                 errors, signal_handler);
    }

    bc_seek_parallel_context_t context;
    bc_core_zero(&context, sizeof(context));
    context.main_memory_context = memory_context;
    context.errors = errors;
    context.predicate = predicate;
    context.output_separator = output->separator;
    context.follow_symlinks = follow_symlinks;
    context.one_file_system = one_file_system;
    atomic_store_explicit(&context.root_device_initialized, 0u, memory_order_relaxed);
    atomic_store_explicit(&context.root_device, 0, memory_order_relaxed);

    bc_concurrency_slot_config_t slot_config = {
        .size = sizeof(bc_seek_parallel_worker_slot_t),
        .init = NULL,
        .destroy = NULL,
        .arg = NULL,
    };
    if (!bc_concurrency_register_slot(concurrency_context, &slot_config, &context.worker_slot_index)) {
        return false;
    }

    bc_seek_parallel_root_result_t root_result;
    bc_core_zero(&root_result, sizeof(root_result));
    if (!bc_seek_output_bytes_reserve(memory_context, &root_result.main_output_buffer, BC_SEEK_PARALLEL_OUTPUT_INITIAL_CAPACITY)) {
        return false;
    }

    bool any_root_ok = true;
    if (root_count == 0) {
        const char* default_root = ".";
        if (!bc_seek_parallel_handle_root(&context, memory_context, concurrency_context, (bc_concurrency_signal_handler_t*)signal_handler,
                                          &root_result, default_root)) {
            any_root_ok = false;
        }
    } else {
        for (size_t root_index = 0; root_index < root_count; root_index++) {
            if (!bc_seek_parallel_handle_root(&context, memory_context, concurrency_context, (bc_concurrency_signal_handler_t*)signal_handler,
                                              &root_result, root_paths[root_index])) {
                any_root_ok = false;
            }
        }
    }

    bc_seek_parallel_iovec_builder_t builder;
    bc_core_zero(&builder, sizeof(builder));
    if (root_result.main_output_buffer.length > 0u && builder.count < BC_SEEK_PARALLEL_MAX_IOVEC) {
        builder.iov[builder.count].iov_base = root_result.main_output_buffer.data;
        builder.iov[builder.count].iov_len = root_result.main_output_buffer.length;
        builder.count += 1u;
        builder.total_emitted += root_result.main_emitted_count;
    }
    bc_concurrency_foreach_slot(concurrency_context, context.worker_slot_index, bc_seek_parallel_collect_slot_iovec, &builder);

    bool merge_ok = true;
    if (builder.count > 0u) {
        if (!bc_core_writer_flush(&output->writer)) {
            merge_ok = false;
        }
        merge_ok = merge_ok && bc_seek_parallel_write_all_iovec(output->fd, builder.iov, builder.count);
    }
    output->emitted_count += builder.total_emitted;

    bc_seek_output_bytes_destroy(memory_context, &root_result.main_output_buffer);

    return any_root_ok && merge_ok;
}
