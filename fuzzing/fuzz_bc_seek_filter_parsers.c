// SPDX-License-Identifier: MIT

#include "bc_seek_cli_internal.h"
#include "bc_seek_types_internal.h"

#include <bc_core_parse.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size == 0) {
    return 0;
  }

  char value[256];
  const size_t copy_length =
      size < sizeof(value) - 1 ? size : sizeof(value) - 1;
  memcpy(value, data, copy_length);
  value[copy_length] = '\0';

  bc_seek_compare_op_t size_op = 0;
  uint64_t size_threshold = 0;
  (void)bc_seek_cli_parse_size_filter(value, &size_op, &size_threshold);

  bc_seek_compare_op_t mtime_op = 0;
  int64_t mtime_days = 0;
  (void)bc_seek_cli_parse_mtime_filter(value, &mtime_op, &mtime_days);

  bc_seek_entry_type_t entry_type = 0;
  (void)bc_seek_cli_parse_type_filter(value, &entry_type);

  unsigned int perm_mask = 0;
  (void)bc_seek_cli_parse_perm_filter(value, &perm_mask);

  bc_seek_threads_mode_t threads_mode = 0;
  size_t worker_count = 0;
  (void)bc_seek_cli_parse_threads(value, &threads_mode, &worker_count);

  return 0;
}

#ifndef BC_FUZZ_LIBFUZZER
int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <iterations> [seed]\n", argv[0]);
    return 2;
  }
  uint64_t iterations = 0;
  size_t consumed = 0;
  const size_t iterations_length = strlen(argv[1]);
  if (!bc_core_parse_unsigned_integer_64_decimal(argv[1], iterations_length,
                                                 &iterations, &consumed) ||
      consumed != iterations_length) {
    fprintf(stderr, "invalid iterations: %s\n", argv[1]);
    return 2;
  }
  uint64_t seed = 0;
  if (argc >= 3) {
    const size_t seed_length = strlen(argv[2]);
    if (!bc_core_parse_unsigned_integer_64_decimal(argv[2], seed_length, &seed,
                                                   &consumed) ||
        consumed != seed_length) {
      fprintf(stderr, "invalid seed: %s\n", argv[2]);
      return 2;
    }
  }
  srand((unsigned int)seed);

  uint8_t buffer[512];
  for (uint64_t i = 0; i < iterations; i++) {
    const size_t length = (size_t)(rand() % (int)sizeof(buffer));
    for (size_t j = 0; j < length; j++) {
      buffer[j] = (uint8_t)(rand() & 0xFF);
    }
    LLVMFuzzerTestOneInput(buffer, length);
  }
  return 0;
}
#endif
