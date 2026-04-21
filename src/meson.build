# SPDX-License-Identifier: MIT

bc_seek_sources = [
  'bc_seek_main.c',
  'cli/bc_seek_cli_spec.c',
  'cli/bc_seek_cli_parsers.c',
  'discovery/bc_seek_discovery_walk_sequential.c',
  'discovery/bc_seek_discovery_walk_parallel.c',
  'filter/bc_seek_filter.c',
  'output/bc_seek_output.c',
  'strings/bc_seek_strings.c',
]

bc_seek_version_c_arg = '-DBC_SEEK_VERSION_STRING="@0@"'.format(meson.project_version())

bc_seek_bin = executable('bc-seek',
  bc_seek_sources,
  include_directories : [bc_seek_internal_inc],
  dependencies : bc_seek_deps,
  c_args : lto_c_args + [bc_seek_version_c_arg],
  link_args : lto_link_args,
  install : true,
)
