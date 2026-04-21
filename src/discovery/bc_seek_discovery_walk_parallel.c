// SPDX-License-Identifier: MIT

#include "bc_io_dirent_reader.h"
#include "bc_seek_discovery_internal.h"
#include "bc_seek_filter_internal.h"

#include "bc_allocators.h"
#include "bc_allocators_pool.h"
#include "bc_allocators_typed_array.h"
#include "bc_concurrency.h"
#include "bc_concurrency_signal.h"
#include "bc_core.h"

BC_TYPED_ARRAY_DEFINE(char, bc_seek_output_bytes)

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#define BC_SEEK_PARALLEL_PATH_CAPACITY ((size_t)4096)
#define BC_SEEK_PARALLEL_QUEUE_CAPACITY ((size_t)16384)
#define BC_SEEK_PARALLEL_OUTPUT_INITIAL_CAPACITY ((size_t)(64 * 1024))
#define BC_SEEK_PARALLEL_OUTPUT_MAX_CAPACITY ((size_t)1U << 30)
#define BC_SEEK_PARALLEL_TERMINATION_SPIN_PAUSES ((int)64)

typedef struct bc_seek_parallel_queue_entry {
    char absolute_path[BC_SEEK_PARALLEL_PATH_CAPACITY];
    size_t absolute_path_length;
    size_t depth;
} bc_seek_parallel_queue_entry_t;

typedef struct bc_seek_parallel_worker_slot {
    bc_seek_output_bytes_t output_bytes;
    size_t emitted_count;
    bool initialized;
    char cache_line_padding[BC_CACHE_LINE_SIZE - ((sizeof(bc_seek_output_bytes_t) + sizeof(size_t) + sizeof(bool)) % BC_CACHE_LINE_SIZE)];
} bc_seek_parallel_worker_slot_t;

typedef struct bc_seek_parallel_shared {
    bc_concurrency_queue_t* directory_queue;
    size_t worker_slot_index;
    bc_allocators_context_t* main_memory_context;
    const bc_concurrency_signal_handler_t* signal_handler;
    const bc_seek_predicate_t* predicate;
    bc_runtime_error_collector_t* errors;
    bc_seek_output_bytes_t main_output_buffer;
    size_t main_emitted_count;
    char output_separator;
    bool follow_symlinks;
    bool one_file_system;
    _Atomic unsigned char root_device_initialized;
    dev_t root_device;

    BC_CACHE_LINE_ALIGNED _Atomic int outstanding_directory_count;
    char outstanding_padding[BC_CACHE_LINE_SIZE - sizeof(_Atomic int)];
} bc_seek_parallel_shared_t;

static bool bc_seek_parallel_should_stop(const bc_seek_parallel_shared_t* shared)
{
    if (shared->signal_handler == NULL) {
        return false;
    }
    bool should_stop = false;
    bc_concurrency_signal_handler_should_stop(shared->signal_handler, &should_stop);
    return should_stop;
}

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

static bool bc_seek_parallel_ensure_worker_slot(const bc_seek_parallel_shared_t* shared, bc_allocators_context_t* worker_memory,
                                                bc_seek_parallel_worker_slot_t** out_slot)
{
    bc_seek_parallel_worker_slot_t* slot = (bc_seek_parallel_worker_slot_t*)bc_concurrency_worker_slot(shared->worker_slot_index);
    if (slot == NULL) {
        return false;
    }
    if (!slot->initialized) {
        if (!bc_seek_output_bytes_reserve(worker_memory, &slot->output_bytes, BC_SEEK_PARALLEL_OUTPUT_INITIAL_CAPACITY)) {
            return false;
        }
        slot->initialized = true;
    }
    *out_slot = slot;
    return true;
}

static void bc_seek_parallel_record_error(bc_runtime_error_collector_t* errors, bc_allocators_context_t* memory_context, const char* path,
                                          int errno_value)
{
    if (errors != NULL) {
        bc_runtime_error_collector_append(errors, memory_context, path, NULL, errno_value);
    }
}

static bool bc_seek_parallel_is_hidden_name(const char* name)
{
    return name[0] == '.' && name[1] != '\0';
}

static bc_seek_entry_type_t bc_seek_parallel_type_from_dtype(unsigned char d_type, bool* out_resolved)
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

