# bc-seek

Parallel hardware-saturating file search for Linux. `find`-compatible
predicates, a `fd`-like defaults, with a work-stealing parallel walk that
saturates multi-core hosts.

**Status:** production-ready for Linux x86_64 (v0.1.0).

## Benchmarks

On a Ryzen 7 5700G / 32 GB / NVMe, warm cache, corpus
`/var/benchmarks` (~1.07M files, 19 GB):

| Scenario | bc-seek (auto) | find | fd | speedup vs find | vs fd |
|---|---|---|---|---|---|
| walk `--type=f` | **0.230s** | 1.599s | 0.263s | 6.9× | 14% faster |
| name `*.c` | **0.212s** | 1.896s | 0.224s | 8.9× | 5% faster |
| size `>1M` | **0.383s** | 3.095s | 0.620s | 8.1× | 38% faster |

Mono-thread (`--threads=0`) also beats `find` by ~1.7× thanks to a
leaner per-entry evaluation (basename + `d_type` + stat-lazy).

Reproduce with `./scripts/bench.sh /path/to/corpus`.

## Install

```bash
# Build requirements: meson >= 0.57, ninja, cmocka (tests).
# Runtime requirements: bc-core, bc-allocators, bc-containers,
# bc-concurrency, bc-io, bc-runtime (via pkg-config).

bc-build . release
sudo bc-install .
```

## Usage

```
bc-seek [GLOBAL OPTIONS] find [OPTIONS] [PATH...]

Global options:
  --threads auto|0|N    worker count (default: auto = physical cores)

find options:
  --name GLOB           basename fnmatch (case-sensitive)
  --iname GLOB          basename fnmatch (case-insensitive)
  --path GLOB           full-path fnmatch
  --type f|d|l          filter by entry type
  --size +N|-N|N[cwbkMGT]   find(1) size semantics
  --mtime +N|-N|N       days since modification (find(1))
  --newer PATH          modified after reference file
  --perm OCTAL          exact permission match (e.g. 755)
  --max-depth N         descend at most N levels
  --min-depth N         skip entries shallower than N
  --hidden              include dotfiles (default: skipped)
  --no-ignore           disable default prune list (.git, node_modules, ...)
  --follow-symlinks     dereference symlinks during walk
  --one-file-system     don't cross filesystem boundaries
  --null, -0            NUL-separated output (xargs -0)
  --output PATH | -     output destination (default: stdout)

Examples:
  bc-seek find --type=f --name='*.c' ~/src
  bc-seek find --type=f --size=+1M --mtime=-7 /var/log
  bc-seek find --no-ignore --hidden /  | wc -l
  bc-seek find --null ~/docs | xargs -0 -n1 wc -c
```

`bc-seek` mirrors `find(1)` semantics for predicates but ships with
`fd(1)`-style sane defaults: hidden files and well-known build
directories (`.git`, `node_modules`, `target`, `__pycache__`, etc.)
are pruned unless `--hidden` / `--no-ignore` is passed.

## Architecture

- **Walk strategy**: parallel 3-phase pattern from
  [`bc-hash` iter8](../../.claude/docs/parallel-walk-pattern.md).
  - Phase A (workers): MPMC queue of directory paths. Each worker
    pops a directory, runs `readdir`, appends matches to a
    per-worker growable byte buffer, enqueues sub-directories.
    Termination by atomic `outstanding_directory_count` counter.
  - Phase B (main): each worker's byte buffer is written to
    `stdout` in one `fwrite(3)` call. No lock-held `printf`.
- **Stat-lazy**: `fstatat` is only called when a predicate requires
  it (`--size`, `--mtime`, `--newer`, `--perm`). Pure name/type
  queries skip all stat syscalls — directly off `d_type`.
- **O_NOFOLLOW strict**: the parallel walk cannot cycle on Linux
  without `--follow-symlinks` (POSIX forbids hardlinks on
  directories, `.`/`..` filtered at readdir).
- **Zero external runtime dependency**: pure `bc-*` ecosystem +
  libc. No gitignore parser, no regex engine in v0.1 (fnmatch-only).
- **Thread-safe error collection**: errors accumulate in a shared
  atomic-flag-protected vector and flush to stderr on exit.

## Exit codes

- `0`: success, all matches written
- `1`: at least one filesystem error was collected (path permission,
       broken symlink, etc.)
- `2`: CLI parse error
- `130`: interrupted by SIGINT / SIGTERM

## Limitations

- **v0.1.0 predicates**: conjunction only (AND). No `-or`, `-not`,
  or expression grouping. Use shell piping / multiple invocations.
- **No regex**: `--name` / `--path` use `fnmatch(3)` (shell globs).
- **No `-exec`**: pair with `xargs -0`:
  `bc-seek find -0 ... | xargs -0 cmd`
- **No gitignore parsing**: default prune list is a fixed whitelist
  (the 14 most common build / VCS directories). `--no-ignore`
  disables it entirely.
- **Output ordering is non-deterministic** in parallel mode
  (depends on worker interleaving). Use `--threads=0` for
  deterministic ordering, or pipe to `sort`.
- **Linux only**: uses `O_NOFOLLOW`, `openat`, `fstatat`, `fnmatch`.

## Development

```bash
# Debug build + tests + sanitizers + cppcheck
bc-build . debug
bc-test . debug
bc-check .
bc-sanitize . asan
bc-sanitize . tsan
bc-sanitize . ubsan
```

See [`PLAN.md`](PLAN.md) for the implementation roadmap, perf
targets, and architectural notes.

## License

MIT. See [`LICENSE`](LICENSE).
