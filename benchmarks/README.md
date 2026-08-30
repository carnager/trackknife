# Generated scale fixtures

`SyntheticTrackFixture` deterministically exposes 10,000, 100,000, or 1,000,000
logical tracks without committing huge generated files. A track is a pure
function of its zero-based index, so a failed measurement can be reproduced.

Build the release preset, then run:

```sh
./build/release/benchmarks/trackknife_fixture_benchmark
```

The probe reports p50, p95, and worst time for generating a 256-row page. It is
an allocation/data-generation baseline, not yet a claim about rendered frame
time, database queries, title formatting, or real metadata.

## Trackbench large-list interaction

The Trackbench benchmark renders the real local list components with long
metadata, album groups, and cue-like rows sharing physical sources. Its default
release corpus is 100,000 rows; the CTest smoke uses 10,000 rows.

```sh
QT_QPA_PLATFORM=offscreen ./build/release/benchmarks/trackbench_ui_benchmark
QT_QPA_PLATFORM=offscreen ./build/release/benchmarks/trackbench_ui_benchmark --quick
QT_QPA_PLATFORM=offscreen ./build/release/benchmarks/trackbench_ui_benchmark --quick --rows=1000000
```

It reports p50, p95, and worst grouping, scroll, selection/status, cached-tab,
and artwork-update times, and exits nonzero when a p95 exceeds the shared UI
budget. The offscreen result is a deterministic regression baseline, not a
claim about compositor presentation latency.