static bool bc_seek_parallel_append_path(char* buffer, size_t buffer_capacity, size_t parent_length, const char* name, size_t name_length,
                                         size_t* out_length)
{
    bool needs_separator = parent_length > 0 && buffer[parent_length - 1] != '/';
    size_t required = parent_length + (needs_separator ? 1u : 0u) + name_length + 1u;
    if (required > buffer_capacity) {
        return false;
    }
    size_t cursor = parent_length;
    if (needs_separator) {
        buffer[cursor] = '/';
        cursor += 1u;
    }
    bc_core_copy(buffer + cursor, name, name_length);
    cursor += name_length;
    buffer[cursor] = '\0';
    *out_length = cursor;
    return true;
}

static bool bc_seek_parallel_should_descend_directory(const bc_seek_parallel_shared_t* shared, const char* name, size_t name_length,
                                                      size_t child_depth)
{
    if (shared->predicate->has_max_depth && child_depth > shared->predicate->max_depth) {
        return false;
    }
    if (shared->predicate->respect_ignore_defaults && bc_seek_filter_ignored_directory_name(name, name_length)) {
        return false;
    }
    if (!shared->predicate->include_hidden && bc_seek_parallel_is_hidden_name(name)) {
        return false;
    }
    return true;
}

static void bc_seek_parallel_enqueue_subdirectory(bc_seek_parallel_shared_t* shared, bc_allocators_context_t* worker_memory,
                                                  bc_seek_parallel_worker_slot_t* worker_slot, const char* child_path, size_t child_path_length,
                                                  size_t child_depth);

static void bc_seek_parallel_process_directory(bc_seek_parallel_shared_t* shared, bc_allocators_context_t* worker_memory,
                                               bc_seek_parallel_worker_slot_t* worker_slot, const char* directory_path,
                                               size_t directory_path_length, size_t directory_depth)
{
    int open_flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
    if (!shared->follow_symlinks) {
        open_flags |= O_NOFOLLOW;
    }
    int directory_file_descriptor = open(directory_path, open_flags);
    if (directory_file_descriptor < 0) {
        bc_seek_parallel_record_error(shared->errors, shared->main_memory_context, directory_path, errno);
        return;
    }

    if (shared->one_file_system) {
        struct stat dir_stat;
        if (fstat(directory_file_descriptor, &dir_stat) == 0) {
            unsigned char expected = 0u;
            if (atomic_compare_exchange_strong_explicit(&shared->root_device_initialized, &expected, 1u, memory_order_acq_rel,
                                                        memory_order_acquire)) {
                shared->root_device = dir_stat.st_dev;
            } else if (dir_stat.st_dev != shared->root_device) {
                close(directory_file_descriptor);
                return;
            }
        }
    }

    char child_path_buffer[BC_SEEK_PARALLEL_PATH_CAPACITY];
    if (directory_path_length >= sizeof(child_path_buffer)) {
        close(directory_file_descriptor);
        bc_seek_parallel_record_error(shared->errors, shared->main_memory_context, directory_path, ENAMETOOLONG);
        return;
    }
    bc_core_copy(child_path_buffer, directory_path, directory_path_length);
    child_path_buffer[directory_path_length] = '\0';

    bc_io_dirent_reader_t reader;
    bc_io_dirent_reader_init(&reader, directory_file_descriptor);

    for (;;) {
        if (bc_seek_parallel_should_stop(shared)) {
            break;
        }
        bc_io_dirent_entry_t dir_entry;
        bool has_entry = false;
        if (!bc_io_dirent_reader_next(&reader, &dir_entry, &has_entry)) {
            bc_seek_parallel_record_error(shared->errors, shared->main_memory_context, directory_path, reader.last_errno);
            break;
        }
        if (!has_entry) {
            break;
        }
        const char* entry_name = dir_entry.name;
        if (!shared->predicate->include_hidden && bc_seek_parallel_is_hidden_name(entry_name)) {
            continue;
        }

        size_t entry_name_length = dir_entry.name_length;
        size_t child_path_length = 0;
        if (!bc_seek_parallel_append_path(child_path_buffer, sizeof(child_path_buffer), directory_path_length, entry_name, entry_name_length,
                                          &child_path_length)) {
            bc_seek_parallel_record_error(shared->errors, shared->main_memory_context, directory_path, ENAMETOOLONG);
            continue;
        }

        bool type_resolved = false;
        bc_seek_entry_type_t entry_type = bc_seek_parallel_type_from_dtype(dir_entry.d_type, &type_resolved);

        bc_seek_candidate_t candidate;
        bc_core_zero(&candidate, sizeof(candidate));
        candidate.path = child_path_buffer;
        candidate.path_length = child_path_length;
        candidate.basename = child_path_buffer + child_path_length - entry_name_length;
        candidate.basename_length = entry_name_length;
        candidate.depth = directory_depth + 1u;
        candidate.entry_type = entry_type;
        candidate.type_resolved = type_resolved;
        candidate.follow_symlinks_enabled = shared->follow_symlinks;
        candidate.parent_directory_fd = directory_file_descriptor;

        bc_seek_filter_decision_t decision = bc_seek_filter_evaluate(shared->predicate, &candidate);
        if (decision == BC_SEEK_FILTER_DECISION_ACCEPT) {
            if (!bc_seek_parallel_append_match(worker_memory, &worker_slot->output_bytes, child_path_buffer, child_path_length,
                                               shared->output_separator)) {
                bc_seek_parallel_record_error(shared->errors, shared->main_memory_context, child_path_buffer, ENOMEM);
            } else {
                worker_slot->emitted_count += 1u;
            }
        }

        bool is_directory = candidate.type_resolved && candidate.entry_type == BC_SEEK_ENTRY_TYPE_DIRECTORY;
        if (!candidate.type_resolved || (shared->follow_symlinks && candidate.entry_type == BC_SEEK_ENTRY_TYPE_SYMLINK)) {
            if (bc_seek_filter_populate_stat(&candidate)) {
                is_directory = candidate.entry_type == BC_SEEK_ENTRY_TYPE_DIRECTORY;
            }
        }

        if (is_directory && bc_seek_parallel_should_descend_directory(shared, entry_name, entry_name_length, candidate.depth)) {
            bc_seek_parallel_enqueue_subdirectory(shared, worker_memory, worker_slot, child_path_buffer, child_path_length, candidate.depth);
        }

        child_path_buffer[directory_path_length] = '\0';
    }

    close(directory_file_descriptor);
}

