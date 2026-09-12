# ADR-0151: Parallel library scan preparation

Date: 2026-09-12

Status: accepted

## Context

The library scan processes one file at a time: an FFmpeg probe plus a
TagLib metadata read per file — two full file opens on a single
thread — followed by that file's own fsynced transaction. On a large
library this serializes into hours, and the migration-30 backfill
(ADR-0150) makes every existing row pay that price once. The cost is
worst exactly where real libraries live: on network storage, where
per-open latency dominates and a single-threaded walk leaves the link
idle most of the time. Trackbench already runs bounded parallel
decodes elsewhere (the ReplayGain scanner) and bounded parallel
TagLib readers (multi-source Apply), so the concurrency ground rules
are established in-tree.

What must not change: scans start only on explicit Refresh
(ADR-0116); every file commits in its own transaction with the fresh
revision re-check and the root scan-token check inside it; a
cancelled or failed scan retains everything already committed; and
deletion pruning stays gated on complete scans (ADR-0126).

## Decision

### Pipeline

The scan keeps a single serial walk-and-commit thread and moves only
the expensive per-file preparation into a bounded worker pool:

- The walk traverses the filesystem, observes each candidate's
  revision, and answers the unchanged check exactly as before.
  Unchanged files are touched serially on the spot.
- Changed files enter a bounded request queue. A pool of
  `clamp(cores / 2, 2, 8)` workers performs the probe, the metadata
  read, the probe-tag merge, and the tag/technical extraction, and
  pushes a prepared result (or a failure marker) into a bounded
  result queue.
- The walk thread drains prepared results opportunistically while it
  keeps walking, and fully once traversal ends. Each result commits
  in its own transaction with the unchanged guards: fresh revision
  observation compared against the walk's, and the root's scan token
  still current. Results may commit in any order; files are
  independent and progress counters are atomic.

Both queues are small (twice the worker count), so in-flight memory
stays bounded regardless of library size. Cancellation stops feeding
the pipeline, wakes every queue wait, and drains without committing;
everything already committed is kept, and the run reports itself
cancelled exactly as today.

### Durability

The library connection sets `PRAGMA synchronous=NORMAL` (it already
runs WAL). In WAL mode NORMAL loses at most the final commit on a
power loss — never consistency — and the index is a cache that the
next Refresh repairs incrementally by design. This removes the
per-file fsync from both the changed and the far more common
unchanged path. The operation and publication journals are untouched
and keep `synchronous=FULL`.

## Consequences

- Wall-clock scan time drops roughly with the worker count on local
  disks; on network storage the win is larger because up to eight
  opens overlap the link latency instead of queueing behind one.
- Commit order within a scan is no longer traversal order. Nothing
  observable depends on it: `seen` tokens, availability, pruning, and
  the progress counters are order-free.
- The serial walk still stats every file; on high-latency storage
  that remains a visible floor. A separate headless scanner binary
  running directly on the NAS (producing the same sqlite index for
  copying) is the recorded follow-up for that, not this change.
