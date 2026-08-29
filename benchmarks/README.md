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
