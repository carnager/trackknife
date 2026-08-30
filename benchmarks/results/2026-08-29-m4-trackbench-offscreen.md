<!-- SPDX-License-Identifier: GPL-3.0-only -->

# M4 Trackbench large-list offscreen baseline — 2026-08-29

This is a release-build, deterministic offscreen baseline for cached local-list
interaction. It measures the real Trackbench window, local model, grouped
delegate, side-artwork overlay, status bar, view-preset actions, and tab widget.
It does not include filesystem discovery, media probing, artwork I/O, or a
physical compositor's presentation latency.

The corpus contains 100,000 logical rows in 10,000 albums. Every fifth album
models ten cue-like logical tracks sharing one physical source; long metadata is
used throughout. The command was:

```text
QT_QPA_PLATFORM=offscreen ./build/release/benchmarks/trackbench_ui_benchmark
```

The run used Linux 7.1.8, Qt 6.11.2, GCC 16.2.1, and an AMD Ryzen 7 9700X
(8 cores/16 threads) with 60 GiB RAM. Measurements use 120 deterministic
samples except the one-time initial grouping/paint value.

| Interaction | Budget | p50 | p95 | Worst |
| --- | ---: | ---: | ---: | ---: |
| Initial grouping and first paint | 75 ms | 9.47 ms | 9.47 ms | 9.47 ms |
| Grouped scroll and paint | 16.7 ms | 3.25 ms | 3.36 ms | 4.35 ms |
| Selection/status update and paint | 50 ms | 3.55 ms | 3.67 ms | 4.54 ms |
| Cached tab switch and paint | 50 ms | 6.45 ms | 6.78 ms | 9.16 ms |
| Cached grouping and paint | 50 ms | 7.17 ms | 7.52 ms | 8.76 ms |
| Cached artwork update and paint | 50 ms | 3.23 ms | 3.34 ms | 3.82 ms |

All p95 values are within the shared provisional budgets. The benchmark exits
nonzero when any p95 exceeds its budget and has a 10,000-row debug smoke in
CTest. `--rows=N` accepts 10,000 through 1,000,000 for diagnostic scale runs.
An additional 12-sample one-million-row run stayed inside every budget: initial
grouping was 47.47 ms, with p95 values of 3.39 ms for scrolling, 7.31 ms for tab
switching, and 9.52 ms for cached regrouping.

The first version of this benchmark exposed `QHeaderView::ResizeToContents` as
an unbounded hidden cost: on the 10,000-row debug smoke it took about 760 ms to
group and roughly 780 ms to switch cached tabs. Trackbench now keeps ordinary
rows at a fixed height, applies a single section override only to album starts,
and uses the model's allocation-free group-start role. The corresponding debug
figures fell to roughly 13 ms for first grouping and 8 ms for cached tab
switching while preserving taller album-start rows and singleton behavior. A
stable model also retains its compact album-start geometry map across plain ↔
grouped preset changes; group-affecting model updates invalidate it explicitly.
