// SPDX-License-Identifier: MIT

#ifndef BC_SEEK_OUTPUT_INTERNAL_H
#define BC_SEEK_OUTPUT_INTERNAL_H

#include "bc_core_io.h"

#include <stdbool.h>
#include <stddef.h>

#define BC_SEEK_OUTPUT_BUFFER_BYTES ((size_t)(64 * 1024))

typedef struct bc_seek_output {
  bc_core_writer_t writer;
  char buffer[BC_SEEK_OUTPUT_BUFFER_BYTES];
  int fd;
  bool owns_fd;
  char separator;
  size_t emitted_count;
} bc_seek_output_t;

bool bc_seek_output_open_stdout(bool null_terminated,
                                bc_seek_output_t *out_output);

bool bc_seek_output_open_file(const char *path, bool null_terminated,
                              bc_seek_output_t *out_output);

bool bc_seek_output_emit(bc_seek_output_t *output, const char *path,
                         size_t path_length);

bool bc_seek_output_close(bc_seek_output_t *output);

#endif /* BC_SEEK_OUTPUT_INTERNAL_H */
