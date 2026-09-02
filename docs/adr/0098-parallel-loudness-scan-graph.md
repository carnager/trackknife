# ADR-0098: Parallel loudness scan graph

- Status: accepted
- Date: 2026-09-02
- Owners: Trackknife project
- Extends: ADR-0097 validated loudness core

## Context

The measurement core analyzes one PCM stream. M7 needs whole selections
scanned quickly — cue segments and subsongs included — without starving
the rest of the application, with album programmes reduced correctly and
per-file failures isolated.

## Decision

### A bounded pull-based worker pool

- `scan_loudness` runs 1–16 `std::jthread` workers pulling items off one
  atomic index — the same shape as the qualified artwork Apply. Each
  worker owns exactly one decoder at a time, so FFmpeg's codec threading
  never multiplies beyond the configured worker count; parallelism is a
  caller decision (the future UI defaults it from the machine), not a
  library guess.
- Items address the same source space as playback: raw path, FFmpeg
  stream/subsong selection, and optional cue sample ranges through the
  decoder's segment opens. Progress reports per-item state transitions
  with a mutex-serialized completed count; cancellation stops cleanly
  after the items already in flight and marks the rest cancelled.

### Revisions are captured, and re-verified, per item

- Every item observes its source revision before decoding and re-checks
  it after: a file modified mid-measurement fails with a typed conflict
  instead of producing a value for bytes that no longer exist. The
  captured revision rides in the result so the application stage can
  gate writes on it (the M7 exit criterion).

### Albums reduce from retained states, or warn

- Items sharing an album key form one programme. After the pool joins,
  the retained analyzer states reduce through the ADR-0097 album
  function — gated programme loudness, never an average — with the album
  sample/true peak as the member maximum. A programme with a failed or
  cancelled member reports an incomplete-album issue instead of a number
  computed from a partial album, and a sub-gate programme reports
  unmeasurable rather than negative infinity.

## Consequences

- The remaining M7 UI slices consume one call: items in, per-track and
  per-album results with typed issues out, live progress in between.
- Tests validate the graph against direct single-threaded analysis on
  generated WAV albums (equal within PCM16 quantization), album
  dominance under gating, failure isolation, incomplete-album warnings,
  pre-cancelled runs, and worker-bound rejection.
