# M1 title-format foundation baseline — 2026-08-23

> Historical baseline: ADR-0008 later replaced the foobar2000-compatible
> evaluator measured here with the repository-specified `tkfmt-1` language.
> Re-run before using these figures as a current performance claim.

This baseline measures the early immutable compiled-program evaluator. It is not
a 1:1 compatibility claim and does not yet include remapping, arbitrary
functions, variables, multi-value metadata, or caching.

## Environment and script

The hardware/toolchain matches `2026-08-23-m0-baseline.md`. The release build
compiled this script exactly once:

```text
[%artist% - ]%title%[ - %album%]
```

Each synthetic context derived three present field strings from its track index.
Every evaluation used the same immutable `Program`.

| Tracks | Total | Throughput | 256-track p50 | 256-track p95 | Worst page |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 10,000 | 1.15 ms | 8.71 M/s | 29.29 µs | 29.92 µs | 31.72 µs |
| 100,000 | 11.72 ms | 8.53 M/s | 29.75 µs | 30.27 µs | 50.25 µs |
| 1,000,000 | 118.60 ms | 8.43 M/s | 30.22 µs | 30.60 µs | 349.90 µs |

The checksum prevents the compiler from discarding results. Unit tests also use
one program concurrently from eight threads and verify independent results.

## Corpus status

The initial runner reports:

```text
corpus_files=1 cases=7 documentation=7 black_box=0 failures=0
```

Documentation cases establish plumbing and explicit documented behavior. They
do not satisfy the black-box compatibility gate.
