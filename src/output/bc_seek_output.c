// SPDX-License-Identifier: MIT

#include "bc_seek_output_internal.h"

#include "bc_core.h"
#include "bc_core_io.h"

#include <fcntl.h>
#include <unistd.h>

#define BC_SEEK_OUTPUT_EMIT_STACK_CAPACITY ((size_t)4097)

static bool bc_seek_output_attach(bc_seek_output_t *output, int fd,
                                  bool null_terminated, bool owns_fd) {
  bc_core_zero(output, sizeof(*output));
  output->fd = fd;
  output->owns_fd = owns_fd;
  output->separator = null_terminated ? '\0' : '\n';
  output->emitted_count = 0;
  if (!bc_core_writer_init(&output->writer, fd, output->buffer,
                           sizeof(output->buffer))) {
    if (owns_fd) {
      (void)close(fd);
    }
    output->fd = -1;
    return false;
  }
  return true;
}

bool bc_seek_output_open_stdout(bool null_terminated,
                                bc_seek_output_t *out_output) {
  return bc_seek_output_attach(out_output, STDOUT_FILENO, null_terminated,
                               false);
}

bool bc_seek_output_open_file(const char *path, bool null_terminated,
                              bc_seek_output_t *out_output) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) {
    return false;
  }
  return bc_seek_output_attach(out_output, fd, null_terminated, true);
}

bool bc_seek_output_emit(bc_seek_output_t *output, const char *path,
                         size_t path_length) {
  if (path_length + 1u <= BC_SEEK_OUTPUT_EMIT_STACK_CAPACITY) {
    char combined[BC_SEEK_OUTPUT_EMIT_STACK_CAPACITY];
    bc_core_copy(combined, path, path_length);
    combined[path_length] = output->separator;
    if (!bc_core_writer_write_bytes(&output->writer, combined,
                                    path_length + 1u)) {
      return false;
    }
  } else {
    if (!bc_core_writer_write_bytes(&output->writer, path, path_length)) {
      return false;
    }
    if (!bc_core_writer_write_char(&output->writer, output->separator)) {
      return false;
    }
  }
  output->emitted_count += 1;
  return true;
}

bool bc_seek_output_close(bc_seek_output_t *output) {
  if (output->fd < 0) {
    return true;
  }
  bool ok = bc_core_writer_flush(&output->writer);
  (void)bc_core_writer_destroy(&output->writer);
  if (output->owns_fd) {
    if (close(output->fd) != 0) {
      ok = false;
    }
  }
  output->fd = -1;
  return ok;
}