static void bc_seek_parallel_enqueue_subdirectory(bc_seek_parallel_shared_t* shared, bc_allocators_context_t* worker_memory,
                                                  bc_seek_parallel_worker_slot_t* worker_slot, const char* child_path, size_t child_path_length,
                                                  size_t child_depth)
{
    bc_seek_parallel_queue_entry_t queue_entry;
    bc_core_zero(&queue_entry, sizeof(queue_entry));
    if (child_path_length >= sizeof(queue_entry.absolute_path)) {
        bc_seek_parallel_record_error(shared->errors, shared->main_memory_context, child_path, ENAMETOOLONG);
        return;
    }
    memcpy(queue_entry.absolute_path, child_path, child_path_length);
    queue_entry.absolute_path[child_path_length] = '\0';
    queue_entry.absolute_path_length = child_path_length;
    queue_entry.depth = child_depth;

    atomic_fetch_add_explicit(&shared->outstanding_directory_count, 1, memory_order_relaxed);
    if (!bc_concurrency_queue_push(shared->directory_queue, &queue_entry)) {
        atomic_fetch_sub_explicit(&shared->outstanding_directory_count, 1, memory_order_relaxed);
        bc_seek_parallel_process_directory(shared, worker_memory, worker_slot, child_path, child_path_length, child_depth);
    }
}

