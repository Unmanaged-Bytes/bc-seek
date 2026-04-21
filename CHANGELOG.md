# Changelog

## v0.1.0 — 2026-04-21

Initial release.

### Features

- `find`-style CLI with `find` subcommand and standard predicates:
  `--name`, `--iname`, `--path`, `--type`, `--size`, `--mtime`,
  `--newer`, `--perm`, `--min-depth`, `--max-depth`.
- Walk-control flags: `--hidden`, `--no-ignore`, `--follow-symlinks`,
  `--one-file-system`.
- Output: `--output PATH|-`, `--null` (NUL-separated), global
  `--threads auto|0|N`.
- Parallel walk via `bc-concurrency` MPMC queue, per-worker growable
  byte buffers, atomic termination counter.
- Sequential fallback when `effective_worker_count < 2` or
  `--threads=0`.
- Stat-lazy evaluation — zero stat syscalls when no predicate
  requires them.
- Default prune list for build / VCS directories (`.git`,
  `node_modules`, `target`, `__pycache__`, `.venv`, etc.); disable
  with `--no-ignore`.
- Thread-safe error collector flushed to stderr on exit.
- Signal handling: SIGINT / SIGTERM → exit code 130.

### Performance

On `/var/benchmarks` (~1.07M files, 19 GB, Ryzen 7 5700G, warm):
- 6.9× faster than `find -type f`
- 8.9× faster than `find -name '*.c'`
- 8.1× faster than `find -size +1M` (stat-lazy wins here)
- Parity with `fd -uu` on walk-only, +5–38 % on filtered scenarios.

### Tests

- Unit: CLI parsers (type / size / mtime / perm / threads), CLI spec
  bindings, predicate filter (glob / depth / type / size /
  stat-required path), error collector.
- End-to-end (fork/exec): help / version, hidden prune,
  `--hidden` / `--no-ignore`, name glob, min/max-depth, size filter,
  NUL separator, follow-symlinks, mono-thread, mono vs parallel
  parity.
- Green on ASAN + TSAN + UBSAN + cppcheck.
