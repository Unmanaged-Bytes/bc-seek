// SPDX-License-Identifier: MIT

#include "bc_seek_filter_internal.h"

#include <bc_core_parse.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size < 3) {
    return 0;
  }

  const uint8_t pattern_length = data[0];
  const bool case_insensitive = (data[1] & 0x1) != 0;
  size_t position = 2;
  if (position + pattern_length > size) {
    return 0;
  }

  char pattern[256];
  char value[512];
  const size_t pattern_actual = pattern_length < sizeof(pattern) - 1
                                    ? pattern_length
                                    : sizeof(pattern) - 1;
  memcpy(pattern, data + position, pattern_actual);
  pattern[pattern_actual] = '\0';
  position += pattern_length;

  const size_t value_available = size - position;
  const size_t value_actual =
      value_available < sizeof(value) - 1 ? value_available : sizeof(value) - 1;
  memcpy(value, data + position, value_actual);
  value[value_actual] = '\0';

  (void)bc_seek_filter_glob_matches(pattern, value, case_insensitive);
  (void)bc_seek_filter_ignored_directory_name(value, value_actual);

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

  uint8_t buffer[1024];
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