static void bc_seek_parallel_worker_task(void* task_argument)
{
    bc_seek_parallel_shared_t* shared = (bc_seek_parallel_shared_t*)task_argument;
    bc_allocators_context_t* worker_memory = bc_concurrency_worker_memory();
    if (worker_memory == NULL) {
        worker_memory = shared->main_memory_context;
    }
    bc_seek_parallel_worker_slot_t* worker_slot = NULL;
    if (!bc_seek_parallel_ensure_worker_slot(shared, worker_memory, &worker_slot)) {
        return;
    }

    for (;;) {
        if (bc_seek_parallel_should_stop(shared)) {
            return;
        }
        bc_seek_parallel_queue_entry_t queue_entry;
        if (bc_concurrency_queue_pop(shared->directory_queue, &queue_entry)) {
            bc_seek_parallel_process_directory(shared, worker_memory, worker_slot, queue_entry.absolute_path,
                                               queue_entry.absolute_path_length, queue_entry.depth);
            atomic_fetch_sub_explicit(&shared->outstanding_directory_count, 1, memory_order_release);
            continue;
        }
        int outstanding = atomic_load_explicit(&shared->outstanding_directory_count, memory_order_acquire);
        if (outstanding == 0) {
            return;
        }
        for (int spin = 0; spin < BC_SEEK_PARALLEL_TERMINATION_SPIN_PAUSES; ++spin) {
            bc_core_spin_pause();
        }
    }
}

#define BC_SEEK_PARALLEL_MAX_IOVEC ((size_t)256)

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

