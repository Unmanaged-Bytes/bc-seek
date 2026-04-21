// SPDX-License-Identifier: MIT

#include "bc_seek_discovery_internal.h"

bool bc_seek_discovery_walk_parallel(bc_allocators_context_t* memory_context, bc_concurrency_context_t* concurrency_context,
                                     const bc_seek_predicate_t* predicate, const char* const* root_paths, size_t root_count,
                                     bool follow_symlinks, bool one_file_system, bc_seek_output_t* output,
                                     bc_seek_error_collector_t* errors, const bc_concurrency_signal_handler_t* signal_handler)
{
    (void)concurrency_context;
    return bc_seek_discovery_walk_sequential(memory_context, predicate, root_paths, root_count, follow_symlinks, one_file_system, output,
                                             errors, signal_handler);
}
