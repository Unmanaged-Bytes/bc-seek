# bc-seek — project context

Parallel hardware-saturating file search CLI (find(1) replacement).
Walks directory trees with a lock-free MPMC queue + per-worker slots
(pattern `~/Workspace/.claude/docs/parallel-walk-pattern.md`) and
emits matching paths to stdout.

Consumer of the full modern bc-* stack: bc-core, bc-allocators,
bc-containers, bc-concurrency, bc-io, bc-runtime. Zero external
runtime dependency.

## Invariants (do not break)

- **No comments in `.c` files** — code names itself. Public / internal
  `.h` may carry one-line contracts if the signature is insufficient.
- **No defensive null-checks at function entry.** Return `false`
  on legitimate failure; never assert in production paths.
- **SPDX-License-Identifier: MIT** header on every `.c` and `.h`.
- **Strict C11** with `-Wall -Wextra -Wpedantic -Werror`.
- **Sanitizers (asan/tsan/ubsan/memcheck) stay green** in CI.
  **tsan is load-bearing** — the tool dispatches parallel tasks.
- **cppcheck stays clean**; never edit `cppcheck-suppressions.txt`
  to hide real findings.
- **Zero shared mutable state across workers** except through the
  MPMC queue and the atomic termination counter. Per-worker
  accumulators live in worker memory contexts.
- **Output ordering is non-deterministic in parallel mode** by
  design (no global sorting). `--threads=0` gives deterministic order.
- **Graceful signal handling** — SIGINT / SIGTERM propagate through
  the walk and exit with code 130.
