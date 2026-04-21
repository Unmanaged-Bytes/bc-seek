// SPDX-License-Identifier: MIT

#include "bc_seek_error_internal.h"

#include "bc_allocators.h"
#include "bc_allocators_pool.h"
#include "bc_containers_vector.h"
#include "bc_core.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define BC_SEEK_ERROR_COLLECTOR_INITIAL_CAPACITY 32
#define BC_SEEK_ERROR_COLLECTOR_MAX_CAPACITY (1ULL << 22)
#define BC_SEEK_ERROR_COLLECTOR_PATH_BUFFER 512

typedef struct bc_seek_error_record {
    int errno_value;
    char path[BC_SEEK_ERROR_COLLECTOR_PATH_BUFFER];
} bc_seek_error_record_t;

struct bc_seek_error_collector {
    bc_allocators_context_t* memory_context;
    bc_containers_vector_t* records;
    atomic_size_t total_count;
    atomic_flag append_lock;
};

static void bc_seek_error_collector_acquire(bc_seek_error_collector_t* collector)
{
    while (atomic_flag_test_and_set_explicit(&collector->append_lock, memory_order_acquire)) {
    }
}

static void bc_seek_error_collector_release(bc_seek_error_collector_t* collector)
{
    atomic_flag_clear_explicit(&collector->append_lock, memory_order_release);
}

bool bc_seek_error_collector_create(bc_allocators_context_t* memory_context, bc_seek_error_collector_t** out_collector)
{
    bc_seek_error_collector_t* collector = NULL;
    if (!bc_allocators_pool_allocate(memory_context, sizeof(*collector), (void**)&collector)) {
        return false;
    }
    bc_core_zero(collector, sizeof(*collector));
    collector->memory_context = memory_context;
    atomic_store_explicit(&collector->total_count, 0, memory_order_relaxed);
    atomic_flag_clear_explicit(&collector->append_lock, memory_order_relaxed);

    if (!bc_containers_vector_create(memory_context, sizeof(bc_seek_error_record_t), BC_SEEK_ERROR_COLLECTOR_INITIAL_CAPACITY,
                                     BC_SEEK_ERROR_COLLECTOR_MAX_CAPACITY, &collector->records)) {
        bc_allocators_pool_free(memory_context, collector);
        return false;
    }
    *out_collector = collector;
    return true;
}

void bc_seek_error_collector_destroy(bc_allocators_context_t* memory_context, bc_seek_error_collector_t* collector)
{
    if (collector->records != NULL) {
        bc_containers_vector_destroy(memory_context, collector->records);
    }
    bc_allocators_pool_free(memory_context, collector);
}

bool bc_seek_error_collector_append(bc_seek_error_collector_t* collector, const char* path, int errno_value)
{
    bc_seek_error_record_t record;
    bc_core_zero(&record, sizeof(record));
    record.errno_value = errno_value;

    size_t path_length = strlen(path);
    if (path_length >= sizeof(record.path)) {
        path_length = sizeof(record.path) - 1;
    }
    memcpy(record.path, path, path_length);
    record.path[path_length] = '\0';

    bc_seek_error_collector_acquire(collector);
    bool push_ok = bc_containers_vector_push(collector->memory_context, collector->records, &record);
    if (push_ok) {
        atomic_fetch_add_explicit(&collector->total_count, 1, memory_order_relaxed);
    }
    bc_seek_error_collector_release(collector);
    return push_ok;
}

size_t bc_seek_error_collector_count(const bc_seek_error_collector_t* collector)
{
    return atomic_load_explicit(&collector->total_count, memory_order_relaxed);
}

void bc_seek_error_collector_flush_to_stderr(const bc_seek_error_collector_t* collector)
{
    size_t record_count = bc_containers_vector_length(collector->records);
    for (size_t index = 0; index < record_count; index++) {
        bc_seek_error_record_t record;
        if (!bc_containers_vector_get(collector->records, index, &record)) {
            continue;
        }
        const char* error_name = strerror(record.errno_value);
        fprintf(stderr, "bc-seek: %s: %s\n", record.path, error_name != NULL ? error_name : "error");
    }
}
