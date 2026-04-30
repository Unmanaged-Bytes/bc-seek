// SPDX-License-Identifier: MIT

#ifndef BC_SEEK_CLI_INTERNAL_H
#define BC_SEEK_CLI_INTERNAL_H

#include "bc_seek_types_internal.h"

#include "bc_runtime.h"
#include "bc_runtime_cli.h"

const bc_runtime_cli_program_spec_t* bc_seek_cli_program_spec(void);

bool bc_seek_cli_bind_global_threads(const bc_runtime_config_store_t* store, bc_seek_threads_mode_t* out_mode, size_t* out_worker_count);

bool bc_seek_cli_bind_options(const bc_runtime_config_store_t* store, const bc_runtime_cli_parsed_t* parsed,
                              bc_seek_cli_options_t* out_options);

bool bc_seek_cli_parse_size_filter(const char* value, bc_seek_compare_op_t* out_op, uint64_t* out_threshold_bytes);

bool bc_seek_cli_parse_mtime_filter(const char* value, bc_seek_compare_op_t* out_op, int64_t* out_days);

bool bc_seek_cli_parse_type_filter(const char* value, bc_seek_entry_type_t* out_type);

bool bc_seek_cli_parse_perm_filter(const char* value, unsigned int* out_mask);

bool bc_seek_cli_parse_threads(const char* value, bc_seek_threads_mode_t* out_mode, size_t* out_worker_count);

#endif /* BC_SEEK_CLI_INTERNAL_H */
