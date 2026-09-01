# ADR-0063: Bounded file-publication Apply

- Status: accepted
- Date: 2026-08-30
- Owners: Trackknife project
- Extends: ADR-0047 bounded metadata Apply jobs, ADR-0056 fresh file
  preflight, ADR-0061 verified cross-filesystem publication, ADR-0062 active-
  playback relocation
- Amended by: ADR-0075 executor-proven directory creation remains batch-owned
  when later work rolls back

## Context

The file-publication core can safely commit or recover one physical source, but
a Trackbench preparation review normally contains several sources. Running the
single-source executor directly from the UI would block it, provide no coherent
partial result, and admit unbounded I/O if callers created a thread per file.

A file batch also has a topology dependency that metadata Apply does not. Many
reviewed targets can share a missing album-directory prefix. If those sources
run independently, both executors retain evidence that the directory is
missing; after one creates it, the other correctly treats its unexplained
appearance as a conflict. Serializing every publication avoids that race but
also needlessly serializes independent verified copies.

## Decision

### Ordered bounded execution

- `apply_file_publications` accepts only an entirely ready immutable
  `OutputPathPreflight`, a journal, and an idempotent dependent-state
  committer. Invalid worker bounds, malformed source cardinality, or an absent
  callback fail before any source is admitted.
- A fixed pool admits at most the configured 1–8 sources, with two as the
  default. It never creates one thread per source. The returned vector retains
  reviewed physical-source order regardless of completion order.
- Each result is explicitly pending, running, unchanged, committed, failed, or
  cancelled and retains source/target raw bytes, publication kind, exact commit
  evidence, and a typed issue. Runtime failure is per source: unrelated
  successful publications remain committed and retry requires a fresh review.
- A reviewed no-change source completes without a worker, journal record, or
  dependent-state callback. It remains in progress/results so counts align
  with the preview.

### Fresh admission and publication dispatch

- Every admitted changed source receives a new single-source filesystem
  preflight made from its reviewed plan. A changed source revision, occupied
  target, new symlink, access loss, filesystem-kind/device change, or any other
  blocker becomes that source's failed result before mutation.
- The refreshed classification dispatches only to the qualified same-
  filesystem rename or cross-filesystem verified-copy executor. A changed
  source can never silently become a no-op.
- The journal interface is wrapped by the batch with serialized method access.
  Individual source I/O and dependent callbacks may run concurrently, while
  durable journal transitions remain safe even for an implementation whose
  own interface did not promise concurrent calls.
- The dependent committer may be invoked concurrently and must remain
  idempotent. Trackbench composes ADR-0062 audio relocation with ADR-0059
  durable list/cache relocation there; those services retain their own
  serialized worker boundaries.

### Shared missing-directory admission

- Sources whose reviewed missing-directory chains begin at the same raw path
  share one topology admission group. Refresh plus the first publication that
  still needs directories stays under that group lock.
- An executor records only the missing directories its descriptor-relative
  creation routine necessarily created, immediately after it has opened and
  synced the exact chain. A later source may accept a shortened missing chain
  only when every appeared prefix has that exact in-batch evidence.
  Otherwise an externally appeared directory remains a conflict.
- Once a source refreshes with no directories left to create, it releases the
  group before file publication. Multiple copies into the now-established
  album tree can therefore proceed in parallel. Different missing roots never
  block one another.
- If later preparation or dependent-state work fails, the source publication
  still rolls back but its already proven directory-creation evidence remains
  valid for related batch members. A directory that appeared before the
  executor proved creation contributes no evidence and still fails closed.

### Cancellation and progress

- Cancellation stops admission of new changed sources. Already running
  executors receive the token and reach their existing safe journaled boundary:
  pre-publication work cleans up, while post-publication work completes or
  rolls back coherently.
- Every admitted changed source emits running and terminal progress. No-change
  sources emit one terminal update. Callback delivery and completed counts are
  serialized and monotonic even when worker completion races.
- Pending changed sources left after cancellation are explicitly classified as
  cancelled, so the final result accounts for the complete reviewed selection.

## Alternatives considered

### Run every source sequentially

Rejected as the permanent boundary. It avoids directory races but wastes
independent I/O capacity, especially for cross-filesystem copies. The admission
groups serialize only the topology dependency.

### Start one worker per source

Rejected. Large selections would produce unbounded threads, open descriptors,
copy buffers, and journal pressure.

### Accept any directory that appears after preview

Rejected. An external path or symlink race must not be mistaken for another
batch member. Only exact directories reported after the related executor's
successful descriptor-relative creation may shorten a reviewed missing chain.

### Roll the entire batch back after one failure

Rejected. Each source already has an independent durable state machine, and a
later rollback can itself fail or race external use. Ordered partial results
tell the truth and require a fresh preview for retry.

## Consequences

- The same immutable review can now safely drive many same- and cross-
  filesystem path-only publications with bounded concurrency, complete result
  accounting, and responsive cancellation.
- Common album-folder moves no longer generate false topology conflicts merely
  because one selected source established the reviewed directory first.
- This is the Qt-free job boundary. Workspace controls still need to capture a
  durable workspace snapshot, compose the real audio/list callback, present
  progress and partial results, refresh visible models, and invalidate the
  preview after every attempt.
- Cross-filesystem undo, combined tag/file publication, companion files, and
  operation-history grouping remain separate work.

## Validation

- Real same-filesystem files with two different target branches under one
  missing prefix commit with two workers while one no-change source produces
  no journal or callback.
- Progress events retain monotonic completion counts and result rows retain
  reviewed order despite concurrent completion.
- An injected dependent failure rolls back only its source while another
  source remains committed.
- Two blocked in-flight dependent callbacks prove the worker bound;
  cancellation admits no later sources and all four sources end in verified
  source-side state.
- A real multi-buffer `/tmp` to `/dev/shm` case, when distinct devices are
  available, proves automatic cross-filesystem dispatch through the batch.

## Revisit when

- one preparation Apply combines metadata rewrite and path publication into a
  single destination artifact;
- per-device I/O budgets replace the initial global mutation-worker bound;
- operation history groups source records beneath a user-visible batch ID.
