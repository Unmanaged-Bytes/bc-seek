# SPDX-License-Identifier: MIT

fuzzer_c_args = []
fuzzer_link_args = []
if cc.get_id() == 'clang'
    fuzzer_c_args += ['-fsanitize=fuzzer', '-DBC_FUZZ_LIBFUZZER']
    fuzzer_link_args += ['-fsanitize=fuzzer']
endif

bc_seek_fuzz_sources = [
    '../src/cli/bc_seek_cli_spec.c',
    '../src/cli/bc_seek_cli_parsers.c',
    '../src/filter/bc_seek_filter.c',
    '../src/strings/bc_seek_strings.c',
]

fuzz_targets = [
    'bc_seek_cli_parse',
    'bc_seek_filter_parsers',
    'bc_seek_glob_match',
]

foreach name : fuzz_targets
    executable('fuzz_' + name,
        'fuzz_' + name + '.c',
        bc_seek_fuzz_sources,
        include_directories : [bc_seek_internal_inc],
        dependencies : bc_seek_deps,
        c_args : fuzzer_c_args,
        link_args : fuzzer_link_args,
    )
endforeach
