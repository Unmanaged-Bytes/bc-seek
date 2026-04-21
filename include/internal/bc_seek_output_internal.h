// SPDX-License-Identifier: MIT

#ifndef BC_SEEK_OUTPUT_INTERNAL_H
#define BC_SEEK_OUTPUT_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef struct bc_seek_output {
    FILE* stream;
    char separator;
    bool owns_stream;
    size_t emitted_count;
} bc_seek_output_t;

bool bc_seek_output_open_stdout(bool null_terminated, bc_seek_output_t* out_output);

bool bc_seek_output_open_file(const char* path, bool null_terminated, bc_seek_output_t* out_output);

bool bc_seek_output_emit(bc_seek_output_t* output, const char* path, size_t path_length);

bool bc_seek_output_close(bc_seek_output_t* output);

#endif /* BC_SEEK_OUTPUT_INTERNAL_H */
