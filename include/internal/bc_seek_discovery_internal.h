// SPDX-License-Identifier: MIT

#ifndef BC_SEEK_DISCOVERY_INTERNAL_H
#define BC_SEEK_DISCOVERY_INTERNAL_H

#include "bc_runtime_error_collector.h"
#include "bc_seek_filter_internal.h"
#include "bc_seek_output_internal.h"

#include "bc_allocators.h"
#include "bc_concurrency.h"
#include "bc_concurrency_signal.h"

#include <stdbool.h>
#include <stddef.h>

bool bc_seek_discovery_walk_sequential(bc_allocators_context_t* memory_context, const bc_seek_predicate_t* predicate,
                                       const char* const* root_paths, size_t root_count, bool follow_symlinks, bool one_file_system,
                                       bc_seek_output_t* output, bc_runtime_error_collector_t* errors,
                                       const bc_concurrency_signal_handler_t* signal_handler);

bool bc_seek_discovery_walk_parallel(bc_allocators_context_t* memory_context, bc_concurrency_context_t* concurrency_context,
                                     const bc_seek_predicate_t* predicate, const char* const* root_paths, size_t root_count,
                                     bool follow_symlinks, bool one_file_system, bc_seek_output_t* output,
                                     bc_runtime_error_collector_t* errors, const bc_concurrency_signal_handler_t* signal_handler);

#endif /* BC_SEEK_DISCOVERY_INTERNAL_H */
