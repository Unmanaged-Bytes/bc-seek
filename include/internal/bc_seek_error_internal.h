// SPDX-License-Identifier: MIT

#ifndef BC_SEEK_ERROR_INTERNAL_H
#define BC_SEEK_ERROR_INTERNAL_H

#include "bc_allocators.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct bc_seek_error_collector bc_seek_error_collector_t;

bool bc_seek_error_collector_create(bc_allocators_context_t* memory_context, bc_seek_error_collector_t** out_collector);

void bc_seek_error_collector_destroy(bc_allocators_context_t* memory_context, bc_seek_error_collector_t* collector);

bool bc_seek_error_collector_append(bc_seek_error_collector_t* collector, const char* path, int errno_value);

size_t bc_seek_error_collector_count(const bc_seek_error_collector_t* collector);

void bc_seek_error_collector_flush_to_stderr(const bc_seek_error_collector_t* collector);

#endif /* BC_SEEK_ERROR_INTERNAL_H */
