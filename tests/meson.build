# SPDX-License-Identifier: MIT

cmocka_dep = dependency('cmocka')

fixtures_directory = meson.current_build_dir() / 'fixtures'

test_bc_seek_cli_parsers_unit = executable('test_bc_seek_cli_parsers_unit',
  'test_bc_seek_cli_parsers_unit.c',
  '../src/cli/bc_seek_cli_parsers.c',
  '../src/strings/bc_seek_strings.c',
  include_directories : [bc_seek_internal_inc],
  dependencies : [cmocka_dep, bc_core_dep, bc_allocators_dep, bc_runtime_dep],
)
test('bc_seek_cli_parsers_unit', test_bc_seek_cli_parsers_unit)

test_bc_seek_cli_spec_unit = executable('test_bc_seek_cli_spec_unit',
  'test_bc_seek_cli_spec_unit.c',
  '../src/cli/bc_seek_cli_spec.c',
  '../src/cli/bc_seek_cli_parsers.c',
  '../src/strings/bc_seek_strings.c',
  include_directories : [bc_seek_internal_inc],
  dependencies : [cmocka_dep, bc_core_dep, bc_allocators_dep, bc_runtime_dep],
)
test('bc_seek_cli_spec_unit', test_bc_seek_cli_spec_unit)

test_bc_seek_filter_unit = executable('test_bc_seek_filter_unit',
  'test_bc_seek_filter_unit.c',
  '../src/filter/bc_seek_filter.c',
  '../src/strings/bc_seek_strings.c',
  include_directories : [bc_seek_internal_inc],
  dependencies : [cmocka_dep, bc_core_dep, bc_allocators_dep],
)
test('bc_seek_filter_unit', test_bc_seek_filter_unit)


test_bc_seek_end_to_end = executable('test_bc_seek_end_to_end',
  'test_bc_seek_end_to_end.c',
  dependencies : [cmocka_dep],
  c_args : [
    '-DBC_SEEK_TEST_BINARY_PATH="' + bc_seek_bin.full_path() + '"',
    '-DBC_SEEK_TEST_FIXTURES_DIRECTORY="' + fixtures_directory + '"',
  ],
)
test('bc_seek_end_to_end', test_bc_seek_end_to_end, depends : [bc_seek_bin], timeout : 120)
