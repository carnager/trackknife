<!-- SPDX-License-Identifier: GPL-3.0-only -->

# M3 workspace offscreen smoke — 2026-08-26

This is a deterministic debug-build/offscreen regression smoke, not a physical
display or network-latency claim. Command:

```text
QT_QPA_PLATFORM=offscreen ./build/dev/benchmarks/trackknife_ui_benchmark --quick
```

The 12-sample run used one million logical rows and finished with 12 of the 48
allowed pages resident. Process RSS grew by 3,468 KiB.

| Interaction | Budget | p50 | p95 | Worst |
| --- | ---: | ---: | ---: | ---: |
| Scroll and paint | 16.7 ms | 0.03 ms | 3.44 ms | 3.50 ms |
| Selection and paint | 50 ms | 0.02 ms | 2.82 ms | 2.94 ms |
| Tab switch and paint | 50 ms | 1.14 ms | 5.03 ms | 5.61 ms |
| Dock move and paint | 50 ms | 0.90 ms | 4.47 ms | 6.40 ms |
| Saved layout restore | 75 ms | 1.38 ms | 4.17 ms | 4.25 ms |
| Search loading acknowledgement | 50 ms | 1.04 ms | 1.38 ms | 1.57 ms |
| Async artwork decode and presentation | 50 ms | 1.07 ms | 4.23 ms | 4.88 ms |
| Background page load | 50 ms | 0.90 ms | 1.68 ms | 2.20 ms |

All measured p95 values were within their acceptance budgets. Live server
latency remains intentionally outside these local acknowledgement measurements.