static bool bc_seek_parallel_handle_root(bc_seek_parallel_shared_t* shared, const char* root_path)
{
    struct stat root_stat;
    int stat_flags = shared->follow_symlinks ? 0 : AT_SYMLINK_NOFOLLOW;
    if (fstatat(AT_FDCWD, root_path, &root_stat, stat_flags) != 0) {
        bc_seek_parallel_record_error(shared->errors, shared->main_memory_context, root_path, errno);
        return false;
    }

    size_t root_path_length = 0;
    if (!bc_core_length(root_path, '\0', &root_path_length)) {
        bc_seek_parallel_record_error(shared->errors, shared->main_memory_context, root_path, EINVAL);
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

    if (shared->one_file_system) {
        unsigned char expected = 0u;
        if (atomic_compare_exchange_strong_explicit(&shared->root_device_initialized, &expected, 1u, memory_order_acq_rel,
                                                    memory_order_acquire)) {
            shared->root_device = root_stat.st_dev;
        }
    }

    bc_seek_candidate_t root_candidate;
    bc_core_zero(&root_candidate, sizeof(root_candidate));
    root_candidate.path = root_path;
    root_candidate.path_length = root_path_length;
    const char* basename_pointer = root_path;
    size_t last_slash_offset = 0;
    if (bc_core_find_last_byte(root_path, root_path_length, '/', &last_slash_offset)) {
        basename_pointer = root_path + last_slash_offset + 1;
    }
    root_candidate.basename = basename_pointer;
    root_candidate.basename_length = root_path_length - (size_t)(basename_pointer - root_path);
    root_candidate.depth = 0u;
    root_candidate.entry_type = root_type;
    root_candidate.type_resolved = root_type != BC_SEEK_ENTRY_TYPE_ANY;
    root_candidate.size_bytes = root_stat.st_size >= 0 ? (uint64_t)root_stat.st_size : 0u;
    root_candidate.modification_time = root_stat.st_mtime;
    root_candidate.permission_mask = (unsigned int)(root_stat.st_mode & 07777);
    root_candidate.stat_populated = true;
    root_candidate.parent_directory_fd = AT_FDCWD;

    bc_seek_filter_decision_t decision = bc_seek_filter_evaluate(shared->predicate, &root_candidate);
    if (decision == BC_SEEK_FILTER_DECISION_ACCEPT) {
        if (!bc_seek_parallel_append_match(shared->main_memory_context, &shared->main_output_buffer, root_path, root_path_length,
                                           shared->output_separator)) {
            bc_seek_parallel_record_error(shared->errors, shared->main_memory_context, root_path, ENOMEM);
        } else {
            shared->main_emitted_count += 1u;
        }
    }

    if (root_type != BC_SEEK_ENTRY_TYPE_DIRECTORY) {
        return true;
    }
    if (shared->predicate->has_max_depth && shared->predicate->max_depth == 0u) {
        return true;
    }

    if (root_path_length >= BC_SEEK_PARALLEL_PATH_CAPACITY) {
        bc_seek_parallel_record_error(shared->errors, shared->main_memory_context, root_path, ENAMETOOLONG);
        return false;
    }

    bc_seek_parallel_queue_entry_t queue_entry;
    bc_core_zero(&queue_entry, sizeof(queue_entry));
    memcpy(queue_entry.absolute_path, root_path, root_path_length);
    queue_entry.absolute_path[root_path_length] = '\0';
    queue_entry.absolute_path_length = root_path_length;
    queue_entry.depth = 0u;

    atomic_fetch_add_explicit(&shared->outstanding_directory_count, 1, memory_order_relaxed);
    if (!bc_concurrency_queue_push(shared->directory_queue, &queue_entry)) {
        atomic_fetch_sub_explicit(&shared->outstanding_directory_count, 1, memory_order_relaxed);
        bc_seek_parallel_record_error(shared->errors, shared->main_memory_context, root_path, ENOSPC);
        return false;
    }
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

    bc_seek_parallel_shared_t shared;
    bc_core_zero(&shared, sizeof(shared));
    shared.main_memory_context = memory_context;
    shared.signal_handler = signal_handler;
    shared.predicate = predicate;
    shared.errors = errors;
    shared.follow_symlinks = follow_symlinks;
    shared.one_file_system = one_file_system;
    shared.output_separator = output->separator;
    atomic_store_explicit(&shared.outstanding_directory_count, 0, memory_order_relaxed);
    atomic_store_explicit(&shared.root_device_initialized, 0u, memory_order_relaxed);

    if (!bc_seek_output_bytes_reserve(memory_context, &shared.main_output_buffer, BC_SEEK_PARALLEL_OUTPUT_INITIAL_CAPACITY)) {
        return false;
    }

    if (!bc_concurrency_queue_create(memory_context, sizeof(bc_seek_parallel_queue_entry_t), BC_SEEK_PARALLEL_QUEUE_CAPACITY,
                                     &shared.directory_queue)) {
        bc_seek_output_bytes_destroy(memory_context, &shared.main_output_buffer);
        return false;
    }

    bc_concurrency_slot_config_t slot_config = {
        .size = sizeof(bc_seek_parallel_worker_slot_t),
        .init = NULL,
        .destroy = NULL,
        .arg = NULL,
    };
    if (!bc_concurrency_register_slot(concurrency_context, &slot_config, &shared.worker_slot_index)) {
        bc_concurrency_queue_destroy(shared.directory_queue);
        bc_seek_output_bytes_destroy(memory_context, &shared.main_output_buffer);
        return false;
    }

    bool any_root_ok = true;
    if (root_count == 0) {
        const char* default_root = ".";
        if (!bc_seek_parallel_handle_root(&shared, default_root)) {
            any_root_ok = false;
        }
    } else {
        for (size_t root_index = 0; root_index < root_count; root_index++) {
            if (!bc_seek_parallel_handle_root(&shared, root_paths[root_index])) {
                any_root_ok = false;
            }
        }
    }

    size_t effective_worker_count = bc_concurrency_effective_worker_count(concurrency_context);
    for (size_t worker_index = 0; worker_index < effective_worker_count; worker_index++) {
        bc_concurrency_submit(concurrency_context, bc_seek_parallel_worker_task, &shared);
    }
    bc_concurrency_dispatch_and_wait(concurrency_context);

    bc_seek_parallel_iovec_builder_t builder;
    bc_core_zero(&builder, sizeof(builder));
    if (shared.main_output_buffer.length > 0u && builder.count < BC_SEEK_PARALLEL_MAX_IOVEC) {
        builder.iov[builder.count].iov_base = shared.main_output_buffer.data;
        builder.iov[builder.count].iov_len = shared.main_output_buffer.length;
        builder.count += 1u;
        builder.total_emitted += shared.main_emitted_count;
    }
    bc_concurrency_foreach_slot(concurrency_context, shared.worker_slot_index, bc_seek_parallel_collect_slot_iovec, &builder);

    bool merge_ok = true;
    if (builder.count > 0u) {
        fflush(output->stream);
        merge_ok = bc_seek_parallel_write_all_iovec(fileno(output->stream), builder.iov, builder.count);
    }
    output->emitted_count += builder.total_emitted;

    bc_seek_output_bytes_destroy(memory_context, &shared.main_output_buffer);
    bc_concurrency_queue_destroy(shared.directory_queue);

    return any_root_ok && merge_ok;
}
